/* Per-CPU data (see arch/percpu.h). */
#include "arch/percpu.h"
#include "arch/x86_64/io.h"
#include "lib.h"

struct cpu g_cpus[MAX_CPUS];
volatile uint32_t g_ncpus_online = 1;
uint32_t g_ncpus = 1;

void percpu_install(uint32_t index)
{
    struct cpu *c = &g_cpus[index];
    c->self = c;
    c->index = index;
    /* gdt_reload_segments() reloads GS with a null selector, which may clear
     * the base on some CPUs: the base is (re)written after every reload. */
    wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}
