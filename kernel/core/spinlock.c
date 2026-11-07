/* Ticket spinlocks, the lock-order validator and the big kernel lock (see
 * core/spinlock.h). */
#include "core/spinlock.h"
#include "core/sched.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/time.h"
#include "arch/percpu.h"
#include "arch/x86_64/io.h"
#include "lib.h"

static inline void raw_acquire(struct spinlock *l)
{
    uint32_t ticket = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != ticket)
        cpu_pause();
}

static inline void raw_release(struct spinlock *l)
{
    __atomic_store_n(&l->owner, l->owner + 1, __ATOMIC_RELEASE);
}

bool spin_is_locked(const struct spinlock *l)
{
    return __atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != __atomic_load_n(&l->next, __ATOMIC_ACQUIRE);
}

/* ---- Lock-order validator --------------------------------------------------- */
#ifdef AURORA_LOCKDEP
#define MAX_CLASSES 64
static uint64_t edges[MAX_CLASSES]; /* edges[a] bit b: b was taken while a was held */
static const char *class_names[MAX_CLASSES];
static volatile uint32_t graph_lock;
static int nclasses;
static uint32_t violations, long_holds;

static void graph_acquire(void)
{
    while (__atomic_exchange_n(&graph_lock, 1, __ATOMIC_ACQUIRE))
        cpu_pause();
}
static void graph_release(void) { __atomic_store_n(&graph_lock, 0, __ATOMIC_RELEASE); }

static int class_id(struct lock_class *c)
{
    int id = __atomic_load_n(&c->id, __ATOMIC_ACQUIRE);
    if (id >= 0)
        return id;
    graph_acquire();
    if (c->id < 0 && nclasses < MAX_CLASSES) {
        class_names[nclasses] = c->name;
        __atomic_store_n(&c->id, nclasses++, __ATOMIC_RELEASE);
    }
    id = c->id;
    graph_release();
    return id;
}

static bool reaches(int from, int to, uint64_t seen)
{
    if (from == to)
        return true;
    seen |= 1ull << from;
    uint64_t e = edges[from] & ~seen;
    while (e) {
        int n = __builtin_ctzll(e);
        e &= e - 1;
        if (reaches(n, to, seen))
            return true;
    }
    return false;
}

static void report(const char *what, const char *a, const char *b)
{
    __atomic_add_fetch(&violations, 1, __ATOMIC_RELAXED);
    klog("LOCKDEP: %s: %s -> %s (cpu %u)", what, a, b, this_cpu()->index);
    TEST_EVENT("LOCKDEP %s %s -> %s", what, a, b);
}

static void check_acquire(struct spinlock *l)
{
    struct cpu *c = this_cpu();
    int b = class_id(l->cls);
    if (b < 0)
        return;
    for (int i = 0; i < c->nheld; i++) {
        struct spinlock *h = c->held[i];
        if (h == l) {
            report("recursive acquisition", l->cls->name, l->cls->name);
            panic("spinlock %s taken twice on cpu %u", l->cls->name, c->index);
        }
        int a = h->cls->id;
        if (a < 0 || (edges[a] >> b) & 1)
            continue;
        graph_acquire();
        if (!((edges[a] >> b) & 1)) {
            if (reaches(b, a, 0))
                report("lock order inversion", h->cls->name, l->cls->name);
            else
                edges[a] |= 1ull << b;
        }
        graph_release();
    }
}

static void push_held(struct spinlock *l)
{
    struct cpu *c = this_cpu();
    if (c->nheld < (int)ARRAY_LEN(c->held))
        c->held[c->nheld++] = l;
    l->holder_cpu = (int)c->index;
    l->taken_ms = time_ms();
}

static void pop_held(struct spinlock *l)
{
    struct cpu *c = this_cpu();
    uint64_t held = time_ms() - l->taken_ms;
    if (held > LOCK_HOLD_WARN_MS) {
        __atomic_add_fetch(&long_holds, 1, __ATOMIC_RELAXED);
        klog("LOCKDEP: %s held for %llu ms (cpu %u)", l->cls->name, (unsigned long long)held, c->index);
    }
    for (int i = c->nheld - 1; i >= 0; i--)
        if (c->held[i] == l) {
            memmove(&c->held[i], &c->held[i + 1], (size_t)(c->nheld - i - 1) * sizeof c->held[0]);
            c->nheld--;
            break;
        }
    l->holder_cpu = -1;
}

