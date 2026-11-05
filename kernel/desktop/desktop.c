/* Desktop shell: input routing, menu bar, dock, menus, cursor and the main
 * event loop. Everything runs in the single main-loop context; IRQ handlers
 * only fill byte rings (see drivers/ps2.c). */
#include "desktop/desktop.h"
#include "graphics/compositor.h"
#include "drivers/framebuffer.h"
#include "core/event_queue.h"
#include "core/log.h"
#include "core/panic.h"
#include "core/time.h"
#include "arch/x86_64/io.h"
#include "memory/heap.h"
#include "lib.h"

static struct surface wallpaper;
static int32_t scr_w, scr_h;
static int32_t cur_x, cur_y;
static uint8_t buttons;

static struct kbd_decoder kbd;
static struct mouse_decoder mouse;
static struct event_queue events;

/* Interaction state; windows are referenced by id and re-validated. */
static uint32_t drag_id, capture_id, hover_win_id;
static int32_t drag_off_x, drag_off_y;
static uint32_t ctrl_press_id;
static int ctrl_press_which; /* 1 close, 2 minimize, 3 maximize */
static int dock_hover = -1, dock_press = -1;
static uint64_t last_click_ms;
static int32_t last_click_x, last_click_y;
static uint32_t last_click_win;
static bool reboot_requested;
static uint64_t last_second;

/* Menus */
enum { MENU_NONE = 0, MENU_AURORA, MENU_APP };
static int menu_open;
static int menu_hover = -1;
static struct rect menu_rect;
static struct rect mb_aurora_rect, mb_app_rect, mb_uptime_rect;

struct menu_item {
    const char *label;
    const char *shortcut;
    int action;
};
enum { ACT_NONE = 0, ACT_ABOUT, ACT_SETTINGS, ACT_THEME, ACT_REBOOT, ACT_MINIMIZE, ACT_CLOSE, ACT_CYCLE, ACT_WELCOME };

static struct menu_item aurora_items[] = {
    { "About Aurora", "", ACT_ABOUT },
    { "Settings...", "", ACT_SETTINGS },
    { "Welcome Guide", "", ACT_WELCOME },
    { "Dark Appearance", "", ACT_THEME },
    { NULL, NULL, ACT_NONE }, /* separator */
    { "Restart", "", ACT_REBOOT },
};
static struct menu_item app_items[] = {
    { "Minimize", "Ctrl+M", ACT_MINIMIZE },
    { "Close Window", "Ctrl+W", ACT_CLOSE },
    { NULL, NULL, ACT_NONE },
    { "Next Window", "Ctrl+Tab", ACT_CYCLE },
};

#define MENU_ITEM_H 24
#define MENU_W 220

int32_t desktop_width(void) { return scr_w; }
int32_t desktop_height(void) { return scr_h; }
uint64_t desktop_now_ms(void) { return time_ms(); }
void desktop_damage(struct rect r) { compositor_damage(r); }
void desktop_damage_all(void) { compositor_damage_all(); }
void desktop_request_reboot(void) { reboot_requested = true; }

void desktop_input_stats(uint32_t *dropped, uint32_t *coalesced)
{
    *dropped = events.dropped;
    *coalesced = events.coalesced;
}

void desktop_theme_changed(void)
{
    aurora_items[3].label = g_theme->dark ? "Light Appearance" : "Dark Appearance";
    wm_invalidate_all();
    compositor_damage_all();
    TEST_EVENT("THEME %s", g_theme->dark ? "dark" : "light");
}

struct rect dock_rect(void)
{
    int32_t n = 4;
    int32_t w = n * DOCK_ICON + (n - 1) * DOCK_GAP + 2 * DOCK_PAD;
    int32_t h = DOCK_ICON + 2 * DOCK_PAD;
    return rect_make((scr_w - w) / 2, scr_h - h - DOCK_MARGIN, w, h);
}

static struct rect dock_item_rect(int i)
{
    struct rect d = dock_rect();
    return rect_make(d.x + DOCK_PAD + i * (DOCK_ICON + DOCK_GAP), d.y + DOCK_PAD, DOCK_ICON, DOCK_ICON);
}

