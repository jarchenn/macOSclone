/* Interrupt dispatch: CPU exceptions are fatal in release 0.1 and go to the
 * panic handler; legacy PIC IRQs are routed to registered handlers. */
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/pic.h"
#include "core/panic.h"
#include "core/process.h"
#include "core/sched.h"

static irq_handler_t irq_handlers[16];
static uint64_t spurious_count;
static uint64_t unexpected_count;

void irq_install(unsigned irq, irq_handler_t h)
{
    if (irq < 16)
        irq_handlers[irq] = h;
}

uint64_t interrupt_spurious_count(void) { return __atomic_load_n(&spurious_count, __ATOMIC_RELAXED); }
uint64_t interrupt_unexpected_count(void) { return __atomic_load_n(&unexpected_count, __ATOMIC_RELAXED); }

void interrupt_dispatch(struct interrupt_frame *f);
void interrupt_dispatch(struct interrupt_frame *f)
{
    uint64_t v = f->vector;
    bool from_user = (f->cs & 3) == 3;
    if (v < 32) {
        if (from_user)
            process_fault(f); /* kills the process; the kernel carries on */
        panic_exception(f);   /* a fault in kernel mode is fatal */
    }

    if (v == SYSCALL_VECTOR) {
        if (from_user)
            syscall_dispatch(f);
        else
            unexpected_count++;
    } else if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
        unsigned irq = (unsigned)(v - IRQ_BASE);
        if (pic_is_spurious(irq)) {
            spurious_count++;
            return;
        }
        if (irq_handlers[irq])
            irq_handlers[irq]();
        else
            unexpected_count++;
        pic_eoi(irq); /* before any task switch, or the PIC stays blocked */
    } else {
        unexpected_count++;
    }
    /* Only user mode and the idle task are preemptible (see sched.h). */
    sched_preempt_point(from_user);
    if (from_user)
        process_check_kill();
}
