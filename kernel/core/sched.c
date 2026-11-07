/* SMP round-robin scheduler with a non-preemptive kernel (see sched.h). */
#include "core/sched.h"
#include "core/process.h"
#include "core/time.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/spinlock.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "arch/percpu.h"
#include "drivers/apic.h"
#include "memory/virtual.h"
#include "lib.h"

void context_switch(uint64_t *save_rsp, uint64_t new_rsp);
void task_trampoline(void);
void task_start_c(void);
void bkl_sched_out(struct task *prev);
void bkl_sched_in(struct task *t);

static struct task tasks[MAX_TASKS];

void fpu_init_state(uint8_t *area)
{
    memset(area, 0, 512);
    area[0] = 0x7F; /* FCW 0x037F: all x87 exceptions masked, 64-bit precision */
    area[1] = 0x03;
    area[24] = 0x80; /* MXCSR 0x1F80: all SSE exceptions masked, round to nearest */
    area[25] = 0x1F;
}
static struct task *desktop_task;
static DEFINE_SPINLOCK(sched_lock, "sched");
static int rr_cursor;   /* round-robin position in tasks[] */
static int next_id = 1;
static uint64_t total_switches;

/* Per-CPU scheduler state lives in struct cpu (arch/percpu.h). */
#define CURRENT (this_cpu()->current)

struct task *sched_current(void) { return CURRENT; }
struct task *sched_desktop(void) { return desktop_task; }
bool sched_need_resched(void) { return this_cpu()->need_resched; }
int sched_task_count(void) { return MAX_TASKS; }
struct task *sched_task_at(int i) { return (i >= 0 && i < MAX_TASKS && tasks[i].state != TASK_UNUSED) ? &tasks[i] : NULL; }
uint64_t sched_context_switches(void) { return __atomic_load_n(&total_switches, __ATOMIC_RELAXED); }

static void idle_main(void *arg)
{
    UNUSED(arg);
    for (;;)
        cpu_sti_hlt(); /* the timer or a reschedule IPI wakes the idle task */
}

void sched_init(void)
{
    struct task *t = &tasks[0];
    t->id = 0;
    t->state = TASK_RUNNABLE;
    str_copy(t->name, "desktop", sizeof t->name);
    t->slot = -1;
    t->stack_top = g_vmm.stack_top;
    t->on_cpu = 0;
    t->cpu_mask_seen = 1;
    struct cpu *c = this_cpu();
    c->current = desktop_task = t;
    c->slice_left = SCHED_SLICE_TICKS;
    c->online = true;
    gdt_set_kernel_stack(t->stack_top);
    c->idle = task_create("idle0", idle_main, NULL, NULL);
    if (!c->idle)
        panic("sched: cannot create idle task");
    c->idle->idle = true;
    klog("sched: round-robin, %d ms slices for user code, non-preemptive kernel, SMP-capable",
         SCHED_SLICE_TICKS * 1000 / PIT_TARGET_HZ);
}

static struct task *alloc_task(const char *name)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }
    struct task *t = NULL;
    if (slot >= 0) {
        t = &tasks[slot];
        memset(t, 0, sizeof *t);
        t->state = TASK_DEAD; /* reserved while the stack is set up */
        t->id = next_id++;
        t->slot = slot;
        t->on_cpu = -1;
        t->last_cpu = -1;
        t->pin_cpu = -1;
        str_copy(t->name, name, sizeof t->name);
    }
    spin_unlock_irqrestore(&sched_lock, f);
    if (!t)
        return NULL;
    uint64_t top = vmm_kstack_alloc(slot);
    if (!top) {
        t->state = TASK_UNUSED;
        return NULL;
    }
    t->stack_top = top;
    return t;
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg, struct process *proc)
{
    struct task *t = alloc_task(name);
    if (!t)
        return NULL;
    t->proc = proc;
    t->entry = entry;
    t->arg = arg;
    if (proc)
        fpu_init_state(t->fpu);
    /* Initial frame consumed by context_switch: rflags, r15..r12, rbx, rbp,
     * then the return address. RSP after `ret` is 16-byte aligned. */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)(uintptr_t)task_trampoline;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    *--sp = 0x2; /* RFLAGS with IF clear; the trampoline enables interrupts */
    t->rsp = (uint64_t)(uintptr_t)sp;
    __atomic_store_n(&t->state, TASK_RUNNABLE, __ATOMIC_RELEASE);
    return t;
}

