/* Keymaps (0.7g): scan codes (set 1, from /dev/input) to key events and
 * text. Layout files live in /system/keymaps/<name>.kmap:
 *
 *   name US-International
 *   # scancode(hex) normal shift [altgr [altgr-shift]]
 *   1e a A U+00E1 U+00C1
 *   28 dead_acute dead_diaeresis
 *   compose dead_acute e U+00E9
 *
 * A symbol is a single UTF-8 character, U+XXXX, "space", "minus", "hash",
 * "-" (nothing) or
 * dead_grave/acute/circumflex/tilde/diaeresis. A dead key produces no text;
 * the next key is composed with it through the compose table, or, if there
 * is no entry, the accent and the key are both emitted (space gives the
 * accent alone). Keys that are not characters (Shift, arrows, F-keys, ...)
 * come from the fixed table below and do not depend on the layout. */
#include "server.h"
#include "lib.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSC 0x80
enum { DEAD_NONE = 0, DEAD_GRAVE, DEAD_ACUTE, DEAD_CIRC, DEAD_TILDE, DEAD_DIAER, DEAD_COUNT };
static const char *const dead_names[DEAD_COUNT] = { "", "dead_grave", "dead_acute", "dead_circumflex",
                                                     "dead_tilde", "dead_diaeresis" };
static const uint32_t dead_chars[DEAD_COUNT] = { 0, '`', '\'', '^', '~', '"' };

/* Each entry: code point, or 0x80000000 | dead index. */
#define DEADBIT 0x80000000u
struct kmap {
    char name[32];
    uint32_t sym[NSC][4];
    struct { uint8_t dead; uint32_t base, out; } compose[160];
    int ncompose;
};

static struct kmap km;
static char km_file[32];
static bool lshift, rshift, lctrl, rctrl, lalt, ralt, caps;
static uint8_t pending_dead;
static uint16_t down[8]; /* keys currently held (to drop hardware repeats) */

static const struct { uint16_t sc; uint16_t key; } specials[] = {
    { 0x01, KEY_ESC }, { 0x0E, KEY_BACKSPACE }, { 0x0F, KEY_TAB }, { 0x1C, KEY_ENTER }, { 0x11C, KEY_ENTER },
    { 0x1D, KEY_LCTRL }, { 0x11D, KEY_RCTRL }, { 0x2A, KEY_LSHIFT }, { 0x36, KEY_RSHIFT }, { 0x38, KEY_LALT },
    { 0x138, KEY_RALT }, { 0x3A, KEY_CAPSLOCK }, { 0x3B, KEY_F1 }, { 0x3C, KEY_F2 }, { 0x3D, KEY_F3 },
    { 0x3E, KEY_F4 }, { 0x3F, KEY_F5 }, { 0x40, KEY_F6 }, { 0x41, KEY_F7 }, { 0x42, KEY_F8 }, { 0x43, KEY_F9 },
    { 0x44, KEY_F10 }, { 0x57, KEY_F11 }, { 0x58, KEY_F12 }, { 0x147, KEY_HOME }, { 0x148, KEY_UP },
    { 0x149, KEY_PGUP }, { 0x14B, KEY_LEFT }, { 0x14D, KEY_RIGHT }, { 0x14F, KEY_END }, { 0x150, KEY_DOWN },
    { 0x151, KEY_PGDN }, { 0x152, KEY_INSERT }, { 0x153, KEY_DELETE }, { 0x15B, KEY_LGUI }, { 0x15C, KEY_RGUI },
    { 0x15D, KEY_MENU },
    /* keypad without Num Lock */
    { 0x47, KEY_HOME }, { 0x48, KEY_UP }, { 0x49, KEY_PGUP }, { 0x4B, KEY_LEFT }, { 0x4D, KEY_RIGHT },
    { 0x4F, KEY_END }, { 0x50, KEY_DOWN }, { 0x51, KEY_PGDN }, { 0x52, KEY_INSERT }, { 0x53, KEY_DELETE },
};

