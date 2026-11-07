#ifndef AURORA_INPUT_H
#define AURORA_INPUT_H
#include "types.h"
#include "core/event_queue.h"

enum keycode {
    KEY_NONE = 0,
    KEY_CHAR,      /* printable; see key_event.ascii */
    KEY_ESC,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ENTER,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_UP,
    KEY_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_LSHIFT,
    KEY_RSHIFT,
    KEY_LCTRL,
    KEY_RCTRL,
    KEY_LALT,
    KEY_RALT,
    KEY_CAPSLOCK,
    KEY_LGUI,
    KEY_RGUI,
    KEY_MENU,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_OTHER,
};

#define MOD_SHIFT 1u
#define MOD_CTRL 2u
#define MOD_ALT 4u
#define MOD_CAPS 8u

/* PS/2 scan code set 1 decoder (the controller translates set 2 to set 1).
 * US QWERTY layout. */
struct kbd_decoder {
    bool e0;
    uint8_t e1_skip;
    bool lshift, rshift, lctrl, rctrl, lalt, ralt;
    bool caps, caps_held;
    uint32_t ignored; /* ACK/resend/error bytes discarded */
};

void kbd_decoder_init(struct kbd_decoder *d);
/* Feeds one byte; returns true and fills *out when a key event results. */
bool kbd_decode(struct kbd_decoder *d, uint8_t byte, struct key_event *out);
uint8_t kbd_mods(const struct kbd_decoder *d);

/* PS/2 mouse packets: 3 bytes, or 4 once IntelliMouse (wheel) mode is
 * negotiated (set `size` to 4). */
struct mouse_decoder {
    uint8_t pkt[4];
    uint8_t idx;
    uint8_t size;
    uint64_t last_ms;
    uint32_t resyncs, overflows;
};

void mouse_decoder_init(struct mouse_decoder *d);
/* now_ms is used to drop partial packets older than 50 ms (resync). */
bool mouse_decode(struct mouse_decoder *d, uint8_t byte, uint64_t now_ms, struct mouse_event *out);

/* Scan code set 1 extractor for /dev/input (0.7): yields the physical key
 * (0xE0-prefixed codes as 0x100 | code) and whether it went down. Skips the
 * Pause sequence, the fake shifts around Print Screen and controller
 * replies. Portable (host-tested). */
struct kbd_scanner {
    bool e0;
    uint8_t e1_skip;
};
void kbd_scanner_init(struct kbd_scanner *s);
bool kbd_scan(struct kbd_scanner *s, uint8_t byte, uint16_t *code, bool *pressed);

/* Kernel PS/2 driver (ps2.c) */
bool ps2_init(void);
bool ps2_has_mouse(void);
bool ps2_mouse_has_wheel(void);
/* Updates the keyboard LEDs (bit 0 Scroll, 1 Num, 2 Caps). Bounded waits;
 * safe to fail. */
void ps2_set_led_bits(uint8_t bits);
void ps2_set_leds(bool caps);
uint32_t ps2_led_updates(void);
struct byte_ring *ps2_kbd_ring(void);
struct byte_ring *ps2_mouse_ring(void);

#endif
