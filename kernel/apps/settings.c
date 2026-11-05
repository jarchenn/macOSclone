/* Settings / About: theme selection plus real system information.
 * No placeholder preferences: every control here does something. */
#include "desktop/desktop.h"
#include "apps/app_util.h"
#include "core/boot.h"
#include "core/log.h"
#include "core/time.h"
#include "drivers/framebuffer.h"
#include "memory/heap.h"
#include "memory/physical.h"
#include "memory/virtual.h"
#include "fs/vfs.h"
#include "lib.h"

enum { TAB_APPEARANCE = 0, TAB_SHORTCUTS = 1, TAB_ABOUT = 2, NTABS = 3 };
static const char *const tab_names[NTABS] = { "Appearance", "Shortcuts", "About" };

static struct {
    uint32_t win_id;
    int tab;
    int hover; /* 1..3 tabs, 10/11 theme cards */
} S;

static struct rect tab_rect(struct surface *s, int i)
{
    int32_t w = 110, total = w * NTABS;
    return rect_make((s->w - total) / 2 + i * w, 12, w, 26);
}

static struct rect theme_card(int dark) { return rect_make(24 + dark * 200, 96, 180, 130); }

static void row(struct surface *s, int32_t *y, const char *k, const char *v)
{
    draw_text(s, FONT, 24, *y, k, g_theme->text_dim);
    draw_text(s, FONT, 190, *y, v, g_theme->text);
    *y += 20;
}

static void render_about(struct surface *s)
{
    const struct theme *t = g_theme;
    int32_t y = 58;
    draw_round_rect_gradient(s, rect_make(24, y, 48, 48), 12, RGB(0x8E, 0x7C, 0xFF), RGB(0x2A, 0x2F, 0x8A));
    logo_draw(s, 48, y + 24);
    draw_text_bold(s, FONT_BIG, 88, y + 2, "Aurora OS", t->text);
    char v[96];
    k_snprintf(v, sizeof v, "Version %s  (%s build, id %s)", AURORA_VERSION, AURORA_VARIANT, AURORA_BUILD_ID);
    draw_text(s, FONT, 88, y + 30, v, t->text_dim);
    y += 64;
    char buf[96];
    row(s, &y, "Architecture", "x86_64, 1 CPU (QEMU pc)");
    k_snprintf(buf, sizeof buf, "%s %s (base rev %llu)", g_boot.bootloader_name, g_boot.bootloader_version,
               (unsigned long long)g_boot.base_revision);
    row(s, &y, "Bootloader", buf);
    row(s, &y, "Firmware", app_firmware_name());
    k_snprintf(buf, sizeof buf, "%ux%u, %u bpp, %s", g_fb.width, g_fb.height, g_fb.bpp,
               g_vmm.fb_write_combining ? "write-combining" : "uncached");
    row(s, &y, "Display", buf);
    uint64_t sec = time_ms() / 1000;
    k_snprintf(buf, sizeof buf, "%u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    row(s, &y, "Uptime", buf);
    const struct pmm *p = pmm_state();
    k_snprintf(buf, sizeof buf, "%llu MiB usable, %llu MiB free", (unsigned long long)(p->usable_pages * 4 / 1024),
               (unsigned long long)(p->free_pages * 4 / 1024));
    row(s, &y, "Memory", buf);
    struct heap_stats hs;
    kheap_stats(&hs);
    k_snprintf(buf, sizeof buf, "%zu KiB in use, peak %zu KiB", hs.used_bytes / 1024, hs.peak_used_bytes / 1024);
    row(s, &y, "Kernel heap", buf);
    struct vfs_usage u;
    vfs_usage(&u);
    k_snprintf(buf, sizeof buf, "%u KiB of %u KiB (RAM)", u.ram_bytes / 1024, RAMFS_MAX_TOTAL / 1024);
    row(s, &y, "Session files", buf);
    y += 6;
    widget_separator(s, 24, y, s->w - 48);
    y += 10;
    draw_text_bold(s, FONT, 24, y, "Supported", t->text);
    y += 20;
    y = app_draw_wrapped(s, FONT, 24, y, s->w - 48, 17,
                         "PS/2 keyboard (US layout) and mouse, software-composited desktop, built-in "
                         "Terminal, Files, Notes and Settings, read-only /system and RAM-backed /home.",
                         t->text_dim);
    y += 6;
    draw_text_bold(s, FONT, 24, y, "Limitations", t->text);
    y += 20;
    draw_round_rect(s, rect_make(20, y - 4, s->w - 40, 76), 6, t->warning_bg, 255, 0xF);
    app_draw_wrapped(s, FONT, 30, y + 2, s->w - 60, 17,
                     "Files are stored in memory and reset when Aurora restarts. Apps run inside one "
                     "kernel address space: they are not isolated, and an app fault stops the system. "
                     "No networking, audio, USB, SMP or persistent disk yet.",
                     t->warning_text);
}

static void render_shortcuts(struct surface *s)
{
    const struct theme *t = g_theme;
    static const char *const keys[][2] = {
        { "Ctrl+Tab", "Bring the next window to the front" },
        { "Ctrl+W", "Close the focused window" },
        { "Ctrl+M", "Minimize the focused window" },
        { "Ctrl+1 .. Ctrl+4", "Open Terminal, Files, Notes, Settings" },
        { "Dock click", "Open, focus or restore an app" },
        { "Ctrl+S", "Notes: save (Ctrl+Shift+S: save as)" },
        { "Ctrl+N / Ctrl+O", "Notes: new note / open via Files" },
        { "Enter / Backspace", "Files: open item / go up" },
        { "Up / Down", "Terminal: command history" },
        { "PgUp / PgDn", "Terminal: scroll back" },
        { "Ctrl+L / Ctrl+C", "Terminal: clear / cancel line" },
        { "Esc", "Close a menu or dialog" },
    };
    int32_t y = 60;
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        widget_badge(s, 24, y - 1, keys[i][0], t->sidebar_bg, t->text);
        draw_text(s, FONT, 200, y, keys[i][1], t->text);
        y += 26;
    }
    draw_text(s, FONT, 24, y + 8, "To release a grabbed mouse in QEMU's GTK window: Ctrl+Alt+G.", t->text_dim);
}

