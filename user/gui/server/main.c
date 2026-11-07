/* Aurora display server (0.7): owns /dev/display and /dev/input, accepts
 * apps on the local socket "system.display", composes their shared-memory
 * buffers with server-side decorations, the menu bar, the dock, menus and
 * the cursor, routes input, keeps the clipboard, and launches apps. init
 * restarts it if it exits. */
#include "server.h"
#include "compose.h"
#include "lib.h"
#include <aurora.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

static int disp_fd = -1, input_fd = -1, listen_fd = -1;
static struct client clients[MAX_CLIENTS];
static struct surface wallpaper;
static int32_t scr_w, scr_h;
static int32_t cur_x, cur_y;
static uint8_t buttons;
static uint32_t generation;

/* Interaction state; windows are referenced by id and re-validated. */
static uint32_t drag_id, capture_id, hover_win_id;
static uint32_t resize_id;
static struct rect resize_ghost;
static int32_t drag_off_x, drag_off_y;
static uint32_t ctrl_press_id;
static int ctrl_press_which;
static int dock_hover = -1, dock_press = -1;
static uint64_t last_click_ms;
static int32_t last_click_x, last_click_y;
static uint32_t last_click_win;
static uint64_t last_second;

/* Key repeat (software; hardware typematic repeats are dropped). */
static uint32_t repeat_delay = 500, repeat_rate = 30;
static uint16_t repeat_sc;
static uint64_t repeat_next;
static uint16_t keys_down[8];

/* Clipboard: the server keeps its own copy (survives the source exiting). */
static char clip_mime[32];
static char *clip_data;
static uint32_t clip_len;

/* Menus */
enum { MENU_NONE = 0, MENU_AURORA, MENU_APP };
static int menu_open;
static int menu_hover = -1;
static struct rect menu_rect;
static struct rect mb_aurora_rect, mb_app_rect, mb_clock_rect;

struct menu_item {
    const char *label;
    const char *shortcut;
    int action;
};
enum { ACT_NONE = 0, ACT_ABOUT, ACT_SETTINGS, ACT_THEME, ACT_REBOOT, ACT_MINIMIZE, ACT_CLOSE, ACT_CYCLE, ACT_WELCOME,
       ACT_POWEROFF };

static struct menu_item aurora_items[] = {
    { "About Aurora", "", ACT_ABOUT },
    { "Settings...", "", ACT_SETTINGS },
    { "Welcome Guide", "", ACT_WELCOME },
    { "Dark Appearance", "", ACT_THEME },
    { NULL, NULL, ACT_NONE },
    { "Restart", "", ACT_REBOOT },
    { "Shut Down", "", ACT_POWEROFF },
};
static struct menu_item app_items[] = {
    { "Minimize", "Ctrl+M", ACT_MINIMIZE },
    { "Close Window", "Ctrl+W", ACT_CLOSE },
    { NULL, NULL, ACT_NONE },
    { "Next Window", "Alt+Tab", ACT_CYCLE },
};

#define MENU_ITEM_H 24
#define MENU_W 220
#define SETTINGS_FILE "/home/.display.conf"

int32_t desktop_width(void) { return scr_w; }
int32_t desktop_height(void) { return scr_h; }
void desktop_damage(struct rect r) { compositor_damage(r); }

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
    int32_t w = text_width(FONT_UI, app_names[i]) + 20;
    return rect_make(it.x + (it.w - w) / 2, it.y - 40, w, 26);
}

/* ---- clients ---------------------------------------------------------------- */

void send_to(struct client *c, uint16_t type, uint32_t win, const void *p, size_t n)
{
    if (!c || !c->alive)
        return;
    if (proto_send(c->fd, type, win, p, n, NULL, 0) < 0 && errno != EAGAIN)
        c->alive = false; /* reaped in the main loop */
}

void send_focus_change(struct window *old, struct window *now)
{
    if (old == now)
        return;
    struct m_focus f0 = { 0 }, f1 = { 1 };
    if (old)
        send_to(old->c, S_FOCUS, old->cid, &f0, sizeof f0);
    if (now)
        send_to(now->c, S_FOCUS, now->cid, &f1, sizeof f1);
}

static void broadcast(uint16_t type, const void *p, size_t n)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].alive)
            send_to(&clients[i], type, 0, p, n);
}

static void report_mem(const char *why)
{
    struct aurora_klog k;
    if (aurora_klog(&k) == 0)
        TEST_EVENT("MEM why=%s heap_used=%llu allocs=%llu mapped=%llu", why, (unsigned long long)k.heap_used,
                   (unsigned long long)k.heap_allocs, (unsigned long long)k.heap_mapped);
}

