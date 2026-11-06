/* ACPI power-off. Tables are reached through the HHDM, which maps the ACPI
 * memory map regions read-only (see vmm_init). The \_S5 package is located
 * by a byte scan of the DSDT (the common technique without an AML
 * interpreter): NameOp "_S5_" PackageOp ... SLP_TYPa, SLP_TYPb. */
#include "drivers/acpi.h"
#include "core/boot.h"
#include "core/log.h"
#include "core/time.h"
#include "arch/x86_64/io.h"
#include "lib.h"

static bool ready;
static uint16_t pm1a_cnt, pm1b_cnt;
static uint16_t slp_typa, slp_typb;
static char desc[96] = "ACPI not found";

bool acpi_available(void) { return ready; }
const char *acpi_describe(void) { return desc; }

static bool checksum_ok(const uint8_t *p, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++)
        s = (uint8_t)(s + p[i]);
    return s == 0;
}

static const uint8_t *map_table(uint64_t phys)
{
    if (!phys)
        return NULL;
    const uint8_t *t = phys_to_virt(phys);
    uint32_t len;
    memcpy(&len, t + 4, 4);
    if (len < 36 || len > (4u << 20) || !checksum_ok(t, len))
        return NULL;
    return t;
}

static bool parse_s5(const uint8_t *dsdt, uint32_t len)
{
    for (uint32_t i = 0; i + 4 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4))
            continue;
        /* NameOp (0x08), optionally preceded by the root prefix '\'. */
        if (!(i >= 1 && (dsdt[i - 1] == 0x08 || (i >= 2 && dsdt[i - 2] == 0x08 && dsdt[i - 1] == '\\'))))
            continue;
        uint32_t o = i + 4;
        if (o >= len || dsdt[o] != 0x12) /* PackageOp */
            continue;
        o++;
        o += (uint32_t)((dsdt[o] >> 6) & 3) + 1; /* PkgLength */
        o++;                                      /* NumElements */
        uint16_t vals[2];
        for (int k = 0; k < 2; k++) {
            if (o >= len)
                return false;
            uint8_t b = dsdt[o];
            if (b == 0x0A) { /* BytePrefix */
                if (o + 1 >= len)
                    return false;
                vals[k] = dsdt[o + 1];
                o += 2;
            } else if (b == 0x00 || b == 0x01) { /* ZeroOp / OneOp */
                vals[k] = b;
                o++;
            } else {
                return false;
            }
        }
        slp_typa = vals[0];
        slp_typb = vals[1];
        return true;
    }
    return false;
}

bool acpi_init(void)
{
    if (!g_boot.rsdp_phys)
        return false;
    const uint8_t *rsdp = phys_to_virt(g_boot.rsdp_phys);
    if (memcmp(rsdp, "RSD PTR ", 8) || !checksum_ok(rsdp, 20))
        return false;
    uint8_t rev = rsdp[15];
    uint64_t xsdt = 0;
    uint32_t rsdt = 0;
    memcpy(&rsdt, rsdp + 16, 4);
    if (rev >= 2)
        memcpy(&xsdt, rsdp + 24, 8);
    const uint8_t *root = map_table(xsdt ? xsdt : rsdt);
    if (!root)
        return false;
    uint32_t rlen;
    memcpy(&rlen, root + 4, 4);
    size_t esz = xsdt ? 8 : 4;
    const uint8_t *fadt = NULL;
    for (size_t off = 36; off + esz <= rlen; off += esz) {
        uint64_t p = 0;
        memcpy(&p, root + off, esz);
        const uint8_t *t = map_table(p);
        if (t && !memcmp(t, "FACP", 4)) {
            fadt = t;
            break;
        }
    }
    if (!fadt)
        return false;
    uint32_t flen, dsdt32 = 0, smi_cmd = 0, a = 0, b = 0;
    memcpy(&flen, fadt + 4, 4);
    memcpy(&dsdt32, fadt + 40, 4);
    memcpy(&smi_cmd, fadt + 48, 4);
    uint8_t acpi_enable = fadt[52];
    memcpy(&a, fadt + 64, 4);
    memcpy(&b, fadt + 68, 4);
    uint64_t dsdt = dsdt32;
    if (flen >= 148) {
        uint64_t x = 0;
        memcpy(&x, fadt + 140, 8);
        if (x)
            dsdt = x;
    }
    pm1a_cnt = (uint16_t)a;
    pm1b_cnt = (uint16_t)b;
    const uint8_t *d = map_table(dsdt);
    if (!d || !pm1a_cnt)
        return false;
    uint32_t dlen;
    memcpy(&dlen, d + 4, 4);
    if (!parse_s5(d + 36, dlen - 36))
        return false;
    /* Switch to ACPI mode if the firmware left it in legacy mode. */
    if (smi_cmd && acpi_enable && !(inw(pm1a_cnt) & 1)) {
        outb((uint16_t)smi_cmd, acpi_enable);
        for (int i = 0; i < 300 && !(inw(pm1a_cnt) & 1); i++)
            time_sleep_ms(1);
    }
    ready = true;
    k_snprintf(desc, sizeof desc, "ACPI rev %u, PM1a_CNT 0x%x, S5 type %u", rev, pm1a_cnt, slp_typa);
    klog("acpi: %s", desc);
    return true;
}

void acpi_poweroff(void)
{
    if (!ready)
        return;
    klog("acpi: entering S5 (power off)");
    cpu_cli();
    outw(pm1a_cnt, (uint16_t)(slp_typa << 10 | 1u << 13));
    if (pm1b_cnt)
        outw(pm1b_cnt, (uint16_t)(slp_typb << 10 | 1u << 13));
    for (int i = 0; i < 1000000; i++)
        io_wait();
    cpu_sti();
}
