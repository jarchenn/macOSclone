/* ACPI table parser: synthetic firmware tables in a fake physical memory. */
#include "harness.h"
#include "drivers/acpi_tables.h"

const char *t_suite = "acpi";

#define MEM_SIZE 0x10000
static uint8_t mem[MEM_SIZE];

static const uint8_t *map(void *ctx, uint64_t phys, uint32_t len)
{
    (void)ctx;
    if (phys >= MEM_SIZE || len > MEM_SIZE - phys)
        return NULL;
    return mem + phys;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static void fix_sum(uint8_t *p, uint32_t len, uint32_t sum_off)
{
    p[sum_off] = 0;
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++)
        s = (uint8_t)(s + p[i]);
    p[sum_off] = (uint8_t)(0 - s);
}

static void sdt(uint32_t at, const char *sig, uint32_t len)
{
    memcpy(mem + at, sig, 4);
    put32(mem + at + 4, len);
    mem[at + 8] = 2;
    fix_sum(mem + at, len, 9);
}

enum { RSDP = 0x100, XSDT = 0x200, FADT = 0x400, DSDT = 0x600, MADT = 0x800, HPET = 0xA00, RSDT = 0xC00 };

static uint32_t madt_len;

/* Builds a QEMU-like table set: 4 CPUs (one disabled), one IOAPIC, two ISOs. */
static void build(bool xsdt)
{
    memset(mem, 0, sizeof mem);
    /* DSDT with a \_S5 package: Name(_S5, Package(4){0x05, 0x05, 0, 0}) */
    static const uint8_t aml[] = { 0x10, 0x08, 0x5C, '_', 'S', 'B', '_', 0x08, '_', 'S', '5', '_', 0x12, 0x08, 0x04, 0x0A, 0x05, 0x0A, 0x05, 0x00, 0x00 };
    memcpy(mem + DSDT + 36, aml, sizeof aml);
    sdt(DSDT, "DSDT", 36 + sizeof aml);
    /* FADT (rev 3 layout, 244 bytes) */
    uint8_t *f = mem + FADT;
    put32(f + 40, DSDT);
    put16(f + 46, 9);
    put32(f + 48, 0xB2);
    f[52] = 0xF1;
    put32(f + 56, 0x600);
    put32(f + 64, 0x604);
    f[88] = 4;
    put32(f + 112, ACPI_FADT_RESET_REG_SUP);
    f[116] = 1; /* I/O space */
    f[117] = 8;
    put64(f + 120, 0xCF9);
    f[128] = 0x06;
    put64(f + 140, DSDT);
    sdt(FADT, "FACP", 244);
    /* MADT */
    uint8_t *m = mem + MADT;
    put32(m + 36, 0xFEE00000);
    put32(m + 40, 1);
    uint32_t o = 44;
    for (int i = 0; i < 4; i++) {
        m[o] = 0; m[o + 1] = 8; m[o + 2] = (uint8_t)i; m[o + 3] = (uint8_t)i;
        put32(m + o + 4, i == 3 ? 0 : 1); /* CPU 3 neither enabled nor online-capable */
        o += 8;
    }
    m[o] = 1; m[o + 1] = 12; m[o + 2] = 0; put32(m + o + 4, 0xFEC00000); put32(m + o + 8, 0); o += 12;
    m[o] = 2; m[o + 1] = 10; m[o + 2] = 0; m[o + 3] = 0; put32(m + o + 4, 2); put16(m + o + 8, 0); o += 10;
    m[o] = 2; m[o + 1] = 10; m[o + 2] = 0; m[o + 3] = 9; put32(m + o + 4, 9); put16(m + o + 8, 0xD); o += 10;
    m[o] = 4; m[o + 1] = 6; m[o + 2] = 0xFF; put16(m + o + 3, 0); m[o + 5] = 1; o += 6;
    madt_len = o;
    sdt(MADT, "APIC", o);
    /* HPET */
    mem[HPET + 40] = 0;
    put64(mem + HPET + 44, 0xFED00000);
    sdt(HPET, "HPET", 56);
    /* Root tables and RSDP */
    put64(mem + XSDT + 36, FADT);
    put64(mem + XSDT + 44, MADT);
    put64(mem + XSDT + 52, HPET);
    put64(mem + XSDT + 60, 0xFFFF0); /* unreadable entry: ignored */
    sdt(XSDT, "XSDT", 68);
    put32(mem + RSDT + 36, FADT);
    put32(mem + RSDT + 40, MADT);
    sdt(RSDT, "RSDT", 44);
    uint8_t *r = mem + RSDP;
    memcpy(r, "RSD PTR ", 8);
    r[15] = xsdt ? 2 : 0;
    put32(r + 16, RSDT);
    if (xsdt) {
        put32(r + 20, 36);
        put64(r + 24, XSDT);
    }
    fix_sum(r, 20, 8);
    if (xsdt)
        fix_sum(r, 36, 32);
}

static struct acpi_info info;

