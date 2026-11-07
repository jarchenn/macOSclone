/* ACPI glue: finds the RSDP (Limine handoff, then a BIOS-area scan), runs
 * the portable table parser (drivers/acpi_tables.c), and implements S5
 * power-off, the FADT reset register and the fixed power-button event.
 *
 * Tables are read through the HHDM, which maps ACPI memory read-only (see
 * vmm_init); anything else is mapped on demand through the MMIO window. The
 * \_S5 package is found by a byte scan of the DSDT: there is no AML
 * interpreter. */
#include "drivers/acpi.h"
#include "core/power.h"
#include "drivers/acpi_tables.h"
#include "core/boot.h"
#include "core/log.h"
#include "core/time.h"
#include "memory/virtual.h"
#include "arch/x86_64/io.h"
#include "arch/cpu.h"
#include "core/sched.h"
#include "lib.h"

static struct acpi_info info;
static bool parsed, s5_ready;
static char desc[128] = "ACPI not found";
static const char *rsdp_source = "none";

bool acpi_available(void) { return s5_ready; }
const char *acpi_describe(void) { return desc; }
const struct acpi_info *acpi_info(void) { return parsed ? &info : NULL; }

/* Readable view of [phys, phys+len): the HHDM if every page is mapped there,
 * otherwise a read-only MMIO-window mapping. */
static const uint8_t *map_phys(void *ctx, uint64_t phys, uint32_t len)
{
    UNUSED(ctx);
    if (!len || phys + len < phys)
        return NULL;
    bool in_hhdm = true;
    for (uint64_t p = ALIGN_DOWN(phys, PAGE_SIZE); p < phys + len; p += PAGE_SIZE)
        if (!vmm_translate(p + g_boot.hhdm_offset)) {
            in_hhdm = false;
            break;
        }
    if (in_hhdm)
        return phys_to_virt(phys);
    if (len > (1u << 20))
        return NULL;
    return vmm_map_mmio(phys, len, false);
}

static const uint8_t *find_rsdp(void)
{
    if (g_boot.rsdp_phys) {
        const uint8_t *r = map_phys(NULL, g_boot.rsdp_phys, 36);
        if (r && !memcmp(r, "RSD PTR ", 8) && acpi_checksum_ok(r, 20)) {
            rsdp_source = "bootloader";
            return r;
        }
    }
    /* Fallback for firmware that did not pass one: the first KiB of the EBDA,
     * then the BIOS ROM area 0xE0000-0xFFFFF (ACPI spec 5.2.5.1). */
    const uint8_t *bda = map_phys(NULL, 0x40E, 2);
    if (bda) {
        uint64_t ebda = (uint64_t)(bda[0] | bda[1] << 8) << 4;
        if (ebda >= 0x80000 && ebda < 0xA0000) {
            const uint8_t *e = map_phys(NULL, ebda, 1024);
            const uint8_t *r = e ? acpi_scan_rsdp(e, 1024) : NULL;
            if (r) {
                rsdp_source = "EBDA scan";
                return r;
            }
        }
    }
    const uint8_t *rom = map_phys(NULL, 0xE0000, 0x20000);
    const uint8_t *r = rom ? acpi_scan_rsdp(rom, 0x20000) : NULL;
    if (r)
        rsdp_source = "BIOS area scan";
    return r;
}

