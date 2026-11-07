#include "memory/virtual.h"
#include "memory/physical.h"
#include "arch/paging.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "core/boot.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/spinlock.h"
#include "memory/tlb.h"

struct vmm_info g_vmm;
static uint64_t *kernel_pml4;
static phys_addr_t kernel_pml4_phys;

extern char __kernel_start[], __text_start[], __text_end[], __rodata_start[], __rodata_end[];
extern char __data_start[], __kernel_end[];

static uint64_t kphys(uint64_t va) { return va - g_boot.kernel_virt_base + g_boot.kernel_phys_base; }

static void map_kernel_range(uint64_t start, uint64_t end, uint64_t flags)
{
    start = ALIGN_DOWN(start, PAGE_SIZE);
    end = ALIGN_UP(end, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        if (!paging_map_4k(kernel_pml4, va, kphys(va), flags))
            panic("vmm: out of memory mapping kernel image");
        g_vmm.kernel_pages++;
    }
}

static void map_hhdm_range(uint64_t base, uint64_t end, uint64_t flags)
{
    base = ALIGN_DOWN(base, PAGE_SIZE);
    end = ALIGN_UP(end, PAGE_SIZE);
    uint64_t off = g_boot.hhdm_offset;
    for (uint64_t pa = base; pa < end;) {
        uint64_t va = pa + off;
        if ((pa & 0x1FFFFF) == 0 && end - pa >= 0x200000 && paging_pd_slot_free(kernel_pml4, va)) {
            if (!paging_map_2m(kernel_pml4, va, pa, flags))
                panic("vmm: failed to map HHDM 2M page at %llx", (unsigned long long)pa);
            g_vmm.hhdm_pages_2m++;
            pa += 0x200000;
            continue;
        }
        if (paging_is_2m(kernel_pml4, va)) {
            pa = ALIGN_UP(pa + 1, 0x200000); /* already covered by an identical large page */
            continue;
        }
        if (!paging_map_4k(kernel_pml4, va, pa, flags))
            panic("vmm: out of memory mapping HHDM at %llx", (unsigned long long)pa);
        g_vmm.hhdm_pages_4k++;
        pa += PAGE_SIZE;
    }
}

/* Kernel page-table changes (heap growth, task stacks, MMIO). Lock order:
 * heap -> vmm -> pmm. */
static DEFINE_SPINLOCK(vmm_lock, "vmm");

/* Rolled-back pages were never visible to other CPUs, so the local invlpg in
 * paging_unmap_4k suffices. */
static bool map_fresh_locked(uint64_t va, size_t bytes)
{
    for (size_t off = 0; off < bytes; off += PAGE_SIZE) {
        phys_addr_t p = pmm_alloc_page();
        if (!p || !paging_map_4k(kernel_pml4, va + off, p, PTE_W | PTE_NX)) {
            if (p)
                pmm_free_page(p);
            /* Roll back what was mapped so far. */
            for (size_t u = 0; u < off; u += PAGE_SIZE) {
                uint64_t old = paging_unmap_4k(kernel_pml4, va + u);
                if (old)
                    pmm_free_page(old);
            }
            return false;
        }
    }
    return true;
}

bool vmm_map_fresh(uint64_t va, size_t bytes)
{
    uint64_t f = spin_lock_irqsave(&vmm_lock);
    bool ok = map_fresh_locked(va, bytes);
    spin_unlock_irqrestore(&vmm_lock, f);
    return ok;
}

uint64_t vmm_translate(uint64_t va) { return paging_translate(kernel_pml4, va); }

