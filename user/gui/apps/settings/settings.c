/* Settings (0.7, on the toolkit): appearance, display resolution,
 * keyboard layout and repeat, shortcuts, and an About page with real
 * system information. Every control does something. */
#include "desktop/desktop.h"
#include "gui/tk.h"
#include "apps/app_util.h"
#include "fs/vfs.h"
#include "lib.h"
#include <aurora.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

enum { TAB_APPEARANCE = 0, TAB_DISPLAY, TAB_KEYBOARD, TAB_SHORTCUTS, TAB_ABOUT, NTABS };
static const char *const tab_names[NTABS] = { "Appearance", "Display", "Keyboard", "Shortcuts", "About" };

static struct {
    uint32_t win_id;
    int tab;
    int hover; /* 1..NTABS tabs, 10/11 theme cards */
    struct m_settings set;
    bool have_settings;
    struct tk_panel disp, kbd;
    struct tk_widget *mode_list, *mode_apply, *mode_note;
    struct tk_widget *map_list, *map_apply, *delay_menu, *rate_menu, *kbd_note, *test_field;
    char mode_names[SETTINGS_MODES][24];
    char map_names[8][40];
} S;

static const char *const delays[] = { "250 ms", "400 ms", "500 ms", "750 ms", "1000 ms" };
static const uint32_t delay_ms[] = { 250, 400, 500, 750, 1000 };
static const char *const rates[] = { "Off", "10 per second", "20 per second", "30 per second", "40 per second" };
static const uint32_t rate_hz[] = { 0, 10, 20, 30, 40 };

static struct rect tab_rect(struct surface *s, int i)
{
    int32_t w = 100, total = w * NTABS;
    return rect_make((s->w - total) / 2 + i * w, 12, w, 26);
}

static struct rect theme_card(int dark) { return rect_make(24 + dark * 200, 96, 180, 130); }

static void row(struct surface *s, int32_t *y, const char *k, const char *v)
{
    text_draw(s, FONT_UI, 24, *y, k, g_theme->text_dim);
    text_draw_fit(s, FONT_UI, 190, *y, v, s->w - 214, g_theme->text);
    *y += 20;
}

