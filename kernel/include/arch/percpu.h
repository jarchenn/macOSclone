#ifndef AURORA_PERCPU_H
#define AURORA_PERCPU_H
#include "types.h"

/* Per-CPU data, reached through the GS base while in the kernel.
 *
 * GS discipline: in kernel mode IA32_GS_BASE holds this CPU's struct cpu and
 * IA32_KERNEL_GS_BASE holds the user value; every entry from ring 3 and every
 * return to ring 3 executes SWAPGS (interrupt_stubs.S, enter_user). NMI, #DF
 * and #MC check the GS_BASE MSR instead of the saved CS, since they can
 * interrupt the instructions right after a ring transition. */

#define MAX_CPUS 16

struct task;
struct spinlock;

struct cpu {
    struct cpu *self;     /* must stay first: this_cpu() loads %gs:0 */
    uint32_t index;       /* 0 = boot CPU */
    uint32_t apic_id;
    struct task *current;
    struct task *idle;
    int slice_left;
    volatile bool need_resched;
    volatile bool online;
    struct task *to_reap;     /* dead task whose stack is freed after the switch */
    struct spinlock *held[16]; /* lockdep: spinlocks held by this CPU */
    int nheld;
    /* statistics */
    uint64_t ticks, user_ticks, idle_ticks, kernel_ticks;
    uint64_t irqs, ipis, switches, tlb_shootdowns;
    /* per-CPU descriptor tables (gdt.c) */
    uint64_t gdt[7] __attribute__((aligned(16)));
    uint8_t tss[104] __attribute__((aligned(16)));
    uint64_t ist_top[2]; /* #DF, NMI/#MC stacks */
};

extern struct cpu g_cpus[MAX_CPUS];
extern volatile uint32_t g_ncpus_online;
extern uint32_t g_ncpus; /* CPUs we will try to use (online or starting) */

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#define MSR_GS_BASE 0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

/* Points GS at g_cpus[index] (KERNEL_GS_BASE = 0 for user mode). */
void percpu_install(uint32_t index);
/* Per-CPU GDT + TSS (gdt.c). */
void gdt_init_cpu(struct cpu *c);

#endif
