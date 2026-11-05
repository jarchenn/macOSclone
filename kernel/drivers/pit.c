#include "core/time.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/pic.h"

static uint64_t ticks; /* written only by IRQ0; 64-bit aligned loads are atomic */
static uint32_t divisor;

static void pit_irq(void) { __atomic_store_n(&ticks, ticks + 1, __ATOMIC_RELEASE); }

void pit_init(void)
{
    divisor = (PIT_INPUT_HZ + PIT_TARGET_HZ / 2) / PIT_TARGET_HZ; /* 4773 -> 249.98 Hz */
    outb(0x43, 0x34); /* channel 0, lo/hi byte, mode 2 (rate generator), binary */
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)(divisor >> 8));
    irq_install(IRQ_TIMER, pit_irq);
    pic_unmask(IRQ_TIMER);
}

uint32_t pit_divisor(void) { return divisor; }
uint64_t time_ticks(void) { return __atomic_load_n(&ticks, __ATOMIC_ACQUIRE); }

uint64_t time_ms(void)
{
    /* ms = ticks * divisor / 1193.182 ; overflows only after ~490 years */
    return time_ticks() * (uint64_t)divisor * 1000ull / PIT_INPUT_HZ;
}

void time_sleep_ms(uint32_t ms)
{
    uint64_t end = time_ms() + ms;
    while (time_ms() < end)
        cpu_hlt();
}
