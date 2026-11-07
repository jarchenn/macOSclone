/* Window manager: z-order, focus, geometry, configure exchanges and the
 * server-side decorations. Windows are referenced by stable ids; anything
 * kept across events is re-validated with wm_get(id). */
#include "server.h"
#include "lib.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static struct window *zorder[MAX_WINDOWS]; /* [0] = bottom */
static int nwin;
static uint32_t next_id = 1;
static uint32_t hover_controls;

int wm_count(void) { return nwin; }
struct window *wm_at_z(int i) { return (i >= 0 && i < nwin) ? zorder[i] : NULL; }

struct window *wm_get(uint32_t id)
{
    for (int i = 0; i < nwin; i++)
        if (zorder[i]->id == id)
            return zorder[i];
    return NULL;
}

struct window *wm_lookup(struct client *c, uint32_t cid)
{
    for (int i = 0; i < nwin; i++)
        if (zorder[i]->c == c && zorder[i]->cid == cid)
            return zorder[i];
    return NULL;
}

struct window *wm_find_app(int app)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (zorder[i]->app == app && !(zorder[i]->flags & WINF_DIALOG))
            return zorder[i];
    return NULL;
}

struct window *wm_focused(void)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (zorder[i]->state != WIN_MINIMIZED && zorder[i]->mapped)
            return zorder[i];
    return NULL;
}

struct rect wm_content_rect(const struct window *w)
{
    return rect_make(w->frame.x, w->frame.y + TITLE_H, w->frame.w, w->frame.h - TITLE_H);
}

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

static const char *state_name(enum win_state s)
{
    return s == WIN_MINIMIZED ? "minimized" : s == WIN_MAXIMIZED ? "maximized" : "normal";
}

void wm_report(struct window *w)
{
    TEST_EVENT("WIN id=%u app=%s x=%d y=%d w=%d h=%d state=%s", w->id, app_names[w->app], w->frame.x, w->frame.y,
               w->frame.w, w->frame.h, state_name(w->state));
}

static void report_focus(void)
{
    struct window *f = wm_focused();
    TEST_EVENT("FOCUS id=%u app=%s", f ? f->id : 0, f ? app_names[f->app] : "none");
}

static void damage_chrome(void)
{
    desktop_damage(rect_make(0, 0, desktop_width(), MENUBAR_H));
    desktop_damage(dock_rect());
}

static void clamp_position(struct window *w, int32_t *x, int32_t *y)
{
    int32_t sw = desktop_width(), sh = desktop_height();
    if (*x < 80 - w->frame.w)
        *x = 80 - w->frame.w;
    if (*x > sw - 80)
        *x = sw - 80;
    if (*y < MENUBAR_H)
        *y = MENUBAR_H;
    if (*y > sh - TITLE_H)
        *y = sh - TITLE_H;
}

/* Content size limits for w, also bounded by the work area. */
static void limits(const struct window *w, int32_t *minw, int32_t *minh, int32_t *maxw, int32_t *maxh)
{
    struct rect wa = wm_work_area();
    *minw = MAX(w->min_w, WM_MIN_W);
    *minh = MAX(w->min_h, WM_MIN_H - TITLE_H);
    *maxw = w->max_w > 0 ? MIN(w->max_w, wa.w) : wa.w;
    *maxh = w->max_h > 0 ? MIN(w->max_h, wa.h - TITLE_H) : wa.h - TITLE_H;
    if (*maxw < *minw)
        *maxw = *minw;
    if (*maxh < *minh)
        *maxh = *minh;
}

static void configure(struct window *w)
{
    struct m_configure m = { w->frame.w, w->frame.h - TITLE_H, ++w->cfg_serial,
                             w->state == WIN_MAXIMIZED ? WS_MAXIMIZED : w->state == WIN_MINIMIZED ? WS_MINIMIZED
                                                                                                   : WS_NORMAL };
    send_to(w->c, S_CONFIGURE, w->cid, &m, sizeof m);
}

