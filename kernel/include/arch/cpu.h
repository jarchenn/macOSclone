#ifndef AURORA_ARCH_CPU_H
#define AURORA_ARCH_CPU_H
#include "types.h"

/* Register frame built by interrupt_stubs.S. Field order mirrors the push
 * order in reverse (r15 is pushed last, so it is at the lowest address). */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    /* pushed by the CPU */
    uint64_t rip, cs, rflags, rsp, ss;
};

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA 0x18
#define GDT_USER_CODE 0x20
#define GDT_TSS 0x28
#define USER_CS (GDT_USER_CODE | 3)
#define USER_SS (GDT_USER_DATA | 3)
#define SYSCALL_VECTOR 0x80

#define IRQ_BASE 0x20
#define IRQ_TIMER 0
#define IRQ_KEYBOARD 1
#define IRQ_CASCADE 2
#define IRQ_MOUSE 12

typedef void (*irq_handler_t)(void);

void gdt_init(void);
void gdt_set_kernel_stack(uint64_t rsp0);
void idt_init(void);
void irq_install(unsigned irq, irq_handler_t h);
uint64_t interrupt_spurious_count(void);
uint64_t interrupt_unexpected_count(void);

struct cpu_features {
    bool nx, pat, apic, x2apic, invariant_tsc;
    char vendor[13];
};
extern struct cpu_features g_cpu;
void cpu_detect(void);

/* Switches to a new stack and calls fn(); never returns. (entry.S) */
NORETURN void switch_stack_and_call(uint64_t stack_top, void (*fn)(void));
/* Reloads CS and data segment registers after lgdt. (entry.S) */
void gdt_reload_segments(uint16_t code, uint16_t data);

#endif
