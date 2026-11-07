#ifndef AURORA_PIC_H
#define AURORA_PIC_H
#include "types.h"

/* Legacy 8259A PIC pair, remapped to vectors 0x20-0x2F. Release 0.1 uses the
 * PIC on the QEMU `pc` reference machine; the local APIC is put into
 * virtual-wire mode (LINT0 = ExtINT) so PIC interrupts reach the CPU. */
void pic_init(void);
void pic_unmask(unsigned irq);
void pic_mask(unsigned irq);
void pic_eoi(unsigned irq);
bool pic_is_spurious(unsigned irq);
uint16_t pic_enabled_mask(void); /* IRQs currently unmasked */
void pic_mask_all(void);

/* Configure the local APIC for virtual-wire mode. Requires the LAPIC MMIO
 * page to be mapped (see vmm) unless the CPU is in x2APIC mode. */
void lapic_virtual_wire(void *lapic_mmio);

#endif
