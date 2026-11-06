#ifndef AURORA_SCHED_H
#define AURORA_SCHED_H
#include "types.h"

/* Tasks and the scheduler (SMP).
 *
 * One global task table protected by sched_lock; each CPU has its own current
 * task, idle task and time slice (struct cpu). sched_lock is held across a
 * context switch and released by the task that is switched to, so a task's
 * saved state is complete before another CPU can pick it.
 *
 * Preemption model: the timer preempts code running in **user mode** and the
 * idle task. Kernel code is never preempted; it gives up the CPU only at
 * explicit blocking points. Kernel code that predates SMP runs under the big
 * kernel lock (core/spinlock.h), which the scheduler drops while a task is
 * switched out. */

#define MAX_TASKS 64
#define SCHED_SLICE_TICKS 3 /* 12 ms at 250 Hz */

enum task_state { TASK_UNUSED = 0, TASK_RUNNABLE, TASK_BLOCKED, TASK_DEAD };

struct process;

struct task {
    int id;
    int state;
    char name[32];
    uint64_t rsp;        /* saved kernel RSP while switched out */
    uint64_t stack_top;  /* kernel stack top (TSS.RSP0 for user tasks) */
    int slot;            /* kernel stack slot, -1 for the boot stack */
    struct process *proc;/* NULL for kernel tasks */
    uint64_t wake_ms;    /* timeout for TASK_BLOCKED, 0 = none */
    bool timed_out;
    const void *wait_obj;/* what the task waits for (wait queues) */
    bool idle;
    void (*entry)(void *);
    void *arg;
    uint64_t cpu_ticks;  /* ticks spent running (accounting) */
    uint64_t switches;
    int on_cpu;          /* CPU index running it, -1 if not running */
    int last_cpu;
    bool wake_pending;   /* woken while not blocked: next block returns at once */
    int bkl_depth;       /* big kernel lock recursion depth */
    uint32_t cpu_mask_seen; /* CPUs this task has run on (statistics) */
    int pin_cpu;         /* run only on this CPU (-1 = any) */
    bool no_bkl;         /* kernel task that never takes the BKL (tests) */
};

void sched_init(void);   /* turns the boot context into task 0 ("desktop") */
void sched_init_ap(void); /* creates this AP's idle task and enters it */
NORETURN void sched_ap_idle(void);
void sched_resched_ipi(void); /* IPI_RESCHED_VECTOR handler */
struct task *sched_current(void);
struct task *sched_desktop(void);
struct task *task_create(const char *name, void (*entry)(void *), void *arg, struct process *proc);
/* Kernel task bound to one CPU that runs without the BKL (diagnostics). */
struct task *task_create_pinned(const char *name, void (*entry)(void *), void *arg, int cpu);
NORETURN void task_exit(void);
void schedule(void);
void sched_yield(void);
/* Blocks the current task until sched_wake() or until timeout_ms elapse
 * (0 = no timeout). Returns false on timeout. Call with interrupts disabled
 * after checking the wait condition to avoid lost wakeups. */
bool sched_block(uint64_t timeout_ms);
void sched_sleep_ms(uint64_t ms);
void sched_wake(struct task *t);
/* Wait queues identified by any stable pointer. */
bool sched_wait_on(const void *obj, uint64_t timeout_ms);
void sched_wake_all(const void *obj);

/* Called from the timer IRQ. */
void sched_tick(void);
/* Called at the end of interrupt dispatch: switches if a reschedule is due
 * and the interrupted context may be preempted (user mode or idle task). */
void sched_preempt_point(bool from_user);
bool sched_need_resched(void);

int sched_task_count(void);
struct task *sched_task_at(int i);
uint64_t sched_context_switches(void);

#endif