bool acpi_init(void)
{
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) {
        klog("acpi: no RSDP found");
        return false;
    }
    int r = acpi_parse(rsdp, map_phys, NULL, &info);
    if (r == ACPI_E_RSDP || r == ACPI_E_ROOT) {
        klog("acpi: root tables invalid (%d)", r);
        return false;
    }
    parsed = true;
    klog("acpi: rev %u via %s, %s; MADT %s: %d CPU(s), %d IOAPIC(s), %d override(s), LAPIC %x, "
         "%u skipped; HPET %s; reset %s",
         info.revision, rsdp_source, info.xsdt ? "XSDT" : "RSDT", info.have_madt ? "ok" : "missing",
         info.ncpus, info.nioapics, info.nisos, info.lapic_address, info.skipped_entries,
         info.have_hpet ? "present" : "absent", info.have_reset ? "register" : "none");
    if (!info.have_fadt || !info.have_s5 || !info.pm1a_cnt) {
        klog("acpi: S5 not available (FADT %d, _S5 %d)", info.have_fadt, info.have_s5);
        return true;
    }
    /* Switch to ACPI mode if the firmware left it in legacy mode. */
    if (info.smi_cmd && info.acpi_enable && !(inw((uint16_t)info.pm1a_cnt) & 1)) {
        outb((uint16_t)info.smi_cmd, info.acpi_enable);
        for (int i = 0; i < 300 && !(inw((uint16_t)info.pm1a_cnt) & 1); i++)
            time_sleep_ms(1);
    }
    s5_ready = true;
    k_snprintf(desc, sizeof desc, "ACPI rev %u, PM1a_CNT 0x%x, S5 type %u, %d CPU(s) in MADT", info.revision,
               info.pm1a_cnt, info.slp_typa, info.ncpus);
    klog("acpi: %s", desc);
    return true;
}

void acpi_poweroff(void)
{
    if (!s5_ready)
        return;
    klog("acpi: entering S5 (power off)");
    cpu_cli();
    outw((uint16_t)info.pm1a_cnt, (uint16_t)(info.slp_typa << 10 | 1u << 13));
    if (info.pm1b_cnt)
        outw((uint16_t)info.pm1b_cnt, (uint16_t)(info.slp_typb << 10 | 1u << 13));
    for (int i = 0; i < 1000000; i++)
        io_wait();
    cpu_sti();
}

bool acpi_reset(void)
{
    if (!parsed || !info.have_reset)
        return false;
    klog("acpi: reset via FADT reset register (%s 0x%llx <- 0x%x)", info.reset_reg.space ? "port" : "mmio",
         (unsigned long long)info.reset_reg.address, info.reset_value);
    if (info.reset_reg.space == 1) {
        outb((uint16_t)info.reset_reg.address, info.reset_value);
    } else {
        volatile uint8_t *p = vmm_map_mmio(info.reset_reg.address, 1, true);
        if (!p)
            return false;
        *p = info.reset_value;
    }
    for (int i = 0; i < 100000; i++)
        io_wait();
    return false; /* still running: the caller falls back */
}

/* Fixed power-button event (PM1 status bit 8, enable bit 8). */
#define PWRBTN (1u << 8)

bool acpi_enable_power_button(void)
{
    if (!parsed || !info.pm1a_evt || info.pm1_evt_len < 4)
        return false;
    uint16_t half = info.pm1_evt_len / 2;
    outw((uint16_t)info.pm1a_evt, PWRBTN); /* clear stale status (write 1) */
    outw((uint16_t)(info.pm1a_evt + half), (uint16_t)(inw((uint16_t)(info.pm1a_evt + half)) | PWRBTN));
    return true;
}

bool acpi_power_button_pressed(void)
{
    if (!parsed || !info.pm1a_evt || info.pm1_evt_len < 4)
        return false;
    uint16_t sts = inw((uint16_t)info.pm1a_evt);
    if (!(sts & PWRBTN))
        return false;
    outw((uint16_t)info.pm1a_evt, PWRBTN);
    return true;
}

uint16_t acpi_sci_irq(void) { return parsed ? info.sci_irq : 0; }

/* SCI (level-triggered): the only event Aurora enables is the fixed power
 * button. Pressing it (QEMU: system_powerdown) starts an orderly shutdown
 * in task 0 (core/power.c), which syncs /home before entering S5. */
static void sci_handler(void)
{
    if (acpi_power_button_pressed()) {
        klog("acpi: power button pressed");
        power_request(POWER_OFF);
    }
}

bool acpi_setup_sci(void)
{
    if (!acpi_enable_power_button() || !info.sci_irq || info.sci_irq >= 16)
        return false;
    irq_install(info.sci_irq, sci_handler);
    irq_enable(info.sci_irq);
    klog("acpi: SCI on IRQ %u, power button enabled", info.sci_irq);
    return true;
}
