/* SMP bring-up: starts every MADT-listed application processor with
 * INIT-SIPI-SIPI through the real-mode trampoline (trampoline.S). An AP that
 * does not respond is logged and skipped. */
#include "arch/smp.h"
#include "arch/percpu.h"
#include "arch/cpu.h"
#include "arch/paging.h"
#include "arch/x86_64/io.h"
#include "drivers/acpi.h"
#include "drivers/acpi_tables.h"
#include "drivers/apic.h"
#include "memory/physical.h"
#include "memory/virtual.h"
#include "core/sched.h"
#include "core/log.h"
#include "core/time.h"
#include "core/boot.h"
#include "lib.h"

extern const uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern const uint32_t ap_tr_pm32_off, ap_tr_lm64_off;
struct task *sched_create_ap_idle(uint32_t cpu_index);

#define TR_DATA 0xF00
struct PACKED tr_data {
    uint32_t cr3, efer;
    uint64_t stack, entry, arg;
    uint16_t gdtr_limit;
    uint32_t gdtr_base;
    uint16_t pad0;
    uint32_t pm_off;
    uint16_t pm_sel, pad1;
    uint32_t lm_off;
    uint16_t lm_sel, pad2;
    uint32_t started, pad3;
    uint64_t gdt[4];
};
_Static_assert(sizeof(struct tr_data) <= 0x100, "trampoline data fits");

static uint64_t bsp_cr0, bsp_cr4, bsp_efer, bsp_pat;
static volatile uint32_t ap_ready_index; /* set by an AP once fully online */
static uint32_t failed;

uint32_t smp_failed_cpus(void) { return failed; }

static void idt_load(void) { idt_reload(); }

/* 64-bit entry from the trampoline: on the AP's idle-task stack, on the
 * temporary page tables. */
NORETURN void ap_entry64(uint64_t index);
NORETURN void ap_entry64(uint64_t index)
{
    write_cr3(vmm_kernel_space());
    __asm__ volatile("mov %0, %%cr4" : : "r"(bsp_cr4) : "memory");
    __asm__ volatile("mov %0, %%cr0" : : "r"(bsp_cr0) : "memory");
    wrmsr(0xC0000080u, bsp_efer);
    if (g_cpu.pat)
        wrmsr(MSR_PAT, bsp_pat); /* same memory types as the boot CPU (framebuffer WC) */
    struct cpu *c = &g_cpus[index];
    gdt_init_cpu(c); /* also installs GS */
    idt_load();
    apic_init_ap();
    sched_init_ap();
    if (!apic_timer_start())
        klog("smp: cpu %llu: LAPIC timer calibration failed", (unsigned long long)index);
    c->online = true;
    __atomic_add_fetch(&g_ncpus_online, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&ap_ready_index, (uint32_t)index, __ATOMIC_RELEASE);
    klog("smp: cpu %llu (apic id %u) online", (unsigned long long)index, c->apic_id);
    sched_ap_idle();
}

static phys_addr_t tmp_pages[3];

static uint64_t *temp_tables(phys_addr_t *pml4_phys)
{
    phys_addr_t pml4p = pmm_alloc_page(), pdptp = pmm_alloc_page(), pdp = pmm_alloc_page();
    tmp_pages[0] = pml4p;
    tmp_pages[1] = pdptp;
    tmp_pages[2] = pdp;
    if (!pml4p || !pdptp || !pdp || pml4p >= (1ull << 32))
        return NULL;
    uint64_t *pml4 = phys_to_virt(pml4p), *pdpt = phys_to_virt(pdptp), *pd = phys_to_virt(pdp);
    const uint64_t *kp = phys_to_virt(vmm_kernel_space());
    for (int i = 256; i < 512; i++)
        pml4[i] = kp[i];
    pml4[0] = pdptp | PTE_P | PTE_W;
    pdpt[0] = pdp | PTE_P | PTE_W;
    pd[0] = 0 | PTE_P | PTE_W | PTE_PS; /* identity map 0-2 MiB (the trampoline page) */
    *pml4_phys = pml4p;
    return pml4;
}

static uint32_t ap_started(struct tr_data *d)
{
    /* 4-byte aligned in the page (offset 0x38); read through volatile */
    return *(volatile uint32_t *)((uint8_t *)d + __builtin_offsetof(struct tr_data, started));
}

static void delay_us(uint32_t us)
{
    for (uint32_t i = 0; i < us; i++)
        io_wait(); /* port 0x80 write: ~1 us */
}

