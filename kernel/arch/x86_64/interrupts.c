/* Interrupt dispatch: CPU exceptions are fatal in release 0.1 and go to the
 * panic handler; legacy PIC IRQs are routed to registered handlers. */
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/pic.h"
#include "core/panic.h"

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
    if (v < 32)
        panic_exception(f); /* never returns */

    if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
        unsigned irq = (unsigned)(v - IRQ_BASE);
        if (pic_is_spurious(irq)) {
            spurious_count++;
            return;
        }
        if (irq_handlers[irq])
            irq_handlers[irq]();
        else
            unexpected_count++;
        pic_eoi(irq);
        return;
    }
    unexpected_count++;
}
