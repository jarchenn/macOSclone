/* 16550 UART on COM1 (0x3F8), 115200 8N1, polled transmit with bounded wait. */
#include "drivers/serial.h"
#include "arch/x86_64/io.h"

#define COM1 0x3F8

static bool serial_ok;

bool serial_init(void)
{
    outb(COM1 + 1, 0x00); /* disable UART interrupts */
    outb(COM1 + 3, 0x80); /* DLAB on */
    outb(COM1 + 0, 0x01); /* divisor 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7); /* enable + clear FIFOs, 14-byte threshold */
    outb(COM1 + 4, 0x1E); /* loopback mode for self test */
    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE) {
        serial_ok = false;
        return false;
    }
    outb(COM1 + 4, 0x0F); /* normal operation: DTR, RTS, OUT1, OUT2 */
    serial_ok = true;
    return true;
}

bool serial_present(void) { return serial_ok; }

void serial_putc(char c)
{
    if (!serial_ok)
        return;
    /* Bounded wait for the transmit holding register; never hang forever. */
    for (int i = 0; i < 100000; i++) {
        if (inb(COM1 + 5) & 0x20) {
            outb(COM1, (uint8_t)c);
            return;
        }
        cpu_pause();
    }
}

void serial_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n')
            serial_putc('\r');
        serial_putc(s[i]);
    }
}