struct window *wm_create(struct client *c, uint32_t cid, const struct m_create *m)
{
    if (nwin >= MAX_WINDOWS || wm_lookup(c, cid))
        return NULL;
    struct window *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->id = next_id++;
    w->c = c;
    w->cid = cid;
    w->app = c->app;
    w->flags = m->flags;
    w->min_w = MAX(m->min_w, 0);
    w->min_h = MAX(m->min_h, 0);
    w->max_w = MAX(m->max_w, 0);
    w->max_h = MAX(m->max_h, 0);
    w->shown = -1;
    memcpy(w->title, m->title, sizeof w->title);
    w->title[sizeof w->title - 1] = 0;
    int32_t minw, minh, maxw, maxh;
    limits(w, &minw, &minh, &maxw, &maxh);
    int32_t cw = CLAMP(m->w, minw, maxw), ch = CLAMP(m->h, minh, maxh);
    struct rect work = wm_work_area();
    static const int32_t offs[APP_COUNT][2] = { { 70, 70 }, { 150, 110 }, { 250, 80 }, { 330, 120 }, { 0, 0 },
                                                { 120, 60 } };
    int32_t x, y;
    if (w->app == APP_WELCOME || (w->flags & WINF_DIALOG)) {
        x = (desktop_width() - cw) / 2;
        y = MENUBAR_H + (work.h - ch - TITLE_H) / 2;
    } else {
        x = work.x + offs[w->app][0];
        y = work.y + offs[w->app][1] - 40;
        if (x + cw > work.x + work.w)
            x = work.x + work.w - cw - 10;
        if (y + ch + TITLE_H > work.y + work.h)
            y = work.y + work.h - ch - TITLE_H;
    }
    w->frame = rect_make(x, y, cw, ch + TITLE_H);
    clamp_position(w, &w->frame.x, &w->frame.y);
    w->restore_frame = w->frame;
    w->state = WIN_NORMAL;
    for (int i = 0; i < 2; i++)
        w->buf[i].fd = -1;
    zorder[nwin++] = w;
    /* The app learns its actual size (limits applied) before drawing. */
    configure(w);
    return w;
}

static int z_index(struct window *w)
{
    for (int i = 0; i < nwin; i++)
        if (zorder[i] == w)
            return i;
    return -1;
}

static void buffer_free(struct buffer *b)
{
    if (b->px)
        munmap(b->px, b->bytes);
    if (b->fd >= 0)
        close(b->fd);
    memset(b, 0, sizeof *b);
    b->fd = -1;
}

void wm_destroy(struct window *w)
{
    int i = z_index(w);
    if (i < 0)
        return;
    struct window *old_focus = wm_focused();
    struct rect dmg = wm_damage_rect(w);
    for (int k = i; k < nwin - 1; k++)
        zorder[k] = zorder[k + 1];
    nwin--;
    zorder[nwin] = NULL;
    uint32_t id = w->id;
    if (hover_controls == id)
        hover_controls = 0;
    bool was_mapped = w->mapped;
    for (int b = 0; b < 2; b++)
        buffer_free(&w->buf[b]);
    free(w);
    if (was_mapped) {
        TEST_EVENT("WIN_CLOSE id=%u", id);
        report_focus();
    }
    struct window *nf = wm_focused();
    if (old_focus == w && nf)
        send_focus_change(NULL, nf);
    desktop_damage(dmg);
    damage_chrome();
}

void wm_destroy_client(struct client *c)
{
    for (int i = nwin - 1; i >= 0; i--)
        if (i < nwin && zorder[i]->c == c) {
            wm_destroy(zorder[i]);
            i = nwin; /* the list shifted: start over from the top */
        }
}

void wm_request_close(struct window *w)
{
    if (w)
        send_to(w->c, S_CLOSE_REQUEST, w->cid, NULL, 0);
}

static void raise_to_top(struct window *w)
{
    int i = z_index(w);
    for (int k = i; k < nwin - 1; k++)
        zorder[k] = zorder[k + 1];
    zorder[nwin - 1] = w;
}

void wm_focus(struct window *w)
{
    if (z_index(w) < 0)
        return;
    if (w->state == WIN_MINIMIZED) {
        wm_restore(w);
        return;
    }
    struct window *old = wm_focused();
    raise_to_top(w);
    if (old != w) {
        if (old)
            desktop_damage(wm_damage_rect(old));
        desktop_damage(wm_damage_rect(w));
        damage_chrome();
        report_focus();
        send_focus_change(old, w);
    }
}

