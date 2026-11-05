#include "memory/physical.h"
#include "lib.h"
#ifndef AURORA_HOST
#include "core/log.h"
#include "core/panic.h"
#endif

static inline bool bit_get(const struct pmm *p, uint64_t i) { return (p->bitmap[i / 64] >> (i % 64)) & 1; }
static inline void bit_set(struct pmm *p, uint64_t i) { p->bitmap[i / 64] |= 1ull << (i % 64); }
static inline void bit_clear(struct pmm *p, uint64_t i) { p->bitmap[i / 64] &= ~(1ull << (i % 64)); }

static bool tracked(uint32_t type) { return type == BOOT_MEM_USABLE || type == BOOT_MEM_BOOTLOADER_RECLAIMABLE; }

static uint64_t top_address(const struct boot_mem_region *r, size_t n)
{
    uint64_t top = 0;
    for (size_t i = 0; i < n; i++) {
        if (!tracked(r[i].type) || r[i].length == 0)
            continue;
        uint64_t end = r[i].base + r[i].length;
        if (end < r[i].base)
            end = UINT64_MAX & ~(PAGE_SIZE - 1); /* saturate on overflow */
        if (end > top)
            top = end;
    }
    return top;
}

size_t pmm_bitmap_bytes_needed(const struct boot_mem_region *r, size_t n)
{
    uint64_t pages = ALIGN_UP(top_address(r, n), PAGE_SIZE) / PAGE_SIZE;
    uint64_t words = (pages + 63) / 64;
    return (size_t)ALIGN_UP(words * 8, PAGE_SIZE);
}

phys_addr_t pmm_find_bitmap_home(const struct boot_mem_region *r, size_t n, size_t bytes)
{
    for (size_t i = 0; i < n; i++) {
        if (r[i].type != BOOT_MEM_USABLE)
            continue;
        uint64_t base = ALIGN_UP(r[i].base, PAGE_SIZE);
        if (base == 0)
            base = PAGE_SIZE; /* keep page 0 out of use */
        uint64_t end = ALIGN_DOWN(r[i].base + r[i].length, PAGE_SIZE);
        if (end > base && end - base >= bytes)
            return base;
    }
    return 0;
}

static void mark_range(struct pmm *p, uint64_t base, uint64_t len, bool used)
{
    uint64_t start = ALIGN_UP(base, PAGE_SIZE) / PAGE_SIZE;
    uint64_t end = ALIGN_DOWN(base + len, PAGE_SIZE) / PAGE_SIZE;
    if (end > p->page_count)
        end = p->page_count;
    for (uint64_t i = start; i < end; i++) {
        if (used) {
            if (!bit_get(p, i)) {
                bit_set(p, i);
                p->free_pages--;
            }
        } else if (bit_get(p, i)) {
            bit_clear(p, i);
            p->free_pages++;
        }
    }
}

void pmm_core_init(struct pmm *p, const struct boot_mem_region *r, size_t n,
                   uint64_t *bitmap, phys_addr_t bitmap_phys, size_t bitmap_bytes)
{
    memset(p, 0, sizeof *p);
    p->bitmap = bitmap;
    p->bitmap_phys = bitmap_phys;
    p->bitmap_bytes = bitmap_bytes;
    uint64_t pages = ALIGN_UP(top_address(r, n), PAGE_SIZE) / PAGE_SIZE;
    if (pages > (uint64_t)bitmap_bytes * 8)
        pages = (uint64_t)bitmap_bytes * 8;
    p->page_count = pages;
    memset(bitmap, 0xFF, bitmap_bytes); /* everything used by default */
    for (size_t i = 0; i < n; i++) {
        if (r[i].type == BOOT_MEM_USABLE)
            mark_range(p, r[i].base, r[i].length, false);
    }
    /* Page 0 and the bitmap itself are never available. */
    if (p->page_count && !bit_get(p, 0)) {
        bit_set(p, 0);
        p->free_pages--;
    }
    mark_range(p, bitmap_phys, ALIGN_UP(bitmap_bytes, PAGE_SIZE), true);
    p->usable_pages = p->free_pages;
    p->hint = 1;
}

