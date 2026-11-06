#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "lib.h"

struct PACKED idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t zero;
};

struct PACKED idtr {
    uint16_t limit;
    uint64_t base;
};

extern char isr_stub_table[];
static struct idt_entry idt[256] __attribute__((aligned(16)));

static void set_gate(int vec, uint64_t handler, uint8_t ist, uint8_t dpl)
{
    idt[vec].offset_lo = handler & 0xFFFF;
    idt[vec].selector = GDT_KERNEL_CODE;
    idt[vec].ist = ist & 7;
    idt[vec].type_attr = (uint8_t)(0x8E | (dpl << 5)); /* present, 64-bit interrupt gate (IF cleared) */
    idt[vec].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vec].offset_hi = (uint32_t)(handler >> 32);
    idt[vec].zero = 0;
}

void idt_init(void)
{
    for (int v = 0; v < 256; v++) {
        uint8_t ist = 0;
        if (v == 8)
            ist = 1; /* double fault */
        else if (v == 2 || v == 18)
            ist = 2; /* NMI, machine check */
        /* Only the system-call vector may be raised from ring 3 with INT. */
        set_gate(v, (uint64_t)(uintptr_t)(isr_stub_table + v * 16), ist, v == SYSCALL_VECTOR ? 3 : 0);
    }
    struct idtr r = { sizeof idt - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile("lidt %0" : : "m"(r) : "memory");
}
