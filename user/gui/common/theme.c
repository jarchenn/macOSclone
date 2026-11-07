/* Aurora visual theme: palettes, procedural wallpaper and app icons.
 * Everything here is original artwork generated with integer math. */
#include "gui/desktop.h"
#include "lib.h"
#include <time.h>

const char *const app_names[APP_COUNT] = { "Terminal", "Files", "Notes", "Settings", "Welcome", "App" };
const char *const app_programs[APP_COUNT] = { "terminal", "files", "notes", "settings", "welcome", "" };

int app_from_name(const char *name)
{
    for (int i = 0; i < APP_OTHER; i++)
        if (!strcmp(name, app_names[i]) || !strcmp(name, app_programs[i]))
            return i;
    return APP_OTHER;
}

uint64_t gui_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const struct theme light = {
    .dark = false,
    .win_bg = RGB(0xF7, 0xF7, 0xF9), .win_border = RGB(0, 0, 0),
    .title_active = RGB(0xEC, 0xEC, 0xF0), .title_inactive = RGB(0xF4, 0xF4, 0xF6),
    .title_text = RGB(0x2A, 0x2A, 0x30), .title_text_inactive = RGB(0x9A, 0x9A, 0xA2),
    .text = RGB(0x1D, 0x1D, 0x22), .text_dim = RGB(0x6E, 0x6E, 0x78),
    .accent = RGB(0x3D, 0x6B, 0xF2), .accent_text = RGB(0xFF, 0xFF, 0xFF),
    .selection = RGB(0xD5, 0xE1, 0xFF), .sel_text = RGB(0x10, 0x20, 0x4A),
    .field_bg = RGB(0xFF, 0xFF, 0xFF), .field_border = RGB(0xC8, 0xC8, 0xD0),
    .button_bg = RGB(0xFF, 0xFF, 0xFF), .button_border = RGB(0xC4, 0xC4, 0xCC),
    .button_text = RGB(0x1D, 0x1D, 0x22), .button_hover = RGB(0xF0, 0xF2, 0xF8),
    .menubar_bg = RGB(0xF5, 0xF6, 0xFB), .menubar_text = RGB(0x16, 0x16, 0x1A),
    .dock_bg = RGB(0xF2, 0xF3, 0xF9), .dock_border = RGB(0xFF, 0xFF, 0xFF),
    .term_bg = RGB(0xFC, 0xFC, 0xFD), .term_text = RGB(0x1F, 0x23, 0x28),
    .term_prompt = RGB(0x1F, 0x88, 0x3D), .term_error = RGB(0xC9, 0x2A, 0x2A),
    .separator = RGB(0xDE, 0xDE, 0xE4), .sidebar_bg = RGB(0xEF, 0xEF, 0xF3),
    .warning_bg = RGB(0xFF, 0xF4, 0xD6), .warning_text = RGB(0x6B, 0x4E, 0x00),
    .danger = RGB(0xD9, 0x30, 0x25), .ctrl_inactive = RGB(0xD0, 0xD0, 0xD6),
    .menubar_alpha = 210, .dock_alpha = 165, .border_alpha = 46,
};

static const struct theme dark = {
    .dark = true,
    .win_bg = RGB(0x24, 0x25, 0x2B), .win_border = RGB(0, 0, 0),
    .title_active = RGB(0x30, 0x31, 0x38), .title_inactive = RGB(0x2A, 0x2B, 0x31),
    .title_text = RGB(0xE6, 0xE6, 0xEC), .title_text_inactive = RGB(0x80, 0x80, 0x8A),
    .text = RGB(0xE8, 0xE8, 0xEE), .text_dim = RGB(0x9C, 0x9C, 0xA8),
    .accent = RGB(0x6E, 0x93, 0xFF), .accent_text = RGB(0x0B, 0x12, 0x2B),
    .selection = RGB(0x34, 0x43, 0x70), .sel_text = RGB(0xF2, 0xF5, 0xFF),
    .field_bg = RGB(0x1B, 0x1C, 0x21), .field_border = RGB(0x48, 0x4A, 0x55),
    .button_bg = RGB(0x36, 0x37, 0x3F), .button_border = RGB(0x4C, 0x4D, 0x58),
    .button_text = RGB(0xEC, 0xEC, 0xF2), .button_hover = RGB(0x42, 0x43, 0x4C),
    .menubar_bg = RGB(0x1C, 0x1D, 0x25), .menubar_text = RGB(0xEE, 0xEE, 0xF4),
    .dock_bg = RGB(0x22, 0x23, 0x2C), .dock_border = RGB(0x55, 0x57, 0x66),
    .term_bg = RGB(0x15, 0x17, 0x1C), .term_text = RGB(0xD9, 0xDC, 0xE2),
    .term_prompt = RGB(0x7E, 0xE7, 0x87), .term_error = RGB(0xFF, 0x7B, 0x72),
    .separator = RGB(0x3A, 0x3B, 0x44), .sidebar_bg = RGB(0x2A, 0x2B, 0x32),
    .warning_bg = RGB(0x3D, 0x33, 0x14), .warning_text = RGB(0xF5, 0xD7, 0x7A),
    .danger = RGB(0xFF, 0x5F, 0x57), .ctrl_inactive = RGB(0x55, 0x56, 0x5E),
    .menubar_alpha = 200, .dock_alpha = 170, .border_alpha = 120,
};