static void render_appearance(struct surface *s)
{
    const struct theme *t = g_theme;
    draw_text_bold(s, FONT, 24, 62, "Theme", t->text);
    for (int d = 0; d < 2; d++) {
        struct rect c = theme_card(d);
        bool sel = t->dark == (bool)d;
        draw_round_rect(s, c, 10, t->field_bg, 255, 0xF);
        draw_round_frame(s, c, 10, sel ? t->accent : t->field_border, 255);
        if (sel)
            draw_round_frame(s, rect_inset(c, 1), 9, t->accent, 255);
        else if (S.hover == 10 + d)
            draw_round_frame(s, rect_inset(c, 1), 9, t->field_border, 255);
        /* Preview: mini desktop */
        struct rect pv = rect_make(c.x + 14, c.y + 14, c.w - 28, 74);
        draw_round_rect_gradient(s, pv, 6, RGB(0x10, 0x1A, 0x48), RGB(0x46, 0x2C, 0x78));
        uint32_t wbg = d ? RGB(0x24, 0x25, 0x2B) : RGB(0xF7, 0xF7, 0xF9);
        uint32_t wtb = d ? RGB(0x30, 0x31, 0x38) : RGB(0xEC, 0xEC, 0xF0);
        struct rect mw = rect_make(pv.x + 22, pv.y + 14, pv.w - 44, 48);
        draw_round_rect(s, mw, 5, wbg, 255, 0xF);
        draw_round_rect(s, rect_make(mw.x, mw.y, mw.w, 14), 5, wtb, 255, 1 | 2);
        draw_circle(s, mw.x + 8, mw.y + 7, 3, RGB(0xFF, 0x5F, 0x57), 255);
        draw_circle(s, mw.x + 17, mw.y + 7, 3, RGB(0xFE, 0xBC, 0x2E), 255);
        draw_circle(s, mw.x + 26, mw.y + 7, 3, RGB(0x28, 0xC8, 0x40), 255);
        const char *lbl = d ? "Dark" : "Light";
        draw_text(s, FONT, c.x + (c.w - text_width(FONT, lbl)) / 2, c.y + c.h - 30, lbl, sel ? t->accent : t->text);
    }
    draw_text(s, FONT, 24, 246, "Changes apply immediately to every window.", t->text_dim);
    draw_text(s, FONT, 24, 266, "Also available: 'theme light|dark' in Terminal.", t->text_dim);
}