void vmm_init(void)
{
    g_vmm.nx = g_cpu.nx;
    kernel_pml4_phys = pmm_alloc_page();
    if (!kernel_pml4_phys)
        panic("vmm: cannot allocate PML4");
    kernel_pml4 = phys_to_virt(kernel_pml4_phys);
    paging_table_pages = 1;

    uint64_t hhdm_top = 0;
    for (size_t i = 0; i < g_boot.memmap_count; i++) {
        const struct boot_mem_region *r = &g_boot.memmap[i];
        uint64_t end = r->base + r->length;
        if (end > hhdm_top)
            hhdm_top = end;
    }
    if (g_boot.hhdm_offset + hhdm_top >= VMM_HEAP_BASE)
        panic("vmm: HHDM (%llx + %llx) collides with the kernel heap region",
              (unsigned long long)g_boot.hhdm_offset, (unsigned long long)hhdm_top);

    /* Kernel image with W^X permissions. */
    map_kernel_range((uint64_t)__kernel_start, (uint64_t)__text_start, PTE_W | PTE_NX); /* Limine requests */
    map_kernel_range((uint64_t)__text_start, (uint64_t)__text_end, 0);                 /* RX */
    map_kernel_range((uint64_t)__rodata_start, (uint64_t)__rodata_end, PTE_NX);        /* R */
    map_kernel_range((uint64_t)__data_start, (uint64_t)__kernel_end, PTE_W | PTE_NX);  /* RW */

    /* Direct map: usable RAM, bootloader memory, the kernel and modules
     * read-write; ACPI and "reserved mapped" firmware regions read-only.
     * Plain reserved and bad memory stay unmapped so stray accesses fault. */
    for (size_t i = 0; i < g_boot.memmap_count; i++) {
        const struct boot_mem_region *r = &g_boot.memmap[i];
        if (r->type == BOOT_MEM_USABLE || r->type == BOOT_MEM_BOOTLOADER_RECLAIMABLE ||
            r->type == BOOT_MEM_KERNEL_AND_MODULES)
            map_hhdm_range(r->base, r->base + r->length, PTE_W | PTE_NX);
        else if (r->type == BOOT_MEM_ACPI_RECLAIMABLE || r->type == BOOT_MEM_ACPI_NVS ||
                 r->type == BOOT_MEM_RESERVED_MAPPED)
            map_hhdm_range(r->base, r->base + r->length, PTE_NX); /* firmware tables: read-only */
    }

    /* Framebuffer. Limine documents its PAT layout (PAT5 = WC); we only use
     * PAT5 after verifying the MSR really holds WC (type 0x01) there. */
    if (g_boot.have_fb) {
        uint64_t cache = PTE_PCD | PTE_PWT; /* PAT index 3 = UC by default */
        if (g_cpu.pat && ((rdmsr(MSR_PAT) >> 40) & 0xFF) == 0x01) {
            cache = PTE_PAT4K | PTE_PWT; /* PAT index 5 */
            g_vmm.fb_write_combining = true;
        }
        uint64_t base = ALIGN_DOWN(g_boot.fb.phys, PAGE_SIZE);
        uint64_t end = ALIGN_UP(g_boot.fb.phys + g_boot.fb.pitch * g_boot.fb.height, PAGE_SIZE);
        for (uint64_t pa = base; pa < end; pa += PAGE_SIZE) {
            if (!paging_map_4k(kernel_pml4, pa + g_boot.hhdm_offset, pa, PTE_W | PTE_NX | cache))
                panic("vmm: out of memory mapping framebuffer");
            g_vmm.fb_pages++;
        }
    }

    /* Kernel stack with an unmapped guard page below it. */
    g_vmm.stack_guard = VMM_STACK_BASE;
    uint64_t stack_bottom = VMM_STACK_BASE + PAGE_SIZE;
    for (uint64_t va = stack_bottom; va < stack_bottom + KERNEL_STACK_SIZE; va += PAGE_SIZE) {
        phys_addr_t p = pmm_alloc_page();
        if (!p || !paging_map_4k(kernel_pml4, va, p, PTE_W | PTE_NX))
            panic("vmm: cannot allocate kernel stack");
    }
    g_vmm.stack_top = stack_bottom + KERNEL_STACK_SIZE;

    /* Local APIC MMIO page (uncached) if the APIC is in xAPIC mode. */
    if (g_cpu.apic) {
        uint64_t apic_base = rdmsr(MSR_APIC_BASE);
        if ((apic_base & (1u << 11)) && !(apic_base & (1u << 10))) {
            uint64_t pa = apic_base & 0xFFFFFF000ull;
            if (!paging_map_4k(kernel_pml4, VMM_MMIO_BASE, pa, PTE_W | PTE_NX | PTE_PCD | PTE_PWT))
                panic("vmm: cannot map LAPIC");
            g_vmm.lapic = (void *)(uintptr_t)VMM_MMIO_BASE;
        }
    }

    /* Pre-create every upper-half PML4 entry. Process address spaces copy
     * these 256 entries, so all later kernel mappings (heap growth, task
     * stacks) land in tables shared by every address space. */
    for (unsigned i = 256; i < 512; i++)
        if (!paging_ensure_pml4_entry(kernel_pml4, i))
            panic("vmm: cannot allocate kernel PDPT %u", i);

    write_cr3(kernel_pml4_phys);
    g_vmm.table_pages = paging_table_pages;
    klog("vmm: CR3=%llx kernel=%llu pages, hhdm=%llu x 2M + %llu x 4K, fb=%llu pages (%s), NX=%s",
         (unsigned long long)kernel_pml4_phys, (unsigned long long)g_vmm.kernel_pages,
         (unsigned long long)g_vmm.hhdm_pages_2m, (unsigned long long)g_vmm.hhdm_pages_4k,
         (unsigned long long)g_vmm.fb_pages, g_vmm.fb_write_combining ? "write-combining" : "uncached",
         g_vmm.nx ? "on" : "unavailable");
}