struct task *task_create_pinned(const char *name, void (*entry)(void *), void *arg, int cpu)
{
    struct task *t = alloc_task(name);
    if (!t)
        return NULL;
    t->entry = entry;
    t->arg = arg;
    t->pin_cpu = cpu;
    t->no_bkl = true;
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)(uintptr_t)task_trampoline;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    *--sp = 0x2;
    t->rsp = (uint64_t)(uintptr_t)sp;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    t->state = TASK_RUNNABLE;
    if (cpu >= 0 && (uint32_t)cpu < g_ncpus && g_cpus[cpu].online) {
        g_cpus[cpu].need_resched = true;
        if (&g_cpus[cpu] != this_cpu())
            lapic_send_ipi(g_cpus[cpu].apic_id, IPI_RESCHED_VECTOR);
    }
    spin_unlock_irqrestore(&sched_lock, f);
    return t;
}

struct task *sched_create_ap_idle(uint32_t cpu_index)
{
    char name[16];
    k_snprintf(name, sizeof name, "idle%u", cpu_index);
    struct task *t = alloc_task(name);
    if (!t)
        return NULL;
    t->idle = true;
    t->on_cpu = (int)cpu_index; /* reserved for that CPU */
    t->state = TASK_RUNNABLE;
    return t;
}

void sched_init_ap(void)
{
    struct cpu *c = this_cpu();
    c->current = c->idle;
    c->idle->cpu_mask_seen |= 1u << c->index;
    c->slice_left = SCHED_SLICE_TICKS;
    gdt_set_kernel_stack(c->idle->stack_top);
}

NORETURN void sched_ap_idle(void)
{
    for (;;) {
        cpu_sti_hlt();
        if (this_cpu()->need_resched)
            schedule();
    }
}

/* Frees a dead task's kernel stack. Runs with no spinlock held, because
 * unmapping the stack may need a TLB shootdown. */
static void reap_dead(void)
{
    struct cpu *c = this_cpu();
    struct task *d = c->to_reap;
    if (d && d != c->current) {
        c->to_reap = NULL;
        vmm_kstack_free(d->slot);
        __atomic_store_n(&d->state, TASK_UNUSED, __ATOMIC_RELEASE);
    }
}

void task_start_c(void)
{
    /* First run of a new task: the CPU that switched to us still holds
     * sched_lock on our behalf. */
    spin_unlock(&sched_lock);
    reap_dead();
    cpu_sti();
    if (!CURRENT->idle && !CURRENT->no_bkl)
        bkl_lock(); /* kernel code of new tasks runs under the BKL */
    CURRENT->entry(CURRENT->arg);
    task_exit();
}

static bool can_run_here(const struct task *t, const struct cpu *c)
{
    return t->state == TASK_RUNNABLE && !t->idle && (t->on_cpu < 0 || t == c->current) &&
           (t->pin_cpu < 0 || t->pin_cpu == (int)c->index);
}

/* sched_lock held. Round-robin over the global table: the first runnable
 * task after the cursor that no other CPU is running. */
static struct task *pick_next(struct cpu *c)
{
    for (int k = 1; k <= MAX_TASKS; k++) {
        int i = (rr_cursor + k) % MAX_TASKS;
        if (can_run_here(&tasks[i], c)) {
            rr_cursor = i;
            return &tasks[i];
        }
    }
    return c->idle;
}