static struct rect tooltip_rect(int i)
{
    struct rect it = dock_item_rect(i);
    int32_t w = text_width(FONT, app_names[i]) + 20;
    return rect_make(it.x + (it.w - w) / 2, it.y - 40, w, 26);
}

/* ---- Painting ------------------------------------------------------------- */

static void paint_menubar(struct surface *s)
{
    const struct theme *t = g_theme;
    draw_blend(s, rect_make(0, 0, scr_w, MENUBAR_H), t->menubar_bg, t->menubar_alpha);
    draw_hline(s, 0, MENUBAR_H - 1, scr_w, t->dark ? RGB(0, 0, 0) : RGB(0xC8, 0xC8, 0xD2));
    if (menu_open == MENU_AURORA)
        draw_round_rect(s, rect_inset(mb_aurora_rect, 0), 5, t->accent, 60, 0xF);
    if (menu_open == MENU_APP)
        draw_round_rect(s, mb_app_rect, 5, t->accent, 60, 0xF);
    logo_draw(s, mb_aurora_rect.x + 12, MENUBAR_H / 2);
    draw_text_bold(s, FONT, mb_aurora_rect.x + 26, 5, "Aurora", t->menubar_text);
    struct window *f = wm_focused();
    const char *app = f ? app_names[f->app] : "Desktop";
    draw_text_bold(s, FONT, mb_app_rect.x + 8, 5, app, t->menubar_text);
    char up[32];
    uint64_t sec = time_ms() / 1000;
    k_snprintf(up, sizeof up, "Uptime %u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    draw_text(s, FONT, mb_uptime_rect.x, 5, up, t->menubar_text);
}

static void layout_menubar(void)
{
    mb_aurora_rect = rect_make(6, 2, 26 + 6 * 8 + 10, MENUBAR_H - 4);
    struct window *f = wm_focused();
    const char *app = f ? app_names[f->app] : "Desktop";
    mb_app_rect = rect_make(mb_aurora_rect.x + mb_aurora_rect.w + 4, 2, text_width(FONT, app) + 18, MENUBAR_H - 4);
    mb_uptime_rect = rect_make(scr_w - 16 - 15 * 8, 0, 15 * 8 + 16, MENUBAR_H);
}

static void paint_dock(struct surface *s)
{
    const struct theme *t = g_theme;
    struct rect d = dock_rect();
    draw_shadow(s, d, 18, 14, 70);
    draw_round_rect(s, d, 18, t->dock_bg, t->dock_alpha, 0xF);
    draw_round_frame(s, d, 18, t->dock_border, t->dark ? 160 : 120);
    for (int i = 0; i < 4; i++) {
        struct rect r = dock_item_rect(i);
        icon_draw(s, i, r.x, r.y, dock_press == i);
        struct window *w = wm_find_app(i);
        if (w) {
            uint32_t c = t->dark ? RGB(0xEE, 0xEE, 0xF4) : RGB(0x30, 0x30, 0x38);
            draw_circle(s, r.x + r.w / 2, d.y + d.h - 5, 2, c, w->state == WIN_MINIMIZED ? 110 : 255);
        }
    }
    if (dock_hover >= 0 && !buttons) {
        struct rect tr = tooltip_rect(dock_hover);
        draw_round_rect(s, tr, 7, t->dark ? RGB(0x30, 0x31, 0x3A) : RGB(0xFF, 0xFF, 0xFF), 235, 0xF);
        draw_round_frame(s, tr, 7, RGB(0, 0, 0), 50);
        draw_text(s, FONT, tr.x + 10, tr.y + 5, app_names[dock_hover], t->text);
    }
}

static struct menu_item *menu_items(int *count)
{
    if (menu_open == MENU_AURORA) {
        *count = (int)ARRAY_LEN(aurora_items);
        return aurora_items;
    }
    *count = (int)ARRAY_LEN(app_items);
    return app_items;
}

static struct rect menu_item_rect(int i)
{
    int32_t y = menu_rect.y + 6;
    int count;
    struct menu_item *it = menu_items(&count);
    for (int k = 0; k < i; k++)
        y += it[k].label ? MENU_ITEM_H : 9;
    return rect_make(menu_rect.x + 6, y, menu_rect.w - 12, it[i].label ? MENU_ITEM_H : 9);
}

static void layout_menu(void)
{
    int count;
    struct menu_item *it = menu_items(&count);
    int32_t h = 12;
    for (int i = 0; i < count; i++)
        h += it[i].label ? MENU_ITEM_H : 9;
    int32_t x = menu_open == MENU_AURORA ? mb_aurora_rect.x : mb_app_rect.x;
    menu_rect = rect_make(x, MENUBAR_H + 2, MENU_W, h);
}

static void paint_menu(struct surface *s)
{
    if (!menu_open)
        return;
    const struct theme *t = g_theme;
    draw_shadow(s, menu_rect, 8, 12, 80);
    draw_round_rect(s, menu_rect, 8, t->dark ? RGB(0x2B, 0x2C, 0x34) : RGB(0xFA, 0xFA, 0xFC), 245, 0xF);
    draw_round_frame(s, menu_rect, 8, RGB(0, 0, 0), t->dark ? 160 : 50);
    int count;
    struct menu_item *it = menu_items(&count);
    bool have_window = wm_focused() != NULL;
    for (int i = 0; i < count; i++) {
        struct rect r = menu_item_rect(i);
        if (!it[i].label) {
            widget_separator(s, r.x + 4, r.y + 4, r.w - 8);
            continue;
        }
        bool enabled = menu_open == MENU_AURORA || have_window;
        uint32_t fg = enabled ? t->text : t->text_dim;
        if (i == menu_hover && enabled) {
            draw_round_rect(s, r, 5, t->accent, 255, 0xF);
            fg = t->accent_text;
        }
        draw_text(s, FONT, r.x + 10, r.y + 4, it[i].label, fg);
        if (it[i].shortcut && it[i].shortcut[0])
            draw_text(s, FONT, r.x + r.w - 10 - text_width(FONT, it[i].shortcut), r.y + 4, it[i].shortcut,
                      i == menu_hover && enabled ? fg : t->text_dim);
    }
}

/* Original arrow cursor: X = outline, o = fill. */
static const char *const cursor_shape[] = {
    "X", "XX", "XoX", "XooX", "XoooX", "XooooX", "XoooooX", "XooooooX", "XoooooooX",
    "XooooooooX", "XoooooooooX", "XooooooXXXX", "XoooXooX", "XooXXooX", "XoX XooX",
    "XX   XooX", "X    XooX", "      XooX", "       XX",
};
#define CURSOR_W 12
#define CURSOR_H 19

static struct rect cursor_rect(void) { return rect_make(cur_x - 1, cur_y - 1, CURSOR_W + 3, CURSOR_H + 3); }

static void paint_cursor(struct surface *s)
{
    for (int y = 0; y < CURSOR_H; y++) {
        const char *row = cursor_shape[y];
        for (int x = 0; row[x]; x++) {
            if (row[x] == ' ')
                continue;
            int32_t px = cur_x + x, py = cur_y + y;
            if (!rect_contains(s->clip, px, py))
                continue;
            uint32_t *p = s->px + (size_t)py * (size_t)s->stride + (size_t)px;
            *p = row[x] == 'X' ? RGB(0x10, 0x10, 0x14) : RGB(0xFF, 0xFF, 0xFF);
        }
        /* subtle drop shadow */
        draw_pixel_blend(s, cur_x + (int32_t)strlen(row), cur_y + y + 1, RGB(0, 0, 0), 60);
    }
}

static void compose(struct surface *s, struct rect clip)
{
    /* Wallpaper */
    struct rect r = rect_intersect(clip, rect_make(0, 0, wallpaper.w, wallpaper.h));
    for (int32_t y = r.y; y < r.y + r.h; y++)
        memcpy(s->px + (size_t)y * (size_t)s->stride + (size_t)r.x,
               wallpaper.px + (size_t)y * (size_t)wallpaper.stride + (size_t)r.x, (size_t)r.w * 4);
    /* Windows, bottom to top */
    struct window *focused = wm_focused();
    for (int i = 0; i < wm_count(); i++) {
        struct window *w = wm_at_z(i);
        if (w->state == WIN_MINIMIZED)
            continue;
        if (!rect_overlaps(wm_damage_rect(w), clip))
            continue;
        wm_paint(s, w, w == focused);
    }
    paint_menubar(s);
    paint_dock(s);
    paint_menu(s);
    paint_cursor(s);
}

/* ---- Actions ---------------------------------------------------------------- */

struct window *app_launch(int app)
{
    switch (app) {
    case APP_TERMINAL: return terminal_open();
    case APP_FILES: return files_open(NULL);
    case APP_NOTES: return notes_open();
    case APP_SETTINGS: return settings_open(0);
    case APP_WELCOME: return welcome_open();
    default: return NULL;
    }
}

static void report_mem(const char *why)
{
    struct heap_stats hs;
    kheap_stats(&hs);
    UNUSED(why);
    TEST_EVENT("MEM why=%s heap_used=%zu allocs=%zu mapped=%zu", why, hs.used_bytes, hs.alloc_count, hs.mapped_bytes);
}

static void run_action(int act)
{
    struct window *f = wm_focused();
    switch (act) {
    case ACT_ABOUT: settings_open(2); break;
    case ACT_SETTINGS: settings_open(0); break;
    case ACT_WELCOME: welcome_open(); break;
    case ACT_THEME:
        theme_set(!g_theme->dark);
        desktop_theme_changed();
        break;
    case ACT_REBOOT: desktop_request_reboot(); break;
    case ACT_MINIMIZE: if (f) wm_minimize(f); break;
    case ACT_CLOSE: if (f) wm_request_close(f); break;
    case ACT_CYCLE: wm_cycle(); break;
    default: break;
    }
}

static void close_menu(void)
{
    if (!menu_open)
        return;
    compositor_damage(rect_inset(menu_rect, -14));
    compositor_damage(rect_make(0, 0, scr_w, MENUBAR_H));
    menu_open = MENU_NONE;
    menu_hover = -1;
}

static void open_menu(int which)
{
    close_menu();
    menu_open = which;
    menu_hover = -1;
    layout_menu();
    compositor_damage(rect_inset(menu_rect, -14));
    compositor_damage(rect_make(0, 0, scr_w, MENUBAR_H));
    TEST_EVENT("MENU open=%s x=%d y=%d w=%d h=%d", which == MENU_AURORA ? "aurora" : "app",
               menu_rect.x, menu_rect.y, menu_rect.w, menu_rect.h);
}

/* ---- Input routing ------------------------------------------------------------ */

static int control_at(struct window *w, int32_t x, int32_t y)
{
    int32_t cy = w->frame.y + CTRL_Y;
    const int32_t xs[3] = { CTRL_CLOSE_X, CTRL_MIN_X, CTRL_MAX_X };
    for (int i = 0; i < 3; i++) {
        int32_t dx = x - (w->frame.x + xs[i]), dy = y - cy;
        if (dx * dx + dy * dy <= (CTRL_R + 3) * (CTRL_R + 3))
            return i + 1;
    }
    return 0;
}

static void send_mouse(struct window *w, uint8_t kind, uint8_t clicks)
{
    if (!w || !w->ops || !w->ops->mouse)
        return;
    struct rect c = wm_content_rect(w);
    struct win_mouse m = { cur_x - c.x, cur_y - c.y, kind, buttons, clicks };
    w->ops->mouse(w, &m);
}

static void handle_press(void)
{
    TEST_EVENT("CLICK x=%d y=%d", cur_x, cur_y);
    /* Menus first */
    if (menu_open) {
        if (rect_contains(menu_rect, cur_x, cur_y))
            return; /* activation happens on release */
        bool on_title = rect_contains(mb_aurora_rect, cur_x, cur_y) || rect_contains(mb_app_rect, cur_x, cur_y);
        close_menu();
        if (!on_title)
            return;
    }
    if (cur_y < MENUBAR_H) {
        if (rect_contains(mb_aurora_rect, cur_x, cur_y))
            open_menu(MENU_AURORA);
        else if (rect_contains(mb_app_rect, cur_x, cur_y))
            open_menu(MENU_APP);
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (rect_contains(dock_item_rect(i), cur_x, cur_y)) {
            dock_press = i;
            compositor_damage(dock_rect());
            return;
        }
    }
    struct window *w = wm_top_at(cur_x, cur_y);
    if (!w)
        return;
    wm_focus(w);
    if (cur_y < w->frame.y + TITLE_H) {
        int c = control_at(w, cur_x, cur_y);
        if (c) {
            ctrl_press_id = w->id;
            ctrl_press_which = c;
            return;
        }
        drag_id = w->id;
        drag_off_x = cur_x - w->frame.x;
        drag_off_y = cur_y - w->frame.y;
        return;
    }
    uint64_t now = time_ms();
    uint8_t clicks = 1;
    int32_t dx = cur_x - last_click_x, dy = cur_y - last_click_y;
    if (last_click_win == w->id && now - last_click_ms < 450 && dx * dx + dy * dy <= 25)
        clicks = 2;
    last_click_ms = clicks == 2 ? 0 : now;
    last_click_x = cur_x;
    last_click_y = cur_y;
    last_click_win = w->id;
    capture_id = w->id;
    send_mouse(w, WM_MOUSE_DOWN, clicks);
}

static void handle_release(void)
{
    if (menu_open && rect_contains(menu_rect, cur_x, cur_y)) {
        int count;
        struct menu_item *it = menu_items(&count);
        for (int i = 0; i < count; i++) {
            if (it[i].label && rect_contains(menu_item_rect(i), cur_x, cur_y)) {
                int act = it[i].action;
                bool enabled = menu_open == MENU_AURORA || wm_focused();
                close_menu();
                if (enabled)
                    run_action(act);
                return;
            }
        }
        return;
    }
    if (dock_press >= 0) {
        int i = dock_press;
        dock_press = -1;
        compositor_damage(dock_rect());
        if (rect_contains(dock_item_rect(i), cur_x, cur_y)) {
            struct window *w = wm_find_app(i);
            TEST_EVENT("DOCK_CLICK app=%s", app_names[i]);
            if (w)
                wm_focus(w); /* restores if minimized */
            else
                app_launch(i);
        }
        return;
    }
    if (ctrl_press_id) {
        struct window *w = wm_get(ctrl_press_id);
        int which = ctrl_press_which;
        ctrl_press_id = 0;
        if (w && control_at(w, cur_x, cur_y) == which) {
            if (which == 1)
                wm_request_close(w);
            else if (which == 2)
                wm_minimize(w);
            else
                wm_toggle_maximize(w);
        }
        return;
    }
    if (drag_id) {
        struct window *w = wm_get(drag_id);
        drag_id = 0;
        if (w)
            TEST_EVENT("WIN id=%u app=%s x=%d y=%d w=%d h=%d state=%s", w->id, app_names[w->app], w->frame.x,
                       w->frame.y, w->frame.w, w->frame.h, w->state == WIN_MAXIMIZED ? "maximized" : "normal");
        return;
    }
    if (capture_id) {
        struct window *w = wm_get(capture_id);
        capture_id = 0;
        send_mouse(w, WM_MOUSE_UP, 1);
    }
}

static void handle_motion(void)
{
    if (drag_id) {
        struct window *w = wm_get(drag_id);
        if (w)
            wm_move(w, cur_x - drag_off_x, cur_y - drag_off_y);
        else
            drag_id = 0;
        return;
    }
    if (capture_id) {
        struct window *w = wm_get(capture_id);
        if (w)
            send_mouse(w, WM_MOUSE_MOVE, 0);
        else
            capture_id = 0;
        return;
    }
    if (menu_open) {
        int hv = -1, count;
        struct menu_item *it = menu_items(&count);
        for (int i = 0; i < count; i++)
            if (it[i].label && rect_contains(menu_item_rect(i), cur_x, cur_y))
                hv = i;
        if (hv != menu_hover) {
            menu_hover = hv;
            compositor_damage(menu_rect);
        }
    }
    /* Dock hover + tooltip */
    int dh = -1;
    for (int i = 0; i < 4; i++)
        if (rect_contains(dock_item_rect(i), cur_x, cur_y))
            dh = i;
    if (dh != dock_hover) {
        if (dock_hover >= 0)
            compositor_damage(rect_inset(tooltip_rect(dock_hover), -2));
        if (dh >= 0)
            compositor_damage(rect_inset(tooltip_rect(dh), -2));
        dock_hover = dh;
    }
    /* Window hover: controls + content */
    struct window *w = (menu_open || cur_y < MENUBAR_H) ? NULL : wm_top_at(cur_x, cur_y);
    if (w && rect_contains(dock_rect(), cur_x, cur_y))
        w = NULL;
    struct window *ctl = (w && cur_y < w->frame.y + TITLE_H && cur_x < w->frame.x + 76) ? w : NULL;
    wm_set_hover_controls(ctl);
    uint32_t hid = w && cur_y >= w->frame.y + TITLE_H ? w->id : 0;
    if (hid != hover_win_id) {
        struct window *old = wm_get(hover_win_id);
        if (old && old->ops && old->ops->mouse) {
            struct win_mouse m = { -1, -1, WM_MOUSE_MOVE, 0, 0 };
            old->ops->mouse(old, &m);
        }
        hover_win_id = hid;
    }
    if (hid)
        send_mouse(w, WM_MOUSE_MOVE, 0);
}

static void handle_mouse(const struct mouse_event *m)
{
    int32_t nx = CLAMP(cur_x + m->dx, 0, scr_w - 1);
    int32_t ny = CLAMP(cur_y + m->dy, 0, scr_h - 1);
    bool moved = nx != cur_x || ny != cur_y;
    if (moved) {
        compositor_damage(cursor_rect());
        cur_x = nx;
        cur_y = ny;
        compositor_damage(cursor_rect());
        handle_motion();
    }
    uint8_t old = buttons;
    buttons = m->buttons;
    if ((buttons & 1) && !(old & 1))
        handle_press();
    else if (!(buttons & 1) && (old & 1))
        handle_release();
}

static bool handle_shortcut(const struct key_event *e)
{
    if (e->key == KEY_ESC && menu_open) {
        close_menu();
        return true;
    }
    if (!(e->mods & MOD_CTRL) || (e->mods & MOD_ALT))
        return false;
    struct window *f = wm_focused();
    if (e->key == KEY_TAB) {
        wm_cycle();
        return true;
    }
    if (e->key != KEY_CHAR)
        return false;
    switch (e->ascii) {
    case 'w': case 'W':
        if (f)
            wm_request_close(f);
        return true;
    case 'm': case 'M':
        if (f)
            wm_minimize(f);
        return true;
    case '1': app_launch(APP_TERMINAL); return true;
    case '2': app_launch(APP_FILES); return true;
    case '3': app_launch(APP_NOTES); return true;
    case '4': app_launch(APP_SETTINGS); return true;
    default: return false;
    }
}

static void handle_key(const struct key_event *e)
{
    if (!e->pressed)
        return;
    TEST_EVENT("KEY key=%u ascii=%u mods=%u", e->key, e->ascii, e->mods);
    if (handle_shortcut(e))
        return;
    struct window *f = wm_focused();
    if (f && f->state != WIN_MINIMIZED && f->ops && f->ops->key)
        f->ops->key(f, e);
}

/* ---- Main loop -------------------------------------------------------------- */

static void do_reboot(void)
{
    klog("desktop: restart requested; session files in /home will be lost");
    TEST_EVENT("REBOOT");
    cpu_cli();
    /* 1. 8042 controller reset line pulse */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        io_wait();
    outb(0x64, 0xFE);
    for (int i = 0; i < 1000000; i++)
        io_wait();
    /* 2. Fallback: triple fault via an empty IDT */
    klog("desktop: 8042 reset failed; forcing triple fault");
    struct PACKED { uint16_t limit; uint64_t base; } idtr = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(idtr));
    for (;;)
        cpu_hlt();
}