static void render_about(struct surface *s)
{
    const struct theme *t = g_theme;
    int32_t y = 58;
    draw_round_rect_gradient(s, rect_make(24, y, 48, 48), 12, RGB(0x8E, 0x7C, 0xFF), RGB(0x2A, 0x2F, 0x8A));
    logo_draw(s, 48, y + 24);
    text_draw(s, FONT_TITLE, 88, y + 2, "Aurora OS", t->text);
    struct utsname u;
    char buf[160];
    if (uname(&u) == 0)
        snprintf(buf, sizeof buf, "Version %s  (%s)", u.release, u.version);
    else
        snprintf(buf, sizeof buf, "Version %s", AURORA_VERSION);
    text_draw_fit(s, FONT_UI, 88, y + 30, buf, s->w - 112, t->text_dim);
    y += 64;
    struct aurora_sysinfo si;
    memset(&si, 0, sizeof si);
    aurora_sysinfo(&si);
    snprintf(buf, sizeof buf, "x86_64, %u CPU%s (QEMU pc)", si.cpus, si.cpus == 1 ? "" : "s");
    row(s, &y, "Architecture", buf);
    row(s, &y, "Firmware", app_firmware_name());
    if (S.have_settings)
        snprintf(buf, sizeof buf, "%dx%d, 32 bpp%s", S.set.screen_w, S.set.screen_h,
                 S.set.can_set_mode ? ", resolution can change" : "");
    else
        snprintf(buf, sizeof buf, "%dx%d", desktop_width(), desktop_height());
    row(s, &y, "Display", buf);
    uint64_t sec = si.uptime_ms / 1000;
    snprintf(buf, sizeof buf, "%u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    row(s, &y, "Uptime", buf);
    snprintf(buf, sizeof buf, "%llu MiB, %llu MiB free", (unsigned long long)(si.total_pages * 4 / 1024),
             (unsigned long long)(si.free_pages * 4 / 1024));
    row(s, &y, "Memory", buf);
    struct aurora_klog k;
    memset(&k, 0, sizeof k);
    aurora_klog(&k);
    snprintf(buf, sizeof buf, "%llu KiB in use", (unsigned long long)(k.heap_used / 1024));
    row(s, &y, "Kernel heap", buf);
    if (k.home_persistent)
        snprintf(buf, sizeof buf, "data disk, %u of %u KiB used", k.pfs_used_blocks * 4, k.pfs_total_blocks * 4);
    else
        snprintf(buf, sizeof buf, "RAM only, %u KiB used", k.ram_bytes / 1024);
    row(s, &y, "/home storage", buf);
    snprintf(buf, sizeof buf, "%u processes, %u threads", si.procs, si.threads);
    row(s, &y, "Running", buf);
    snprintf(buf, sizeof buf, "%s", text_font_is_bitmap(FONT_UI) ? "bitmap fallback (Spleen)" : "DejaVu (TrueType)");
    row(s, &y, "Fonts", buf);
    y += 6;
    widget_separator(s, 24, y, s->w - 48);
    y += 10;
    text_draw(s, FONT_UI_BOLD, 24, y, "Limitations", t->text);
    y += 20;
    draw_round_rect(s, rect_make(20, y - 4, s->w - 40, 60), 6, t->warning_bg, 255, 0xF);
    app_draw_wrapped(s, FONT_UI, 30, y + 2, s->w - 60, 17,
                     "Software rendering only, one monitor, Latin text without shaping. "
                     "No audio or USB. Tested only in QEMU.",
                     t->warning_text);
}

static void render_shortcuts(struct surface *s)
{
    const struct theme *t = g_theme;
    static const char *const keys[][2] = {
        { "Alt+Tab", "Bring the next window to the front" },
        { "Ctrl+W", "Close the focused window" },
        { "Ctrl+M", "Minimize the focused window" },
        { "Ctrl+1 .. Ctrl+4", "Open Terminal, Files, Notes, Settings" },
        { "Dock click", "Open, focus or restore an app" },
        { "Ctrl+S", "Notes: save (Ctrl+Shift+S: save as)" },
        { "Ctrl+N / Ctrl+O", "Notes: new note / open a file" },
        { "Ctrl+C / X / V", "Copy, cut, paste (Terminal: Ctrl+V)" },
        { "Enter / Backspace", "Files: open item / go up" },
        { "Up / Down", "Terminal: command history" },
        { "PgUp / PgDn, wheel", "Scroll back" },
        { "Esc", "Close a menu or dialog" },
    };
    int32_t y = 60;
    for (size_t i = 0; i < ARRAY_LEN(keys); i++) {
        widget_badge(s, 24, y - 1, keys[i][0], t->sidebar_bg, t->text);
        text_draw(s, FONT_UI, 200, y, keys[i][1], t->text);
        y += 26;
    }
}

static void render_appearance(struct surface *s)
{
    const struct theme *t = g_theme;
    text_draw(s, FONT_UI_BOLD, 24, 62, "Theme", t->text);
    for (int d = 0; d < 2; d++) {
        struct rect c = theme_card(d);
        bool sel = t->dark == (bool)d;
        draw_round_rect(s, c, 10, t->field_bg, 255, 0xF);
        draw_round_frame(s, c, 10, sel ? t->accent : t->field_border, 255);
        if (sel)
            draw_round_frame(s, rect_inset(c, 1), 9, t->accent, 255);
        else if (S.hover == 10 + d)
            draw_round_frame(s, rect_inset(c, 1), 9, t->field_border, 255);
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
        text_draw(s, FONT_UI, c.x + (c.w - text_width(FONT_UI, lbl)) / 2, c.y + c.h - 30, lbl,
                  sel ? t->accent : t->text);
    }
    text_draw(s, FONT_UI, 24, 246, "Changes apply immediately to every window.", t->text_dim);
    text_draw(s, FONT_UI, 24, 266, "Also available: 'theme light|dark' in Terminal.", t->text_dim);
    /* Text samples at three sizes (also used by the golden screenshots). */
    text_draw(s, FONT_UI_BOLD, 24, 300, "Text", t->text);
    text_draw(s, text_font_at(0, 11), 24, 324, "Small 11 px: The quick brown fox jumps over the lazy dog.", t->text);
    text_draw(s, text_font_at(0, 15), 24, 344, "Body 15 px: Ångström, café, naïve, déjà vu.", t->text);
    text_draw(s, text_font_at(1, 24), 24, 370, "Large 24 px: AVATAR Wavy", t->text);
}

static void render_display(struct surface *s)
{
    const struct theme *t = g_theme;
    text_draw(s, FONT_UI_BOLD, 24, 62, "Resolution", t->text);
    char cur[64];
    snprintf(cur, sizeof cur, "Current: %dx%d", desktop_width(), desktop_height());
    text_draw(s, FONT_UI, 24, 84, cur, t->text_dim);
    tk_render(&S.disp, s);
}

static void render_keyboard(struct surface *s)
{
    const struct theme *t = g_theme;
    text_draw(s, FONT_UI_BOLD, 24, 62, "Layout", t->text);
    text_draw(s, FONT_UI_BOLD, 290, 62, "Key repeat", t->text);
    text_draw(s, FONT_UI, 290, 88, "Delay", t->text_dim);
    text_draw(s, FONT_UI, 290, 140, "Rate", t->text_dim);
    text_draw(s, FONT_UI_BOLD, 24, 262, "Try it", t->text);
    tk_render(&S.kbd, s);
}

static void render(struct window *w, struct surface *s)
{
    (void)w;
    const struct theme *t = g_theme;
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->win_bg);
    struct rect all = rect_make(tab_rect(s, 0).x, 12, tab_rect(s, NTABS - 1).x + 100 - tab_rect(s, 0).x, 26);
    draw_round_rect(s, all, 7, t->sidebar_bg, 255, 0xF);
    for (int i = 0; i < NTABS; i++) {
        struct rect r = tab_rect(s, i);
        bool sel = S.tab == i;
        if (sel) {
            draw_round_rect(s, rect_inset(r, 2), 6, t->button_bg, 255, 0xF);
            draw_round_frame(s, rect_inset(r, 2), 6, t->button_border, 255);
        }
        text_draw(s, FONT_UI, r.x + (r.w - text_width(FONT_UI, tab_names[i])) / 2,
                  r.y + (r.h - text_height(FONT_UI)) / 2, tab_names[i], sel ? t->text : t->text_dim);
    }
    widget_separator(s, 0, 48, s->w);
    switch (S.tab) {
    case TAB_APPEARANCE: render_appearance(s); break;
    case TAB_DISPLAY: render_display(s); break;
    case TAB_KEYBOARD: render_keyboard(s); break;
    case TAB_SHORTCUTS: render_shortcuts(s); break;
    default: render_about(s); break;
    }
}

