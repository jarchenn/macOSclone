/* Scan code set 1 decoder, US layout. Portable (host-tested). */
#include "drivers/input.h"
#include "lib.h"

static const char lower[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6',
    [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't', [0x15] = 'y',
    [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h',
    [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x39] = ' ',
    [0x37] = '*', [0x4A] = '-', [0x4E] = '+',
};

static const char upper[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%', [0x07] = '^',
    [0x08] = '&', [0x09] = '*', [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y',
    [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
    [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|', [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B',
    [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x39] = ' ',
    [0x37] = '*', [0x4A] = '-', [0x4E] = '+',
};

void kbd_decoder_init(struct kbd_decoder *d) { memset(d, 0, sizeof *d); }

uint8_t kbd_mods(const struct kbd_decoder *d)
{
    uint8_t m = 0;
    if (d->lshift || d->rshift)
        m |= MOD_SHIFT;
    if (d->lctrl || d->rctrl)
        m |= MOD_CTRL;
    if (d->lalt || d->ralt)
        m |= MOD_ALT;
    if (d->caps)
        m |= MOD_CAPS;
    return m;
}

static uint16_t map_plain(uint8_t sc)
{
    switch (sc) {
    case 0x01: return KEY_ESC;
    case 0x0E: return KEY_BACKSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1C: return KEY_ENTER;
    case 0x1D: return KEY_LCTRL;
    case 0x2A: return KEY_LSHIFT;
    case 0x36: return KEY_RSHIFT;
    case 0x38: return KEY_LALT;
    case 0x3A: return KEY_CAPSLOCK;
    case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F:
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
        return (uint16_t)(KEY_F1 + (sc - 0x3B));
    case 0x57: return KEY_F11;
    case 0x58: return KEY_F12;
    /* Keypad without NumLock: navigation keys. */
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    default:
        return lower[sc] ? KEY_CHAR : KEY_OTHER;
    }
}

static uint16_t map_e0(uint8_t sc)
{
    switch (sc) {
    case 0x1C: return KEY_ENTER; /* keypad enter */
    case 0x1D: return KEY_RCTRL;
    case 0x38: return KEY_RALT;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PGUP;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x4F: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5B: return KEY_LGUI;
    case 0x5C: return KEY_RGUI;
    case 0x5D: return KEY_MENU;
    case 0x35: return KEY_CHAR; /* keypad '/' */
    default: return KEY_OTHER;
    }
}

bool kbd_decode(struct kbd_decoder *d, uint8_t b, struct key_event *out)
{
    if (d->e1_skip) { /* Pause/Break: E1 1D 45 E1 9D C5 — ignore the rest */
        d->e1_skip--;
        return false;
    }
    /* Controller/keyboard protocol bytes are never key events. 0xFA (ACK),
     * 0xFE (resend), 0xEE (echo), 0x00/0xFF (errors). */
    if (b == 0xFA || b == 0xFE || b == 0xEE || b == 0x00 || b == 0xFF) {
        d->ignored++;
        d->e0 = false;
        return false;
    }
    if (b == 0xE0) {
        d->e0 = true;
        return false;
    }
    if (b == 0xE1) {
        d->e1_skip = 5;
        return false;
    }
    bool release = b & 0x80;
    uint8_t sc = b & 0x7F;
    bool ext = d->e0;
    d->e0 = false;
    /* E0 2A / E0 AA / E0 36 / E0 B6 are "fake shifts" around PrtSc etc. */
    if (ext && (sc == 0x2A || sc == 0x36))
        return false;

    uint16_t key = ext ? map_e0(sc) : map_plain(sc);
    switch (key) {
    case KEY_LSHIFT: d->lshift = !release; break;
    case KEY_RSHIFT: d->rshift = !release; break;
    case KEY_LCTRL: d->lctrl = !release; break;
    case KEY_RCTRL: d->rctrl = !release; break;
    case KEY_LALT: d->lalt = !release; break;
    case KEY_RALT: d->ralt = !release; break;
    case KEY_CAPSLOCK:
        if (!release && !d->caps_held)
            d->caps = !d->caps; /* toggle once per physical press, not per repeat */
        d->caps_held = !release;
        break;
    default: break;
    }
    out->key = key;
    out->pressed = !release;
    out->mods = kbd_mods(d);
    out->ascii = 0;
    if (key == KEY_CHAR) {
        if (ext) {
            out->ascii = '/';
        } else {
            bool shift = (out->mods & MOD_SHIFT) != 0;
            char lc = lower[sc];
            bool letter = lc >= 'a' && lc <= 'z';
            if (letter && (out->mods & MOD_CAPS))
                shift = !shift;
            out->ascii = (uint8_t)(shift ? upper[sc] : lc);
        }
    } else if (key == KEY_ENTER) {
        out->ascii = '\n';
    } else if (key == KEY_TAB) {
        out->ascii = '\t';
    }
    return true;
}