bool keymap_is_modifier(uint16_t k)
{
    return k == KEY_LSHIFT || k == KEY_RSHIFT || k == KEY_LCTRL || k == KEY_RCTRL || k == KEY_LALT ||
           k == KEY_RALT || k == KEY_CAPSLOCK || k == KEY_LGUI || k == KEY_RGUI;
}

static uint32_t parse_sym(const char *t)
{
    if (!strcmp(t, "-"))
        return 0;
    if (!strcmp(t, "space"))
        return ' ';
    if (!strcmp(t, "minus"))
        return '-';
    if (!strcmp(t, "hash"))
        return '#';
    for (int d = 1; d < DEAD_COUNT; d++)
        if (!strcmp(t, dead_names[d]))
            return DEADBIT | (uint32_t)d;
    if ((t[0] == 'U' || t[0] == 'u') && t[1] == '+')
        return (uint32_t)strtoul(t + 2, NULL, 16);
    size_t len;
    uint32_t cp = utf8_decode(t, strlen(t), &len);
    return cp == 0xFFFD ? 0 : cp;
}

static bool parse(FILE *f, struct kmap *m)
{
    memset(m, 0, sizeof *m);
    char line[256];
    bool any = false;
    while (fgets(line, sizeof line, f)) {
        char *tok[6];
        int n = 0;
        for (char *p = strtok(line, " \t\r\n"); p && n < 6; p = strtok(NULL, " \t\r\n"))
            tok[n++] = p;
        if (n == 0 || tok[0][0] == '#')
            continue;
        if (!strcmp(tok[0], "name") && n >= 2) {
            str_copy(m->name, tok[1], sizeof m->name);
            continue;
        }
        if (!strcmp(tok[0], "compose") && n >= 4 && m->ncompose < (int)(sizeof m->compose / sizeof m->compose[0])) {
            uint32_t d = parse_sym(tok[1]);
            if (!(d & DEADBIT))
                continue;
            m->compose[m->ncompose].dead = (uint8_t)(d & 0xFF);
            m->compose[m->ncompose].base = parse_sym(tok[2]);
            m->compose[m->ncompose].out = parse_sym(tok[3]);
            m->ncompose++;
            continue;
        }
        unsigned long sc = strtoul(tok[0], NULL, 16);
        if (sc == 0 || sc >= NSC)
            continue;
        for (int i = 1; i < n && i <= 4; i++)
            m->sym[sc][i - 1] = parse_sym(tok[i]);
        any = true;
    }
    return any;
}

bool keymap_load(const char *name)
{
    char path[96];
    snprintf(path, sizeof path, "/system/keymaps/%s.kmap", name);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    static struct kmap tmp;
    bool ok = parse(f, &tmp);
    fclose(f);
    if (!ok)
        return false;
    km = tmp;
    if (!km.name[0])
        str_copy(km.name, name, sizeof km.name);
    str_copy(km_file, name, sizeof km_file);
    pending_dead = 0;
    TEST_EVENT("KEYMAP name=%s file=%s", km.name, km_file);
    return true;
}

const char *keymap_name(void) { return km_file; }

int keymap_list(char names[][32], int max)
{
    DIR *d = opendir("/system/keymaps");
    int n = 0;
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (l > 5 && l - 5 < 32 && !strcmp(e->d_name + l - 5, ".kmap")) {
            memcpy(names[n], e->d_name, l - 5);
            names[n][l - 5] = 0;
            n++;
        }
    }
    closedir(d);
    return n;
}

bool keymap_caps(void) { return caps; }

uint8_t keymap_mods(void)
{
    return (uint8_t)(((lshift || rshift) ? MOD_SHIFT : 0) | ((lctrl || rctrl) ? MOD_CTRL : 0) |
                     ((lalt || ralt) ? MOD_ALT : 0) | (caps ? MOD_CAPS : 0));
}

static bool held(uint16_t sc)
{
    for (int i = 0; i < 8; i++)
        if (down[i] == sc)
            return true;
    return false;
}

static void set_held(uint16_t sc, bool on)
{
    for (int i = 0; i < 8; i++) {
        if (on && down[i] == 0) {
            down[i] = sc;
            return;
        }
        if (!on && down[i] == sc)
            down[i] = 0;
    }
}