void wm_minimize(struct window *w)
{
    if (!w || w->state == WIN_MINIMIZED)
        return;
    struct window *old = wm_focused();
    w->prev_state = w->state;
    w->state = WIN_MINIMIZED;
    int i = z_index(w);
    for (int k = i; k > 0; k--)
        zorder[k] = zorder[k - 1];
    zorder[0] = w;
    wm_report(w);
    report_focus();
    struct window *f = wm_focused();
    if (f != old)
        send_focus_change(old, f);
    desktop_damage(wm_damage_rect(w));
    damage_chrome();
}

void wm_restore(struct window *w)
{
    if (!w || w->state != WIN_MINIMIZED)
        return;
    struct window *old = wm_focused();
    w->state = w->prev_state == WIN_MAXIMIZED ? WIN_MAXIMIZED : WIN_NORMAL;
    raise_to_top(w);
    if (old)
        desktop_damage(wm_damage_rect(old));
    wm_report(w);
    report_focus();
    send_focus_change(old, w);
    desktop_damage(wm_damage_rect(w));
    damage_chrome();
}

/* New frame for w; the app is asked to redraw at the new content size. */
static void set_frame(struct window *w, struct rect frame)
{
    struct rect before = wm_damage_rect(w);
    bool same_size = frame.w == w->frame.w && frame.h == w->frame.h;
    w->frame = frame;
    if (!same_size)
        configure(w);
    desktop_damage(before);
    desktop_damage(wm_damage_rect(w));
}

bool wm_resize(struct window *w, int32_t width, int32_t height)
{
    if (!w || w->state != WIN_NORMAL || !(w->flags & WINF_RESIZABLE))
        return false;
    int32_t minw, minh, maxw, maxh;
    limits(w, &minw, &minh, &maxw, &maxh);
    width = CLAMP(width, minw, maxw);
    height = CLAMP(height, minh + TITLE_H, maxh + TITLE_H);
    if (width == w->frame.w && height == w->frame.h)
        return true;
    set_frame(w, rect_make(w->frame.x, w->frame.y, width, height));
    w->restore_frame = w->frame;
    wm_report(w);
    return true;
}

void wm_toggle_maximize(struct window *w)
{
    if (!w || w->state == WIN_MINIMIZED || !(w->flags & WINF_RESIZABLE))
        return;
    if (w->state == WIN_MAXIMIZED) {
        w->state = WIN_NORMAL;
        set_frame(w, w->restore_frame);
    } else {
        struct rect wa = wm_work_area();
        int32_t minw, minh, maxw, maxh;
        limits(w, &minw, &minh, &maxw, &maxh);
        struct rect target = rect_make(wa.x + 6, wa.y + 4, MIN(wa.w - 12, maxw), MIN(wa.h - 4, maxh + TITLE_H));
        w->restore_frame = w->frame;
        w->state = WIN_MAXIMIZED;
        set_frame(w, target);
    }
    wm_report(w);
}

void wm_move(struct window *w, int32_t x, int32_t y)
{
    clamp_position(w, &x, &y);
    if (x == w->frame.x && y == w->frame.y)
        return;
    struct rect before = wm_damage_rect(w);
    w->frame.x = x;
    w->frame.y = y;
    if (w->state == WIN_MAXIMIZED)
        w->state = WIN_NORMAL; /* dragging a maximized window keeps its size */
    w->restore_frame = w->frame;
    desktop_damage(before);
    desktop_damage(wm_damage_rect(w));
}

void wm_cycle(void)
{
    for (int i = 0; i < nwin; i++) {
        if (zorder[i]->state != WIN_MINIMIZED && zorder[i]->mapped && zorder[i] != wm_focused()) {
            wm_focus(zorder[i]);
            return;
        }
    }
}

