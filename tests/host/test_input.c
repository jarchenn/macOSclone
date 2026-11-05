/* Keyboard scan-code decoding, mouse packet parsing, event queues. */
#include "harness.h"
#include "drivers/input.h"
#include "core/event_queue.h"

const char *t_suite = "input";

static int feed(struct kbd_decoder *d, const uint8_t *b, int n, struct key_event *out, int max)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        struct key_event e;
        if (kbd_decode(d, b[i], &e) && k < max)
            out[k++] = e;
    }
    return k;
}

TEST(plain_and_shift)
{
    struct kbd_decoder d;
    kbd_decoder_init(&d);
    struct key_event ev[8];
    uint8_t seq[] = { 0x1E, 0x9E, 0x2A, 0x1E, 0x9E, 0xAA, 0x1E };
    int n = feed(&d, seq, sizeof seq, ev, 8);
    CHECK_EQ(n, 7);
    CHECK_EQ(ev[0].ascii, 'a');
    CHECK(ev[0].pressed);
    CHECK(!ev[1].pressed);
    CHECK_EQ(ev[2].key, KEY_LSHIFT);
    CHECK_EQ(ev[3].ascii, 'A');
    CHECK(ev[3].mods & MOD_SHIFT);
    CHECK_EQ(ev[5].key, KEY_LSHIFT); /* 0xAA is the LShift break code, not a self-test byte */
    CHECK(!ev[5].pressed);
    CHECK_EQ(ev[6].ascii, 'a');      /* shift released: no stuck modifier */
    CHECK(!(ev[6].mods & MOD_SHIFT));
}

TEST(shifted_symbols)
{
    struct kbd_decoder d;
    kbd_decoder_init(&d);
    struct key_event ev[4];
    uint8_t seq[] = { 0x36, 0x02, 0x28, 0xB6, 0x28 };
    int n = feed(&d, seq, sizeof seq, ev, 4);
    CHECK_EQ(n, 4);
    CHECK_EQ(ev[1].ascii, '!');
    CHECK_EQ(ev[2].ascii, '"');
    CHECK_EQ(ev[3].key, KEY_RSHIFT);
}

TEST(caps_lock_toggle_once_per_press)
{
    struct kbd_decoder d;
    kbd_decoder_init(&d);
    struct key_event e;
    /* press + typematic repeat + release: toggles exactly once */
    kbd_decode(&d, 0x3A, &e);
    kbd_decode(&d, 0x3A, &e);
    kbd_decode(&d, 0x3A, &e);
    kbd_decode(&d, 0xBA, &e);
    CHECK(d.caps);
    kbd_decode(&d, 0x10, &e);
    CHECK_EQ(e.ascii, 'Q');
    kbd_decode(&d, 0x02, &e);
    CHECK_EQ(e.ascii, '1'); /* caps does not shift digits */
    kbd_decode(&d, 0x2A, &e);
    kbd_decode(&d, 0x10, &e);
    CHECK_EQ(e.ascii, 'q'); /* shift inverts caps for letters */
    kbd_decode(&d, 0xAA, &e);
    kbd_decode(&d, 0x3A, &e);
    kbd_decode(&d, 0xBA, &e);
    CHECK(!d.caps);
}

TEST(extended_keys)
{
    struct kbd_decoder d;
    kbd_decoder_init(&d);
    struct key_event e;
    CHECK(!kbd_decode(&d, 0xE0, &e));
    CHECK(kbd_decode(&d, 0x48, &e));
    CHECK_EQ(e.key, KEY_UP);
    CHECK(e.pressed);
    kbd_decode(&d, 0xE0, &e);
    kbd_decode(&d, 0xC8, &e);
    CHECK_EQ(e.key, KEY_UP);
    CHECK(!e.pressed);
    kbd_decode(&d, 0xE0, &e);
    kbd_decode(&d, 0x4B, &e);
    CHECK_EQ(e.key, KEY_LEFT);
    /* Right ctrl via E0 1D, released via E0 9D */
    kbd_decode(&d, 0xE0, &e);
    kbd_decode(&d, 0x1D, &e);
    CHECK(kbd_mods(&d) & MOD_CTRL);
    kbd_decode(&d, 0xE0, &e);
    kbd_decode(&d, 0x9D, &e);
    CHECK(!(kbd_mods(&d) & MOD_CTRL));
    /* Fake shift around PrtSc must not set shift */
    CHECK(!kbd_decode(&d, 0xE0, &e));
    CHECK(!kbd_decode(&d, 0x2A, &e));
    CHECK(!(kbd_mods(&d) & MOD_SHIFT));
    /* Keypad 4 without E0 is the numpad left arrow */
    kbd_decode(&d, 0x4B, &e);
    CHECK_EQ(e.key, KEY_LEFT);
}

