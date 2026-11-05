#include "graphics/console.h"
#include "graphics/font.h"
#include "drivers/framebuffer.h"

#define CON_FG 0xD8E2F0
#define CON_BG 0x0A1020
#define PANIC_BG 0x3A0A12
#define PANIC_FG 0xFFFFFF

static int32_t cur_x, cur_y, cols, rows;
static uint32_t fg = CON_FG, bg = CON_BG;
static bool ready;

void console_init(void)
{
    if (!fb_ready())
        return;
    cols = (int32_t)g_fb.width / 8 - 2;
    rows = (int32_t)g_fb.height / 16 - 2;
    cur_x = cur_y = 0;
    fb_fill_rect(rect_make(0, 0, (int32_t)g_fb.width, (int32_t)g_fb.height), bg);
    ready = true;
}

static void glyph(int32_t cx, int32_t cy, char c)
{
    const uint16_t *g = font_glyph(&font_spleen_8x16, c);
    int32_t px = 8 + cx * 8, py = 16 + cy * 16;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 8; x++)
            fb_put_pixel(px + x, py + y, (g[y] & (0x8000u >> x)) ? fg : bg);
}

static void newline(void)
{
    cur_x = 0;
    if (++cur_y >= rows) {
        /* Wrap to the top instead of scrolling: cheap and allocation free. */
        cur_y = 0;
        fb_fill_rect(rect_make(0, 0, (int32_t)g_fb.width, (int32_t)g_fb.height), bg);
    }
}

void console_write(const char *s)
{
    if (!ready)
        return;
    for (; *s; s++) {
        if (*s == '\n') {
            newline();
        } else if (*s == '\r') {
            continue;
        } else {
            if (cur_x >= cols)
                newline();
            glyph(cur_x++, cur_y, *s);
        }
    }
}

void console_begin_panic(void)
{
    if (!fb_ready())
        return;
    fg = PANIC_FG;
    bg = PANIC_BG;
    ready = false;
    console_init();
}
