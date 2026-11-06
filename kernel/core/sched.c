/* Round-robin scheduler with a non-preemptive kernel (see sched.h). */
#include "core/sched.h"
#include "core/process.h"
#include "core/time.h"
#include "core/log.h"
#include "core/panic.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "memory/virtual.h"
#include "lib.h"

void context_switch(uint64_t *save_rsp, uint64_t new_rsp);
void task_trampoline(void);
void task_start_c(void);

static struct task tasks[MAX_TASKS];
static struct task *current;
static struct task *desktop_task;
static struct task *idle_task;
static struct task *to_reap;
static volatile bool need_resched;
static int slice_left = SCHED_SLICE_TICKS;
static int next_id = 1;
static uint64_t total_switches;

struct task *sched_current(void) { return current; }
struct task *sched_desktop(void) { return desktop_task; }
bool sched_need_resched(void) { return need_resched; }
int sched_task_count(void) { return MAX_TASKS; }
struct task *sched_task_at(int i) { return (i >= 0 && i < MAX_TASKS && tasks[i].state != TASK_UNUSED) ? &tasks[i] : NULL; }
uint64_t sched_context_switches(void) { return total_switches; }

static void idle_main(void *arg)
{
    UNUSED(arg);
    for (;;)
        cpu_sti_hlt(); /* the timer preempts the idle task */
}

void sched_init(void)
{
    struct task *t = &tasks[0];
    t->id = 0;
    t->state = TASK_RUNNABLE;
    str_copy(t->name, "desktop", sizeof t->name);
    t->slot = -1;
    t->stack_top = g_vmm.stack_top;
    current = desktop_task = t;
    gdt_set_kernel_stack(t->stack_top);
    idle_task = task_create("idle", idle_main, NULL, NULL);
    if (!idle_task)
        panic("sched: cannot create idle task");
    idle_task->idle = true;
    klog("sched: round-robin, %d ms slices for user code, non-preemptive kernel",
         SCHED_SLICE_TICKS * 1000 / PIT_TARGET_HZ);
}

struct task *task_create(const char *name, void (*entry)(void *), void *arg, struct process *proc)
{
    uint64_t f = irq_save();
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        irq_restore(f);
        return NULL;
    }
    struct task *t = &tasks[slot];
    memset(t, 0, sizeof *t);
    t->state = TASK_DEAD; /* reserved while the stack is set up */
    irq_restore(f);

    uint64_t top = vmm_kstack_alloc(slot);
    if (!top) {
        t->state = TASK_UNUSED;
        return NULL;
    }
    t->id = next_id++;
    str_copy(t->name, name, sizeof t->name);
    t->slot = slot;
    t->stack_top = top;
    t->proc = proc;
    t->entry = entry;
    t->arg = arg;
    /* Initial frame consumed by context_switch: rflags, r15..r12, rbx, rbp,
     * then the return address. RSP after `ret` is 16-byte aligned. */
    uint64_t *sp = (uint64_t *)top;
    *--sp = (uint64_t)(uintptr_t)task_trampoline;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    *--sp = 0x2; /* RFLAGS with IF clear; the trampoline enables interrupts */
    t->rsp = (uint64_t)(uintptr_t)sp;
    t->state = TASK_RUNNABLE;
    return t;
}

static void finish_switch(void)
{
    if (to_reap && to_reap != current) {
        struct task *d = to_reap;
        to_reap = NULL;
        vmm_kstack_free(d->slot);
        d->state = TASK_UNUSED;
    }
}

void task_start_c(void)
{
    finish_switch();
    cpu_sti();
    current->entry(current->arg);
    task_exit();
}

static struct task *pick_next(void)
{
    int start = (int)(current - tasks);
    for (int k = 1; k <= MAX_TASKS; k++) {
        struct task *t = &tasks[(start + k) % MAX_TASKS];
        if (t->state == TASK_RUNNABLE && !t->idle)
            return t;
    }
    return idle_task;
}

static void switch_to(struct task *next)
{
    struct task *prev = current;
    slice_left = SCHED_SLICE_TICKS;
    current = next;
    next->switches++;
    total_switches++;
    phys_addr_t space = next->proc ? process_space(next->proc) : vmm_kernel_space();
    if (read_cr3() != space)
        write_cr3(space);
    gdt_set_kernel_stack(next->stack_top);
    context_switch(&prev->rsp, next->rsp);
    /* Execution resumes here when `prev` is scheduled again. */
    finish_switch();
}

void schedule(void)
{
    uint64_t f = irq_save();
    need_resched = false;
    struct task *next = pick_next();
    if (next != current)
        switch_to(next);
    irq_restore(f);
}

void sched_yield(void) { schedule(); }

NORETURN void task_exit(void)
{
    cpu_cli();
    if (current == desktop_task)
        panic("sched: the desktop task tried to exit");
    current->state = TASK_DEAD;
    to_reap = current;
    schedule();
    panic("sched: dead task resumed");
}

bool sched_block(uint64_t timeout_ms)
{
    uint64_t f = irq_save();
    current->state = TASK_BLOCKED;
    current->timed_out = false;
    current->wake_ms = timeout_ms ? time_ms() + timeout_ms : 0;
    schedule();
    bool woke = !current->timed_out;
    current->wait_obj = NULL;
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
        uint64_t f = irq_save();
        sched_block(end - now);
        irq_restore(f);
        if (current->proc && process_kill_pending(current->proc))
            return;
    }
}

void sched_wake(struct task *t)
{
    if (!t)
        return;
    uint64_t f = irq_save();
    if (t->state == TASK_BLOCKED) {
        t->state = TASK_RUNNABLE;
        t->wake_ms = 0;
        if (current->idle || current->proc)
            need_resched = true;
    }
    irq_restore(f);
}

bool sched_wait_on(const void *obj, uint64_t timeout_ms)
{
    uint64_t f = irq_save();
    current->wait_obj = obj;
    bool r = sched_block(timeout_ms);
    irq_restore(f);
    return r;
}

void sched_wake_all(const void *obj)
{
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wait_obj == obj)
            sched_wake(&tasks[i]);
    irq_restore(f);
}

void sched_tick(void)
{
    uint64_t now = time_ms();
    bool woke_any = false;
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_BLOCKED && t->wake_ms && now >= t->wake_ms) {
            t->state = TASK_RUNNABLE;
            t->timed_out = true;
            t->wake_ms = 0;
            woke_any = true;
        }
    }
    if (current)
        current->cpu_ticks++;
    if (current && current->idle) {
        if (woke_any || pick_next() != idle_task)
            need_resched = true;
    } else if (current && current->proc) {
        if (--slice_left <= 0)
            need_resched = true;
        else if (woke_any)
            need_resched = true;
    }
}

void sched_preempt_point(bool from_user)
{
    if (!need_resched || !current)
        return;
    if (from_user || current->idle)
        schedule();
}
