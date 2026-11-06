/* Per-CPU GDT and TSS. Each CPU has flat 64-bit kernel/user segments and its
 * own TSS, so RSP0 (the kernel stack for ring-3 entries) and the IST stacks
 * for #DF, NMI and #MC are private to that CPU. The IDT is shared. */
#include "arch/cpu.h"
#include "arch/percpu.h"
#include "arch/x86_64/io.h"
#include "lib.h"

struct PACKED tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};
_Static_assert(sizeof(struct tss) == 104, "TSS layout");

struct PACKED gdtr {
    uint16_t limit;
    uint64_t base;
};

#define IST_STACK_SIZE 16384
static uint8_t boot_rsp0_stack[IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t ist_stacks[MAX_CPUS][2][IST_STACK_SIZE] __attribute__((aligned(16)));

void gdt_set_kernel_stack(uint64_t rsp0) { ((struct tss *)this_cpu()->tss)->rsp[0] = rsp0; }

void gdt_init_cpu(struct cpu *c)
{
    struct tss *tss = (struct tss *)c->tss;
    memset(tss, 0, sizeof *tss);
    c->ist_top[0] = (uint64_t)(uintptr_t)(ist_stacks[c->index][0] + IST_STACK_SIZE);
    c->ist_top[1] = (uint64_t)(uintptr_t)(ist_stacks[c->index][1] + IST_STACK_SIZE);
    tss->ist[0] = c->ist_top[0]; /* IST1: #DF */
    tss->ist[1] = c->ist_top[1]; /* IST2: NMI, #MC */
    tss->rsp[0] = (uint64_t)(uintptr_t)(boot_rsp0_stack + IST_STACK_SIZE); /* replaced per task */
    tss->iomap_base = sizeof *tss; /* no I/O permission bitmap: ring 3 has no port access */

    uint64_t *gdt = c->gdt;
    gdt[0] = 0;
    /* 0x08: 64-bit code, present, DPL0, L=1 */
    gdt[1] = (1ull << 43) | (1ull << 44) | (1ull << 47) | (1ull << 53) | (1ull << 41);
    /* 0x10: data, present, writable */
    gdt[2] = (1ull << 44) | (1ull << 47) | (1ull << 41);
    /* 0x18: user data (DPL3); 0x20: user 64-bit code (DPL3). Data precedes
     * code so a future SYSRET (CS = base+16, SS = base+8) can reuse them. */
    gdt[3] = (1ull << 44) | (1ull << 47) | (1ull << 41) | (3ull << 45);
    gdt[4] = (1ull << 43) | (1ull << 44) | (1ull << 47) | (1ull << 53) | (1ull << 41) | (3ull << 45);
    /* 0x28: 64-bit TSS descriptor (16 bytes) */
    uint64_t base = (uint64_t)(uintptr_t)tss;
    uint64_t limit = sizeof *tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x9ull << 40) | (1ull << 47) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdtr g = { sizeof c->gdt - 1, (uint64_t)(uintptr_t)gdt };
    __asm__ volatile("lgdt %0" : : "m"(g) : "memory");
    gdt_reload_segments(GDT_KERNEL_CODE, GDT_KERNEL_DATA);
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS) : "memory");
    percpu_install(c->index); /* after the segment reload, which clears GS */
}

/* Boot CPU: per-CPU slot 0 and its descriptor tables. */
void gdt_init(void)
{
    g_cpus[0].index = 0;
    gdt_init_cpu(&g_cpus[0]);
}
