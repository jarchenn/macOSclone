#ifndef AURORA_VIRTUAL_H
#define AURORA_VIRTUAL_H
#include "types.h"

/* Kernel virtual address layout (4-level paging, 48-bit):
 *
 *   0x0000000000000000 - 0x00007fffffffffff  unmapped (NULL and low addresses fault)
 *   HHDM (bootloader offset, e.g. 0xffff800000000000)
 *                                            direct map of usable RAM, kernel/modules
 *                                            and the framebuffer; RW, NX
 *   0xffffc00000000000                       kernel heap, grows on demand, bounded
 *   0xffffd00000000000                       kernel stacks, each below an unmapped guard page
 *   0xffffe00000000000                       MMIO window (LAPIC), uncached
 *   0xffffffff80000000                       kernel image (text RX, rodata R, data RW+NX)
 */
#define VMM_HEAP_BASE 0xffffc00000000000ull
#define VMM_STACK_BASE 0xffffd00000000000ull
#define VMM_MMIO_BASE 0xffffe00000000000ull
#define VMM_KERNEL_BASE 0xffffffff80000000ull

#define KERNEL_STACK_SIZE (64 * 1024)

struct vmm_info {
    uint64_t kernel_pages, hhdm_pages_4k, hhdm_pages_2m, fb_pages, table_pages;
    bool fb_write_combining;
    bool nx;
    uint64_t stack_top, stack_guard;
    void *lapic;
};
extern struct vmm_info g_vmm;

/* Builds Aurora's page tables and loads CR3. Must run on a stack mapped in
 * both the old and new tables (Limine's stack lives in the HHDM). */
void vmm_init(void);
/* Maps `bytes` (page multiple) of fresh zeroed RAM at va. */
bool vmm_map_fresh(uint64_t va, size_t bytes);
uint64_t vmm_translate(uint64_t va);

#endif
