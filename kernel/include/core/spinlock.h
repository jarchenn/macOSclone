#ifndef AURORA_SPINLOCK_H
#define AURORA_SPINLOCK_H
#include "types.h"

/* Ticket spinlocks (FIFO-fair) with IRQ-save variants.
 *
 * Debug builds (AURORA_LOCKDEP, enabled in the test build) add:
 *   - a lock-order validator: every lock belongs to a class (one per lock
 *     definition); acquiring B while holding A records the edge A -> B, and
 *     an edge that closes a cycle is reported as an order violation;
 *   - a hold-time watchdog (holds longer than LOCK_HOLD_WARN_MS are logged);
 *   - lock_assert_none_held(), used by the scheduler to catch sleeping while
 *     holding a spinlock.
 * Violations are logged and counted (lockdep_violations()); the test suite
 * fails on any. */

struct lock_class {
    const char *name;
    int id; /* assigned on first use, -1 = unregistered */
};

struct spinlock {
    volatile uint32_t next;  /* ticket dispenser */
    volatile uint32_t owner; /* now serving */
    struct lock_class *cls;
    int holder_cpu;
    uint64_t taken_ms;
};

#define SPINLOCK_INIT(nm)                                       \
    { 0, 0, &(struct lock_class){ nm, -1 }, -1, 0 }
#define DEFINE_SPINLOCK(var, nm) struct spinlock var = SPINLOCK_INIT(nm)

void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);
bool spin_trylock(struct spinlock *l);
uint64_t spin_lock_irqsave(struct spinlock *l);
void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);
bool spin_is_locked(const struct spinlock *l);

/* Locks held by this CPU (debug builds; 0 otherwise). `except` is a lock
 * the caller legitimately holds (e.g. the scheduler lock). */
int lock_held_count(void);
void lock_assert_none_held(const struct spinlock *except, const char *where);
uint32_t lockdep_violations(void);
uint32_t lockdep_classes(void);
uint32_t lockdep_long_holds(void);

#define LOCK_HOLD_WARN_MS 200

/* ---- Big kernel lock --------------------------------------------------------
 * Transitional (0.5): kernel code that predates SMP (VFS, drivers, network,
 * desktop, process management, system calls) runs holding the BKL. It is
 * recursive per task and is dropped automatically while the task is switched
 * out, then re-taken when it runs again. User-mode code, the scheduler, the
 * timer, IRQ handlers, the page allocator and the heap do not need it. */
void bkl_lock(void);
void bkl_unlock(void);
bool bkl_held(void);           /* by the current task */
int bkl_release_all(void);     /* returns the depth released */
void bkl_reacquire(int depth);
uint64_t bkl_contended(void);  /* acquisitions that had to spin */

#endif
