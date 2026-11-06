#ifndef AURORA_UVM_H
#define AURORA_UVM_H
#include "types.h"

/* User address spaces (0.6): a page table plus a sorted list of regions
 * (VMAs). Pages appear on first touch (demand-zero) inside a region; fork
 * shares every page copy-on-write: both PTEs become read-only and the first
 * write to a page still owned by both copies it (pmm share counts).
 *
 * Locking: callers hold the BKL (system calls and user page faults take it).
 * Functions that change existing translations issue TLB shootdowns for the
 * address space themselves and free pages only afterwards. */

#define UVM_MAX_VMAS 64

enum vma_prot { VMA_R = 1, VMA_W = 2, VMA_X = 4 };
enum vma_kind { VMA_ANON = 0, VMA_IMAGE = 1, VMA_STACK = 2, VMA_HEAP = 3 };

struct vma {
    uint64_t start, end; /* page aligned, end exclusive */
    uint8_t prot;
    uint8_t kind;
};

struct mm {
    uint64_t *pml4;
    phys_addr_t pml4_phys;
    struct vma vma[UVM_MAX_VMAS];
    int nvma;
    uint64_t brk_base, brk; /* heap: [brk_base, ALIGN_UP(brk)) */
    uint64_t faults, cow_copies;
};

int mm_create(struct mm *m);  /* empty space; 0 or AE_NOMEM */
void mm_destroy(struct mm *m); /* must not be the loaded CR3 */
/* Fork: dst gets every region and shares every page copy-on-write. */
int mm_clone(struct mm *dst, struct mm *src);

/* Adds a region over [start, start+len) (page aligned), replacing whatever
 * was mapped there. */
int mm_map(struct mm *m, uint64_t start, uint64_t len, uint8_t prot, uint8_t kind);
int mm_unmap(struct mm *m, uint64_t start, uint64_t len);
int mm_protect(struct mm *m, uint64_t start, uint64_t len, uint8_t prot);
/* Lowest free gap of len bytes at or above `hint`, or 0. */
uint64_t mm_find_gap(struct mm *m, uint64_t hint, uint64_t len);
struct vma *mm_find(struct mm *m, uint64_t addr);

/* Resolves a user fault (demand-zero or copy-on-write). Returns 0 if the
 * access may now be retried, -1 if it is a real fault. */
int mm_fault(struct mm *m, uint64_t addr, bool write, bool exec);
/* Makes [addr, addr+len) present (and private + writable if `write`) for a
 * kernel access on behalf of the process; false on any bad page. */
bool mm_prepare(struct mm *m, uint64_t addr, uint64_t len, bool write);
/* Kernel pointer to the byte at user address va (page present), or NULL. */
void *mm_kptr(struct mm *m, uint64_t va, bool write);

/* Loader: kernel pointer to va inside a region of a space that is being
 * built (not shared, maybe not loaded), allocating the page regardless of
 * the region's protection. */
void *mm_populate(struct mm *m, uint64_t va);

/* brk: returns the new break (the old one on failure). */
uint64_t mm_brk(struct mm *m, uint64_t want);
uint64_t mm_resident_pages(struct mm *m);

#endif