struct window *wm_top_at(int32_t x, int32_t y)
{
    for (int i = nwin - 1; i >= 0; i--) {
        struct window *w = zorder[i];
        if (w->state != WIN_MINIMIZED && w->mapped && rect_contains(w->frame, x, y))
            return w;
    }
    return NULL;
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

/* ---- buffers --------------------------------------------------------------- */

void wm_attach(struct window *w, uint32_t id, int fd, const struct m_attach *a)
{
    size_t bytes;
    if (fd < 0 || a->stride < a->w || !surface_size_bytes((int64_t)a->stride, (int64_t)a->h, &bytes)) {
        if (fd >= 0)
            close(fd);
        return;
    }
    /* Replace the slot that is not on screen (or the one with this id). */
    int slot = w->buf[0].id == id && w->buf[0].fd >= 0 ? 0 : w->buf[1].id == id && w->buf[1].fd >= 0 ? 1 : -1;
    if (slot < 0)
        slot = w->shown == 0 ? 1 : 0;
    if (slot == w->shown)
        w->shown = -1;
    buffer_free(&w->buf[slot]);
    void *p = mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        return;
    }
    struct buffer *b = &w->buf[slot];
    b->id = id;
    b->fd = fd;
    b->px = p;
    b->bytes = bytes;
    b->w = (int32_t)a->w;
    b->h = (int32_t)a->h;
    b->stride = (int32_t)a->stride;
}

void wm_commit(struct window *w, const struct m_commit *m)
{
    int slot = -1;
    for (int i = 0; i < 2; i++)
        if (w->buf[i].fd >= 0 && w->buf[i].id == m->buffer)
            slot = i;
    if (slot < 0)
        return;
    int prev = w->shown;
    w->shown = slot;
    if (m->ack_serial && m->ack_serial != w->acked) {
        w->acked = m->ack_serial;
        struct rect cr = wm_content_rect(w);
        TEST_EVENT("WIN_ACK id=%u app=%s serial=%u w=%d h=%d match=%d", w->id, app_names[w->app], m->ack_serial,
                   w->buf[slot].w, w->buf[slot].h,
                   m->ack_serial == w->cfg_serial && w->buf[slot].w == cr.w && w->buf[slot].h == cr.h);
    }
    if (prev >= 0 && prev != slot) {
        struct m_release r = { w->buf[prev].id };
        send_to(w->c, S_RELEASE, w->cid, &r, sizeof r);
    }
    struct rect c = wm_content_rect(w);
    if (!w->mapped) {
        /* First frame: the window appears now. */
        w->mapped = true;
        struct window *old = NULL;
        for (int i = nwin - 1; i >= 0; i--)
            if (zorder[i] != w && zorder[i]->state != WIN_MINIMIZED && zorder[i]->mapped) {
                old = zorder[i];
                break;
            }
        raise_to_top(w);
        if (old)
            desktop_damage(wm_damage_rect(old));
        TEST_EVENT("WIN_OPEN id=%u app=%s", w->id, app_names[w->app]);
        wm_report(w);
        report_focus();
        send_focus_change(old, w);
        desktop_damage(wm_damage_rect(w));
        damage_chrome();
        return;
    }
    if (w->state == WIN_MINIMIZED)
        return;
    if (m->n == 0 || m->n > COMMIT_RECTS) {
        desktop_damage(c);
        return;
    }
    for (uint32_t i = 0; i < m->n; i++)
        desktop_damage(rect_intersect(c, rect_make(c.x + m->r[i].x, c.y + m->r[i].y, m->r[i].w, m->r[i].h)));
}

/* After a resolution change: keep every window reachable, refit maximized
 * ones and windows larger than the new work area (each gets a configure). */
void wm_screen_changed(void)
{
    struct rect wa = wm_work_area();
    for (int i = 0; i < nwin; i++) {
        struct window *w = zorder[i];
        struct rect f = w->frame;
        if (w->state == WIN_MAXIMIZED) {
            f = rect_make(wa.x + 6, wa.y + 4, wa.w - 12, wa.h - 4);
        } else {
            f.w = MIN(f.w, wa.w);
            f.h = MIN(f.h, wa.h);
            f.x = CLAMP(f.x, wa.x, MAX(wa.x, wa.x + wa.w - f.w));
            f.y = CLAMP(f.y, wa.y, MAX(wa.y, wa.y + wa.h - f.h));
            if (w->state == WIN_NORMAL)
                w->restore_frame = f;
        }
        bool resized = f.w != w->frame.w || f.h != w->frame.h;
        w->frame = f;
        if (resized)
            configure(w);
        wm_report(w);
    }
}

/* ---- painting ------------------------------------------------------------------- */

