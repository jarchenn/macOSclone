/* Restart and power-off (0.7). Before 0.7 the desktop task did this; now
 * task 0 waits here for a request from SYS_POWER (root) or the ACPI power
 * button, syncs /home and resets or powers the machine off. */
#include "core/power.h"
#include "core/sched.h"
#include "core/log.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/acpi.h"
#include "fs/vfs.h"
#include "lib.h"

static volatile int requested;
static struct task *power_task;

void power_request(int how)
{
    if (how != POWER_RESTART && how != POWER_OFF)
        return;
    requested = how;
    sched_wake(power_task); /* IRQ-safe */
}

static void do_reboot(void)
{
    int sr = vfs_sync();
    klog("power: restart requested (sync %s; %s)", sr ? vfs_strerror(sr) : "ok",
         vfs_home_persistent() ? "/home is on the data disk" : "RAM files in /home will be lost");
    TEST_EVENT("REBOOT");
    /* 1. ACPI FADT reset register */
    acpi_reset();
    cpu_cli();
    /* 2. 8042 controller reset line pulse */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        io_wait();
    outb(0x64, 0xFE);
    for (int i = 0; i < 1000000; i++)
        io_wait();
    /* 3. Fallback: triple fault via an empty IDT */
    klog("power: 8042 reset failed; forcing triple fault");
    struct PACKED { uint16_t limit; uint64_t base; } idtr = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(idtr));
    for (;;)
        cpu_hlt();
}

static void do_poweroff(void)
{
    int s = vfs_sync();
    klog("power: power off requested (sync %s)", s ? vfs_strerror(s) : "ok");
    TEST_EVENT("POWEROFF");
    acpi_poweroff();
    outw(0x604, 0x2000); /* QEMU PIIX4 PM fallback */
    klog("power: power-off did not take effect; it is safe to turn the machine off");
}

void power_main(void)
{
    power_task = sched_current();
    for (;;) {
        cpu_cli();
        if (!requested)
            sched_block(0);
        cpu_sti();
        int how = requested;
        requested = 0;
        if (how == POWER_RESTART)
            do_reboot();
        else if (how == POWER_OFF)
            do_poweroff();
    }
}
