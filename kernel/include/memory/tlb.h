#ifndef AURORA_TLB_H
#define AURORA_TLB_H
#include "types.h"

/* TLB shootdown. Invalidates [va, va + pages*4K) on every other online CPU
 * whose current address space is `space` (0 = a kernel mapping: all CPUs)
 * and waits until each has done so. The local TLB is the caller's job
 * (paging_unmap_4k already issues invlpg).
 *
 * Rules: call with no spinlock held (other CPUs must be able to take the
 * IPI); free the physical pages only after this returns. */
void tlb_shootdown(uint64_t va, uint64_t pages, uint64_t space);
void tlb_ipi_handler(void);
uint64_t tlb_shootdown_count(void);

#endif