phys_addr_t pmm_core_alloc(struct pmm *p)
{
    if (p->free_pages == 0)
        return 0;
    uint64_t words = (p->page_count + 63) / 64;
    uint64_t start = p->hint / 64;
    for (uint64_t k = 0; k < words; k++) {
        uint64_t w = (start + k) % words;
        if (p->bitmap[w] == ~0ull)
            continue;
        for (unsigned b = 0; b < 64; b++) {
            uint64_t i = w * 64 + b;
            if (i >= p->page_count)
                break;
            if (!bit_get(p, i)) {
                bit_set(p, i);
                p->free_pages--;
                p->hint = i + 1 < p->page_count ? i + 1 : 1;
                return i * PAGE_SIZE;
            }
        }
    }
    return 0;
}

phys_addr_t pmm_core_alloc_contig(struct pmm *p, size_t pages)
{
    if (pages == 0 || pages > p->free_pages)
        return 0;
    uint64_t run = 0;
    for (uint64_t i = 1; i < p->page_count; i++) {
        run = bit_get(p, i) ? 0 : run + 1;
        if (run == pages) {
            uint64_t first = i + 1 - pages;
            for (uint64_t j = first; j <= i; j++)
                bit_set(p, j);
            p->free_pages -= pages;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

int pmm_core_free(struct pmm *p, phys_addr_t addr)
{
    if ((addr & (PAGE_SIZE - 1)) || addr == 0 || addr / PAGE_SIZE >= p->page_count ||
        (addr >= p->bitmap_phys && addr < p->bitmap_phys + p->bitmap_bytes)) {
        p->invalid_frees++;
        return PMM_INVALID;
    }
    uint64_t i = addr / PAGE_SIZE;
    if (!bit_get(p, i)) {
        p->double_frees++;
        return PMM_DOUBLE_FREE;
    }
    bit_clear(p, i);
    p->free_pages++;
    return PMM_OK;
}

uint64_t pmm_core_release_type(struct pmm *p, const struct boot_mem_region *r, size_t n, uint32_t type)
{
    uint64_t before = p->free_pages;
    for (size_t i = 0; i < n; i++)
        if (r[i].type == type)
            mark_range(p, r[i].base, r[i].length, false);
    if (p->page_count && !bit_get(p, 0)) {
        bit_set(p, 0);
        p->free_pages--;
    }
    uint64_t gained = p->free_pages - before;
    p->usable_pages += gained;
    p->reclaimed_pages += gained;
    return gained;
}

bool pmm_core_is_used(const struct pmm *p, phys_addr_t addr)
{
    uint64_t i = addr / PAGE_SIZE;
    return i >= p->page_count || bit_get(p, i);
}

#ifndef AURORA_HOST
static struct pmm g_pmm;

void pmm_init(void)
{
    size_t bytes = pmm_bitmap_bytes_needed(g_boot.memmap, g_boot.memmap_count);
    phys_addr_t home = pmm_find_bitmap_home(g_boot.memmap, g_boot.memmap_count, bytes);
    if (!home)
        panic("pmm: no usable region can hold the %zu-byte page bitmap", bytes);
    pmm_core_init(&g_pmm, g_boot.memmap, g_boot.memmap_count, phys_to_virt(home), home, bytes);
    klog("pmm: %llu pages tracked, %llu usable (%llu MiB), bitmap %zu KiB at phys %llx",
         (unsigned long long)g_pmm.page_count, (unsigned long long)g_pmm.free_pages,
         (unsigned long long)(g_pmm.free_pages * PAGE_SIZE >> 20), bytes / 1024, (unsigned long long)home);
}

phys_addr_t pmm_alloc_page(void)
{
    uint64_t f = 0;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    phys_addr_t p = pmm_core_alloc(&g_pmm);
    if (f & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
    if (p)
        memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

void pmm_free_page(phys_addr_t addr)
{
    int r = pmm_core_free(&g_pmm, addr);
    if (r == PMM_DOUBLE_FREE)
        panic("pmm: double free of physical page %llx", (unsigned long long)addr);
    if (r == PMM_INVALID)
        panic("pmm: invalid free of physical address %llx", (unsigned long long)addr);
}

void pmm_reclaim_bootloader(void)
{
    uint64_t n = pmm_core_release_type(&g_pmm, g_boot.memmap, g_boot.memmap_count, BOOT_MEM_BOOTLOADER_RECLAIMABLE);
    klog("pmm: reclaimed %llu bootloader pages (%llu KiB); %llu pages free",
         (unsigned long long)n, (unsigned long long)(n * 4), (unsigned long long)g_pmm.free_pages);
}

const struct pmm *pmm_state(void) { return &g_pmm; }
#endif