/* sched_lock held: a task became runnable; get an idle CPU to take it. */
static void kick_idle_cpu(void)
{
    struct cpu *self = this_cpu();
    for (uint32_t i = 0; i < g_ncpus; i++) {
        struct cpu *c = &g_cpus[i];
        if (!c->online || !c->current || !c->current->idle)
            continue;
        c->need_resched = true;
        if (c != self) {
            c->ipis++;
            lapic_send_ipi(c->apic_id, IPI_RESCHED_VECTOR);
        }
        return;
    }
    /* No CPU idle: if this CPU runs user code, let the next tick decide. */
}

void sched_resched_ipi(void) { this_cpu()->need_resched = true; }

static void switch_to(struct cpu *c, struct task *prev, struct task *next)
{
    c->slice_left = SCHED_SLICE_TICKS;
    prev->on_cpu = -1;
    prev->last_cpu = (int)c->index;
    next->on_cpu = (int)c->index;
    next->cpu_mask_seen |= 1u << c->index;
    c->current = next;
    next->switches++;
    __atomic_add_fetch(&total_switches, 1, __ATOMIC_RELAXED);
    c->switches++;
    phys_addr_t space = next->proc ? process_space(next->proc) : vmm_kernel_space();
    if (read_cr3() != space)
        write_cr3(space);
    gdt_set_kernel_stack(next->stack_top);
    if (prev->proc)
        fpu_save(prev->fpu);
    if (next->proc) {
        fpu_restore(next->fpu);
        wrmsr(MSR_FS_BASE, next->fs_base);
    }
    bkl_sched_out(prev);
    context_switch(&prev->rsp, next->rsp);
    /* Resumes here when `prev` runs again, possibly on another CPU; the CPU
     * that switched to it holds sched_lock, released by schedule(). */
}

void schedule(void)
{
    uint64_t f = irq_save();
    lock_assert_none_held(NULL, "schedule");
    spin_lock(&sched_lock);
    struct cpu *c = this_cpu();
    c->need_resched = false;
    struct task *prev = c->current;
    struct task *next = pick_next(c);
    if (next == prev) {
        spin_unlock(&sched_lock);
        irq_restore(f);
        return;
    }
    switch_to(c, prev, next);
    spin_unlock(&sched_lock);
    reap_dead();
    bkl_sched_in(CURRENT); /* re-take the BKL if this task held it */
    irq_restore(f);
}

void sched_yield(void) { schedule(); }

NORETURN void task_exit(void)
{
    cpu_cli();
    if (CURRENT == desktop_task)
        panic("sched: the desktop task tried to exit");
    spin_lock(&sched_lock);
    CURRENT->state = TASK_DEAD;
    this_cpu()->to_reap = CURRENT;
    spin_unlock(&sched_lock);
    schedule();
    panic("sched: dead task resumed");
}

bool sched_block(uint64_t timeout_ms)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    struct task *t = CURRENT;
    if (t->wake_pending) {
        /* A wakeup arrived after the caller checked its condition. */
        t->wake_pending = false;
        t->wait_obj = NULL;
        spin_unlock_irqrestore(&sched_lock, f);
        return true;
    }
    t->state = TASK_BLOCKED;
    t->timed_out = false;
    t->wake_ms = timeout_ms ? time_ms() + timeout_ms : 0;
    spin_unlock(&sched_lock);
    schedule();
    bool woke = !t->timed_out;
    t->wait_obj = NULL;
    irq_restore(f);
    return woke;
}

void sched_sleep_ms(uint64_t ms)
{
    uint64_t end = time_ms() + ms;
    for (;;) {
        uint64_t now = time_ms();
        if (now >= end)
            return;
        sched_block(end - now);
        if (CURRENT->proc && task_interrupted(CURRENT))
            return;
    }
}

/* sched_lock held. */
static void wake_locked(struct task *t)
{
    if (t->state == TASK_BLOCKED) {
        t->state = TASK_RUNNABLE;
        t->wake_ms = 0;
        if (t->on_cpu < 0)
            kick_idle_cpu();
        struct cpu *c = this_cpu();
        if (c->current && (c->current->idle || c->current->proc))
            c->need_resched = true;
    } else if (t->state == TASK_RUNNABLE) {
        t->wake_pending = true;
    }
}

