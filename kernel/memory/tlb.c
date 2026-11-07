/* TLB shootdown through IPI_TLB_VECTOR (see memory/tlb.h). */
#include "memory/tlb.h"
#include "core/spinlock.h"
#include "core/panic.h"
#include "core/log.h"
#include "arch/percpu.h"
#include "arch/x86_64/io.h"
#include "drivers/apic.h"
#include "lib.h"

#define FULL_FLUSH_PAGES 32

static volatile uint32_t sender_lock; /* serializes shootdowns */
static struct {
    uint64_t va, pages, space;
    volatile uint32_t pending; /* CPUs that have not acknowledged yet */
} req;
static uint64_t count;

uint64_t tlb_shootdown_count(void) { return __atomic_load_n(&count, __ATOMIC_RELAXED); }

static void flush_local(uint64_t va, uint64_t pages)
{
    if (pages > FULL_FLUSH_PAGES) {
        write_cr3(read_cr3()); /* no global pages are used: this flushes everything */
        return;
    }
    for (uint64_t i = 0; i < pages; i++)
        invlpg(va + i * 4096);
}

void tlb_ipi_handler(void)
{
    struct cpu *c = this_cpu();
    c->tlb_shootdowns++;
    if (!req.space || read_cr3() == req.space)
        flush_local(req.va, req.pages);
    __atomic_sub_fetch(&req.pending, 1, __ATOMIC_ACQ_REL);
}

void tlb_shootdown(uint64_t va, uint64_t pages, uint64_t space)
{
    if (__atomic_load_n(&g_ncpus_online, __ATOMIC_ACQUIRE) <= 1 || !pages)
        return;
    if (lock_held_count())
        panic("tlb_shootdown with a spinlock held (deadlock risk)");
    /* Take the sender lock with interrupts enabled while spinning, so this
     * CPU keeps acknowledging other senders' requests. */
    uint64_t f = cpu_flags();
    for (;;) {
        cpu_cli();
        if (!__atomic_exchange_n(&sender_lock, 1, __ATOMIC_ACQUIRE))
            break;
        cpu_sti();
        while (__atomic_load_n(&sender_lock, __ATOMIC_RELAXED))
            cpu_pause();
    }
    struct cpu *self = this_cpu();
    uint32_t targets = 0;
    req.va = va;
    req.pages = pages;
    req.space = space;
    for (uint32_t i = 0; i < g_ncpus; i++)
        if (&g_cpus[i] != self && g_cpus[i].online)
            targets++;
    __atomic_store_n(&req.pending, targets, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < g_ncpus; i++) {
        struct cpu *c = &g_cpus[i];
        if (c != self && c->online && !lapic_send_ipi(c->apic_id, IPI_TLB_VECTOR))
            __atomic_sub_fetch(&req.pending, 1, __ATOMIC_ACQ_REL); /* undeliverable: not running */
    }
    uint64_t spins = 0;
    while (__atomic_load_n(&req.pending, __ATOMIC_ACQUIRE)) {
        cpu_pause();
        if (++spins == 2000000000ull)
            panic("tlb: shootdown not acknowledged by %u CPU(s)", req.pending);
    }
    __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&sender_lock, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}
