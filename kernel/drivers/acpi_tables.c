/* Portable ACPI table parser (see drivers/acpi_tables.h). */
#include "drivers/acpi_tables.h"
#include "lib.h"

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

bool acpi_checksum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++)
        s = (uint8_t)(s + p[i]);
    return s == 0;
}

/* Maps a whole SDT: header first (to learn the length), then all of it. */
static const uint8_t *map_sdt(acpi_map_fn map, void *ctx, uint64_t phys, uint32_t *len_out)
{
    if (!phys)
        return NULL;
    const uint8_t *h = map(ctx, phys, 36);
    if (!h)
        return NULL;
    uint32_t len = rd32(h + 4);
    if (len < 36 || len > (4u << 20))
        return NULL;
    const uint8_t *t = map(ctx, phys, len);
    if (!t || !acpi_checksum_ok(t, len))
        return NULL;
    *len_out = len;
    return t;
}

const uint8_t *acpi_scan_rsdp(const uint8_t *area, uint32_t len)
{
    for (uint32_t off = 0; off + 20 <= len; off += 16)
        if (!memcmp(area + off, "RSD PTR ", 8) && acpi_checksum_ok(area + off, 20))
            return area + off;
    return NULL;
}

bool acpi_find_s5(const uint8_t *aml, uint32_t len, uint16_t *a, uint16_t *b)
{
    for (uint32_t i = 0; i + 4 < len; i++) {
        if (memcmp(aml + i, "_S5_", 4))
            continue;
        /* NameOp (0x08), optionally followed by the root prefix '\'. */
        if (!(i >= 1 && (aml[i - 1] == 0x08 || (i >= 2 && aml[i - 2] == 0x08 && aml[i - 1] == '\\'))))
            continue;
        uint32_t o = i + 4;
        if (o >= len || aml[o] != 0x12) /* PackageOp */
            continue;
        o++;
        if (o >= len)
            return false;
        o += (uint32_t)((aml[o] >> 6) & 3) + 1; /* PkgLength */
        o++;                                     /* NumElements */
        uint16_t vals[2];
        for (int k = 0; k < 2; k++) {
            if (o >= len)
                return false;
            uint8_t v = aml[o];
            if (v == 0x0A) { /* BytePrefix */
                if (o + 1 >= len)
                    return false;
                vals[k] = aml[o + 1];
                o += 2;
            } else if (v == 0x00 || v == 0x01) { /* ZeroOp / OneOp */
                vals[k] = v;
                o++;
            } else {
                return false;
            }
        }
        *a = vals[0];
        *b = vals[1];
        return true;
    }
    return false;
}

static void parse_fadt(const uint8_t *t, uint32_t len, acpi_map_fn map, void *ctx, struct acpi_info *o)
{
    if (len < 116)
        return;
    o->have_fadt = true;
    o->dsdt = rd32(t + 40);
    o->sci_irq = rd16(t + 46);
    o->smi_cmd = rd32(t + 48);
    o->acpi_enable = t[52];
    o->pm1a_evt = rd32(t + 56);
    o->pm1b_evt = rd32(t + 60);
    o->pm1a_cnt = rd32(t + 64);
    o->pm1b_cnt = rd32(t + 68);
    o->pm1_evt_len = t[88];
    o->fadt_flags = rd32(t + 112);
    if (len >= 129 && (o->fadt_flags & ACPI_FADT_RESET_REG_SUP)) {
        const uint8_t *g = t + 116;
        o->reset_reg.space = g[0];
        o->reset_reg.bit_width = g[1];
        o->reset_reg.bit_offset = g[2];
        o->reset_reg.access = g[3];
        o->reset_reg.address = rd64(g + 4);
        o->reset_value = t[128];
        o->have_reset = o->reset_reg.address && (o->reset_reg.space == 0 || o->reset_reg.space == 1);
    }
    if (len >= 148 && rd64(t + 140))
        o->dsdt = rd64(t + 140);
    uint32_t dlen;
    const uint8_t *d = map_sdt(map, ctx, o->dsdt, &dlen);
    if (d && !memcmp(d, "DSDT", 4))
        o->have_s5 = acpi_find_s5(d + 36, dlen - 36, &o->slp_typa, &o->slp_typb);
}

