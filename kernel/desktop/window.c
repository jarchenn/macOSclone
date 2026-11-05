/* Window manager: z-order, focus, geometry and window chrome painting.
 * Windows are referenced by stable ids; callers that keep references across
 * events must re-validate them with wm_get(id). */
#include "desktop/desktop.h"
#include "core/log.h"
#include "kalloc.h"
#include "lib.h"

static struct window *zorder[MAX_WINDOWS]; /* [0] = bottom */
static int nwin;
static uint32_t next_id = 1;
static uint32_t hover_controls; /* id of window whose controls are hovered */

int wm_count(void) { return nwin; }
struct window *wm_at_z(int i) { return (i >= 0 && i < nwin) ? zorder[i] : NULL; }

struct window *wm_get(uint32_t id)
{
    for (int i = 0; i < nwin; i++)
        if (zorder[i]->id == id)
            return zorder[i];
    return NULL;
}

struct window *wm_find_app(int app)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (zorder[i]->app == app)
            return zorder[i];
    return NULL;
}

struct window *wm_focused(void)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (zorder[i]->state != WIN_MINIMIZED)
            return zorder[i];
    return NULL;
}

struct rect wm_content_rect(const struct window *w)
{
    return rect_make(w->frame.x, w->frame.y + TITLE_H, w->frame.w, w->frame.h - TITLE_H);
}

/* Area affected when the window changes: frame plus shadow. */
struct rect wm_damage_rect(const struct window *w)
{
    struct rect r = rect_inset(w->frame, -16);
    r.h += 6;
    return r;
}

struct rect wm_work_area(void)
{
    struct rect d = dock_rect();
    int32_t bottom = d.y - 6;
    return rect_make(0, MENUBAR_H, desktop_width(), bottom - MENUBAR_H);
}

__attribute__((unused)) static const char *state_name(enum win_state s)
{
    return s == WIN_MINIMIZED ? "minimized" : s == WIN_MAXIMIZED ? "maximized" : "normal";
}

static void report_geometry(struct window *w)
{
    UNUSED(w);
    TEST_EVENT("WIN id=%u app=%s x=%d y=%d w=%d h=%d state=%s", w->id, app_names[w->app], w->frame.x,
               w->frame.y, w->frame.w, w->frame.h, state_name(w->state));
}

static void report_focus(void)
{
    struct window *f = wm_focused();
    UNUSED(f);
    TEST_EVENT("FOCUS id=%u app=%s", f ? f->id : 0, f ? app_names[f->app] : "none");
}

static void clamp_position(struct window *w, int32_t *x, int32_t *y)
{
    int32_t sw = desktop_width(), sh = desktop_height();
    /* Keep the title bar reachable: at least 80 px horizontally on screen,
     * never above the menu bar, never below the bottom edge. */
    if (*x < 80 - w->frame.w)
        *x = 80 - w->frame.w;
    if (*x > sw - 80)
        *x = sw - 80;
    if (*y < MENUBAR_H)
        *y = MENUBAR_H;
    if (*y > sh - TITLE_H)
        *y = sh - TITLE_H;
}

