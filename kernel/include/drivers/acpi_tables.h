#ifndef AURORA_ACPI_TABLES_H
#define AURORA_ACPI_TABLES_H
#include "types.h"

/* Portable ACPI table parser (host-tested). It never trusts the firmware:
 * every table is length- and checksum-checked before use, and every MADT
 * entry is bounds-checked against its table. Physical addresses are turned
 * into readable memory through the caller's map() callback, which returns
 * NULL when the range is not accessible. */

#define ACPI_MAX_CPUS 16
#define ACPI_MAX_IOAPICS 4
#define ACPI_MAX_ISOS 16

struct acpi_gas { /* Generic Address Structure */
    uint8_t space; /* 0 = memory, 1 = I/O port */
    uint8_t bit_width, bit_offset, access;
    uint64_t address;
};

struct acpi_cpu {
    uint8_t uid, apic_id;
    bool enabled;
};

struct acpi_ioapic {
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
};

struct acpi_iso { /* interrupt source override */
    uint8_t bus_irq;
    uint32_t gsi;
    uint16_t flags; /* MPS INTI flags: bits 0-1 polarity, 2-3 trigger */
};

struct acpi_info {
    uint8_t revision; /* RSDP revision */
    bool xsdt;
    /* FADT */
    bool have_fadt;
    uint16_t sci_irq;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint32_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt;
    uint8_t pm1_evt_len;
    uint32_t fadt_flags;
    bool have_reset;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint64_t dsdt;
    bool have_s5;
    uint16_t slp_typa, slp_typb;
    /* MADT */
    bool have_madt;
    uint32_t lapic_address;
    bool pcat_compat; /* dual 8259s present */
    int ncpus;        /* enabled or online-capable entries recorded */
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    int nioapics;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    int nisos;
    struct acpi_iso isos[ACPI_MAX_ISOS];
    uint8_t nmi_lint; /* LAPIC LINT used for NMI (0xFF = none listed) */
    uint32_t skipped_entries; /* malformed or unsupported MADT entries */
    /* HPET */
    bool have_hpet;
    uint64_t hpet_address;
};

enum {
    ACPI_OK = 0,
    ACPI_E_RSDP = -1,  /* bad signature or checksum */
    ACPI_E_ROOT = -2,  /* RSDT/XSDT unreadable or bad checksum */
    ACPI_E_NOFADT = -3,
};

#define ACPI_FADT_RESET_REG_SUP (1u << 10)

typedef const uint8_t *(*acpi_map_fn)(void *ctx, uint64_t phys, uint32_t len);

int acpi_parse(const uint8_t *rsdp, acpi_map_fn map, void *ctx, struct acpi_info *out);
bool acpi_checksum_ok(const uint8_t *p, uint32_t len);
/* Scans a DSDT body for the \_S5 package (no AML interpreter). */
bool acpi_find_s5(const uint8_t *aml, uint32_t len, uint16_t *a, uint16_t *b);
/* Finds "RSD PTR " with a valid checksum on a 16-byte boundary. */
const uint8_t *acpi_scan_rsdp(const uint8_t *area, uint32_t len);
/* ISA IRQ -> GSI and flags, applying overrides (identity if none). */
uint32_t acpi_isa_gsi(const struct acpi_info *a, uint8_t irq, uint16_t *flags);

#endif