phys_addr_t vmm_kernel_space(void) { return kernel_pml4_phys; }

uint64_t *vmm_space_create(phys_addr_t *phys_out)
{
    phys_addr_t p = pmm_alloc_page();
    if (!p)
        return NULL;
    uint64_t *pml4 = phys_to_virt(p);
    for (unsigned i = 256; i < 512; i++)
        pml4[i] = kernel_pml4[i];
    *phys_out = p;
    return pml4;
}

uint64_t vmm_space_destroy(uint64_t *pml4, phys_addr_t phys)
{
    if (phys == kernel_pml4_phys || read_cr3() == phys)
        panic("vmm: refusing to destroy the active or kernel address space");
    uint64_t pages = paging_free_user_half(pml4);
    pmm_free_page(phys);
    return pages;
}

bool vmm_map_user(uint64_t *pml4, uint64_t va, phys_addr_t pa, bool writable, bool executable)
{
    uint64_t f = PTE_U | (writable ? PTE_W : 0) | (executable ? 0 : PTE_NX);
    return paging_map_4k(pml4, va, pa, f);
}

uint64_t vmm_user_pte(uint64_t *pml4, uint64_t va) { return paging_get_pte(pml4, va); }

/* Kernel task stacks: slots in the stack region above the boot stack, each
 * an unmapped guard page followed by KSTACK_SIZE bytes. */
#define KSTACK_REGION (VMM_STACK_BASE + 0x100000ull)
#define KSTACK_SLOT (KSTACK_SIZE + 2 * PAGE_SIZE)

uint64_t vmm_kstack_alloc(int slot)
{
    uint64_t bottom = KSTACK_REGION + (uint64_t)slot * KSTACK_SLOT + PAGE_SIZE;
    if (!vmm_map_fresh(bottom, KSTACK_SIZE))
        return 0;
    return bottom + KSTACK_SIZE;
}

void vmm_kstack_free(int slot)
{
    uint64_t bottom = KSTACK_REGION + (uint64_t)slot * KSTACK_SLOT + PAGE_SIZE;
    phys_addr_t pas[KSTACK_SIZE / PAGE_SIZE];
    unsigned n = 0;
    uint64_t f = spin_lock_irqsave(&vmm_lock);
    for (uint64_t va = bottom; va < bottom + KSTACK_SIZE; va += PAGE_SIZE)
        pas[n++] = paging_unmap_4k(kernel_pml4, va);
    spin_unlock_irqrestore(&vmm_lock, f);
    /* Other CPUs may have run this task: flush their TLBs before the pages
     * can be reused. */
    tlb_shootdown(bottom, KSTACK_SIZE / PAGE_SIZE, 0);
    for (unsigned i = 0; i < n; i++)
        if (pas[i])
            pmm_free_page(pas[i]);
}

