/* Local APIC and I/O APIC (see drivers/apic.h). */
#include "drivers/apic.h"
#include "drivers/acpi.h"
#include "drivers/acpi_tables.h"
#include "drivers/pic.h"
#include "core/log.h"
#include "core/time.h"
#include "memory/virtual.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "core/sched.h"
#include "lib.h"

#define LAPIC_ID 0x20
#define LAPIC_VER 0x30
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xB0
#define LAPIC_SVR 0xF0
#define LAPIC_ESR 0x280
#define LAPIC_ICR_HI 0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERROR 0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define LVT_MASKED (1u << 16)
#define LVT_PERIODIC (1u << 17)
#define ICR_PENDING (1u << 12)

static volatile uint32_t *lapic;
static bool active;
static uint32_t timer_count; /* initial count for 250 Hz at divide-by-16 */

struct ioapic {
    volatile uint32_t *mmio;
    uint32_t gsi_base, count;
};
static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static int nioapic;
static uint32_t bsp_apic_id;

bool apic_active(void) { return active; }
uint32_t apic_timer_initial_count(void) { return timer_count; }
uint32_t lapic_read(uint32_t reg) { return lapic[reg / 4]; }
void lapic_write(uint32_t reg, uint32_t v) { lapic[reg / 4] = v; }
void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }
uint32_t lapic_id(void) { return lapic ? lapic_read(LAPIC_ID) >> 24 : 0; }

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg)
{
    io->mmio[0] = reg;
    return io->mmio[4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t v)
{
    io->mmio[0] = reg;
    io->mmio[4] = v;
}

static void lapic_setup_local(void)
{
    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED); /* the PIC is no longer used */
    const struct acpi_info *a = acpi_info();
    uint8_t nmi = a ? a->nmi_lint : 0xFF;
    lapic_write(LAPIC_LVT_LINT1, nmi == 1 ? (4u << 8) : LVT_MASKED); /* NMI delivery mode */
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_SVR, 0x100u | APIC_SPURIOUS_VECTOR); /* software enable */
    lapic_eoi();
}

bool ioapic_route_isa(unsigned irq, bool masked)
{
    const struct acpi_info *a = acpi_info();
    if (!active || !a || irq >= 16)
        return false;
    uint16_t flags;
    uint32_t gsi = acpi_isa_gsi(a, (uint8_t)irq, &flags);
    for (int i = 0; i < nioapic; i++) {
        struct ioapic *io = &ioapics[i];
        if (gsi < io->gsi_base || gsi >= io->gsi_base + io->count)
            continue;
        uint32_t pin = gsi - io->gsi_base;
        uint32_t lo = IRQ_BASE + irq; /* fixed delivery, physical destination */
        if ((flags & 3) == 3)
            lo |= 1u << 13; /* active low */
        if (((flags >> 2) & 3) == 3)
            lo |= 1u << 15; /* level triggered */
        if (masked)
            lo |= LVT_MASKED;
        ioapic_write(io, 0x10 + 2 * pin + 1, bsp_apic_id << 24);
        ioapic_write(io, 0x10 + 2 * pin, lo);
        return true;
    }
    return false;
}

