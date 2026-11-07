/* Aurora OS kernel entry and main loop. */
#include "core/boot.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/time.h"
#include "core/event_queue.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/serial.h"
#include "drivers/framebuffer.h"
#include "drivers/pic.h"
#include "drivers/input.h"
#include "graphics/console.h"
#include "memory/physical.h"
#include "memory/virtual.h"
#include "memory/heap.h"
#include "fs/vfs.h"
#include "core/sched.h"
#include "core/process.h"
#include "core/spinlock.h"
#include "net/net.h"
#include "drivers/pci.h"
#include "drivers/display.h"
#include "drivers/blk.h"
#include "drivers/acpi.h"
#include "drivers/apic.h"
#include "arch/smp.h"
#include "memory/tlb.h"
#include "core/power.h"
#include "lib/inflate.h"
#include "lib.h"
void panic_halt_ipi(void);
#include "limine.h"

NORETURN void kmain_early(void);
static NORETURN void kmain_stage2(void);

static void selftest_heap(void)
{
    struct heap_stats before, after;
    kheap_stats(&before);
    void *a = kmalloc(24), *b = kmalloc(4000), *c = kmalloc(100000);
    KASSERT(a && b && c);
    KASSERT(((uintptr_t)a & 15) == 0 && ((uintptr_t)b & 15) == 0);
    memset(a, 0x11, 24);
    memset(b, 0x22, 4000);
    memset(c, 0x33, 100000);
    KASSERT(kmalloc(0) == NULL);
    KASSERT(kmalloc((size_t)1 << 40) == NULL);
    kfree(b);
    kfree(a);
    kfree(c);
    kfree(NULL);
    KASSERT(kheap_check());
    kheap_stats(&after);
    KASSERT(after.used_bytes == before.used_bytes && after.alloc_count == before.alloc_count);
    klog("selftest: heap alloc/free ok (mapped %zu KiB)", after.mapped_bytes / 1024);
}

static void selftest_pmm(void)
{
    uint64_t free_before = pmm_state()->free_pages;
    phys_addr_t p1 = pmm_alloc_page(), p2 = pmm_alloc_page();
    KASSERT(p1 && p2 && p1 != p2 && !(p1 & 0xFFF));
    KASSERT(*(uint64_t *)phys_to_virt(p1) == 0);
    pmm_free_page(p1);
    pmm_free_page(p2);
    KASSERT(pmm_state()->free_pages == free_before);
    klog("selftest: physical page alloc/free ok");
}

void kmain_early(void)
{
    serial_init();
    klog_raw("\n");
    klog("Aurora OS %s (%s build, id %s) - x86_64", AURORA_VERSION, AURORA_VARIANT, AURORA_BUILD_ID);
    klog("AURORA_MILESTONE serial");

    boot_collect();
    klog("boot: %s %s, base revision %llu, firmware %s, HHDM %llx",
         g_boot.bootloader_name, g_boot.bootloader_version, (unsigned long long)g_boot.base_revision,
         g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_EFI64 ? "UEFI x64" :
         g_boot.firmware_type == LIMINE_FIRMWARE_TYPE_X86BIOS ? "BIOS" : "other",
         (unsigned long long)g_boot.hhdm_offset);
    klog("boot: kernel phys %llx virt %llx, %zu memmap entries, %zu module(s)",
         (unsigned long long)g_boot.kernel_phys_base, (unsigned long long)g_boot.kernel_virt_base,
         g_boot.memmap_count, g_boot.module_count);
    uint64_t usable = 0;
    for (size_t i = 0; i < g_boot.memmap_count; i++) {
        const struct boot_mem_region *r = &g_boot.memmap[i];
        if (r->type == BOOT_MEM_USABLE)
            usable += r->length;
        klog("  mem %016llx-%016llx %s", (unsigned long long)r->base,
             (unsigned long long)(r->base + r->length), boot_mem_type_name(r->type));
    }
    klog("boot: %llu KiB usable RAM reported", (unsigned long long)(usable / 1024));
    klog("AURORA_MILESTONE bootinfo");

    if (fb_init()) {
        console_init();
        klog_set_console(true);
        klog("fb: %ux%u, pitch %llu, %u bpp, R@%u G@%u B@%u", g_fb.width, g_fb.height,
             (unsigned long long)g_fb.pitch, g_fb.bpp, g_fb.rs, g_fb.gs, g_fb.bs);
        klog("AURORA_MILESTONE framebuffer");
    }

    cpu_detect();
    cpu_enable_sse();
    klog("cpu: %s nx=%d pat=%d apic=%d x2apic=%d", g_cpu.vendor, g_cpu.nx, g_cpu.pat, g_cpu.apic, g_cpu.x2apic);
    gdt_init();
    idt_init();
    klog("AURORA_MILESTONE gdt_idt");

    pmm_init();
    vmm_init();
    klog("AURORA_MILESTONE paging");
    /* Leave Limine's stack (bootloader-reclaimable) for our guarded stack. */
    switch_stack_and_call(g_vmm.stack_top, kmain_stage2);
}

