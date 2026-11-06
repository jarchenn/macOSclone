#include "drivers/pic.h"
#include "arch/x86_64/io.h"
#include "arch/cpu.h"
#include "core/log.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

static uint16_t mask = 0xFFFF;

static void write_masks(void)
{
    outb(PIC1_DATA, (uint8_t)mask);
    outb(PIC2_DATA, (uint8_t)(mask >> 8));
}

void pic_init(void)
{
    /* ICW1: init + expect ICW4 */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_BASE); io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    /* ICW3: slave on IRQ2 */
    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();
    /* ICW4: 8086 mode, normal EOI */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();
    mask = 0xFFFF; /* everything masked until a driver installs a handler */
    write_masks();
}

void pic_unmask(unsigned irq)
{
    if (irq >= 16)
        return;
    mask &= (uint16_t)~(1u << irq);
    if (irq >= 8)
        mask &= (uint16_t)~(1u << IRQ_CASCADE);
    write_masks();
}

void pic_mask(unsigned irq)
{
    if (irq >= 16 || irq == IRQ_CASCADE)
        return;
    mask |= (uint16_t)(1u << irq);
    write_masks();
}

static uint8_t read_isr(uint16_t cmd)
{
    outb(cmd, 0x0B); /* OCW3: read ISR */
    return inb(cmd);
}

bool pic_is_spurious(unsigned irq)
{
    if (irq == 7)
        return !(read_isr(PIC1_CMD) & 0x80);
    if (irq == 15) {
        if (!(read_isr(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, 0x20); /* master saw a real cascade IRQ */
            return true;
        }
    }
    return false;
}

void pic_eoi(unsigned irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

#define LAPIC_LVT_LINT0 0x350
#define X2APIC_MSR_LVT_LINT0 0x835
#define LVT_EXTINT (7u << 8)

void lapic_virtual_wire(void *mmio)
{
    if (!g_cpu.apic)
        return; /* no local APIC: PIC INTR goes straight to the CPU */
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (!(base & (1u << 11)))
        return; /* APIC globally disabled: legacy INTR path is active */
    if (base & (1u << 10)) {
        wrmsr(X2APIC_MSR_LVT_LINT0, LVT_EXTINT);
        klog("pic: x2APIC LINT0 set to ExtINT (virtual wire)");
        return;
    }
    if (!mmio) {
        klog("pic: warning: LAPIC MMIO not mapped; PIC interrupts may be blocked");
        return;
    }
    volatile uint32_t *lint0 = (volatile uint32_t *)((uint8_t *)mmio + LAPIC_LVT_LINT0);
    *lint0 = LVT_EXTINT; /* unmasked, ExtINT delivery, edge */
    klog("pic: LAPIC LINT0 set to ExtINT (virtual wire), was masked by bootloader");
}

uint16_t pic_enabled_mask(void) { return (uint16_t)~mask; }

void pic_mask_all(void)
{
    mask = 0xFFFF;
    write_masks();
}
