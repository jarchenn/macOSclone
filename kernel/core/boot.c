/* Limine protocol requests and validation of the bootloader's responses. */
#include "core/boot.h"
#include "core/log.h"
#include "core/panic.h"
#include "lib.h"
#include "limine.h"

#define REQ __attribute__((used, section(".limine_requests"))) static volatile

__attribute__((used, section(".limine_requests_start"))) static volatile uint64_t limine_start_marker[4] =
    LIMINE_REQUESTS_START_MARKER;

/* Base revision 6 is the newest revision specified by the pinned protocol. */
REQ uint64_t limine_base_revision[3] = LIMINE_BASE_REVISION(6);

REQ struct limine_bootloader_info_request bootloader_info_req = { .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0 };
REQ struct limine_firmware_type_request firmware_req = { .id = LIMINE_FIRMWARE_TYPE_REQUEST_ID, .revision = 0 };
REQ struct limine_hhdm_request hhdm_req = { .id = LIMINE_HHDM_REQUEST_ID, .revision = 0 };
REQ struct limine_framebuffer_request fb_req = { .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0 };
REQ struct limine_memmap_request memmap_req = { .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0 };
REQ struct limine_module_request module_req = { .id = LIMINE_MODULE_REQUEST_ID, .revision = 0 };
REQ struct limine_executable_address_request kaddr_req = { .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0 };
REQ struct limine_paging_mode_request paging_req = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID, .revision = 0,
    .mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .max_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_4LVL,
};

__attribute__((used, section(".limine_requests_end"))) static volatile uint64_t limine_end_marker[2] =
    LIMINE_REQUESTS_END_MARKER;

struct boot_info g_boot;

const char *boot_mem_type_name(uint32_t type)
{
    static const char *names[] = { "usable", "reserved", "acpi-reclaimable", "acpi-nvs", "bad",
                                   "bootloader-reclaimable", "kernel+modules", "framebuffer", "reserved-mapped" };
    return type < ARRAY_LEN(names) ? names[type] : "unknown";
}

static void copy_cstr(char *dst, size_t cap, const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) {
        char c = src[i];
        dst[i] = (c >= 32 && c < 127) ? c : '?';
    }
    dst[i] = '\0';
}

void boot_collect(void)
{
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("boot: Limine did not accept base revision 6 (loaded revision %llu)",
              (unsigned long long)(LIMINE_LOADED_BASE_REVISION_VALID(limine_base_revision)
                                       ? LIMINE_LOADED_BASE_REVISION(limine_base_revision) : 0));
    g_boot.base_revision = LIMINE_LOADED_BASE_REVISION_VALID(limine_base_revision)
                               ? LIMINE_LOADED_BASE_REVISION(limine_base_revision) : 6;

    struct limine_hhdm_response *hhdm = hhdm_req.response;
    if (!hhdm)
        panic("boot: missing HHDM response");
    if (hhdm->offset < 0xffff800000000000ull || (hhdm->offset & 0xFFF))
        panic("boot: implausible HHDM offset %llx", (unsigned long long)hhdm->offset);
    g_boot.hhdm_offset = hhdm->offset;

    struct limine_paging_mode_response *pm = paging_req.response;
    if (pm && pm->mode != LIMINE_PAGING_MODE_X86_64_4LVL)
        panic("boot: bootloader did not provide 4-level paging");

    struct limine_executable_address_response *ka = kaddr_req.response;
    if (!ka)
        panic("boot: missing executable address response");
    g_boot.kernel_phys_base = ka->physical_base;
    g_boot.kernel_virt_base = ka->virtual_base;
    if ((ka->physical_base & 0xFFF) || ka->virtual_base != 0xffffffff80000000ull)
        panic("boot: unexpected kernel load address phys=%llx virt=%llx",
              (unsigned long long)ka->physical_base, (unsigned long long)ka->virtual_base);

    struct limine_bootloader_info_response *bi = bootloader_info_req.response;
    if (bi) {
        copy_cstr(g_boot.bootloader_name, sizeof g_boot.bootloader_name, bi->name);
        copy_cstr(g_boot.bootloader_version, sizeof g_boot.bootloader_version, bi->version);
    } else {
        str_copy(g_boot.bootloader_name, "unknown", sizeof g_boot.bootloader_name);
    }
    struct limine_firmware_type_response *fw = firmware_req.response;
    g_boot.firmware_type = fw ? fw->firmware_type : 0xFF;

    struct limine_memmap_response *mm = memmap_req.response;
    if (!mm || !mm->entries || mm->entry_count == 0)
        panic("boot: missing memory map response");
    if (mm->entry_count > BOOT_MAX_MEMMAP)
        panic("boot: memory map has %llu entries (max %d)", (unsigned long long)mm->entry_count, BOOT_MAX_MEMMAP);
    uint64_t prev_end = 0;
    for (size_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (!e)
            panic("boot: memory map entry %zu is NULL", i);
        if (e->base + e->length < e->base)
            panic("boot: memory map entry %zu overflows", i);
        if (e->base < prev_end)
            klog("boot: warning: memory map entry %zu overlaps the previous entry", i);
        prev_end = e->base + e->length;
        g_boot.memmap[i].base = e->base;
        g_boot.memmap[i].length = e->length;
        g_boot.memmap[i].type = (uint32_t)(e->type <= 8 ? e->type : BOOT_MEM_RESERVED);
    }
    g_boot.memmap_count = mm->entry_count;

    struct limine_framebuffer_response *fbr = fb_req.response;
    if (fbr && fbr->framebuffer_count > 0 && fbr->framebuffers && fbr->framebuffers[0]) {
        struct limine_framebuffer *f = fbr->framebuffers[0];
        g_boot.have_fb = true;
        g_boot.fb.phys = virt_to_phys_hhdm(f->address);
        g_boot.fb.width = f->width;
        g_boot.fb.height = f->height;
        g_boot.fb.pitch = f->pitch;
        g_boot.fb.bpp = f->bpp;
        g_boot.fb.red_size = f->red_mask_size;
        g_boot.fb.red_shift = f->red_mask_shift;
        g_boot.fb.green_size = f->green_mask_size;
        g_boot.fb.green_shift = f->green_mask_shift;
        g_boot.fb.blue_size = f->blue_mask_size;
        g_boot.fb.blue_shift = f->blue_mask_shift;
        if (f->memory_model != LIMINE_FRAMEBUFFER_RGB)
            panic("boot: framebuffer memory model %u unsupported", f->memory_model);
    }

    struct limine_module_response *mr = module_req.response;
    if (mr && mr->modules) {
        size_t n = mr->module_count;
        if (n > BOOT_MAX_MODULES)
            n = BOOT_MAX_MODULES;
        for (size_t i = 0; i < n; i++) {
            struct limine_file *f = mr->modules[i];
            if (!f || !f->address)
                continue;
            struct boot_module *m = &g_boot.modules[g_boot.module_count++];
            copy_cstr(m->path, sizeof m->path, f->path);
            copy_cstr(m->string, sizeof m->string, f->string);
            m->phys = virt_to_phys_hhdm(f->address);
            m->size = f->size;
        }
    }
}