/* ---- toolkit pages ------------------------------------------------------------- */

static const char *mode_item(void *ctx, int i, int *icon)
{
    (void)ctx;
    *icon = 0;
    return S.mode_names[i];
}

static const char *map_item(void *ctx, int i, int *icon)
{
    (void)ctx;
    *icon = 0;
    return S.map_names[i];
}

static void apply_mode(struct tk_widget *w, void *ctx)
{
    (void)w;
    (void)ctx;
    int i = S.mode_list->selected;
    if (!S.have_settings || i < 0 || i >= (int)S.set.nmodes)
        return;
    TEST_EVENT("SETTINGS_MODE w=%d h=%d", S.set.modes[i].w, S.set.modes[i].h);
    ui_set_mode(S.set.modes[i].w, S.set.modes[i].h);
}

static void apply_map(struct tk_widget *w, void *ctx)
{
    (void)w;
    (void)ctx;
    int i = S.map_list->selected;
    if (!S.have_settings || i < 0 || i >= (int)S.set.nkeymaps)
        return;
    TEST_EVENT("SETTINGS_KEYMAP name=%s", S.set.keymaps[i]);
    ui_set_keymap(S.set.keymaps[i]);
}

static void apply_repeat(struct tk_widget *w, void *ctx)
{
    (void)ctx;
    uint32_t d = S.set.delay_ms, r = S.set.rate_hz;
    if (w == S.delay_menu && w->selected >= 0)
        d = delay_ms[w->selected];
    if (w == S.rate_menu && w->selected >= 0)
        r = rate_hz[w->selected];
    ui_set_repeat(d, r);
}