TEST(protocol_bytes_ignored)
{
    struct kbd_decoder d;
    kbd_decoder_init(&d);
    struct key_event e;
    CHECK(!kbd_decode(&d, 0xFA, &e));
    CHECK(!kbd_decode(&d, 0xFE, &e));
    CHECK(!kbd_decode(&d, 0xEE, &e));
    CHECK(!kbd_decode(&d, 0x00, &e));
    CHECK(!kbd_decode(&d, 0xFF, &e));
    CHECK_EQ(d.ignored, 5);
    /* Pause: E1 1D 45 E1 9D C5 produces nothing and leaves state clean */
    uint8_t pause[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };
    for (unsigned i = 0; i < sizeof pause; i++)
        CHECK(!kbd_decode(&d, pause[i], &e));
    CHECK(kbd_decode(&d, 0x1C, &e));
    CHECK_EQ(e.key, KEY_ENTER);
    CHECK_EQ(e.ascii, '\n');
    CHECK_EQ(kbd_mods(&d), 0);
}

TEST(mouse_packets)
{
    struct mouse_decoder m;
    mouse_decoder_init(&m);
    struct mouse_event e;
    /* dx = -5 (sign bit), dy = +3 (up) -> screen dy = -3, left button */
    CHECK(!mouse_decode(&m, 0x19, 0, &e));
    CHECK(!mouse_decode(&m, 0xFB, 0, &e));
    CHECK(mouse_decode(&m, 0x03, 0, &e));
    CHECK_EQ(e.dx, -5);
    CHECK_EQ(e.dy, -3);
    CHECK_EQ(e.buttons, 1);
    /* dy negative (down) -> positive screen dy */
    mouse_decode(&m, 0x28, 1, &e);
    mouse_decode(&m, 0x00, 1, &e);
    CHECK(mouse_decode(&m, 0xF6, 1, &e));
    CHECK_EQ(e.dy, 10);
    CHECK_EQ(e.dx, 0);
    /* extreme values */
    mouse_decode(&m, 0x18, 2, &e);
    mouse_decode(&m, 0x00, 2, &e);
    CHECK(mouse_decode(&m, 0x00, 2, &e));
    CHECK_EQ(e.dx, -256);
}

TEST(mouse_resync_and_overflow)
{
    struct mouse_decoder m;
    mouse_decoder_init(&m);
    struct mouse_event e;
    /* garbage first byte without bit 3 is skipped */
    CHECK(!mouse_decode(&m, 0x05, 0, &e));
    CHECK_EQ(m.resyncs, 1);
    CHECK(!mouse_decode(&m, 0x08, 0, &e));
    CHECK(!mouse_decode(&m, 0x10, 0, &e));
    CHECK(mouse_decode(&m, 0x00, 0, &e));
    CHECK_EQ(e.dx, 16);
    /* overflow bit: motion discarded, buttons kept */
    mouse_decode(&m, 0x49, 5, &e);
    mouse_decode(&m, 0xFF, 5, &e);
    CHECK(mouse_decode(&m, 0x7F, 5, &e));
    CHECK_EQ(e.dx, 0);
    CHECK_EQ(e.dy, 0);
    CHECK_EQ(e.buttons, 1);
    CHECK_EQ(m.overflows, 1);
    /* stale partial packet (>50 ms gap) is dropped */
    mouse_decode(&m, 0x08, 100, &e);
    mouse_decode(&m, 0x01, 100, &e);
    CHECK(!mouse_decode(&m, 0x08, 200, &e)); /* treated as a new first byte */
    CHECK(!mouse_decode(&m, 0x02, 200, &e));
    CHECK(mouse_decode(&m, 0x00, 200, &e));
    CHECK_EQ(e.dx, 2);
}