int lock_held_count(void) { return this_cpu()->nheld; }

void lock_assert_none_held(const struct spinlock *except, const char *where)
{
    struct cpu *c = this_cpu();
    for (int i = 0; i < c->nheld; i++)
        if (c->held[i] != except) {
            report("sleeping while holding", c->held[i]->cls->name, where);
            panic("%s while holding spinlock %s", where, c->held[i]->cls->name);
        }
}

uint32_t lockdep_violations(void) { return violations; }
uint32_t lockdep_classes(void) { return (uint32_t)nclasses; }
uint32_t lockdep_long_holds(void) { return long_holds; }
#else
static inline void check_acquire(struct spinlock *l) { UNUSED(l); }
static inline void push_held(struct spinlock *l) { UNUSED(l); }
static inline void pop_held(struct spinlock *l) { UNUSED(l); }
int lock_held_count(void) { return 0; }
void lock_assert_none_held(const struct spinlock *except, const char *where) { UNUSED(except); UNUSED(where); }
uint32_t lockdep_violations(void) { return 0; }
uint32_t lockdep_classes(void) { return 0; }
uint32_t lockdep_long_holds(void) { return 0; }
#endif

void spin_lock(struct spinlock *l)
{
    check_acquire(l);
    raw_acquire(l);
    push_held(l);
}

bool spin_trylock(struct spinlock *l)
{
    uint32_t owner = __atomic_load_n(&l->owner, __ATOMIC_RELAXED);
    uint32_t expect = owner;
    if (!__atomic_compare_exchange_n(&l->next, &expect, owner + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return false;
    check_acquire(l);
    push_held(l);
    return true;
}

void spin_unlock(struct spinlock *l)
{
    pop_held(l);
    raw_release(l);
}

uint64_t spin_lock_irqsave(struct spinlock *l)
{
    uint64_t f = irq_save();
    spin_lock(l);
    return f;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

/* ---- Big kernel lock -------------------------------------------------------- */
static struct spinlock bkl = SPINLOCK_INIT("bkl");
static uint64_t contended;

uint64_t bkl_contended(void) { return contended; }

static void bkl_take(void)
{
#ifdef AURORA_LOCKDEP
    if (this_cpu()->nheld)
        report("BKL taken while holding", this_cpu()->held[0]->cls->name, "bkl");
#endif
    uint32_t ticket = __atomic_fetch_add(&bkl.next, 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&bkl.owner, __ATOMIC_ACQUIRE) == ticket)
        return;
    __atomic_add_fetch(&contended, 1, __ATOMIC_RELAXED);
    /* Spin with interrupts enabled so IPIs (TLB shootdown, reschedule) are
     * still answered while this CPU waits. */
    bool was_on = cpu_flags() & (1u << 9);
    while (__atomic_load_n(&bkl.owner, __ATOMIC_ACQUIRE) != ticket) {
        if (!was_on) {
            cpu_sti();
            cpu_pause();
            cpu_cli();
        } else {
            cpu_pause();
        }
    }
}

static void bkl_give(void) { raw_release(&bkl); }

void bkl_lock(void)
{
    struct task *t = sched_current();
    if (t->bkl_depth++ == 0)
        bkl_take();
}

void bkl_unlock(void)
{
    struct task *t = sched_current();
    if (t->bkl_depth <= 0)
        panic("bkl_unlock without bkl_lock (task %s)", t->name);
    if (--t->bkl_depth == 0)
        bkl_give();
}

bool bkl_held(void) { return sched_current()->bkl_depth > 0; }

int bkl_release_all(void)
{
    struct task *t = sched_current();
    int d = t->bkl_depth;
    if (d > 0) {
        t->bkl_depth = 0;
        bkl_give();
    }
    return d;
}

void bkl_reacquire(int depth)
{
    if (depth <= 0)
        return;
    bkl_take();
    sched_current()->bkl_depth = depth;
}

/* Scheduler hooks: the BKL is not held across a context switch. */
void bkl_sched_out(struct task *prev)
{
    if (prev->bkl_depth > 0)
        bkl_give();
}

void bkl_sched_in(struct task *t)
{
    if (t->bkl_depth > 0)
        bkl_take();
}
