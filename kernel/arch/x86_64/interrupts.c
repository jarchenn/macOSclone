/* Interrupt dispatch: exceptions from user mode kill the process, from
 * kernel mode they panic. ISA IRQs (vectors 0x20-0x2F) come from the 8259 PIC
 * until the IOAPIC takes over (drivers/apic.c); CPU-local vectors (LAPIC
 * timer, IPIs) have their own handler table. */
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/pic.h"
#include "drivers/apic.h"
#include "core/panic.h"
#include "core/process.h"
#include "core/sched.h"

static irq_handler_t irq_handlers[16];
static irq_handler_t vector_handlers[256];
static uint64_t spurious_count;
static uint64_t unexpected_count;

void irq_install(unsigned irq, irq_handler_t h)
{
    if (irq < 16)
        irq_handlers[irq] = h;
}

void vector_install(unsigned vector, irq_handler_t h)
{
    if (vector >= 0x30 && vector < 0x100 && vector != SYSCALL_VECTOR)
        vector_handlers[vector] = h;
}

void irq_enable(unsigned irq)
{
    if (apic_active())
        ioapic_route_isa(irq, false);
    else
        pic_unmask(irq);
}

uint64_t interrupt_spurious_count(void) { return __atomic_load_n(&spurious_count, __ATOMIC_RELAXED); }
uint64_t interrupt_unexpected_count(void) { return __atomic_load_n(&unexpected_count, __ATOMIC_RELAXED); }

void interrupt_dispatch(struct interrupt_frame *f);
void interrupt_dispatch(struct interrupt_frame *f)
{
    uint64_t v = f->vector;
    bool from_user = (f->cs & 3) == 3;
    if (v < 32) {
        extern char probe_read64[], probe_read64_fault[];
        if (v == 14 && !from_user && f->rip == (uint64_t)(uintptr_t)probe_read64) {
            f->rip = (uint64_t)(uintptr_t)probe_read64_fault; /* expected fault in a probe */
            return;
        }
        if (!from_user)
            panic_exception(f); /* a fault in kernel mode is fatal */
        /* User mode: page faults are resolved (demand paging, copy-on-write);
         * anything else becomes a signal for the thread. */
        process_user_exception(f);
        sched_preempt_point(true);
        signal_return_to_user(f);
        return;
    }

    if (v == SYSCALL_VECTOR) {
        if (from_user)
            syscall_dispatch(f);
        else
            unexpected_count++;
    } else if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
        unsigned irq = (unsigned)(v - IRQ_BASE);
        if (apic_active()) {
            /* Only IRQs with handlers are routed; anything else is a
             * spurious 8259 interrupt from before the switch (no EOI). */
            if (!irq_handlers[irq]) {
                __atomic_add_fetch(&spurious_count, 1, __ATOMIC_RELAXED);
                return;
            }
            irq_handlers[irq]();
            lapic_eoi();
        } else {
            if (pic_is_spurious(irq)) {
                __atomic_add_fetch(&spurious_count, 1, __ATOMIC_RELAXED);
                return;
            }
            if (irq_handlers[irq])
                irq_handlers[irq]();
            else
                unexpected_count++;
            pic_eoi(irq); /* before any task switch, or the PIC stays blocked */
        }
    } else if (v == APIC_SPURIOUS_VECTOR) {
        __atomic_add_fetch(&spurious_count, 1, __ATOMIC_RELAXED); /* LAPIC spurious: no EOI */
        return;
    } else if (vector_handlers[v]) {
        vector_handlers[v]();
        lapic_eoi();
    } else {
        unexpected_count++;
    }
    /* Only user mode and the idle task are preemptible (see sched.h). */
    sched_preempt_point(from_user);
    if (from_user)
        signal_return_to_user(f);
}