static bool work_pending(void)
{
    return byte_ring_count(ps2_kbd_ring()) || byte_ring_count(ps2_mouse_ring()) || events.count ||
           compositor_pending();
}

void desktop_main(void)
{
    if (!fb_ready())
        panic("desktop: no usable framebuffer");
    scr_w = (int32_t)g_fb.width;
    scr_h = (int32_t)g_fb.height;
    if (!surface_alloc(&wallpaper, scr_w, scr_h))
        panic("desktop: cannot allocate wallpaper (%dx%d)", scr_w, scr_h);
    wallpaper_generate(&wallpaper);
    if (!compositor_init(scr_w, scr_h, compose))
        panic("desktop: cannot allocate backbuffer");
    klog_set_console(false);
    kbd_decoder_init(&kbd);
    mouse_decoder_init(&mouse);
    event_queue_init(&events);
    cur_x = scr_w / 2;
    cur_y = scr_h / 2;
    layout_menubar();

    TEST_EVENT("SCREEN w=%d h=%d", scr_w, scr_h);
    for (int i = 0; i < 4; i++) {
        struct rect r = dock_item_rect(i);
        UNUSED(r);
        TEST_EVENT("DOCK_ITEM app=%s x=%d y=%d w=%d h=%d", app_names[i], r.x, r.y, r.w, r.h);
    }
    TEST_EVENT("MENUBAR aurora_x=%d app_x=%d", mb_aurora_rect.x + mb_aurora_rect.w / 2, mb_app_rect.x + 10);
    TEST_EVENT("CURSOR x=%d y=%d", cur_x, cur_y);

    welcome_open();
    wm_render_dirty();
    compositor_frame();
    klog("AURORA_DESKTOP_READY %dx%d", scr_w, scr_h);
    report_mem("startup");

    uint64_t last_tick = 0;
    int last_nwin = wm_count();
    for (;;) {
        uint8_t b;
        struct event ev;
        while (byte_ring_pop(ps2_kbd_ring(), &b)) {
            ev.type = EV_KEY;
            if (kbd_decode(&kbd, b, &ev.key))
                event_queue_push(&events, &ev);
        }
        bool mouse_moved = false;
        uint64_t now = time_ms();
        while (byte_ring_pop(ps2_mouse_ring(), &b)) {
            ev.type = EV_MOUSE;
            if (mouse_decode(&mouse, b, now, &ev.mouse))
                event_queue_push(&events, &ev);
        }
        while (event_queue_pop(&events, &ev)) {
            if (ev.type == EV_KEY) {
                handle_key(&ev.key);
            } else {
                int32_t ox = cur_x, oy = cur_y;
                handle_mouse(&ev.mouse);
                if (ox != cur_x || oy != cur_y)
                    mouse_moved = true;
            }
        }
        if (mouse_moved)
            TEST_EVENT("CURSOR x=%d y=%d", cur_x, cur_y);

        now = time_ms();
        if (now / 1000 != last_second) {
            last_second = now / 1000;
            compositor_damage(mb_uptime_rect);
        }
        if (now - last_tick >= 250) {
            last_tick = now;
            for (int i = 0; i < wm_count(); i++) {
                struct window *w = wm_at_z(i);
                if (w->ops && w->ops->tick)
                    w->ops->tick(w, now);
            }
        }
        if (wm_count() != last_nwin) {
            if (wm_count() < last_nwin)
                report_mem("close");
            last_nwin = wm_count();
        }
        struct rect old_app = mb_app_rect;
        layout_menubar();
        if (old_app.w != mb_app_rect.w)
            compositor_damage(rect_make(0, 0, scr_w, MENUBAR_H));
        wm_render_dirty();
        compositor_frame();
        if (reboot_requested)
            do_reboot();

        /* Race-free idle: interrupts are disabled while checking for work, and
         * `sti; hlt` re-enables them atomically with the halt. */
        cpu_cli();
        if (!work_pending())
            cpu_sti_hlt();
        else
            cpu_sti();
    }
}