static void label_menus(void)
{
    char d[32], r[32];
    snprintf(d, sizeof d, "%u ms", S.set.delay_ms);
    snprintf(r, sizeof r, S.set.rate_hz ? "%u per second" : "Off", S.set.rate_hz);
    tk_set_text(S.delay_menu, d);
    tk_set_text(S.rate_menu, r);
}

static void on_settings(const struct m_settings *s)
{
    S.set = *s;
    S.have_settings = true;
    int cur = -1;
    for (uint32_t i = 0; i < s->nmodes && i < SETTINGS_MODES; i++) {
        snprintf(S.mode_names[i], sizeof S.mode_names[i], "%d x %d", s->modes[i].w, s->modes[i].h);
        if (s->modes[i].w == s->screen_w && s->modes[i].h == s->screen_h)
            cur = (int)i;
    }
    tk_list_set(S.mode_list, (int)s->nmodes, mode_item, NULL);
    if (S.mode_list->selected < 0)
        tk_list_select(S.mode_list, cur);
    S.mode_apply->enabled = s->can_set_mode != 0;
    tk_set_text(S.mode_note, s->can_set_mode ? "Windows are moved and resized to fit the new size."
                                             : "This display adapter cannot change resolution.");
    int curmap = -1;
    for (uint32_t i = 0; i < s->nkeymaps && i < 8; i++) {
        snprintf(S.map_names[i], sizeof S.map_names[i], "%s%s", s->keymaps[i],
                 !strcmp(s->keymaps[i], s->keymap) ? "  (active)" : "");
        if (!strcmp(s->keymaps[i], s->keymap))
            curmap = (int)i;
    }
    tk_list_set(S.map_list, (int)s->nkeymaps, map_item, NULL);
    if (S.map_list->selected < 0)
        tk_list_select(S.map_list, curmap);
    label_menus();
    TEST_EVENT("SETTINGS_STATE w=%d h=%d keymap=%s delay=%u rate=%u modes=%u", s->screen_w, s->screen_h, s->keymap,
               s->delay_ms, s->rate_hz, s->nmodes);
    wm_invalidate(wm_get(S.win_id));
}

static void build_pages(struct window *w)
{
    tk_init(&S.disp, w);
    S.mode_list = tk_add(&S.disp, TK_LIST, rect_make(24, 108, 240, 260), NULL);
    S.mode_list->on_activate = apply_mode;
    S.mode_apply = tk_add(&S.disp, TK_BUTTON, rect_make(284, 108, 120, 28), "Apply");
    S.mode_apply->style = BTN_PRIMARY;
    S.mode_apply->on_activate = apply_mode;
    S.mode_note = tk_add(&S.disp, TK_LABEL, rect_make(24, 378, 500, 22), "");
    S.mode_note->style = 1;

    tk_init(&S.kbd, w);
    S.map_list = tk_add(&S.kbd, TK_LIST, rect_make(24, 84, 240, 130), NULL);
    S.map_list->on_activate = apply_map;
    S.map_apply = tk_add(&S.kbd, TK_BUTTON, rect_make(24, 222, 120, 28), "Use Layout");
    S.map_apply->style = BTN_PRIMARY;
    S.map_apply->on_activate = apply_map;
    S.delay_menu = tk_add(&S.kbd, TK_MENU, rect_make(290, 108, 180, 26), "500 ms");
    S.delay_menu->nitems = (int)ARRAY_LEN(delays);
    for (int i = 0; i < S.delay_menu->nitems; i++)
        S.delay_menu->items[i] = delays[i];
    S.delay_menu->on_activate = apply_repeat;
    S.rate_menu = tk_add(&S.kbd, TK_MENU, rect_make(290, 160, 180, 26), "30 per second");
    S.rate_menu->nitems = (int)ARRAY_LEN(rates);
    for (int i = 0; i < S.rate_menu->nitems; i++)
        S.rate_menu->items[i] = rates[i];
    S.rate_menu->on_activate = apply_repeat;
    S.test_field = tk_add(&S.kbd, TK_FIELD, rect_make(24, 286, 446, 28), "");
    str_copy(S.test_field->placeholder, "Type here to try the layout (US-International: ' then e gives \xC3\xA9)",
             sizeof S.test_field->placeholder);
    S.kbd_note = tk_add(&S.kbd, TK_LABEL, rect_make(24, 322, 446, 22),
                        "Caps Lock lights the keyboard LED. Dead keys combine with the next letter.");
    S.kbd_note->style = 1;
}