static void drop_client(struct client *c)
{
    wm_destroy_client(c);
    close(c->fd);
    TEST_EVENT("CLIENT_GONE app=%s pid=%d", c->name[0] ? c->name : "?", c->pid);
    memset(c, 0, sizeof *c);
    c->fd = -1;
    report_mem("close");
}

/* ---- settings ------------------------------------------------------------------ */

static void save_settings(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f)
        return;
    fprintf(f, "dark=%d\nkeymap=%s\nrepeat_delay=%u\nrepeat_rate=%u\n", g_theme->dark ? 1 : 0, keymap_name(),
            repeat_delay, repeat_rate);
    fclose(f);
}

static void load_settings(void)
{
    bool loaded = false;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (f) {
        char line[96];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = 0;
            const char *v = eq + 1;
            if (!strcmp(line, "dark"))
                theme_set(atoi(v) != 0);
            else if (!strcmp(line, "keymap"))
                loaded = keymap_load(v);
            else if (!strcmp(line, "repeat_delay"))
                repeat_delay = (uint32_t)CLAMP(atoi(v), 150, 2000);
            else if (!strcmp(line, "repeat_rate"))
                repeat_rate = (uint32_t)CLAMP(atoi(v), 0, 60);
        }
        fclose(f);
    }
    if (!loaded && !keymap_load("us"))
        fprintf(stderr, "display: no keymap in /system/keymaps; keyboard text is unavailable\n");
}

static void fill_settings(struct m_settings *s)
{
    memset(s, 0, sizeof *s);
    str_copy(s->keymap, keymap_name(), sizeof s->keymap);
    s->nkeymaps = (uint32_t)keymap_list(s->keymaps, 8);
    s->delay_ms = repeat_delay;
    s->rate_hz = repeat_rate;
    s->screen_w = scr_w;
    s->screen_h = scr_h;
    struct aurora_display_info info;
    if (ioctl(disp_fd, AURORA_DISP_INFO, &info) == 0) {
        s->can_set_mode = info.can_set_mode;
        s->nmodes = MIN(info.nmodes, (uint32_t)SETTINGS_MODES);
        for (uint32_t i = 0; i < s->nmodes; i++) {
            s->modes[i].w = (int32_t)info.modes[i].width;
            s->modes[i].h = (int32_t)info.modes[i].height;
        }
    }
    s->dark = g_theme->dark;
}

static void broadcast_settings(void)
{
    struct m_settings s;
    fill_settings(&s);
    broadcast(S_SETTINGS, &s, sizeof s);
}

/* ---- painting -------------------------------------------------------------------- */

static const char *focused_app_name(void)
{
    struct window *f = wm_focused();
    return f ? app_names[f->app] : "Desktop";
}

static void layout_menubar(void)
{
    mb_aurora_rect = rect_make(6, 2, 26 + text_width(FONT_UI_BOLD, "Aurora") + 10, MENUBAR_H - 4);
    mb_app_rect = rect_make(mb_aurora_rect.x + mb_aurora_rect.w + 4, 2,
                            text_width(FONT_UI_BOLD, focused_app_name()) + 18, MENUBAR_H - 4);
    int32_t cw = text_width(FONT_UI, "Uptime 00:00:00") + 16;
    mb_clock_rect = rect_make(scr_w - cw - 4, 0, cw + 4, MENUBAR_H);
}

static void paint_menubar(struct surface *s)
{
    const struct theme *t = g_theme;
    draw_blend(s, rect_make(0, 0, scr_w, MENUBAR_H), t->menubar_bg, t->menubar_alpha);
    draw_hline(s, 0, MENUBAR_H - 1, scr_w, t->dark ? RGB(0, 0, 0) : RGB(0xC8, 0xC8, 0xD2));
    if (menu_open == MENU_AURORA)
        draw_round_rect(s, mb_aurora_rect, 5, t->accent, 60, 0xF);
    if (menu_open == MENU_APP)
        draw_round_rect(s, mb_app_rect, 5, t->accent, 60, 0xF);
    int32_t ty = (MENUBAR_H - text_height(FONT_UI_BOLD)) / 2;
    logo_draw(s, mb_aurora_rect.x + 12, MENUBAR_H / 2);
    text_draw(s, FONT_UI_BOLD, mb_aurora_rect.x + 26, ty, "Aurora", t->menubar_text);
    text_draw(s, FONT_UI_BOLD, mb_app_rect.x + 8, ty, focused_app_name(), t->menubar_text);
    char up[32];
    uint64_t sec = gui_now_ms() / 1000;
    snprintf(up, sizeof up, "Uptime %u:%02u:%02u", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60),
             (unsigned)(sec % 60));
    text_draw(s, FONT_UI, mb_clock_rect.x + 8, ty, up, t->menubar_text);
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
        text_draw(s, FONT_UI, tr.x + 10, tr.y + (tr.h - text_height(FONT_UI)) / 2, app_names[dock_hover], t->text);
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
        int32_t ty = r.y + (r.h - text_height(FONT_UI)) / 2;
        text_draw(s, FONT_UI, r.x + 10, ty, it[i].label, fg);
        if (it[i].shortcut && it[i].shortcut[0])
            text_draw(s, FONT_UI, r.x + r.w - 10 - text_width(FONT_UI, it[i].shortcut), ty, it[i].shortcut,
                      i == menu_hover && enabled ? fg : t->text_dim);
    }
}

