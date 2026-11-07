/* /dev/input (0.7): PS/2 keyboard scan codes and mouse packets as
 * fixed-size records for the display server. The IRQ handlers only queue
 * bytes (ps2.c); decoding happens here, in the reader's system call. Runs
 * under the BKL; one opener at a time. */
#include "drivers/input_dev.h"
#include "drivers/input.h"
#include "core/event_queue.h"
#include "core/file.h"
#include "core/process.h"
#include "core/sched.h"
#include "core/time.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "lib.h"

static struct {
    struct file *owner;
    struct kbd_scanner kbd;
    struct mouse_decoder mouse;
    uint64_t events;
} I;

static int input_wq;

void input_dev_wake(void)
{
    sched_wake_all(&input_wq);
    file_poll_wake();
}

struct file *input_dev_open(int *err)
{
    if (I.owner) {
        *err = AE_BUSY;
        return NULL;
    }
    struct file *f = file_new(FK_INPUT, AURORA_O_RDWR);
    if (!f) {
        *err = AE_NOMEM;
        return NULL;
    }
    I.owner = f;
    kbd_scanner_init(&I.kbd);
    mouse_decoder_init(&I.mouse);
    if (ps2_mouse_has_wheel())
        I.mouse.size = 4;
    /* Bytes queued before anyone listened are stale. */
    uint8_t b;
    while (byte_ring_pop(ps2_kbd_ring(), &b))
        ;
    while (byte_ring_pop(ps2_mouse_ring(), &b))
        ;
    return f;
}

void input_dev_close(struct file *f)
{
    if (I.owner == f)
        I.owner = NULL;
}

static bool pending(void) { return byte_ring_count(ps2_kbd_ring()) || byte_ring_count(ps2_mouse_ring()); }

/* Decodes queued bytes into at most `cap` records. */
static size_t drain(struct aurora_input_event *out, size_t cap)
{
    size_t n = 0;
    uint64_t now = time_ms();
    uint8_t b;
    while (n < cap && byte_ring_pop(ps2_kbd_ring(), &b)) {
        uint16_t code;
        bool down;
        if (kbd_scan(&I.kbd, b, &code, &down)) {
            memset(&out[n], 0, sizeof out[n]);
            out[n].type = AURORA_INPUT_KEY;
            out[n].flags = down ? AURORA_INPUT_PRESSED : 0;
            out[n].code = code;
            out[n].time_ms = (uint32_t)now;
            n++;
        }
    }
    while (n < cap && byte_ring_pop(ps2_mouse_ring(), &b)) {
        struct mouse_event m;
        if (mouse_decode(&I.mouse, b, now, &m)) {
            memset(&out[n], 0, sizeof out[n]);
            out[n].type = AURORA_INPUT_MOUSE;
            out[n].dx = m.dx;
            out[n].dy = m.dy;
            out[n].wheel = m.wheel;
            out[n].buttons = m.buttons;
            out[n].time_ms = (uint32_t)now;
            n++;
        }
    }
    return n;
}

long input_dev_read(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    if (I.owner != f)
        return AE_BADF;
    size_t cap = (size_t)(len / sizeof(struct aurora_input_event));
    if (cap == 0)
        return AE_INVAL;
    if (cap > 64)
        cap = 64;
    struct aurora_input_event ev[64];
    for (;;) {
        size_t n = drain(ev, cap);
        if (n) {
            I.events += n;
            int r = copy_to_user(p, ubuf, ev, n * sizeof ev[0]);
            return r ? r : (long)(n * sizeof ev[0]);
        }
        if (f->flags & AURORA_O_NONBLOCK)
            return AE_AGAIN;
        if (task_interrupted(sched_current()))
            return AE_INTR;
        uint64_t fl = irq_save();
        if (!pending())
            sched_wait_on(&input_wq, 0);
        irq_restore(fl);
    }
}

long input_dev_ioctl(struct process *p, struct file *f, uint64_t req, uint64_t arg)
{
    (void)p;
    if (I.owner != f)
        return AE_BADF;
    switch (req) {
    case AURORA_INPUT_SET_LEDS:
        ps2_set_led_bits((uint8_t)(arg & 7));
        return 0;
    case AURORA_INPUT_HAS_WHEEL:
        return ps2_mouse_has_wheel() ? 1 : 0;
    default:
        return AE_NOTTY;
    }
}

int input_dev_poll(struct file *f, int events)
{
    if (I.owner != f)
        return AURORA_POLLERR;
    return (pending() ? AURORA_POLLIN : 0) & events;
}
