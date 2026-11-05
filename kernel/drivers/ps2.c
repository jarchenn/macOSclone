/* i8042 PS/2 controller, keyboard (port 1) and mouse (port 2).
 *
 * Initialisation runs with interrupts disabled and polls the controller with
 * bounded timeouts, so command acknowledgements (0xFA) and self-test results
 * (0xAA) are consumed here and never enter the input rings. After init, IRQ1
 * and IRQ12 read one byte at a time, use status bit 5 (AUX) to separate mouse
 * bytes from keyboard bytes, and push them to bounded rings.
 */
#include "drivers/input.h"
#include "drivers/pic.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "core/log.h"

#define DATA 0x60
#define STATUS 0x64
#define CMD 0x64
#define ST_OUT_FULL 0x01
#define ST_IN_FULL 0x02
#define ST_AUX 0x20

#define TIMEOUT_POLLS 200000

static struct byte_ring kbd_ring, mouse_ring;
static bool have_mouse;
static bool have_kbd;

struct byte_ring *ps2_kbd_ring(void) { return &kbd_ring; }
struct byte_ring *ps2_mouse_ring(void) { return &mouse_ring; }
bool ps2_has_mouse(void) { return have_mouse; }

static bool wait_write(void)
{
    for (int i = 0; i < TIMEOUT_POLLS; i++) {
        if (!(inb(STATUS) & ST_IN_FULL))
            return true;
        io_wait();
    }
    return false;
}

static bool wait_read(void)
{
    for (int i = 0; i < TIMEOUT_POLLS; i++) {
        if (inb(STATUS) & ST_OUT_FULL)
            return true;
        io_wait();
    }
    return false;
}

static bool ctrl_cmd(uint8_t c)
{
    if (!wait_write())
        return false;
    outb(CMD, c);
    return true;
}

static bool ctrl_write_data(uint8_t v)
{
    if (!wait_write())
        return false;
    outb(DATA, v);
    return true;
}

static int read_byte(void)
{
    if (!wait_read())
        return -1;
    return inb(DATA);
}

static void flush(void)
{
    for (int i = 0; i < 64 && (inb(STATUS) & ST_OUT_FULL); i++) {
        (void)inb(DATA);
        io_wait();
    }
}

/* Send a byte to a device and wait for ACK (0xFA), retrying on 0xFE. */
static bool dev_send(bool aux, uint8_t v)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (aux && !ctrl_cmd(0xD4))
            return false;
        if (!ctrl_write_data(v))
            return false;
        for (int k = 0; k < 4; k++) {
            int r = read_byte();
            if (r < 0)
                return false;
            if (r == 0xFA)
                return true;
            if (r == 0xFE)
                break; /* resend */
            /* anything else: stray byte, keep waiting for the ACK */
        }
    }
    return false;
}

static void kbd_irq(void)
{
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(STATUS);
        if (!(st & ST_OUT_FULL))
            break;
        uint8_t b = inb(DATA);
        byte_ring_push((st & ST_AUX) ? &mouse_ring : &kbd_ring, b);
    }
}

static void mouse_irq(void) { kbd_irq(); /* same routing logic */ }

bool ps2_init(void)
{
    /* Disable both ports while configuring. */
    ctrl_cmd(0xAD);
    ctrl_cmd(0xA7);
    flush();

    if (!ctrl_cmd(0x20))
        goto fail;
    int cfg = read_byte();
    if (cfg < 0)
        goto fail;
    /* IRQs off, translation on (keyboard set 2 -> set 1), clocks enabled. */
    cfg &= ~0x03;
    cfg |= 0x40;
    cfg &= ~0x30;
    ctrl_cmd(0x60);
    ctrl_write_data((uint8_t)cfg);

    /* Controller self-test (may reset config on some hardware; rewrite it). */
    ctrl_cmd(0xAA);
    int st = read_byte();
    if (st != 0x55)
        klog("ps2: controller self-test returned %x", st);
    ctrl_cmd(0x60);
    ctrl_write_data((uint8_t)cfg);

    /* Detect the second (aux) port. */
    ctrl_cmd(0xA8);
    ctrl_cmd(0x20);
    int cfg2 = read_byte();
    bool dual = cfg2 >= 0 && !(cfg2 & 0x20);
    ctrl_cmd(0xA7);

    ctrl_cmd(0xAB);
    have_kbd = read_byte() == 0x00;
    if (dual) {
        ctrl_cmd(0xA9);
        dual = read_byte() == 0x00;
    }

    ctrl_cmd(0xAE);
    if (dual)
        ctrl_cmd(0xA8);
    flush();

    if (have_kbd) {
        bool ok = dev_send(false, 0xFF); /* reset */
        if (ok) {
            int r = read_byte();         /* 0xAA self-test passed */
            if (r != 0xAA)
                klog("ps2: keyboard reset returned %x", r);
        }
        /* Explicitly select scan code set 2 (translated to set 1). */
        if (!dev_send(false, 0xF0) || !dev_send(false, 0x02))
            klog("ps2: keyboard did not accept scan code set 2; relying on default");
        if (!dev_send(false, 0xF4))
            klog("ps2: keyboard enable scanning not acknowledged");
        flush();
    }
    if (dual) {
        if (dev_send(true, 0xFF)) {
            int r = read_byte(); /* 0xAA */
            int id = read_byte(); /* device id 0x00 */
            if (r != 0xAA || id != 0x00)
                klog("ps2: mouse reset returned %x id %x", r, id);
        }
        have_mouse = dev_send(true, 0xF6) && dev_send(true, 0xF4); /* defaults, enable reporting */
        flush();
    }

    /* Enable IRQs for present devices. */
    ctrl_cmd(0x20);
    cfg = read_byte();
    if (cfg < 0)
        goto fail;
    cfg |= 0x01 | 0x40;
    if (have_mouse)
        cfg |= 0x02;
    ctrl_cmd(0x60);
    ctrl_write_data((uint8_t)cfg);

    irq_install(IRQ_KEYBOARD, kbd_irq);
    pic_unmask(IRQ_KEYBOARD);
    if (have_mouse) {
        irq_install(IRQ_MOUSE, mouse_irq);
        pic_unmask(IRQ_MOUSE);
    }
    klog("ps2: keyboard %s, mouse %s (3-byte packets)", have_kbd ? "ok" : "missing", have_mouse ? "ok" : "missing");
    return have_kbd;

fail:
    klog("ps2: controller not responding");
    return false;
}