struct window *wm_create(int app, const char *title, int32_t cw, int32_t ch, const struct window_ops *ops, void *data)
{
    if (nwin >= MAX_WINDOWS) {
        klog("wm: window limit (%d) reached", MAX_WINDOWS);
        return NULL;
    }
    struct rect work = wm_work_area();
    cw = CLAMP(cw, 200, work.w);
    ch = CLAMP(ch, 100, work.h - TITLE_H);
    struct window *w = kzalloc(sizeof *w);
    if (!w)
        return NULL;
    if (!surface_alloc(&w->content, cw, ch)) {
        kfree(w);
        return NULL;
    }
    w->id = next_id++;
    w->app = app;
    str_copy(w->title, title, sizeof w->title);
    /* Cascade new windows per app so they do not stack exactly. */
    static const int32_t offs[APP_COUNT][2] = { { 70, 70 }, { 150, 110 }, { 250, 80 }, { 330, 120 }, { 0, 0 } };
    int32_t x, y;
    if (app == APP_WELCOME || app >= APP_COUNT) {
        x = (desktop_width() - cw) / 2;
        y = MENUBAR_H + (work.h - ch - TITLE_H) / 2;
    } else {
        x = work.x + offs[app][0];
        y = work.y + offs[app][1] - 40;
        if (x + cw > work.x + work.w)
            x = work.x + work.w - cw - 10;
        if (y + ch + TITLE_H > work.y + work.h)
            y = work.y + work.h - ch - TITLE_H;
    }
    w->frame = rect_make(x, y, cw, ch + TITLE_H);
    clamp_position(w, &w->frame.x, &w->frame.y);
    w->restore_frame = w->frame;
    w->state = WIN_NORMAL;
    w->ops = ops;
    w->data = data;
    w->dirty = true;
    struct window *old = wm_focused();
    if (old) {
        old->dirty = true; /* repaint its chrome as inactive */
        desktop_damage(wm_damage_rect(old));
    }
    zorder[nwin++] = w;
    TEST_EVENT("WIN_OPEN id=%u app=%s", w->id, app_names[app]);
    report_geometry(w);
    report_focus();
    desktop_damage(wm_damage_rect(w));
    desktop_damage(rect_make(0, 0, desktop_width(), MENUBAR_H));
    desktop_damage(dock_rect());
    return w;
}

static int z_index(struct window *w)
{
    for (int i = 0; i < nwin; i++)
        if (zorder[i] == w)
            return i;
    return -1;
}

void wm_destroy(struct window *w)
{
    int i = z_index(w);
    if (i < 0)
        return;
    struct rect dmg = wm_damage_rect(w);
    for (int k = i; k < nwin - 1; k++)
        zorder[k] = zorder[k + 1];
    nwin--;
    zorder[nwin] = NULL;
    uint32_t id = w->id;
    if (hover_controls == id)
        hover_controls = 0;
    if (w->ops && w->ops->destroyed)
        w->ops->destroyed(w);
    surface_free(&w->content);
    kfree(w);
    TEST_EVENT("WIN_CLOSE id=%u", id);
    UNUSED(id);
    report_focus();
    desktop_damage(dmg);
    desktop_damage(rect_make(0, 0, desktop_width(), MENUBAR_H));
    desktop_damage(dock_rect());
}

void wm_request_close(struct window *w)
{
    if (!w)
        return;
    if (w->ops && w->ops->close_request && !w->ops->close_request(w))
        return; /* vetoed: the app will call wm_destroy itself */
    wm_destroy(w);
}

void wm_focus(struct window *w)
{
    int i = z_index(w);
    if (i < 0)
        return;
    struct window *old = wm_focused();
    if (w->state == WIN_MINIMIZED) {
        wm_restore(w);
        return;
    }
    if (i != nwin - 1) {
        for (int k = i; k < nwin - 1; k++)
            zorder[k] = zorder[k + 1];
        zorder[nwin - 1] = w;
    }
    if (old != w) {
        if (old) {
            old->dirty = true; /* repaint inactive chrome/caret */
            desktop_damage(wm_damage_rect(old));
        }
        w->dirty = true;
        desktop_damage(wm_damage_rect(w));
        desktop_damage(rect_make(0, 0, desktop_width(), MENUBAR_H));
        report_focus();
    }
}

void wm_minimize(struct window *w)
{
    if (!w || w->state == WIN_MINIMIZED)
        return;
    w->prev_state = w->state;
    w->state = WIN_MINIMIZED;
    /* Move to the bottom so focus passes to the next window. */
    int i = z_index(w);
    for (int k = i; k > 0; k--)
        zorder[k] = zorder[k - 1];
    zorder[0] = w;
    report_geometry(w);
    report_focus();
    struct window *f = wm_focused();
    if (f)
        f->dirty = true;
    desktop_damage(wm_damage_rect(w));
    desktop_damage(rect_make(0, 0, desktop_width(), MENUBAR_H));
    desktop_damage(dock_rect());
}

