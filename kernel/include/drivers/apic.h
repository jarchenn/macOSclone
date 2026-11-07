#ifndef AURORA_APIC_H
#define AURORA_APIC_H
#include "types.h"

/* Local APIC (xAPIC MMIO mode) and I/O APIC support, configured from the
 * ACPI MADT. Until apic_init() succeeds, interrupts come through the 8259 PIC
 * in virtual-wire mode; afterwards the PIC is fully masked, ISA IRQs are
 * routed through the IOAPIC (honouring interrupt source overrides) to the
 * boot CPU, and each CPU's LAPIC timer drives its scheduler tick. */

#define APIC_TIMER_VECTOR 0x40
#define IPI_RESCHED_VECTOR 0x41
#define IPI_TLB_VECTOR 0x42
#define IPI_HALT_VECTOR 0x43
#define APIC_SPURIOUS_VECTOR 0xFF

bool apic_init(void);               /* boot CPU: LAPIC + IOAPIC; false = stay on the PIC */
bool apic_active(void);
void apic_init_ap(void);            /* LAPIC setup on an application processor */
bool apic_timer_start(void);        /* calibrate against the PIT and start a 250 Hz periodic timer */
void lapic_eoi(void);
uint32_t lapic_id(void);
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t v);
/* IPIs. dest is an APIC ID. Return false on a delivery timeout. */
bool lapic_send_ipi(uint32_t dest, uint8_t vector);
bool lapic_send_init(uint32_t dest);
bool lapic_send_sipi(uint32_t dest, uint8_t page);
void lapic_broadcast_ipi(uint8_t vector); /* all CPUs except self */
/* Routes an ISA IRQ (0-15) through the IOAPIC to the boot CPU. */
bool ioapic_route_isa(unsigned irq, bool masked);
uint32_t apic_timer_initial_count(void);
void apic_timer_tick(void); /* LAPIC timer vector handler */

#define LAPIC_ICR_LO 0x300

#endif