const struct theme *g_theme = &light;

void theme_set(bool d) { g_theme = d ? &dark : &light; }

/* ---- Wallpaper ------------------------------------------------------------ */

static inline int32_t isin(uint32_t angle) { return sine_table_q12[angle & 255]; } /* -4096..4096 */

static uint32_t lcg_state = 0x2545F491u;
static uint32_t lcg(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

struct ribbon {
    int32_t base;      /* centre line, permille of height */
    int32_t amp;       /* pixels */
    int32_t f1, f2;    /* frequency numerators over 1024 px */
    int32_t p1, p2;    /* phase */
    int32_t up, down;  /* glow extent above/below the centre (px) */
    uint32_t color;
    uint32_t alpha;
};

void wallpaper_generate(struct surface *s)
{
    int32_t w = s->w, h = s->h;
    /* Sky: deep navy -> indigo -> violet. */
    for (int32_t y = 0; y < h; y++) {
        uint32_t t = (uint32_t)((int64_t)y * 512 / h);
        uint32_t c = t < 256 ? color_mix(RGB(0x07, 0x0E, 0x26), RGB(0x1B, 0x1E, 0x5A), t)
                             : color_mix(RGB(0x1B, 0x1E, 0x5A), RGB(0x46, 0x2C, 0x78), t - 256 > 255 ? 255 : t - 256);
        memset32(s->px + (size_t)y * (size_t)s->stride, c, (size_t)w);
    }
    /* Stars in the upper sky. */
    lcg_state = 0x2545F491u;
    for (int i = 0; i < w * h / 2600; i++) {
        int32_t x = (int32_t)(lcg() % (uint32_t)w);
        int32_t y = (int32_t)(lcg() % (uint32_t)(h * 6 / 10));
        uint32_t a = 60 + lcg() % 150;
        uint32_t *p = s->px + (size_t)y * (size_t)s->stride + (size_t)x;
        *p = color_blend(*p, RGB(0xFF, 0xFF, 0xFF), a);
        if ((lcg() & 15) == 0 && x + 1 < w)
            p[1] = color_blend(p[1], RGB(0xFF, 0xFF, 0xFF), a / 2);
    }
    /* Aurora ribbons: soft glow that extends further upwards (curtains). */
    const struct ribbon ribbons[] = {
        { 380, 46, 3, 7, 40, 170, 150, 26, RGB(0x3E, 0xE6, 0xC2), 150 },
        { 450, 34, 4, 9, 120, 20, 120, 22, RGB(0x6C, 0xF0, 0x8A), 110 },
        { 300, 40, 2, 5, 200, 90, 110, 20, RGB(0xA8, 0x7C, 0xFF), 120 },
    };
    for (size_t k = 0; k < ARRAY_LEN(ribbons); k++) {
        const struct ribbon *rb = &ribbons[k];
        for (int32_t x = 0; x < w; x++) {
            int32_t ang1 = (int32_t)(((int64_t)x * rb->f1 * 256) / 1024) + rb->p1;
            int32_t ang2 = (int32_t)(((int64_t)x * rb->f2 * 256) / 1024) + rb->p2;
            int32_t cy = h * rb->base / 1000 + (rb->amp * isin((uint32_t)ang1) + rb->amp / 3 * isin((uint32_t)ang2)) / 4096;
            /* Vertical rays: brightness varies quickly along x. */
            int32_t ray = 192 + (isin((uint32_t)(x * 37 / 8 + rb->p2)) + isin((uint32_t)(x * 11 / 4 + rb->p1))) / 64;
            if (ray < 64)
                ray = 64;
            if (ray > 255)
                ray = 255;
            int32_t y0 = cy - rb->up, y1 = cy + rb->down;
            if (y0 < 0)
                y0 = 0;
            if (y1 > h)
                y1 = h;
            for (int32_t y = y0; y < y1; y++) {
                int32_t d = y - cy;
                int32_t ext = d < 0 ? rb->up : rb->down;
                int32_t f = ext - (d < 0 ? -d : d); /* 0..ext */
                uint32_t a = (uint32_t)((int64_t)rb->alpha * f * f / ((int64_t)ext * ext));
                a = a * (uint32_t)ray / 255;
                if (d > 0) /* sharper lower edge */
                    a = a * (uint32_t)f / (uint32_t)ext;
                uint32_t *p = s->px + (size_t)y * (size_t)s->stride + (size_t)x;
                *p = color_blend(*p, rb->color, a);
            }
        }
    }
    /* Distant ridge line along the bottom. */
    for (int32_t x = 0; x < w; x++) {
        int32_t ridge = h * 86 / 100 + (18 * isin((uint32_t)(x * 2 * 256 / 1024 + 30)) +
                                        9 * isin((uint32_t)(x * 5 * 256 / 1024 + 100)) +
                                        4 * isin((uint32_t)(x * 13 * 256 / 1024))) / 4096;
        for (int32_t y = ridge < 0 ? 0 : ridge; y < h; y++) {
            uint32_t t = (uint32_t)CLAMP((y - ridge) * 4, 0, 255);
            uint32_t c = color_mix(RGB(0x12, 0x12, 0x33), RGB(0x07, 0x08, 0x18), t);
            s->px[(size_t)y * (size_t)s->stride + (size_t)x] = c;
        }
        if (ridge >= 0 && ridge < h) {
            uint32_t *p = s->px + (size_t)ridge * (size_t)s->stride + (size_t)x;
            *p = color_blend(*p, RGB(0x6E, 0x6A, 0xC8), 90);
        }
    }
}

/* ---- Icons ------------------------------------------------------------------ */

void icon_draw(struct surface *s, int app, int32_t x, int32_t y, bool pressed)
{
    const int32_t S = DOCK_ICON;
    struct rect r = rect_make(x, y, S, S);
    /* Soft base shadow */
    draw_round_rect(s, rect_make(x, y + 2, S, S), 13, RGB(0, 0, 0), 50, 0xF);
    switch (app) {
    case APP_TERMINAL:
        draw_round_rect_gradient(s, r, 13, RGB(0x45, 0x4B, 0x5A), RGB(0x1C, 0x1F, 0x27));
        draw_round_rect(s, rect_make(x + 6, y + 8, S - 12, 3), 1, RGB(0xFF, 0xFF, 0xFF), 40, 0xF);
        text_draw(s, text_font_at(2, 20), x + 11, y + 15, ">_", RGB(0x7E, 0xE7, 0x87));
        break;
    case APP_FILES:
        draw_round_rect_gradient(s, r, 13, RGB(0x6C, 0xB6, 0xFF), RGB(0x2D, 0x68, 0xE0));
        draw_round_rect(s, rect_make(x + 10, y + 14, 16, 8), 3, RGB(0xA9, 0xD6, 0xFF), 255, 0xF);
        draw_round_rect(s, rect_make(x + 10, y + 18, 32, 22), 4, RGB(0xC9, 0xE5, 0xFF), 255, 0xF);
        draw_round_rect(s, rect_make(x + 10, y + 23, 32, 17), 4, RGB(0xEA, 0xF5, 0xFF), 255, 0xF);
        break;
    case APP_NOTES:
        draw_round_rect_gradient(s, r, 13, RGB(0xFF, 0xFE, 0xF8), RGB(0xEF, 0xE8, 0xD4));
        draw_round_rect(s, rect_make(x, y, S, 15), 13, RGB(0xFF, 0xCB, 0x45), 255, 1 | 2);
        draw_fill(s, rect_make(x, y + 13, S, 3), RGB(0xFF, 0xCB, 0x45));
        for (int i = 0; i < 4; i++)
            draw_fill(s, rect_make(x + 10, y + 23 + i * 6, i == 3 ? 20 : 32, 2), RGB(0xC4, 0xBA, 0xA0));
        break;
    case APP_SETTINGS: {
        draw_round_rect_gradient(s, r, 13, RGB(0xA4, 0xAD, 0xBD), RGB(0x5A, 0x63, 0x74));
        int32_t cx = x + S / 2, cy = y + S / 2;
        for (int i = 0; i < 8; i++) {
            uint32_t ang = (uint32_t)(i * 32);
            int32_t tx = cx + 14 * isin(ang + 64) / 4096;
            int32_t ty = cy + 14 * isin(ang) / 4096;
            draw_circle(s, tx, ty, 5, RGB(0xEC, 0xEF, 0xF4), 255);
        }
        draw_circle(s, cx, cy, 13, RGB(0xEC, 0xEF, 0xF4), 255);
        draw_circle(s, cx, cy, 6, RGB(0x7A, 0x83, 0x94), 255);
        break;
    }
    default:
        draw_round_rect_gradient(s, r, 13, RGB(0x8E, 0x7C, 0xFF), RGB(0x3A, 0x2C, 0x9A));
        logo_draw(s, x + S / 2, y + S / 2);
        break;
    }
    draw_round_frame(s, r, 13, RGB(0, 0, 0), 40);
    if (pressed)
        draw_round_rect(s, r, 13, RGB(0, 0, 0), 70, 0xF);
}

/* The Aurora mark: two overlapping translucent discs (teal + violet). */
void logo_draw(struct surface *s, int32_t cx, int32_t cy)
{
    draw_circle(s, cx - 2, cy, 6, RGB(0x3E, 0xD8, 0xC0), 230);
    draw_circle(s, cx + 3, cy, 6, RGB(0x8E, 0x6C, 0xFF), 190);
}