static bool resize_content(struct window *w, struct rect frame)
{
    struct surface ns;
    if (!surface_alloc(&ns, frame.w, frame.h - TITLE_H))
        return false;
    surface_free(&w->content);
    w->content = ns;
    w->frame = frame;
    w->dirty = true;
    if (w->ops && w->ops->resized)
        w->ops->resized(w);
    return true;
}

void wm_restore(struct window *w)
{
    if (!w)
        return;
    if (w->state == WIN_MINIMIZED) {
        w->state = w->prev_state == WIN_MAXIMIZED ? WIN_MAXIMIZED : WIN_NORMAL;
        int i = z_index(w);
        struct window *old = wm_focused();
        for (int k = i; k < nwin - 1; k++)
            zorder[k] = zorder[k + 1];
        zorder[nwin - 1] = w;
        if (old) {
            old->dirty = true;
            desktop_damage(wm_damage_rect(old));
        }
        w->dirty = true;
        report_geometry(w);
        report_focus();
        desktop_damage(wm_damage_rect(w));
        desktop_damage(rect_make(0, 0, desktop_width(), MENUBAR_H));
        desktop_damage(dock_rect());
    }
}

void wm_toggle_maximize(struct window *w)
{
    if (!w || w->state == WIN_MINIMIZED)
        return;
    struct rect before = wm_damage_rect(w);
    if (w->state == WIN_MAXIMIZED) {
        if (resize_content(w, w->restore_frame))
            w->state = WIN_NORMAL;
    } else {
        struct rect wa = wm_work_area();
        struct rect target = rect_make(wa.x + 6, wa.y + 4, wa.w - 12, wa.h - 4);
        struct rect saved = w->frame;
        if (resize_content(w, target)) {
            w->restore_frame = saved;
            w->state = WIN_MAXIMIZED;
        } else {
            klog("wm: not enough memory to maximize window %u", w->id);
        }
    }
    report_geometry(w);
    desktop_damage(before);
    desktop_damage(wm_damage_rect(w));
}

void wm_move(struct window *w, int32_t x, int32_t y)
{
    clamp_position(w, &x, &y);
    if (x == w->frame.x && y == w->frame.y)
        return;
    struct rect before = wm_damage_rect(w);
    w->frame.x = x;
    w->frame.y = y;
    if (w->state == WIN_MAXIMIZED) {
        w->state = WIN_NORMAL; /* dragging a maximized window keeps its size */
        w->restore_frame = w->frame;
    } else {
        w->restore_frame = w->frame;
    }
    desktop_damage(before);
    desktop_damage(wm_damage_rect(w));
}

void wm_cycle(void)
{
    /* Bring the bottom-most visible window to the front. */
    for (int i = 0; i < nwin; i++) {
        if (zorder[i]->state != WIN_MINIMIZED && zorder[i] != wm_focused()) {
            wm_focus(zorder[i]);
            return;
        }
    }
}

struct window *wm_top_at(int32_t x, int32_t y)
{
    for (int i = nwin - 1; i >= 0; i--) {
        struct window *w = zorder[i];
        if (w->state != WIN_MINIMIZED && rect_contains(w->frame, x, y))
            return w;
    }
    return NULL;
}

void wm_invalidate(struct window *w)
{
    if (w)
        w->dirty = true;
}

void wm_invalidate_all(void)
{
    for (int i = 0; i < nwin; i++)
        zorder[i]->dirty = true;
}

void wm_set_title(struct window *w, const char *title)
{
    if (strcmp(w->title, title) == 0)
        return;
    str_copy(w->title, title, sizeof w->title);
    desktop_damage(rect_make(w->frame.x, w->frame.y, w->frame.w, TITLE_H));
}