/* Single test page in a dedicated region (diagnostics: TLB test). */
#define VMM_TEST_VA (VMM_MMIO_BASE + (2ull << 30))

uint64_t vmm_test_page_map(void)
{
    uint64_t f = spin_lock_irqsave(&vmm_lock);
    bool ok = paging_get_pte(kernel_pml4, VMM_TEST_VA) || map_fresh_locked(VMM_TEST_VA, PAGE_SIZE);
    spin_unlock_irqrestore(&vmm_lock, f);
    return ok ? VMM_TEST_VA : 0;
}

void vmm_test_page_unmap(bool shootdown)
{
    uint64_t f = spin_lock_irqsave(&vmm_lock);
    phys_addr_t pa = paging_unmap_4k(kernel_pml4, VMM_TEST_VA);
    spin_unlock_irqrestore(&vmm_lock, f);
    if (shootdown)
        tlb_shootdown(VMM_TEST_VA, 1, 0);
    if (pa && shootdown)
        pmm_free_page(pa);
    /* without a shootdown the page is leaked on purpose: another CPU may
     * still reach it through a stale TLB entry */
}

bool vmm_map_framebuffer(phys_addr_t pa, size_t bytes)
{
    uint64_t cache = g_vmm.fb_write_combining ? (PTE_PAT4K | PTE_PWT) : (PTE_PCD | PTE_PWT);
    uint64_t base = ALIGN_DOWN(pa, PAGE_SIZE), end = ALIGN_UP(pa + bytes, PAGE_SIZE);
    uint64_t f = spin_lock_irqsave(&vmm_lock);
    for (uint64_t p = base; p < end; p += PAGE_SIZE)
        if (!paging_map_4k(kernel_pml4, p + g_boot.hhdm_offset, p, PTE_W | PTE_NX | cache)) {
            spin_unlock_irqrestore(&vmm_lock, f);
            return false;
        }
    spin_unlock_irqrestore(&vmm_lock, f);
    return true;
}

/* MMIO window: a bump allocator above the LAPIC page. Mappings are never
 * removed (device registers stay mapped for the kernel's lifetime). The
 * same physical page is only mapped once. */
static uint64_t mmio_next = VMM_MMIO_BASE + PAGE_SIZE;
static struct { phys_addr_t pa; uint64_t va; uint64_t pages; } mmio_maps[32];
static int mmio_count;

void *vmm_map_mmio(phys_addr_t pa, size_t bytes, bool writable)
{
    phys_addr_t base = ALIGN_DOWN(pa, PAGE_SIZE);
    uint64_t pages = (ALIGN_UP(pa + bytes, PAGE_SIZE) - base) / PAGE_SIZE;
    for (int i = 0; i < mmio_count; i++)
        if (base >= mmio_maps[i].pa && base + pages * PAGE_SIZE <= mmio_maps[i].pa + mmio_maps[i].pages * PAGE_SIZE)
            return (void *)(uintptr_t)(mmio_maps[i].va + (pa - mmio_maps[i].pa));
    uint64_t f = spin_lock_irqsave(&vmm_lock);
    if (!pages || mmio_count >= (int)ARRAY_LEN(mmio_maps) || mmio_next + pages * PAGE_SIZE > VMM_MMIO_BASE + (1ull << 30)) {
        spin_unlock_irqrestore(&vmm_lock, f);
        return NULL;
    }
    uint64_t va = mmio_next;
    for (uint64_t i = 0; i < pages; i++)
        if (!paging_map_4k(kernel_pml4, va + i * PAGE_SIZE, base + i * PAGE_SIZE,
                           (writable ? PTE_W : 0) | PTE_NX | PTE_PCD | PTE_PWT)) {
            spin_unlock_irqrestore(&vmm_lock, f);
            return NULL;
        }
    mmio_next += pages * PAGE_SIZE;
    mmio_maps[mmio_count].pa = base;
    mmio_maps[mmio_count].va = va;
    mmio_maps[mmio_count].pages = pages;
    mmio_count++;
    spin_unlock_irqrestore(&vmm_lock, f);
    return (void *)(uintptr_t)(va + (pa - base));
}