TEST(acpi_full_parse)
{
    build(true);
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_OK);
    CHECK(info.xsdt);
    CHECK_EQ(info.revision, 2);
    CHECK_EQ(info.sci_irq, 9);
    CHECK_EQ(info.pm1a_cnt, 0x604);
    CHECK_EQ(info.pm1a_evt, 0x600);
    CHECK_EQ(info.pm1_evt_len, 4);
    CHECK(info.have_reset);
    CHECK_EQ(info.reset_reg.space, 1);
    CHECK_EQ(info.reset_reg.address, 0xCF9);
    CHECK_EQ(info.reset_value, 6);
    CHECK(info.have_s5);
    CHECK_EQ(info.slp_typa, 5);
    CHECK(info.have_madt);
    CHECK_EQ(info.lapic_address, 0xFEE00000u);
    CHECK(info.pcat_compat);
    CHECK_EQ(info.ncpus, 3); /* the unusable CPU is not recorded */
    CHECK_EQ(info.cpus[2].apic_id, 2);
    CHECK_EQ(info.nioapics, 1);
    CHECK_EQ(info.ioapics[0].address, 0xFEC00000u);
    CHECK_EQ(info.nisos, 2);
    CHECK_EQ(info.nmi_lint, 1);
    CHECK_EQ(info.skipped_entries, 0);
    CHECK(info.have_hpet);
    CHECK_EQ(info.hpet_address, 0xFED00000u);
    uint16_t fl;
    CHECK_EQ(acpi_isa_gsi(&info, 0, &fl), 2); /* PIT override */
    CHECK_EQ(acpi_isa_gsi(&info, 9, &fl), 9);
    CHECK_EQ(fl, 0xD); /* level, active high */
    CHECK_EQ(acpi_isa_gsi(&info, 1, &fl), 1);
    CHECK_EQ(fl, 0);
}

TEST(acpi_rsdt_only)
{
    build(false);
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_OK);
    CHECK(!info.xsdt);
    CHECK(info.have_madt);
    CHECK(!info.have_hpet); /* not listed in the RSDT */
}

TEST(acpi_rejects_bad_tables)
{
    build(true);
    mem[RSDP + 3] ^= 1; /* signature */
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_E_RSDP);
    build(true);
    mem[RSDP + 16] ^= 1; /* RSDP checksum */
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_E_RSDP);
    build(true);
    mem[XSDT + 40] ^= 0x55; /* root checksum */
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_E_ROOT);
    build(true);
    mem[FADT + 100] ^= 1; /* FADT checksum: the table is ignored */
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_E_NOFADT);
    CHECK(info.have_madt);
    build(true);
    put32(mem + FADT + 4, 0x7FFFFFFF); /* absurd length */
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_E_NOFADT);
    build(true);
    mem[DSDT + 45] = 'X'; /* \_S5 name damaged (checksum fixed) */
    fix_sum(mem + DSDT, 36 + 21, 9);
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_OK);
    CHECK(!info.have_s5);
    CHECK_EQ(acpi_parse(NULL, map, NULL, &info), ACPI_E_RSDP);
}

TEST(acpi_malformed_madt)
{
    /* A zero-length entry must stop the walk, not loop forever. */
    build(true);
    mem[MADT + 44 + 8 + 1] = 0; /* second CPU entry length = 0 */
    fix_sum(mem + MADT, madt_len, 9);
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_OK);
    CHECK_EQ(info.ncpus, 1);
    CHECK_EQ(info.skipped_entries, 1);
    CHECK_EQ(info.nioapics, 0);
    /* An entry running past the table end is not read. */
    build(true);
    mem[MADT + 44 + 1] = 200;
    fix_sum(mem + MADT, madt_len, 9);
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_OK);
    CHECK_EQ(info.ncpus, 0);
    /* Short entries of known types are skipped. */
    build(true);
    mem[MADT + 44 + 32 + 1] = 6; /* IOAPIC entry with length 6 */
    mem[MADT + 44 + 32 + 6] = 7; /* following bytes become an unknown entry */
    mem[MADT + 44 + 32 + 7] = 6;
    fix_sum(mem + MADT, madt_len, 9);
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_OK);
    CHECK_EQ(info.nioapics, 0);
    CHECK(info.skipped_entries >= 2);
}

TEST(acpi_many_cpus)
{
    build(true);
    uint8_t *m = mem + MADT;
    uint32_t o = 44;
    for (int i = 0; i < ACPI_MAX_CPUS + 4; i++) {
        m[o] = 0; m[o + 1] = 8; m[o + 2] = (uint8_t)i; m[o + 3] = (uint8_t)i; put32(m + o + 4, 1);
        o += 8;
    }
    sdt(MADT, "APIC", o);
    CHECK_EQ(acpi_parse(mem + RSDP, map, NULL, &info), ACPI_OK);
    CHECK_EQ(info.ncpus, ACPI_MAX_CPUS);
    CHECK_EQ(info.skipped_entries, 4);
}

TEST(acpi_rsdp_scan)
{
    build(true);
    static uint8_t area[1024];
    memset(area, 0, sizeof area);
    memcpy(area + 100, "RSD PTR ", 8); /* unaligned: not found */
    CHECK(acpi_scan_rsdp(area, sizeof area) == NULL);
    memcpy(area + 512, mem + RSDP, 36);
    CHECK(acpi_scan_rsdp(area, sizeof area) == area + 512);
    area[520] ^= 1;
    CHECK(acpi_scan_rsdp(area, sizeof area) == NULL);
    uint16_t a, b;
    static const uint8_t trunc[] = { 0x08, '_', 'S', '5', '_', 0x12 };
    CHECK(!acpi_find_s5(trunc, sizeof trunc, &a, &b));
    static const uint8_t ones[] = { 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x02, 0x01, 0x00 };
    CHECK(acpi_find_s5(ones, sizeof ones, &a, &b));
    CHECK_EQ(a, 1);
    CHECK_EQ(b, 0);
}

void run_tests(void)
{
    RUN(acpi_full_parse);
    RUN(acpi_rsdt_only);
    RUN(acpi_rejects_bad_tables);
    RUN(acpi_malformed_madt);
    RUN(acpi_many_cpus);
    RUN(acpi_rsdp_scan);
}
