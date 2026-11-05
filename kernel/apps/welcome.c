/* Welcome window: shows /system/welcome.txt from the read-only initramfs. */
#include "desktop/desktop.h"
#include "apps/app_util.h"
#include "fs/vfs.h"
#include "lib.h"

static struct {
    uint32_t win_id;
    char text[2048];
    bool loaded;
    bool hover;
} W;

static struct rect start_btn(struct surface *s) { return rect_make(s->w - 150, s->h - 46, 130, 30); }

static void render(struct window *w, struct surface *s)
{
    UNUSED(w);
    const struct theme *t = g_theme;
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->win_bg);
    draw_round_rect_gradient(s, rect_make(24, 22, 56, 56), 14, RGB(0x8E, 0x7C, 0xFF), RGB(0x2A, 0x2F, 0x8A));
    logo_draw(s, 52, 50);
    draw_text_bold(s, FONT_BIG, 98, 24, "Welcome to Aurora", t->text);
    draw_text(s, FONT, 98, 54, "An original experimental operating system", t->text_dim);
    int32_t y = 100;
    if (W.loaded)
        y = app_draw_wrapped(s, FONT, 24, y, s->w - 48, 18, W.text, t->text);
    else
        y = app_draw_wrapped(s, FONT, 24, y, s->w - 48, 18, "(/system/welcome.txt could not be read)", t->danger);
    struct rect note = rect_make(20, s->h - 92, s->w - 40, 34);
    draw_round_rect(s, note, 6, t->warning_bg, 255, 0xF);
    draw_text(s, FONT, note.x + 10, note.y + 9, "Files are stored in memory and reset when Aurora restarts.", t->warning_text);
    widget_button(s, start_btn(s), "Get Started", BTN_PRIMARY, W.hover);
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    bool h = rect_contains(start_btn(&w->content), m->x, m->y);
    if (h != W.hover) {
        W.hover = h;
        wm_invalidate(w);
    }
    if (m->kind == WM_MOUSE_DOWN && h)
        wm_request_close(w);
}

static void key(struct window *w, const struct key_event *e)
{
    if (e->key == KEY_ENTER || e->key == KEY_ESC)
        wm_request_close(w);
}

static void destroyed(struct window *w)
{
    UNUSED(w);
    W.win_id = 0;
}

static void resized(struct window *w) { wm_invalidate(w); }

static const struct window_ops ops = { .render = render, .mouse = mouse, .key = key, .destroyed = destroyed, .resized = resized };

struct window *welcome_open(void)
{
    struct window *w = wm_get(W.win_id);
    if (w) {
        wm_focus(w);
        return w;
    }
    size_t len = 0;
    W.loaded = vfs_read_file("/system/welcome.txt", W.text, sizeof W.text - 1, &len) == VFS_OK;
    W.text[W.loaded ? len : 0] = '\0';
    w = wm_create(APP_WELCOME, "Welcome", 560, 470, &ops, NULL);
    if (w)
        W.win_id = w->id;
    return w;
}