static struct tk_panel *page(void)
{
    return S.tab == TAB_DISPLAY ? &S.disp : S.tab == TAB_KEYBOARD ? &S.kbd : NULL;
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    struct surface *s = &w->content;
    struct tk_panel *p = page();
    if (p && m->y > 48 && tk_mouse(p, m)) {
        if (S.tab == TAB_KEYBOARD && m->kind == WM_MOUSE_DOWN && S.test_field->hover == false)
            ; /* focus stays where the panel put it */
        return;
    }
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
        if (S.tab == TAB_DISPLAY || S.tab == TAB_KEYBOARD)
            ui_on_settings(on_settings);
    } else if (h >= 10) {
        if (g_theme->dark != (h == 11))
            ui_set_theme(h == 11);
    }
    wm_invalidate(w);
}

static void key(struct window *w, const struct key_event *e)
{
    struct tk_panel *p = page();
    if (p && tk_key(p, e))
        return;
    if ((e->mods & MOD_CTRL) && e->key == KEY_TAB)
        return;
    if (e->key == KEY_LEFT && S.tab > 0)
        S.tab--;
    else if (e->key == KEY_RIGHT && S.tab < NTABS - 1)
        S.tab++;
    else
        return;
    if (S.tab == TAB_DISPLAY || S.tab == TAB_KEYBOARD)
        ui_on_settings(on_settings);
    wm_invalidate(w);
}

static void text(struct window *w, const char *utf8)
{
    struct tk_panel *p = page();
    if (p && tk_text(p, utf8)) {
        if (S.tab == TAB_KEYBOARD)
            TEST_EVENT("SETTINGS_TYPED text=%s", S.test_field->text);
        wm_invalidate(w);
    }
}

static void tick(struct window *w, uint64_t now)
{
    static uint64_t last;
    struct tk_panel *p = page();
    if (p)
        tk_tick(p, now);
    if (S.tab == TAB_ABOUT && now / 1000 != last) {
        last = now / 1000;
        wm_invalidate(w);
    }
}

static void destroyed(struct window *w)
{
    (void)w;
    S.win_id = 0;
}

static void resized(struct window *w) { wm_invalidate(w); }

static const struct window_ops ops = {
    .render = render, .key = key, .text = text, .mouse = mouse, .tick = tick, .destroyed = destroyed,
    .resized = resized,
};

static struct window *settings_open(const char *arg)
{
    int tab = TAB_APPEARANCE;
    if (arg && !strcmp(arg, "about"))
        tab = TAB_ABOUT;
    else if (arg && !strcmp(arg, "display"))
        tab = TAB_DISPLAY;
    else if (arg && !strcmp(arg, "keyboard"))
        tab = TAB_KEYBOARD;
    struct window *w = wm_get(S.win_id);
    if (!w) {
        w = wm_create_ex(APP_SETTINGS, "Settings", 560, 520, WINF_RESIZABLE, 520, 440, 0, 0, &ops, NULL);
        if (!w)
            return NULL;
        S.win_id = w->id;
        build_pages(w);
        ui_on_settings(on_settings);
    }
    S.tab = tab;
    wm_set_title(w, S.tab == TAB_ABOUT ? "About Aurora" : "Settings");
    wm_invalidate(w);
    wm_focus(w);
    TEST_EVENT("SETTINGS tab=%s", tab_names[S.tab]);
    return w;
}

int main(int argc, char **argv) { return app_main(APP_SETTINGS, "Settings", settings_open, argc, argv); }