TEST(byte_ring_wrap_and_overflow)
{
    static struct byte_ring r;
    memset(&r, 0, sizeof r);
    uint8_t b;
    CHECK(!byte_ring_pop(&r, &b));
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < BYTE_RING_CAP; i++)
            CHECK(byte_ring_push(&r, (uint8_t)i));
        CHECK(!byte_ring_push(&r, 0xAA)); /* full: new byte dropped */
        CHECK_EQ(byte_ring_count(&r), BYTE_RING_CAP);
        for (int i = 0; i < BYTE_RING_CAP; i++) {
            CHECK(byte_ring_pop(&r, &b));
            if (b != (uint8_t)i) {
                CHECK_EQ(b, (uint8_t)i);
                break;
            }
        }
        CHECK(!byte_ring_pop(&r, &b));
    }
    CHECK_EQ(r.dropped, 3);
    /* indices wrap past 2^32 without losing order */
    r.head = r.tail = UINT32_MAX - 2;
    for (int i = 0; i < 6; i++)
        CHECK(byte_ring_push(&r, (uint8_t)(i + 1)));
    for (int i = 0; i < 6; i++) {
        CHECK(byte_ring_pop(&r, &b));
        CHECK_EQ(b, i + 1);
    }
}

TEST(event_queue_policy)
{
    static struct event_queue q;
    event_queue_init(&q);
    struct event e = { 0 };
    e.type = EV_MOUSE;
    e.mouse.dx = 3;
    e.mouse.dy = -2;
    CHECK(event_queue_push(&q, &e));
    CHECK(event_queue_push(&q, &e)); /* coalesced */
    CHECK_EQ(q.count, 1);
    CHECK_EQ(q.coalesced, 1);
    e.mouse.buttons = 1; /* button change: not coalesced */
    CHECK(event_queue_push(&q, &e));
    CHECK_EQ(q.count, 2);
    struct event k = { 0 };
    k.type = EV_KEY;
    for (int i = 2; i < EVENT_QUEUE_CAP; i++) {
        k.key.key = (uint16_t)i;
        CHECK(event_queue_push(&q, &k));
    }
    CHECK(!event_queue_push(&q, &k)); /* full: dropped and counted */
    CHECK_EQ(q.dropped, 1);
    struct event o;
    CHECK(event_queue_pop(&q, &o));
    CHECK_EQ(o.mouse.dx, 6);
    CHECK_EQ(o.mouse.dy, -4);
    CHECK(event_queue_pop(&q, &o));
    CHECK_EQ(o.mouse.buttons, 1);
    for (int i = 2; i < EVENT_QUEUE_CAP; i++) {
        CHECK(event_queue_pop(&q, &o));
        if (o.key.key != i) {
            CHECK_EQ(o.key.key, i);
            break;
        }
    }
    CHECK(!event_queue_pop(&q, &o));
    /* huge coalesced deltas do not overflow int16 */
    e.mouse.buttons = 0;
    e.mouse.dx = 30000;
    CHECK(event_queue_push(&q, &e));
    CHECK(event_queue_push(&q, &e));
    CHECK_EQ(q.count, 2);
}

void run_tests(void)
{
    RUN(plain_and_shift);
    RUN(shifted_symbols);
    RUN(caps_lock_toggle_once_per_press);
    RUN(extended_keys);
    RUN(protocol_bytes_ignored);
    RUN(mouse_packets);
    RUN(mouse_resync_and_overflow);
    RUN(byte_ring_wrap_and_overflow);
    RUN(event_queue_policy);
}
