#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "lib.h"

struct cpu_features g_cpu;

void cpu_enable_sse(void)
{
    /* CR0: MP set, EM and TS clear (no lazy FPU switching); CR4: OSFXSR and
     * OSXMMEXCPT so user code may use SSE and get #XM for unmasked faults.
     * APs copy CR0/CR4 from the boot CPU (smp.c). */
    uint64_t cr0 = read_cr0();
    cr0 = (cr0 | (1ull << 1)) & ~((1ull << 2) | (1ull << 3));
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    uint64_t cr4 = read_cr4() | (1ull << 9) | (1ull << 10);
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    __asm__ volatile("fninit");
}

void cpu_detect(void)
{
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    memcpy(g_cpu.vendor + 0, &b, 4);
    memcpy(g_cpu.vendor + 4, &d, 4);
    memcpy(g_cpu.vendor + 8, &c, 4);
    g_cpu.vendor[12] = '\0';
    uint32_t max = a;
    if (max >= 1) {
        cpuid(1, 0, &a, &b, &c, &d);
        g_cpu.apic = d & (1u << 9);
        g_cpu.pat = d & (1u << 16);
        g_cpu.x2apic = c & (1u << 21);
    }
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    uint32_t emax = a;
    if (emax >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        g_cpu.nx = d & (1u << 20);
    }
    if (emax >= 0x80000007) {
        cpuid(0x80000007, 0, &a, &b, &c, &d);
        g_cpu.invariant_tsc = d & (1u << 8);
    }
    /* NX is only usable if the bootloader enabled EFER.NXE. */
    if (g_cpu.nx && !(rdmsr(MSR_EFER) & (1u << 11)))
        g_cpu.nx = false;
}