uint32_t smp_init(void)
{
    const struct acpi_info *a = acpi_info();
    g_cpus[0].apic_id = apic_active() ? lapic_id() : 0;
    if (!a || !apic_active() || a->ncpus <= 1) {
        klog("smp: single CPU (%s)", !apic_active() ? "no APIC" : "MADT lists one CPU");
        TEST_EVENT("SMP cpus=1 listed=%d failed=0", a ? a->ncpus : 1);
        return 1;
    }
    if (!g_low_page) {
        klog("smp: no low-memory page for the trampoline; using one CPU");
        return 1;
    }
    phys_addr_t tmp_phys;
    uint64_t *tmp = temp_tables(&tmp_phys);
    if (!tmp) {
        klog("smp: cannot build temporary page tables; using one CPU");
        return 1;
    }
    __asm__ volatile("mov %%cr0, %0" : "=r"(bsp_cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(bsp_cr4));
    bsp_efer = rdmsr(0xC0000080u);
    if (g_cpu.pat)
        bsp_pat = rdmsr(MSR_PAT);

    uint8_t *page = phys_to_virt(g_low_page);
    size_t code = (size_t)(ap_trampoline_end - ap_trampoline_start);
    memset(page, 0, 4096);
    memcpy(page, ap_trampoline_start, code);
    struct tr_data *d = (struct tr_data *)(page + TR_DATA);
    d->cr3 = (uint32_t)tmp_phys;
    d->efer = (1u << 8) | (bsp_efer & (1u << 11)); /* LME, and NXE if the BSP uses NX */
    d->entry = (uint64_t)(uintptr_t)ap_entry64;
    d->gdt[0] = 0;
    d->gdt[1] = 0x00CF9A000000FFFFull; /* 0x08: 32-bit code */
    d->gdt[2] = 0x00CF92000000FFFFull; /* 0x10: 32-bit data */
    d->gdt[3] = 0x00AF9A000000FFFFull; /* 0x18: 64-bit code */
    d->gdtr_limit = sizeof d->gdt - 1;
    d->gdtr_base = (uint32_t)(g_low_page + TR_DATA + __builtin_offsetof(struct tr_data, gdt));
    d->pm_off = (uint32_t)g_low_page + ap_tr_pm32_off;
    d->pm_sel = 0x08;
    d->lm_off = (uint32_t)g_low_page + ap_tr_lm64_off;
    d->lm_sel = 0x18;

    uint32_t next = 1;
    bool keep_tables = false;
    UNUSED(tmp);
    for (int i = 0; i < a->ncpus && next < MAX_CPUS; i++) {
        const struct acpi_cpu *ac = &a->cpus[i];
        if (!ac->enabled || ac->apic_id == g_cpus[0].apic_id)
            continue;
        struct cpu *c = &g_cpus[next];
        c->index = next;
        c->apic_id = ac->apic_id;
        struct task *idle = sched_create_ap_idle(next);
        if (!idle) {
            klog("smp: no task slot for cpu %u", next);
            break;
        }
        c->idle = idle;
        d->stack = idle->stack_top;
        d->arg = next;
        d->started = 0;
        __atomic_store_n(&ap_ready_index, 0, __ATOMIC_RELEASE);
        g_ncpus = next + 1; /* visible to shootdown/kick loops (online stays false until ready) */
        uint8_t vec = (uint8_t)(g_low_page >> 12);
        bool ok = lapic_send_init(ac->apic_id);
        time_sleep_ms(10);
        for (int sipi = 0; ok && sipi < 2 && !ap_started(d); sipi++) {
            ok = lapic_send_sipi(ac->apic_id, vec);
            delay_us(200);
        }
        uint64_t deadline = time_ms() + 500;
        while (ok && __atomic_load_n(&ap_ready_index, __ATOMIC_ACQUIRE) != next && time_ms() < deadline)
            cpu_pause();
        if (__atomic_load_n(&ap_ready_index, __ATOMIC_ACQUIRE) != next) {
            failed++;
            klog("smp: cpu with apic id %u did not start (%s); skipped", ac->apic_id,
                 !ok ? "IPI not delivered" : ap_started(d) ? "stalled after real mode" : "no response");
            /* The slot, its idle task and the trampoline tables stay reserved
             * in case the AP wakes up late; it is never marked online. */
            keep_tables = true;
            next++;
            continue;
        }
        next++;
    }
    g_ncpus = next;
    /* Every started AP switched to the kernel CR3 first thing, so the
     * temporary tables can go (unless a CPU might still wake up on them). */
    if (!keep_tables)
        for (int i = 0; i < 3; i++)
            pmm_free_page(tmp_pages[i]);
    klog("smp: %u CPU(s) online (%u listed in the MADT, %u failed to start)", g_ncpus_online, a->ncpus, failed);
    TEST_EVENT("SMP cpus=%u listed=%d failed=%u", g_ncpus_online, a->ncpus, failed);
    return g_ncpus_online;
}