/* Original arrow cursor: X = outline, o = fill. */
static const char *const cursor_shape[] = {
    "X", "XX", "XoX", "XooX", "XoooX", "XooooX", "XoooooX", "XooooooX", "XoooooooX",
    "XooooooooX", "XoooooooooX", "XooooooXXXX", "XoooXooX", "XooXXooX", "XoX XooX",
    "XX   XooX", "X    XooX", "      XooX", "       XX",
};
#define CURSOR_H 19

static void paint_cursor(struct surface *s, int32_t cx, int32_t cy)
{
    for (int y = 0; y < CURSOR_H; y++) {
        const char *row = cursor_shape[y];
        for (int x = 0; row[x]; x++) {
            if (row[x] == ' ')
                continue;
            int32_t px = cx + x, py = cy + y;
            if (!rect_contains(s->clip, px, py))
                continue;
            s->px[(size_t)py * (size_t)s->stride + (size_t)px] =
                row[x] == 'X' ? RGB(0x10, 0x10, 0x14) : RGB(0xFF, 0xFF, 0xFF);
        }
        draw_pixel_blend(s, cx + (int32_t)strlen(row), cy + y + 1, RGB(0, 0, 0), 60);
    }
}

static void compose(struct surface *s, struct rect clip)
{
    struct rect r = rect_intersect(clip, rect_make(0, 0, wallpaper.w, wallpaper.h));
    for (int32_t y = r.y; y < r.y + r.h; y++)
        memcpy(s->px + (size_t)y * (size_t)s->stride + (size_t)r.x,
               wallpaper.px + (size_t)y * (size_t)wallpaper.stride + (size_t)r.x, (size_t)r.w * 4);
    struct window *focused = wm_focused();
    for (int i = 0; i < wm_count(); i++) {
        struct window *w = wm_at_z(i);
        if (w->state == WIN_MINIMIZED || !w->mapped)
            continue;
        if (!rect_overlaps(wm_damage_rect(w), clip))
            continue;
        wm_paint(s, w, w == focused);
    }
    paint_menubar(s);
    paint_dock(s);
    if (resize_id) {
        draw_blend(s, resize_ghost, g_theme->accent, 28);
        draw_frame(s, resize_ghost, g_theme->accent);
        draw_frame(s, rect_inset(resize_ghost, 1), g_theme->accent);
    }
    paint_menu(s);
}

/* ---- actions -------------------------------------------------------------------- */

static bool program_name_ok(const char *p)
{
    if (!p[0])
        return false;
    for (const char *c = p; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
            return false;
    return true;
}

static uint64_t launching[APP_COUNT]; /* spawn time, until the app connects */

static int launch(const char *program, const char *arg)
{
    if (!program_name_ok(program))
        return -1;
    /* Apps are single-instance: a running one gets the request instead. */
    int app = app_from_name(program);
    if (app != APP_OTHER) {
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].alive && clients[i].app == app) {
                struct m_launch m;
                memset(&m, 0, sizeof m);
                str_copy(m.program, program, sizeof m.program);
                if (arg)
                    str_copy(m.arg, arg, sizeof m.arg);
                send_to(&clients[i], S_OPEN, 0, &m, sizeof m);
                TEST_EVENT("LAUNCH program=%s arg=%s result=running pid=%d", program, arg ? arg : "", clients[i].pid);
                return clients[i].pid;
            }
        if (launching[app] && gui_now_ms() - launching[app] < 3000)
            return 0; /* already starting */
        launching[app] = gui_now_ms();
    }
    char path[64];
    snprintf(path, sizeof path, "/system/bin/%s", program);
    char *argv[3] = { (char *)program, arg && arg[0] ? (char *)arg : NULL, NULL };
    pid_t pid;
    int r = posix_spawn(&pid, path, NULL, NULL, argv, environ);
    TEST_EVENT("LAUNCH program=%s arg=%s result=%d pid=%d", program, arg ? arg : "", r, r == 0 ? (int)pid : -1);
    return r == 0 ? (int)pid : -1;
}

static void launch_app(int app, const char *arg)
{
    if (app >= 0 && app < APP_OTHER)
        launch(app_programs[app], arg);
}

