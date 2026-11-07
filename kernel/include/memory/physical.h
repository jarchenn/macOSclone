#ifndef AURORA_PHYSICAL_H
#define AURORA_PHYSICAL_H
#include "types.h"
#include "core/boot.h"

/* Bitmap physical page allocator. One bit per 4 KiB page from physical 0 up
 * to the highest usable/bootloader-reclaimable address; 1 = in use.
 * Page 0 is never handed out, so 0 doubles as the failure value. */

enum pmm_status { PMM_OK = 0, PMM_INVALID = -1, PMM_DOUBLE_FREE = -2 };

struct pmm {
    uint64_t *bitmap;
    uint64_t page_count;     /* pages covered by the bitmap */
    uint64_t usable_pages;   /* pages that were ever made available */
    uint64_t free_pages;
    uint64_t reclaimed_pages;
    uint64_t hint;
    phys_addr_t bitmap_phys;
    size_t bitmap_bytes;
    uint64_t invalid_frees, double_frees;
};

/* Portable core (host-testable). */
size_t pmm_bitmap_bytes_needed(const struct boot_mem_region *r, size_t n);
phys_addr_t pmm_find_bitmap_home(const struct boot_mem_region *r, size_t n, size_t bytes);
void pmm_core_init(struct pmm *p, const struct boot_mem_region *r, size_t n,
                   uint64_t *bitmap, phys_addr_t bitmap_phys, size_t bitmap_bytes);
phys_addr_t pmm_core_alloc(struct pmm *p);
phys_addr_t pmm_core_alloc_contig(struct pmm *p, size_t pages);
int pmm_core_free(struct pmm *p, phys_addr_t addr);
/* Takes the lowest free page in [lo, hi), or returns 0. */
phys_addr_t pmm_core_claim_below(struct pmm *p, phys_addr_t lo, phys_addr_t hi);
extern phys_addr_t g_low_page; /* page below 1 MiB reserved at boot (0 = none) */
/* Makes every page of regions with the given type available. */
uint64_t pmm_core_release_type(struct pmm *p, const struct boot_mem_region *r, size_t n, uint32_t type);
bool pmm_core_is_used(const struct pmm *p, phys_addr_t addr);

#ifndef AURORA_HOST
void pmm_init(void);
phys_addr_t pmm_alloc_page(void);        /* zeroed page, 0 on OOM */
phys_addr_t pmm_alloc_contig(size_t pages); /* zeroed, physically contiguous */
void pmm_free_page(phys_addr_t addr);
void pmm_reclaim_bootloader(void);
const struct pmm *pmm_state(void);
/* Page sharing for copy-on-write (0.6). A page starts with one owner;
 * pmm_page_share adds one, pmm_page_release drops one and frees the page
 * when the last owner lets go. Only pages from pmm_alloc_page qualify. */
void pmm_page_share(phys_addr_t pa);
void pmm_page_release(phys_addr_t pa);
uint32_t pmm_page_owners(phys_addr_t pa);
uint64_t pmm_free_count(void);
#endif

#endif