static void render(struct window *w, struct surface *s)
{
    UNUSED(w);
    const struct theme *t = g_theme;
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->win_bg);
    /* Segmented tab control */
    struct rect all = rect_make(tab_rect(s, 0).x, 12, tab_rect(s, NTABS - 1).x + 110 - tab_rect(s, 0).x, 26);
    draw_round_rect(s, all, 7, t->sidebar_bg, 255, 0xF);
    for (int i = 0; i < NTABS; i++) {
        struct rect r = tab_rect(s, i);
        bool sel = S.tab == i;
        if (sel) {
            draw_round_rect(s, rect_inset(r, 2), 6, t->button_bg, 255, 0xF);
            draw_round_frame(s, rect_inset(r, 2), 6, t->button_border, 255);
        }
        draw_text(s, FONT, r.x + (r.w - text_width(FONT, tab_names[i])) / 2, r.y + 5, tab_names[i],
                  sel ? t->text : t->text_dim);
    }
    widget_separator(s, 0, 48, s->w);
    if (S.tab == TAB_APPEARANCE)
        render_appearance(s);
    else if (S.tab == TAB_SHORTCUTS)
        render_shortcuts(s);
    else
        render_about(s);
}

static void select_theme(bool dark)
{
    if (g_theme->dark == dark)
        return;
    theme_set(dark);
    desktop_theme_changed();
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    struct surface *s = &w->content;
    int h = 0;
    for (int i = 0; i < NTABS; i++)
        if (rect_contains(tab_rect(s, i), m->x, m->y))
            h = 1 + i;
    if (S.tab == TAB_APPEARANCE)
        for (int d = 0; d < 2; d++)
            if (rect_contains(theme_card(d), m->x, m->y))
                h = 10 + d;
    if (m->kind == WM_MOUSE_MOVE) {
        if (h != S.hover) {
            S.hover = h;
            wm_invalidate(w);
        }
        return;
    }
    if (m->kind != WM_MOUSE_DOWN)
        return;
    if (h >= 1 && h <= NTABS) {
        S.tab = h - 1;
        TEST_EVENT("SETTINGS tab=%s", tab_names[S.tab]);
    } else if (h >= 10) {
        select_theme(h == 11);
    }
    wm_invalidate(w);
}

static void key(struct window *w, const struct key_event *e)
{
    if (e->key == KEY_LEFT && S.tab > 0)
        S.tab--;
    else if (e->key == KEY_RIGHT && S.tab < NTABS - 1)
        S.tab++;
    wm_invalidate(w);
}

static void tick(struct window *w, uint64_t now)
{
    static uint64_t last;
    if (S.tab == TAB_ABOUT && now / 1000 != last) {
        last = now / 1000;
        wm_invalidate(w);
    }
}

static void destroyed(struct window *w)
{
    UNUSED(w);
    S.win_id = 0;
}

static void resized(struct window *w) { wm_invalidate(w); }

static const struct window_ops ops = {
    .render = render, .key = key, .mouse = mouse, .tick = tick, .destroyed = destroyed, .resized = resized,
};

struct window *settings_open(int tab)
{
    struct window *w = wm_get(S.win_id);
    if (!w) {
        w = wm_create(APP_SETTINGS, "Settings", 560, 520, &ops, NULL);
        if (!w)
            return NULL;
        S.win_id = w->id;
    }
    S.tab = CLAMP(tab, 0, NTABS - 1);
    wm_set_title(w, S.tab == TAB_ABOUT ? "About Aurora" : "Settings");
    wm_invalidate(w);
    wm_focus(w);
    return w;
}