static void append(char *out, uint32_t cp)
{
    size_t n = strlen(out);
    if (n + 4 >= 16)
        return;
    n += utf8_encode(cp, out + n);
    out[n] = 0;
}

static bool is_letter(uint32_t cp)
{
    return (cp >= 'a' && cp <= 'z') || (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7);
}

bool keymap_feed(uint16_t sc, bool pressed, struct key_out *o)
{
    memset(o, 0, sizeof *o);
    uint16_t key = KEY_NONE;
    for (size_t i = 0; i < sizeof specials / sizeof specials[0]; i++)
        if (specials[i].sc == sc)
            key = specials[i].key;
    /* With an AltGr layer the right Alt is AltGr, not a modifier for
     * shortcuts. */
    bool altgr_layer = false;
    for (int i = 0; i < NSC && !altgr_layer; i++)
        altgr_layer = km.sym[i][2] != 0;
    switch (key) {
    case KEY_LSHIFT: lshift = pressed; break;
    case KEY_RSHIFT: rshift = pressed; break;
    case KEY_LCTRL: lctrl = pressed; break;
    case KEY_RCTRL: rctrl = pressed; break;
    case KEY_LALT: lalt = pressed; break;
    case KEY_RALT: ralt = pressed; break;
    default: break;
    }
    if (pressed && held(sc) && keymap_is_modifier(key))
        return false; /* hardware repeat of a modifier */
    bool repeat = pressed && held(sc);
    set_held(sc, pressed);
    if (key == KEY_CAPSLOCK && pressed && !repeat)
        caps = !caps;
    o->key.pressed = pressed;
    o->key.mods = keymap_mods();
    if (altgr_layer && ralt)
        o->key.mods &= (uint8_t)~MOD_ALT;
    if (key != KEY_NONE) {
        o->key.key = key;
        if (pressed && (key == KEY_ENTER || key == KEY_TAB || key == KEY_BACKSPACE) && pending_dead) {
            append(o->text, dead_chars[pending_dead]);
            pending_dead = 0;
        }
        return true;
    }
    if (sc >= NSC)
        return false;
    uint32_t *s = km.sym[sc];
    if (!s[0] && !s[1] && !s[2])
        return false;
    bool shift = lshift || rshift;
    bool gr = altgr_layer && ralt;
    int layer = (gr ? 2 : 0) + (shift ? 1 : 0);
    uint32_t sym = s[layer];
    if (!sym && gr)
        sym = s[shift ? 1 : 0];
    if (caps && !gr && is_letter(s[0]))
        sym = shift ? s[0] : s[1]; /* Caps Lock inverts Shift for letters */
    o->key.key = KEY_CHAR;
    uint32_t base = s[0] & DEADBIT ? dead_chars[s[0] & 0xFF] : s[0];
    uint32_t a = (sym & DEADBIT) ? dead_chars[sym & 0xFF] : sym;
    o->key.ascii = (uint8_t)(a < 128 ? a : base < 128 ? base : 0);
    if ((o->key.mods & (MOD_CTRL | MOD_ALT)) && base < 128)
        o->key.ascii = (uint8_t)base;
    if (!pressed || (o->key.mods & (MOD_CTRL | MOD_ALT)))
        return true; /* shortcuts produce no text */
    if (sym & DEADBIT) {
        uint8_t d = (uint8_t)(sym & 0xFF);
        if (pending_dead) {
            /* Two dead keys: the first one's accent, then wait again. */
            append(o->text, dead_chars[pending_dead]);
        }
        pending_dead = d;
        return true;
    }
    if (pending_dead) {
        uint8_t d = pending_dead;
        pending_dead = 0;
        if (sym == ' ') {
            append(o->text, dead_chars[d]);
            return true;
        }
        for (int i = 0; i < km.ncompose; i++)
            if (km.compose[i].dead == d && km.compose[i].base == sym) {
                append(o->text, km.compose[i].out);
                TEST_EVENT("DEADKEY dead=%s base=%u out=U+%04X", dead_names[d], (unsigned)sym,
                           (unsigned)km.compose[i].out);
                return true;
            }
        append(o->text, dead_chars[d]);
    }
    if (sym)
        append(o->text, sym);
    return true;
}
