#ifndef AURORA_PAGING_H
#define AURORA_PAGING_H
#include "types.h"

#define PTE_P (1ull << 0)
#define PTE_W (1ull << 1)
#define PTE_U (1ull << 2)
#define PTE_PWT (1ull << 3)
#define PTE_PCD (1ull << 4)
#define PTE_PS (1ull << 7)    /* large page in PDPT/PD entries */
#define PTE_PAT4K (1ull << 7) /* PAT bit in 4 KiB PTEs */
#define PTE_NX (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull

/* Flags for page table operations; NX is dropped automatically if the CPU
 * does not support it (setting it would be a reserved-bit fault). */
bool paging_map_4k(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags);
bool paging_map_2m(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags);
/* Returns true if a 2 MiB mapping covers va. */
bool paging_is_2m(uint64_t *pml4, uint64_t va);
bool paging_pd_slot_free(uint64_t *pml4, uint64_t va);
uint64_t paging_unmap_4k(uint64_t *pml4, uint64_t va); /* returns old pa or 0 */
uint64_t paging_translate(uint64_t *pml4, uint64_t va);
extern uint64_t paging_table_pages;

#endif