static void parse_madt(const uint8_t *t, uint32_t len, struct acpi_info *o)
{
    if (len < 44)
        return;
    o->have_madt = true;
    o->lapic_address = rd32(t + 36);
    o->pcat_compat = rd32(t + 40) & 1;
    uint32_t off = 44;
    while (off + 2 <= len) {
        uint8_t type = t[off], elen = t[off + 1];
        if (elen < 2 || off + elen > len) {
            o->skipped_entries++;
            break; /* a zero or overlong length would loop or overrun */
        }
        const uint8_t *e = t + off;
        switch (type) {
        case 0: /* processor local APIC */
            if (elen < 8) {
                o->skipped_entries++;
                break;
            }
            if ((rd32(e + 4) & 3) && o->ncpus < ACPI_MAX_CPUS) {
                struct acpi_cpu *c = &o->cpus[o->ncpus++];
                c->uid = e[2];
                c->apic_id = e[3];
                c->enabled = rd32(e + 4) & 1;
            } else if (rd32(e + 4) & 3) {
                o->skipped_entries++; /* more CPUs than supported */
            }
            break;
        case 1: /* I/O APIC */
            if (elen < 12 || o->nioapics >= ACPI_MAX_IOAPICS) {
                o->skipped_entries++;
                break;
            }
            o->ioapics[o->nioapics].id = e[2];
            o->ioapics[o->nioapics].address = rd32(e + 4);
            o->ioapics[o->nioapics].gsi_base = rd32(e + 8);
            o->nioapics++;
            break;
        case 2: /* interrupt source override */
            if (elen < 10 || e[2] != 0 || o->nisos >= ACPI_MAX_ISOS) {
                o->skipped_entries++;
                break;
            }
            o->isos[o->nisos].bus_irq = e[3];
            o->isos[o->nisos].gsi = rd32(e + 4);
            o->isos[o->nisos].flags = rd16(e + 8);
            o->nisos++;
            break;
        case 4: /* local APIC NMI */
            if (elen < 6) {
                o->skipped_entries++;
                break;
            }
            if (e[2] == 0xFF || o->nmi_lint == 0xFF)
                o->nmi_lint = e[5];
            break;
        case 5: /* local APIC address override */
            if (elen >= 12 && rd64(e + 4) < (1ull << 32))
                o->lapic_address = (uint32_t)rd64(e + 4);
            else
                o->skipped_entries++;
            break;
        default:
            o->skipped_entries++; /* x2APIC and others: not supported */
            break;
        }
        off += elen;
    }
}

int acpi_parse(const uint8_t *rsdp, acpi_map_fn map, void *ctx, struct acpi_info *o)
{
    memset(o, 0, sizeof *o);
    o->nmi_lint = 0xFF;
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8) || !acpi_checksum_ok(rsdp, 20))
        return ACPI_E_RSDP;
    o->revision = rsdp[15];
    uint64_t root_phys = rd32(rsdp + 16);
    if (o->revision >= 2) {
        uint32_t xlen = rd32(rsdp + 20);
        if (xlen >= 36 && acpi_checksum_ok(rsdp, xlen > 64 ? 64 : xlen) && rd64(rsdp + 24)) {
            root_phys = rd64(rsdp + 24);
            o->xsdt = true;
        }
    }
    uint32_t rlen;
    const uint8_t *root = map_sdt(map, ctx, root_phys, &rlen);
    if (!root || memcmp(root, o->xsdt ? "XSDT" : "RSDT", 4))
        return ACPI_E_ROOT;
    uint32_t esz = o->xsdt ? 8 : 4;
    for (uint32_t off = 36; off + esz <= rlen; off += esz) {
        uint64_t p = o->xsdt ? rd64(root + off) : rd32(root + off);
        uint32_t len;
        const uint8_t *t = map_sdt(map, ctx, p, &len);
        if (!t)
            continue; /* bad checksum or unreadable: ignore that table */
        if (!memcmp(t, "FACP", 4) && !o->have_fadt)
            parse_fadt(t, len, map, ctx, o);
        else if (!memcmp(t, "APIC", 4) && !o->have_madt)
            parse_madt(t, len, o);
        else if (!memcmp(t, "HPET", 4) && len >= 56 && !o->have_hpet) {
            o->have_hpet = t[40] == 0; /* memory space */
            o->hpet_address = rd64(t + 44);
        }
    }
    return o->have_fadt ? ACPI_OK : ACPI_E_NOFADT;
}

uint32_t acpi_isa_gsi(const struct acpi_info *a, uint8_t irq, uint16_t *flags)
{
    for (int i = 0; i < a->nisos; i++)
        if (a->isos[i].bus_irq == irq) {
            if (flags)
                *flags = a->isos[i].flags;
            return a->isos[i].gsi;
        }
    if (flags)
        *flags = 0; /* bus default: ISA is edge-triggered, active high */
    return irq;
}
