#ifndef AURORA_ARCH_IO_H
#define AURORA_ARCH_IO_H

#include "types.h"

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl_cf8(uint32_t v) { outl(0xCF8, v); }
/* ~1us delay on PC hardware: write to an unused diagnostic port. */
static inline void io_wait(void) { outb(0x80, 0); }

static inline void cpu_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline void cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }
/* Atomically enable interrupts and halt: the STI shadow guarantees no
 * interrupt is taken between STI and HLT, so a wakeup cannot be lost. */
static inline void cpu_sti_hlt(void) { __asm__ volatile("sti; hlt" ::: "memory"); }

static inline uint64_t cpu_flags(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f) : : "memory");
    return f;
}
/* Save interrupt state and disable; restore with irq_restore(). */
static inline uint64_t irq_save(void)
{
    uint64_t f = cpu_flags();
    cpu_cli();
    return f;
}
static inline void irq_restore(uint64_t f)
{
    if (f & (1u << 9))
        cpu_sti();
}

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void invlpg(uint64_t va) { __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory"); }

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#define MSR_APIC_BASE 0x1B
#define MSR_EFER 0xC0000080
#define MSR_PAT 0x277

#endif