static void set_theme(bool dark)
{
    theme_set(dark);
    aurora_items[3].label = g_theme->dark ? "Light Appearance" : "Dark Appearance";
    wallpaper_generate(&wallpaper);
    compositor_damage_all();
    struct m_theme m = { dark };
    broadcast(S_THEME, &m, sizeof m);
    save_settings();
    TEST_EVENT("THEME %s", g_theme->dark ? "dark" : "light");
}

static void run_action(int act)
{
    struct window *f = wm_focused();
    switch (act) {
    case ACT_ABOUT: launch("settings", "about"); break;
    case ACT_SETTINGS: launch_app(APP_SETTINGS, NULL); break;
    case ACT_WELCOME: launch_app(APP_WELCOME, NULL); break;
    case ACT_THEME: set_theme(!g_theme->dark); break;
    case ACT_REBOOT: aurora_power(1); break;
    case ACT_POWEROFF: aurora_power(2); break;
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
    TEST_EVENT("MENU open=%s x=%d y=%d w=%d h=%d", which == MENU_AURORA ? "aurora" : "app", menu_rect.x,
               menu_rect.y, menu_rect.w, menu_rect.h);
}

/* ---- resolution -------------------------------------------------------------------- */

static void publish_screen(void)
{
    TEST_EVENT("SCREEN w=%d h=%d", scr_w, scr_h);
    for (int i = 0; i < 4; i++) {
        struct rect r = dock_item_rect(i);
        TEST_EVENT("DOCK_ITEM app=%s x=%d y=%d w=%d h=%d", app_names[i], r.x, r.y, r.w, r.h);
    }
    TEST_EVENT("MENUBAR aurora_x=%d app_x=%d", mb_aurora_rect.x + mb_aurora_rect.w / 2, mb_app_rect.x + 10);
}

static bool set_mode(int32_t w, int32_t h)
{
    struct aurora_display_mode m = { (uint32_t)w, (uint32_t)h };
    if (ioctl(disp_fd, AURORA_DISP_SET_MODE, &m) != 0) {
        TEST_EVENT("MODE_FAIL w=%d h=%d errno=%d", w, h, errno);
        return false;
    }
    struct aurora_display_info info;
    ioctl(disp_fd, AURORA_DISP_INFO, &info);
    scr_w = (int32_t)info.width;
    scr_h = (int32_t)info.height;
    generation = info.generation;
    free(wallpaper.px);
    if (!surface_alloc(&wallpaper, scr_w, scr_h) || !compositor_resize(scr_w, scr_h)) {
        fprintf(stderr, "display: out of memory for %dx%d\n", scr_w, scr_h);
        exit(1);
    }
    wallpaper_generate(&wallpaper);
    cur_x = MIN(cur_x, scr_w - 1);
    cur_y = MIN(cur_y, scr_h - 1);
    close_menu();
    layout_menubar();
    wm_screen_changed();
    struct rect wa = wm_work_area();
    struct m_screen sm = { scr_w, scr_h, { wa.x, wa.y, wa.w, wa.h }, generation };
    broadcast(S_SCREEN, &sm, sizeof sm);
    broadcast_settings();
    publish_screen();
    compositor_damage_all();
    return true;
}

/* ---- input routing ------------------------------------------------------------------- */

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

static void send_mouse(struct window *w, uint8_t kind, uint8_t clicks, int8_t wheel)
{
    if (!w)
        return;
    struct rect c = wm_content_rect(w);
    struct m_mouse m = { cur_x - c.x, cur_y - c.y, kind, buttons, clicks, wheel };
    send_to(w->c, S_MOUSE, w->cid, &m, sizeof m);
}

static void handle_press(void)
{
    TEST_EVENT("CLICK x=%d y=%d", cur_x, cur_y);
    if (menu_open) {
        if (rect_contains(menu_rect, cur_x, cur_y))
            return;
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
    if (w->state == WIN_NORMAL && (w->flags & WINF_RESIZABLE) && cur_x >= w->frame.x + w->frame.w - WM_GRIP &&
        cur_y >= w->frame.y + w->frame.h - WM_GRIP) {
        resize_id = w->id;
        resize_ghost = w->frame;
        return;
    }
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
    uint64_t now = gui_now_ms();
    uint8_t clicks = 1;
    int32_t dx = cur_x - last_click_x, dy = cur_y - last_click_y;
    if (last_click_win == w->id && now - last_click_ms < 450 && dx * dx + dy * dy <= 25)
        clicks = 2;
    last_click_ms = clicks == 2 ? 0 : now;
    last_click_x = cur_x;
    last_click_y = cur_y;
    last_click_win = w->id;
    capture_id = w->id;
    send_mouse(w, MK_DOWN, clicks, 0);
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
                wm_focus(w);
            else
                launch_app(i, NULL);
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
    if (resize_id) {
        struct window *w = wm_get(resize_id);
        resize_id = 0;
        compositor_damage(rect_inset(resize_ghost, -2));
        if (w)
            wm_resize(w, resize_ghost.w, resize_ghost.h);
        return;
    }
    if (drag_id) {
        struct window *w = wm_get(drag_id);
        drag_id = 0;
        if (w)
            wm_report(w);
        return;
    }
    if (capture_id) {
        struct window *w = wm_get(capture_id);
        capture_id = 0;
        send_mouse(w, MK_UP, 1, 0);
    }
}

static void handle_motion(void)
{
    if (resize_id) {
        struct window *w = wm_get(resize_id);
        if (!w) {
            resize_id = 0;
            return;
        }
        struct rect wa = wm_work_area();
        struct rect g = resize_ghost;
        compositor_damage(rect_inset(g, -2));
        int32_t minw = MAX(w->min_w, WM_MIN_W), minh = MAX(w->min_h + TITLE_H, WM_MIN_H);
        int32_t maxw = w->max_w > 0 ? w->max_w : wa.w, maxh = w->max_h > 0 ? w->max_h + TITLE_H : wa.h;
        g.w = CLAMP(cur_x - w->frame.x + 1, minw, MIN(maxw, wa.w));
        g.h = CLAMP(cur_y - w->frame.y + 1, minh, MAX(minh, MIN(maxh, wa.y + wa.h - w->frame.y)));
        resize_ghost = g;
        compositor_damage(rect_inset(g, -2));
        return;
    }
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
            send_mouse(w, MK_MOVE, 0, 0);
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
    struct window *w = (menu_open || cur_y < MENUBAR_H) ? NULL : wm_top_at(cur_x, cur_y);
    if (w && rect_contains(dock_rect(), cur_x, cur_y))
        w = NULL;
    struct window *ctl = (w && cur_y < w->frame.y + TITLE_H && cur_x < w->frame.x + 76) ? w : NULL;
    wm_set_hover_controls(ctl);
    uint32_t hid = w && cur_y >= w->frame.y + TITLE_H ? w->id : 0;
    if (hid != hover_win_id) {
        struct window *old = wm_get(hover_win_id);
        if (old)
            send_mouse(old, MK_LEAVE, 0, 0);
        hover_win_id = hid;
    }
    if (hid)
        send_mouse(w, MK_MOVE, 0, 0);
}

static void handle_mouse(const struct aurora_input_event *e)
{
    int32_t nx = CLAMP(cur_x + e->dx, 0, scr_w - 1);
    int32_t ny = CLAMP(cur_y + e->dy, 0, scr_h - 1);
    if (nx != cur_x || ny != cur_y) {
        cur_x = nx;
        cur_y = ny;
        compositor_move_cursor(cur_x, cur_y);
        handle_motion();
    }
    uint8_t old = buttons;
    buttons = e->buttons;
    if ((buttons & 1) && !(old & 1))
        handle_press();
    else if (!(buttons & 1) && (old & 1))
        handle_release();
    if (e->wheel) {
        struct window *w = (menu_open || cur_y < MENUBAR_H) ? NULL : wm_top_at(cur_x, cur_y);
        TEST_EVENT("WHEEL delta=%d win=%u", e->wheel, w ? w->id : 0);
        if (w && cur_y >= w->frame.y + TITLE_H)
            send_mouse(w, MK_WHEEL, 0, e->wheel);
    }
}

static bool handle_shortcut(const struct key_event *e)
{
    if (e->key == KEY_ESC && menu_open) {
        close_menu();
        return true;
    }
    /* Alt+Tab cycles windows (Alt+Shift+Tab too, in the same order). */
    if (e->key == KEY_TAB && (e->mods & MOD_ALT) && !(e->mods & MOD_CTRL)) {
        wm_cycle();
        TEST_EVENT("ALT_TAB");
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
    case '1': launch_app(APP_TERMINAL, NULL); return true;
    case '2': launch_app(APP_FILES, NULL); return true;
    case '3': launch_app(APP_NOTES, NULL); return true;
    case '4': launch_app(APP_SETTINGS, NULL); return true;
    default: return false;
    }
}

static void deliver_key(const struct key_out *o, bool repeat)
{
    const struct key_event *e = &o->key;
    if (e->pressed && !repeat) {
        TEST_EVENT("KEY key=%u ascii=%u mods=%u", e->key, e->ascii, e->mods);
        if (handle_shortcut(e))
            return;
    }
    struct window *f = wm_focused();
    if (!f)
        return;
    struct m_key k = { e->key, 0, e->ascii, e->mods, e->pressed, repeat };
    send_to(f->c, S_KEY, f->cid, &k, sizeof k);
    if (o->text[0] && e->pressed) {
        struct m_text t;
        memset(&t, 0, sizeof t);
        str_copy(t.utf8, o->text, sizeof t.utf8);
        send_to(f->c, S_TEXT, f->cid, &t, sizeof t);
    }
}

static bool key_is_down(uint16_t sc)
{
    for (int i = 0; i < 8; i++)
        if (keys_down[i] == sc)
            return true;
    return false;
}

static void set_key_down(uint16_t sc, bool on)
{
    for (int i = 0; i < 8; i++) {
        if (on && !keys_down[i]) {
            keys_down[i] = sc;
            return;
        }
        if (!on && keys_down[i] == sc)
            keys_down[i] = 0;
    }
}

static void handle_key(const struct aurora_input_event *e)
{
    bool pressed = (e->flags & AURORA_INPUT_PRESSED) != 0;
    if (pressed && key_is_down(e->code))
        return; /* hardware typematic: the server repeats keys itself */
    set_key_down(e->code, pressed);
    struct key_out o;
    if (!keymap_feed(e->code, pressed, &o))
        return;
    if (o.key.key == KEY_CAPSLOCK && pressed)
        ioctl(input_fd, AURORA_INPUT_SET_LEDS, (unsigned long)(keymap_caps() ? 4 : 0));
    if (pressed && !keymap_is_modifier(o.key.key) && repeat_rate) {
        repeat_sc = e->code;
        repeat_next = gui_now_ms() + repeat_delay;
    } else if (!pressed && e->code == repeat_sc) {
        repeat_sc = 0;
    }
    deliver_key(&o, false);
}

static void key_repeat(uint64_t now)
{
    if (!repeat_sc || now < repeat_next)
        return;
    repeat_next = now + 1000 / MAX(repeat_rate, 1u);
    struct key_out o;
    if (keymap_feed(repeat_sc, true, &o)) {
        o.key.mods = keymap_mods();
        deliver_key(&o, true);
    }
}

/* ---- messages from apps --------------------------------------------------------------- */

static void handle_message(struct client *c, uint8_t *buf, long n, int *fds, unsigned nfds)
{
    if (n < (long)sizeof(struct msg_hdr)) {
        for (unsigned i = 0; i < nfds; i++)
            close(fds[i]);
        return;
    }
    struct msg_hdr h;
    memcpy(&h, buf, sizeof h);
    uint8_t *p = buf + sizeof h;
    size_t len = (size_t)n - sizeof h;
    struct window *w = h.win ? wm_lookup(c, h.win) : NULL;
#define NEED(T) if (len < sizeof(T)) break
    switch (h.type) {
    case C_HELLO: {
        NEED(struct m_hello);
        struct m_hello m;
        memcpy(&m, p, sizeof m);
        m.name[sizeof m.name - 1] = 0;
        c->app = m.app >= 0 && m.app < APP_COUNT ? m.app : APP_OTHER;
        launching[c->app] = 0;
        str_copy(c->name, m.name, sizeof c->name);
        c->pid = m.pid;
        struct rect wa = wm_work_area();
        struct m_welcome wel = { PROTO_VERSION, scr_w, scr_h, { wa.x, wa.y, wa.w, wa.h }, g_theme->dark, { 0 } };
        send_to(c, S_WELCOME, 0, &wel, sizeof wel);
        TEST_EVENT("CLIENT app=%s pid=%d", c->name, c->pid);
        break;
    }
    case C_CREATE: {
        NEED(struct m_create);
        struct m_create m;
        memcpy(&m, p, sizeof m);
        m.title[sizeof m.title - 1] = 0;
        if (!wm_create(c, h.win, &m)) {
            struct m_name e = { "create" };
            send_to(c, S_ERROR, h.win, &e, sizeof e);
        }
        break;
    }
    case C_DESTROY:
        if (w)
            wm_destroy(w);
        break;
    case C_ATTACH: {
        if (len < sizeof(struct m_attach) || nfds < 1 || !w)
            break;
        struct m_attach m;
        memcpy(&m, p, sizeof m);
        wm_attach(w, m.buffer, fds[0], &m);
        fds[0] = -1;
        break;
    }
    case C_COMMIT: {
        NEED(struct m_commit);
        if (!w)
            break;
        struct m_commit m;
        memcpy(&m, p, sizeof m);
        wm_commit(w, &m);
        break;
    }
    case C_SET_TITLE: {
        NEED(struct m_title);
        if (!w)
            break;
        struct m_title m;
        memcpy(&m, p, sizeof m);
        m.title[sizeof m.title - 1] = 0;
        wm_set_title(w, m.title);
        break;
    }
    case C_WINDOW_OP: {
        NEED(struct m_op);
        if (!w)
            break;
        struct m_op m;
        memcpy(&m, p, sizeof m);
        if (m.op == OP_MINIMIZE)
            wm_minimize(w);
        else if (m.op == OP_MAXIMIZE)
            wm_toggle_maximize(w);
        else if (m.op == OP_FOCUS || m.op == OP_RESTORE)
            wm_focus(w);
        break;
    }
    case C_SET_CLIPBOARD: {
        NEED(struct m_clip);
        struct m_clip m;
        memcpy(&m, p, sizeof m);
        if (m.len > len - sizeof m || m.len > CLIP_MAX)
            break;
        char *d = malloc(m.len + 1);
        if (!d)
            break;
        memcpy(d, p + sizeof m, m.len);
        d[m.len] = 0;
        free(clip_data);
        clip_data = d;
        clip_len = m.len;
        memcpy(clip_mime, m.mime, sizeof clip_mime);
        clip_mime[sizeof clip_mime - 1] = 0;
        TEST_EVENT("CLIP_SET len=%u crc=%08x from=%s", clip_len, crc32_update(0, clip_data, clip_len), c->name);
        break;
    }
    case C_GET_CLIPBOARD: {
        static uint8_t out[sizeof(struct m_clip) + CLIP_MAX];
        struct m_clip m;
        memset(&m, 0, sizeof m);
        memcpy(m.mime, clip_mime, sizeof m.mime);
        m.len = clip_data ? clip_len : 0;
        memcpy(out, &m, sizeof m);
        if (m.len)
            memcpy(out + sizeof m, clip_data, m.len);
        send_to(c, S_CLIPBOARD, h.win, out, sizeof m + m.len);
        TEST_EVENT("CLIP_GET len=%u to=%s", m.len, c->name);
        break;
    }
    case C_LAUNCH: {
        NEED(struct m_launch);
        struct m_launch m;
        memcpy(&m, p, sizeof m);
        m.program[sizeof m.program - 1] = 0;
        m.arg[sizeof m.arg - 1] = 0;
        launch(m.program, m.arg);
        break;
    }
    case C_SET_THEME: {
        NEED(struct m_theme);
        if (p[0] != (g_theme->dark ? 1 : 0))
            set_theme(p[0] != 0);
        break;
    }
    case C_SET_MODE: {
        NEED(struct m_size);
        struct m_size m;
        memcpy(&m, p, sizeof m);
        if (m.w != scr_w || m.h != scr_h)
            set_mode(m.w, m.h);
        break;
    }
    case C_SET_KEYMAP: {
        NEED(struct m_name);
        struct m_name m;
        memcpy(&m, p, sizeof m);
        m.name[sizeof m.name - 1] = 0;
        if (keymap_load(m.name)) {
            save_settings();
            broadcast_settings();
        }
        break;
    }
    case C_SET_REPEAT: {
        NEED(struct m_repeat);
        struct m_repeat m;
        memcpy(&m, p, sizeof m);
        repeat_delay = CLAMP(m.delay_ms, 150u, 2000u);
        repeat_rate = MIN(m.rate_hz, 60u);
        TEST_EVENT("REPEAT delay=%u rate=%u", repeat_delay, repeat_rate);
        save_settings();
        broadcast_settings();
        break;
    }
    case C_GET_SETTINGS: {
        struct m_settings s;
        fill_settings(&s);
        send_to(c, S_SETTINGS, 0, &s, sizeof s);
        break;
    }
    case C_POWER: {
        NEED(struct m_op);
        struct m_op m;
        memcpy(&m, p, sizeof m);
        if (m.op == 1 || m.op == 2)
            aurora_power((int)m.op);
        break;
    }
    default:
        break;
    }
#undef NEED
    for (unsigned i = 0; i < nfds; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

static void accept_clients(void)
{
    for (;;) {
        int fd = aurora_accept(listen_fd, O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
            return;
        struct client *c = NULL;
        for (int i = 0; i < MAX_CLIENTS && !c; i++)
            if (!clients[i].alive && clients[i].fd < 0)
                c = &clients[i];
        if (!c) {
            close(fd);
            continue;
        }
        memset(c, 0, sizeof *c);
        c->fd = fd;
        c->alive = true;
        c->app = APP_OTHER;
    }
}

static void read_client(struct client *c)
{
    static uint8_t buf[PROTO_MAX_MSG];
    for (int k = 0; k < 64 && c->alive; k++) {
        int fds[AURORA_MSG_FDS];
        unsigned nfds = AURORA_MSG_FDS;
        long n = proto_recv(c->fd, buf, sizeof buf, fds, &nfds, true);
        if (n == 0) {
            c->alive = false;
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN)
                c->alive = false;
            return;
        }
        handle_message(c, buf, n, fds, nfds);
    }
}

static void read_input(void)
{
    struct aurora_input_event ev[64];
    for (;;) {
        ssize_t n = read(input_fd, ev, sizeof ev);
        if (n <= 0)
            break;
        bool moved = false;
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; i++) {
            if (ev[i].type == AURORA_INPUT_KEY) {
                handle_key(&ev[i]);
            } else if (ev[i].type == AURORA_INPUT_MOUSE) {
                int32_t ox = cur_x, oy = cur_y;
                handle_mouse(&ev[i]);
                moved |= ox != cur_x || oy != cur_y;
            }
        }
        if (moved)
            TEST_EVENT("CURSOR x=%d y=%d", cur_x, cur_y);
    }
}

/* ---- main ---------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    signal(SIGCHLD, SIG_IGN); /* apps are reaped by the kernel */
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
    disp_fd = open("/dev/display", O_RDWR | O_CLOEXEC);
    input_fd = open("/dev/input", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (disp_fd < 0 || input_fd < 0) {
        fprintf(stderr, "display: cannot open %s: %s\n", disp_fd < 0 ? "/dev/display" : "/dev/input",
                strerror(errno));
        return 1;
    }
    listen_fd = aurora_local_listen(DISPLAY_SOCKET, 16, O_CLOEXEC | O_NONBLOCK);
    if (listen_fd < 0) {
        fprintf(stderr, "display: cannot listen on %s: %s\n", DISPLAY_SOCKET, strerror(errno));
        return 1;
    }
    bool fonts = text_init();
    load_settings();
    aurora_items[3].label = g_theme->dark ? "Light Appearance" : "Dark Appearance";
    struct aurora_display_info info;
    if (ioctl(disp_fd, AURORA_DISP_INFO, &info) != 0)
        return 1;
    scr_w = (int32_t)info.width;
    scr_h = (int32_t)info.height;
    generation = info.generation;
    if (!surface_alloc(&wallpaper, scr_w, scr_h) || !compositor_init(disp_fd, scr_w, scr_h, compose, paint_cursor)) {
        fprintf(stderr, "display: cannot allocate %dx%d buffers\n", scr_w, scr_h);
        return 1;
    }
    wallpaper_generate(&wallpaper);
    cur_x = scr_w / 2;
    cur_y = scr_h / 2;
    compositor_move_cursor(cur_x, cur_y);
    layout_menubar();
    publish_screen();
    TEST_EVENT("CURSOR x=%d y=%d", cur_x, cur_y);
    TEST_EVENT("FONTS truetype=%d", fonts ? 1 : 0);
    compositor_frame();
    printf("AURORA_DESKTOP_READY %dx%d\n", scr_w, scr_h);
    fflush(stdout);
    TEST_EVENT("DISPLAY_READY w=%d h=%d pid=%d", scr_w, scr_h, (int)getpid());
    report_mem("startup");
    launch_app(APP_WELCOME, NULL);

    uint64_t last_frame = 0;
    struct rect old_app = mb_app_rect;
    for (;;) {
        struct pollfd pfd[2 + MAX_CLIENTS];
        struct client *who[2 + MAX_CLIENTS];
        int n = 0;
        pfd[n].fd = listen_fd;
        pfd[n].events = POLLIN;
        who[n++] = NULL;
        pfd[n].fd = input_fd;
        pfd[n].events = POLLIN;
        who[n++] = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i].alive) {
                pfd[n].fd = clients[i].fd;
                pfd[n].events = POLLIN;
                who[n++] = &clients[i];
            }
        uint64_t now = gui_now_ms();
        int timeout = (int)(1000 - now % 1000);
        if (compositor_pending())
            timeout = (int)MAX(0, (int64_t)16 - (int64_t)(now - last_frame));
        if (repeat_sc)
            timeout = (int)MIN((int64_t)timeout, MAX(0, (int64_t)repeat_next - (int64_t)now));
        poll(pfd, (nfds_t)n, timeout);
        if (pfd[0].revents)
            accept_clients();
        if (pfd[1].revents)
            read_input();
        for (int i = 2; i < n; i++)
            if (pfd[i].revents && who[i]->alive)
                read_client(who[i]);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i].alive && clients[i].fd >= 0)
                drop_client(&clients[i]);
        now = gui_now_ms();
        key_repeat(now);
        if (now / 1000 != last_second) {
            last_second = now / 1000;
            compositor_damage(mb_clock_rect);
        }
        layout_menubar();
        if (old_app.w != mb_app_rect.w) {
            compositor_damage(rect_make(0, 0, scr_w, MENUBAR_H));
            old_app = mb_app_rect;
        }
        if (compositor_pending() && now - last_frame >= 16) {
            compositor_frame();
            last_frame = now;
        }
    }
}