void wm_set_hover_controls(struct window *w)
{
    uint32_t id = w ? w->id : 0;
    if (id == hover_controls)
        return;
    struct window *old = wm_get(hover_controls);
    if (old)
        desktop_damage(rect_make(old->frame.x, old->frame.y, 80, TITLE_H));
    hover_controls = id;
    if (w)
        desktop_damage(rect_make(w->frame.x, w->frame.y, 80, TITLE_H));
}

uint32_t wm_hover_controls(void) { return hover_controls; }

void wm_render_dirty(void)
{
    for (int i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        if (!w->dirty)
            continue;
        w->dirty = false;
        if (w->state == WIN_MINIMIZED)
            continue;
        surface_reset_clip(&w->content);
        if (w->ops && w->ops->render)
            w->ops->render(w, &w->content);
        desktop_damage(wm_content_rect(w));
    }
}

static void draw_control(struct surface *s, int32_t cx, int32_t cy, uint32_t color, int glyph, bool show)
{
    draw_circle(s, cx, cy, CTRL_R, color, 255);
    draw_round_frame(s, rect_make(cx - CTRL_R, cy - CTRL_R, CTRL_R * 2, CTRL_R * 2), CTRL_R, RGB(0, 0, 0), 40);
    if (!show)
        return;
    uint32_t g = RGB(0x4A, 0x10, 0x0C);
    if (glyph == 0) { /* x */
        for (int i = -2; i <= 2; i++) {
            draw_pixel_blend(s, cx + i, cy + i, g, 220);
            draw_pixel_blend(s, cx + i, cy - i, g, 220);
        }
    } else if (glyph == 1) { /* - */
        draw_fill(s, rect_make(cx - 3, cy, 6, 1), RGB(0x6A, 0x45, 0x00));
    } else { /* + */
        draw_fill(s, rect_make(cx - 3, cy, 7, 1), RGB(0x0A, 0x4A, 0x16));
        draw_fill(s, rect_make(cx, cy - 3, 1, 7), RGB(0x0A, 0x4A, 0x16));
    }
}

void wm_paint(struct surface *s, struct window *w, bool focused)
{
    const struct theme *t = g_theme;
    struct rect f = w->frame;
    draw_shadow(s, rect_make(f.x, f.y + 4, f.w, f.h), WIN_RADIUS, focused ? 18 : 12, focused ? 90 : 55);
    /* Title bar with rounded top corners. */
    draw_round_rect(s, rect_make(f.x, f.y, f.w, TITLE_H + WIN_RADIUS), WIN_RADIUS,
                    focused ? t->title_active : t->title_inactive, 255, 1 | 2);
    draw_hline(s, f.x, f.y + TITLE_H - 1, f.w, t->separator);
    /* Title text, centred. */
    int32_t tw = text_width(FONT, w->title);
    int32_t maxw = f.w - 2 * 80;
    int32_t tx = f.x + (f.w - MIN(tw, maxw)) / 2;
    draw_text_fit(s, FONT, tx, f.y + (TITLE_H - 16) / 2, w->title, maxw,
                  focused ? t->title_text : t->title_text_inactive);
    /* Controls: coloured when focused or hovered. */
    bool hov = hover_controls == w->id;
    bool col = focused || hov;
    draw_control(s, f.x + CTRL_CLOSE_X, f.y + CTRL_Y, col ? RGB(0xFF, 0x5F, 0x57) : t->ctrl_inactive, 0, hov);
    draw_control(s, f.x + CTRL_MIN_X, f.y + CTRL_Y, col ? RGB(0xFE, 0xBC, 0x2E) : t->ctrl_inactive, 1, hov);
    draw_control(s, f.x + CTRL_MAX_X, f.y + CTRL_Y, col ? RGB(0x28, 0xC8, 0x40) : t->ctrl_inactive, 2, hov);
    /* Content with rounded bottom corners. */
    blit_round_bottom(s, &w->content, f.x, f.y + TITLE_H, WIN_RADIUS);
    draw_round_frame(s, f, WIN_RADIUS, t->win_border, t->border_alpha);
}