static NORETURN void kmain_stage2(void)
{
    klog("stack: switched to %llx (guard page at %llx)", (unsigned long long)g_vmm.stack_top,
         (unsigned long long)g_vmm.stack_guard);
    /* All bootloader responses were copied into g_boot; Limine's GDT, page
     * tables and stack are no longer in use, so reclaim that memory. */
    pmm_reclaim_bootloader();
    selftest_pmm();
    kheap_init();
    selftest_heap();
    klog("AURORA_MILESTONE memory");
    sched_init();
    bkl_lock(); /* the desktop task (this context) runs kernel code under the BKL */

    pic_init();
    lapic_virtual_wire(g_vmm.lapic);
    pit_init();
    ps2_init();
    cpu_sti();
    uint64_t t0 = time_ticks();
    for (int i = 0; i < 1000000 && time_ticks() < t0 + 3; i++)
        cpu_hlt();
    if (time_ticks() < t0 + 3)
        panic("timer: PIT interrupts are not arriving");
    klog("timer: PIT %u Hz (divisor %u), ticks advancing", PIT_TARGET_HZ, pit_divisor());
    if (!acpi_init())
        klog("acpi: not available; power-off falls back to the QEMU PM port");
    if (apic_init()) {
        vector_install(APIC_TIMER_VECTOR, apic_timer_tick);
        vector_install(IPI_RESCHED_VECTOR, sched_resched_ipi);
        vector_install(IPI_TLB_VECTOR, tlb_ipi_handler);
        vector_install(IPI_HALT_VECTOR, panic_halt_ipi);
        if (!apic_timer_start())
            klog("apic: LAPIC timer unusable; the PIT keeps driving the scheduler");
        smp_init();
    }
    acpi_setup_sci();
    klog("AURORA_MILESTONE interrupts");

#ifdef AURORA_PANIC_TEST
    klog("panic-test: dereferencing NULL to verify the page-fault diagnostic");
    *(volatile uint32_t *)0 = 0xDEAD;
#endif

    process_init_clock();
    vfs_init();
    const struct boot_module *initrd = NULL;
    for (size_t i = 0; i < g_boot.module_count; i++)
        if (!strcmp(g_boot.modules[i].string, "initramfs"))
            initrd = &g_boot.modules[i];
    if (!initrd) {
        klog("vfs: warning: no initramfs module; /system will be empty");
    } else {
        const uint8_t *data = phys_to_virt(initrd->phys);
        uint64_t size = initrd->size;
        /* 0.7: "AZIP" + 64-bit size + zlib stream (tools/build.py); the
         * bootloader then has less to load (the fonts made it too big for
         * Limine at 128 MiB). The inflated copy stays for the kernel's
         * lifetime: /system's nodes point into it. */
        if (size > 12 && !memcmp(data, "AZIP", 4)) {
            uint64_t want;
            memcpy(&want, data + 4, 8);
            phys_addr_t pa = want && want < (256ull << 20) ? pmm_alloc_contig(ALIGN_UP(want, PAGE_SIZE) / PAGE_SIZE) : 0;
            size_t got = 0;
            uint64_t zt0 = time_ms();
            int zr = pa ? zlib_inflate(data + 12, size - 12, phys_to_virt(pa), want, &got) : INFLATE_EFULL;
            if (zr != INFLATE_OK || got != want) {
                klog("vfs: initramfs could not be inflated (error %d, %zu of %llu bytes)", zr, got,
                     (unsigned long long)want);
                size = 0;
            } else {
                klog("vfs: initramfs inflated %llu -> %llu bytes in %llu ms", (unsigned long long)size,
                     (unsigned long long)want, (unsigned long long)(time_ms() - zt0));
                data = phys_to_virt(pa);
                size = want;
            }
        }
        int r = size ? vfs_load_initramfs(data, size) : VFS_EBADARCHIVE;
        if (r != VFS_OK)
            klog("vfs: warning: initramfs rejected (%s); /system may be incomplete", vfs_strerror(r));
        else
            klog("vfs: initramfs %s (%llu bytes) mounted at /system, seeded /home", initrd->path,
                 (unsigned long long)size);
    }
    pci_init();
    display_init();
    if (vblk_init()) {
        bool formatted = false;
        int pr = vfs_attach_persistent(vblk_blockdev(), &formatted);
        if (pr == VFS_OK)
            klog("vfs: /home is on the data disk (%s)", formatted ? "blank disk formatted" : "existing files loaded");
        else
            klog("vfs: warning: data disk not used (%s); /home stays in RAM", vfs_strerror(pr));
    } else {
        klog("vfs: no data disk; /home is RAM-backed");
    }
    struct vfs_stat st;
    if (vfs_stat("/home/notes", &st) != VFS_OK)
        vfs_mkdir("/home/notes");
    klog("AURORA_MILESTONE vfs");
    net_init();
    process_start_init();

    klog("AURORA_BOOT_OK");
    /* 0.7: the desktop is a user process (/system/bin/display, started by
     * init). Task 0 stays in the kernel and serves power requests. */
    power_main();
}
