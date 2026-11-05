#ifndef AURORA_BOOT_H
#define AURORA_BOOT_H

#include "types.h"

/* Everything the kernel needs from the bootloader is copied into this
 * structure (in kernel .bss) before bootloader-reclaimable memory is reused. */

enum boot_mem_type {
    BOOT_MEM_USABLE = 0,
    BOOT_MEM_RESERVED = 1,
    BOOT_MEM_ACPI_RECLAIMABLE = 2,
    BOOT_MEM_ACPI_NVS = 3,
    BOOT_MEM_BAD = 4,
    BOOT_MEM_BOOTLOADER_RECLAIMABLE = 5,
    BOOT_MEM_KERNEL_AND_MODULES = 6,
    BOOT_MEM_FRAMEBUFFER = 7,
    BOOT_MEM_RESERVED_MAPPED = 8,
};

#define BOOT_MAX_MEMMAP 256
#define BOOT_MAX_MODULES 4

struct boot_mem_region {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

struct boot_framebuffer {
    phys_addr_t phys;
    uint64_t width, height, pitch;
    uint16_t bpp;
    uint8_t red_size, red_shift, green_size, green_shift, blue_size, blue_shift;
};

struct boot_module {
    char path[128];
    char string[64];
    phys_addr_t phys;
    uint64_t size;
};

struct boot_info {
    uint64_t hhdm_offset;
    phys_addr_t kernel_phys_base;
    virt_addr_t kernel_virt_base;
    uint64_t base_revision; /* revision actually used by the bootloader */
    uint64_t firmware_type; /* LIMINE_FIRMWARE_TYPE_* */
    char bootloader_name[32];
    char bootloader_version[32];
    size_t memmap_count;
    struct boot_mem_region memmap[BOOT_MAX_MEMMAP];
    bool have_fb;
    struct boot_framebuffer fb;
    size_t module_count;
    struct boot_module modules[BOOT_MAX_MODULES];
};

extern struct boot_info g_boot;

/* Validates Limine responses and copies them into g_boot.
 * Panics with a diagnostic if a required response is missing or invalid. */
void boot_collect(void);
const char *boot_mem_type_name(uint32_t type);

static inline void *phys_to_virt(phys_addr_t p) { return (void *)(uintptr_t)(p + g_boot.hhdm_offset); }
static inline phys_addr_t virt_to_phys_hhdm(const void *v) { return (phys_addr_t)(uintptr_t)v - g_boot.hhdm_offset; }

#endif