static void draw_control(struct surface *s, int32_t cx, int32_t cy, uint32_t color, int glyph, bool show)
{
    draw_circle(s, cx, cy, CTRL_R, color, 255);
    draw_round_frame(s, rect_make(cx - CTRL_R, cy - CTRL_R, CTRL_R * 2, CTRL_R * 2), CTRL_R, RGB(0, 0, 0), 40);
    if (!show)
        return;
    uint32_t g = RGB(0x4A, 0x10, 0x0C);
    if (glyph == 0) {
        for (int i = -2; i <= 2; i++) {
            draw_pixel_blend(s, cx + i, cy + i, g, 220);
            draw_pixel_blend(s, cx + i, cy - i, g, 220);
        }
    } else if (glyph == 1) {
        draw_fill(s, rect_make(cx - 3, cy, 6, 1), RGB(0x6A, 0x45, 0x00));
    } else {
        draw_fill(s, rect_make(cx - 3, cy, 7, 1), RGB(0x0A, 0x4A, 0x16));
        draw_fill(s, rect_make(cx, cy - 3, 1, 7), RGB(0x0A, 0x4A, 0x16));
    }
}

void wm_paint(struct surface *s, struct window *w, bool focused)
{
    const struct theme *t = g_theme;
    struct rect f = w->frame;
    draw_shadow(s, rect_make(f.x, f.y + 4, f.w, f.h), WIN_RADIUS, focused ? 18 : 12, focused ? 90 : 55);
    draw_round_rect(s, rect_make(f.x, f.y, f.w, TITLE_H + WIN_RADIUS), WIN_RADIUS,
                    focused ? t->title_active : t->title_inactive, 255, 1 | 2);
    draw_hline(s, f.x, f.y + TITLE_H - 1, f.w, t->separator);
    int32_t tw = text_width(FONT_UI_BOLD, w->title);
    int32_t maxw = f.w - 2 * 80;
    int32_t tx = f.x + (f.w - MIN(tw, maxw)) / 2;
    text_draw_fit(s, FONT_UI_BOLD, tx, f.y + (TITLE_H - text_height(FONT_UI_BOLD)) / 2, w->title, maxw,
                  focused ? t->title_text : t->title_text_inactive);
    bool hov = hover_controls == w->id;
    bool col = focused || hov;
    draw_control(s, f.x + CTRL_CLOSE_X, f.y + CTRL_Y, col ? RGB(0xFF, 0x5F, 0x57) : t->ctrl_inactive, 0, hov);
    draw_control(s, f.x + CTRL_MIN_X, f.y + CTRL_Y, col ? RGB(0xFE, 0xBC, 0x2E) : t->ctrl_inactive, 1, hov);
    bool can_max = (w->flags & WINF_RESIZABLE) != 0;
    draw_control(s, f.x + CTRL_MAX_X, f.y + CTRL_Y,
                 col && can_max ? RGB(0x28, 0xC8, 0x40) : t->ctrl_inactive, 2, hov && can_max);
    /* Content: the committed buffer, clipped to the content area; anything
     * it does not cover yet (resize in flight) shows the window colour. */
    struct rect c = wm_content_rect(w);
    struct buffer *b = w->shown >= 0 ? &w->buf[w->shown] : NULL;
    struct surface view = { 0 };
    if (b && b->px) {
        view.px = b->px;
        view.w = MIN(b->w, c.w);
        view.h = MIN(b->h, c.h);
        view.stride = b->stride;
        surface_reset_clip(&view);
    }
    if (!b || view.w < c.w || view.h < c.h) {
        struct rect old = s->clip;
        surface_set_clip(s, rect_intersect(old, c));
        draw_round_rect(s, rect_make(c.x, c.y - WIN_RADIUS, c.w, c.h + WIN_RADIUS), WIN_RADIUS, t->win_bg, 255,
                        4 | 8);
        s->clip = old;
    }
    if (view.px)
        blit_round_bottom(s, &view, c.x, c.y, view.w == c.w && view.h == c.h ? WIN_RADIUS : 0);
    if (w->state == WIN_NORMAL && (w->flags & WINF_RESIZABLE)) {
        int32_t gx = f.x + f.w - 5, gy = f.y + f.h - 5;
        for (int k = 0; k < 3; k++)
            for (int i = 0; i <= 3 + 3 * k; i++)
                draw_pixel_blend(s, gx - i, gy - (3 + 3 * k) + i, t->text_dim, 150);
    }
    draw_round_frame(s, f, WIN_RADIUS, t->win_border, t->border_alpha);
}