bool apic_init(void)
{
    const struct acpi_info *a = acpi_info();
    if (!a || !a->have_madt || !a->nioapics || !g_cpu.apic) {
        klog("apic: no usable MADT/IOAPIC; staying on the 8259 PIC");
        return false;
    }
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (base & (1u << 10)) {
        klog("apic: x2APIC mode is not supported; staying on the 8259 PIC");
        return false;
    }
    if ((base & 0xFFFFFF000ull) != a->lapic_address)
        klog("apic: note: MADT LAPIC %x differs from IA32_APIC_BASE %llx; using the MSR", a->lapic_address,
             (unsigned long long)(base & 0xFFFFFF000ull));
    lapic = g_vmm.lapic;
    if (!lapic)
        lapic = vmm_map_mmio(base & 0xFFFFFF000ull, 4096, true);
    if (!lapic)
        return false;
    for (int i = 0; i < a->nioapics && nioapic < ACPI_MAX_IOAPICS; i++) {
        volatile uint32_t *m = vmm_map_mmio(a->ioapics[i].address, 32, true);
        if (!m)
            continue;
        struct ioapic *io = &ioapics[nioapic];
        io->mmio = m;
        io->gsi_base = a->ioapics[i].gsi_base;
        io->count = ((ioapic_read(io, 1) >> 16) & 0xFF) + 1;
        for (uint32_t p = 0; p < io->count; p++)
            ioapic_write(io, 0x10 + 2 * p, LVT_MASKED);
        nioapic++;
    }
    if (!nioapic)
        return false;

    uint64_t f = irq_save();
    bsp_apic_id = lapic_read(LAPIC_ID) >> 24;
    lapic_setup_local();
    active = true;
    /* Move every IRQ that is enabled on the PIC over to the IOAPIC, then
     * mask the PIC completely. */
    uint16_t enabled = pic_enabled_mask();
    for (unsigned irq = 0; irq < 16; irq++)
        if ((enabled & (1u << irq)) && irq != IRQ_CASCADE)
            ioapic_route_isa(irq, false);
    pic_mask_all();
    irq_restore(f);
    klog("apic: LAPIC id %u (version %x), %d IOAPIC(s) with %u pins; PIC masked, ISA IRQs via IOAPIC",
         bsp_apic_id, lapic_read(LAPIC_VER) & 0xFF, nioapic, ioapics[0].count);
    return true;
}

void apic_init_ap(void) { lapic_setup_local(); }

bool apic_timer_start(void)
{
    if (!active)
        return false;
    /* Measure the LAPIC timer against PIT ticks (the PIT interrupt keeps
     * arriving on the boot CPU, so APs can measure too). */
    lapic_write(LAPIC_TIMER_DIV, 0x3); /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    uint64_t t = time_ticks();
    while (time_ticks() == t)
        cpu_pause();
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    uint64_t start = time_ticks();
    while (time_ticks() < start + 10)
        cpu_pause();
    uint32_t elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_CUR);
    uint32_t per_tick = elapsed / 10;
    if (per_tick < 100) {
        klog("apic: LAPIC timer calibration failed (%u counts in 10 PIT ticks)", elapsed);
        lapic_write(LAPIC_TIMER_INIT, 0);
        return false;
    }
    if (!timer_count)
        timer_count = per_tick;
    lapic_write(LAPIC_LVT_TIMER, APIC_TIMER_VECTOR | LVT_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, per_tick);
    klog("apic: cpu apic id %u timer %u counts per 4 ms tick (div 16)", lapic_id(), per_tick);
    return true;
}

static bool icr_wait(void)
{
    for (int i = 0; i < 1000000; i++) {
        if (!(lapic_read(LAPIC_ICR_LO) & ICR_PENDING))
            return true;
        cpu_pause();
    }
    return false;
}

static bool send_icr(uint32_t dest, uint32_t lo)
{
    uint64_t f = irq_save();
    bool ok = icr_wait();
    if (ok) {
        lapic_write(LAPIC_ICR_HI, dest << 24);
        lapic_write(LAPIC_ICR_LO, lo);
        ok = icr_wait();
    }
    irq_restore(f);
    return ok;
}

bool lapic_send_ipi(uint32_t dest, uint8_t vector) { return send_icr(dest, vector); }
bool lapic_send_init(uint32_t dest) { return send_icr(dest, (5u << 8) | (1u << 14)); } /* INIT, assert */
bool lapic_send_sipi(uint32_t dest, uint8_t page) { return send_icr(dest, (6u << 8) | page); }
void lapic_broadcast_ipi(uint8_t vector) { send_icr(0, vector | (3u << 18)); } /* all excluding self */

void apic_timer_tick(void) { sched_tick(); }