void sched_wake(struct task *t)
{
    if (!t)
        return;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    wake_locked(t);
    spin_unlock_irqrestore(&sched_lock, f);
}

bool sched_wait_on(const void *obj, uint64_t timeout_ms)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    CURRENT->wait_obj = obj;
    spin_unlock_irqrestore(&sched_lock, f);
    return sched_block(timeout_ms);
}

void sched_wake_all(const void *obj)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].wait_obj == obj && (tasks[i].state == TASK_BLOCKED || tasks[i].state == TASK_RUNNABLE))
            wake_locked(&tasks[i]);
    spin_unlock_irqrestore(&sched_lock, f);
}

int sched_wake_n(const void *obj, int n)
{
    int woke = 0;
    uint64_t f = spin_lock_irqsave(&sched_lock);
    for (int i = 0; i < MAX_TASKS && woke < n; i++)
        if (tasks[i].wait_obj == obj && tasks[i].state == TASK_BLOCKED) {
            wake_locked(&tasks[i]);
            woke++;
        }
    spin_unlock_irqrestore(&sched_lock, f);
    return woke;
}

void sched_kick(struct task *t)
{
    uint64_t f = spin_lock_irqsave(&sched_lock);
    int cpu = t->on_cpu;
    struct cpu *self = this_cpu();
    if (cpu >= 0 && (uint32_t)cpu < g_ncpus && &g_cpus[cpu] != self && g_cpus[cpu].online) {
        g_cpus[cpu].ipis++;
        lapic_send_ipi(g_cpus[cpu].apic_id, IPI_RESCHED_VECTOR);
    } else if (cpu >= 0 && &g_cpus[cpu] == self) {
        self->need_resched = true;
    }
    spin_unlock_irqrestore(&sched_lock, f);
}

void sched_tick(void)
{
    struct cpu *c = this_cpu();
    c->ticks++;
    struct task *cur = c->current;
    if (cur) {
        cur->cpu_ticks++;
        if (cur->idle)
            c->idle_ticks++;
        else if (cur->proc)
            c->user_ticks++;
        else
            c->kernel_ticks++;
    }
    spin_lock(&sched_lock); /* IRQ context: interrupts are already off */
    bool woke_any = false;
    if (c->index == 0) {
        uint64_t now = time_ms();
        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            struct process *ap = t->proc;
            if (ap && ap->alarm_ms && now >= ap->alarm_ms && ap->nthreads && ap->threads[0] == t) {
                /* SIGALRM: set the pending bit and wake the main thread. */
                ap->alarm_ms = 0;
                __atomic_or_fetch(&ap->sigpending, 1ull << (AURORA_SIGALRM - 1), __ATOMIC_SEQ_CST);
                if (t->state == TASK_BLOCKED) {
                    t->state = TASK_RUNNABLE;
                    t->wake_ms = 0;
                    woke_any = true;
                    if (t->on_cpu < 0)
                        kick_idle_cpu();
                } else if (t->on_cpu >= 0 && (uint32_t)t->on_cpu != c->index) {
                    lapic_send_ipi(g_cpus[t->on_cpu].apic_id, IPI_RESCHED_VECTOR);
                }
            }
            if (t->state == TASK_BLOCKED && t->wake_ms && now >= t->wake_ms) {
                t->state = TASK_RUNNABLE;
                t->timed_out = true;
                t->wake_ms = 0;
                woke_any = true;
                if (t->on_cpu < 0)
                    kick_idle_cpu();
            }
        }
    }
    if (cur && cur->idle) {
        if (woke_any || pick_next(c) != c->idle)
            c->need_resched = true;
    } else if (cur && cur->proc) {
        if (--c->slice_left <= 0 || woke_any)
            c->need_resched = true;
    }
    spin_unlock(&sched_lock);
}

void sched_preempt_point(bool from_user)
{
    struct cpu *c = this_cpu();
    if (!c->need_resched || !c->current)
        return;
    if (from_user || c->current->idle)
        schedule();
}
