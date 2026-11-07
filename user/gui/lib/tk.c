/* Toolkit widgets (gui/tk.h). */
#include "gui/tk.h"
#include "apps/app_util.h"
#include "lib.h"
#include <stdio.h>
#include <string.h>

#define SCROLLBAR_W 10

void tk_init(struct tk_panel *p, struct window *win)
{
    memset(p, 0, sizeof *p);
    p->win = win;
    p->focus = -1;
    p->capture = -1;
    p->caret_on = true;
}

struct tk_widget *tk_add(struct tk_panel *p, int kind, struct rect r, const char *text)
{
    if (p->n >= TK_MAX_WIDGETS)
        return NULL;
    struct tk_widget *w = &p->w[p->n];
    memset(w, 0, sizeof *w);
    w->kind = kind;
    w->id = p->n;
    w->r = r;
    w->visible = w->enabled = true;
    w->selected = -1;
    w->row_h = 24;
    w->menu_hover = -1;
    if (text)
        str_copy(w->text, text, sizeof w->text);
    if (kind == TK_FIELD)
        w->caret = strlen(w->text);
    p->n++;
    return w;
}

void tk_set_text(struct tk_widget *w, const char *text)
{
    str_copy(w->text, text, sizeof w->text);
    w->caret = strlen(w->text);
    w->has_sel = false;
    w->scroll_x = 0;
}

static bool focusable(const struct tk_widget *w)
{
    return w->visible && w->enabled && (w->kind == TK_FIELD || w->kind == TK_LIST || w->kind == TK_BUTTON);
}

void tk_focus(struct tk_panel *p, struct tk_widget *w)
{
    int i = w ? (int)(w - p->w) : -1;
    if (i == p->focus)
        return;
    p->focus = i;
    p->caret_on = true;
    if (p->win)
        wm_invalidate(p->win);
}

struct tk_widget *tk_focused(struct tk_panel *p) { return p->focus >= 0 ? &p->w[p->focus] : NULL; }

void tk_field_select_all(struct tk_widget *w)
{
    w->anchor = 0;
    w->caret = strlen(w->text);
    w->has_sel = w->caret > 0;
}

/* ---- list ------------------------------------------------------------------------ */

static int list_rows(const struct tk_widget *w) { return MAX(1, (w->r.h - 2) / w->row_h); }

void tk_list_set(struct tk_widget *w, int count, tk_item_fn item, void *ctx)
{
    w->count = count;
    w->item = item;
    w->ctx = ctx;
    if (w->selected >= count)
        w->selected = count - 1;
    if (w->top > MAX(0, count - list_rows(w)))
        w->top = MAX(0, count - list_rows(w));
}

void tk_list_select(struct tk_widget *w, int index)
{
    if (index < -1 || index >= w->count)
        return;
    w->selected = index;
    int rows = list_rows(w);
    if (index >= 0 && index < w->top)
        w->top = index;
    if (index >= w->top + rows)
        w->top = index - rows + 1;
}

/* ---- drawing ------------------------------------------------------------------------- */

static void draw_label(struct surface *s, const struct tk_widget *w)
{
    const struct theme *t = g_theme;
    const struct text_font *f = w->style == 2 ? FONT_UI_BOLD : w->style == 3 ? FONT_TITLE : FONT_UI;
    uint32_t c = w->style == 1 ? t->text_dim : w->style == 4 ? t->danger : t->text;
    text_draw_fit(s, f, w->r.x, w->r.y + (w->r.h - text_height(f)) / 2, w->text, w->r.w, c);
}

static void draw_field(struct surface *s, struct tk_widget *w, bool focused, bool caret_on)
{
    const struct theme *t = g_theme;
    struct rect r = w->r;
    draw_round_rect(s, r, 5, t->field_bg, 255, 0xF);
    draw_round_frame(s, r, 5, focused ? t->accent : t->field_border, 255);
    if (focused)
        draw_round_frame(s, rect_inset(r, -2), 7, t->accent, 70);
    size_t len = strlen(w->text);
    int32_t inner = r.w - 16;
    int32_t cx = text_offset(FONT_UI, w->text, len, w->caret);
    if (cx - w->scroll_x > inner)
        w->scroll_x = cx - inner;
    if (cx < w->scroll_x)
        w->scroll_x = cx;
    struct rect old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_inset(r, 3)));
    int32_t tx = r.x + 8 - w->scroll_x, ty = r.y + (r.h - text_height(FONT_UI)) / 2;
    if (w->has_sel && w->anchor != w->caret) {
        size_t a = MIN(w->anchor, w->caret), b = MAX(w->anchor, w->caret);
        int32_t xa = text_offset(FONT_UI, w->text, len, a), xb = text_offset(FONT_UI, w->text, len, b);
        draw_fill(s, rect_make(tx + xa, ty, xb - xa, text_height(FONT_UI)), t->selection);
    }
    if (!len && w->placeholder[0])
        text_draw(s, FONT_UI, tx, ty, w->placeholder, t->text_dim);
    else
        text_draw(s, FONT_UI, tx, ty, w->text, t->text);
    if (focused && caret_on)
        draw_fill(s, rect_make(tx + cx, r.y + 5, 2, r.h - 10), t->accent);
    s->clip = old;
}

static void draw_list(struct surface *s, const struct tk_widget *w, bool focused)
{
    const struct theme *t = g_theme;
    struct rect r = w->r;
    draw_round_rect(s, r, 6, t->field_bg, 255, 0xF);
    draw_round_frame(s, r, 6, focused ? t->accent : t->field_border, 255);
    struct rect old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_inset(r, 1)));
    int rows = list_rows(w);
    bool bar = w->count > rows;
    int32_t iw = r.w - 8 - (bar ? SCROLLBAR_W : 0);
    for (int i = w->top; i < w->count && i < w->top + rows; i++) {
        struct rect row = rect_make(r.x + 4, r.y + 1 + (i - w->top) * w->row_h, iw, w->row_h);
        bool sel = i == w->selected;
        if (sel)
            draw_round_rect(s, row, 5, focused ? t->accent : t->selection, 255, 0xF);
        int icon = 0;
        const char *label = w->item ? w->item(w->ctx, i, &icon) : "";
        int32_t x = row.x + 8;
        if (icon == 1)
            app_draw_folder_icon(s, x, row.y + (row.h - 16) / 2);
        else if (icon == 2)
            app_draw_file_icon(s, x, row.y + (row.h - 16) / 2);
        if (icon)
            x += 26;
        uint32_t fg = sel ? (focused ? t->accent_text : t->sel_text) : t->text;
        text_draw_fit(s, FONT_UI, x, row.y + (row.h - text_height(FONT_UI)) / 2, label, row.x + row.w - x - 6, fg);
    }
    if (bar) {
        int32_t th = MAX(20, (r.h - 4) * rows / w->count);
        int32_t ty = r.y + 2 + (r.h - 4 - th) * w->top / MAX(1, w->count - rows);
        draw_round_rect(s, rect_make(r.x + r.w - SCROLLBAR_W - 2, ty, SCROLLBAR_W - 2, th), 4, t->text_dim, 120, 0xF);
    }
    s->clip = old;
}

static void draw_scroll(struct surface *s, const struct tk_widget *w)
{
    const struct theme *t = g_theme;
    struct rect r = w->r;
    struct rect old = s->clip;
    surface_set_clip(s, rect_intersect(old, r));
    if (w->paint)
        w->paint(w->ctx, s, r, w->offset);
    if (w->content_h > r.h) {
        int32_t th = MAX(20, r.h * r.h / w->content_h);
        int32_t ty = r.y + (r.h - th) * w->offset / MAX(1, w->content_h - r.h);
        draw_round_rect(s, rect_make(r.x + r.w - SCROLLBAR_W, ty, SCROLLBAR_W - 3, th), 4, t->text_dim, 110, 0xF);
    }
    s->clip = old;
}

static struct rect menu_rect(const struct tk_widget *w)
{
    int32_t mw = w->r.w;
    for (int i = 0; i < w->nitems; i++)
        mw = MAX(mw, text_width(FONT_UI, w->items[i]) + 28);
    return rect_make(w->r.x, w->r.y + w->r.h + 2, mw, 8 + w->nitems * 24);
}

static void draw_menu(struct surface *s, const struct tk_widget *w)
{
    const struct theme *t = g_theme;
    char label[TK_TEXT_MAX + 4];
    snprintf(label, sizeof label, "%s \xE2\x96\xBE", w->text); /* small down triangle */
    widget_button(s, w->r, label, BTN_NORMAL, w->hover || w->menu_open);
    if (!w->menu_open)
        return;
    struct rect m = menu_rect(w);
    draw_shadow(s, m, 8, 10, 70);
    draw_round_rect(s, m, 8, t->dark ? RGB(0x2B, 0x2C, 0x34) : RGB(0xFA, 0xFA, 0xFC), 250, 0xF);
    draw_round_frame(s, m, 8, RGB(0, 0, 0), t->dark ? 160 : 50);
    for (int i = 0; i < w->nitems; i++) {
        struct rect it = rect_make(m.x + 4, m.y + 4 + i * 24, m.w - 8, 24);
        uint32_t fg = t->text;
        if (i == w->menu_hover) {
            draw_round_rect(s, it, 5, t->accent, 255, 0xF);
            fg = t->accent_text;
        }
        text_draw(s, FONT_UI, it.x + 10, it.y + (24 - text_height(FONT_UI)) / 2, w->items[i], fg);
    }
}

void tk_render(struct tk_panel *p, struct surface *s)
{
    struct tk_widget *open_menu = NULL;
    for (int i = 0; i < p->n; i++) {
        struct tk_widget *w = &p->w[i];
        if (!w->visible)
            continue;
        bool focused = i == p->focus;
        switch (w->kind) {
        case TK_LABEL: draw_label(s, w); break;
        case TK_BUTTON:
            widget_button(s, w->r, w->text, w->enabled ? w->style : BTN_NORMAL, w->hover && w->enabled);
            if (focused)
                draw_round_frame(s, rect_inset(w->r, -2), 8, g_theme->accent, 90);
            break;
        case TK_FIELD: draw_field(s, w, focused, p->caret_on); break;
        case TK_LIST: draw_list(s, w, focused); break;
        case TK_SCROLL: draw_scroll(s, w); break;
        case TK_MENU:
            if (w->menu_open)
                open_menu = w;
            else
                draw_menu(s, w);
            break;
        default: break;
        }
    }
    if (open_menu)
        draw_menu(s, open_menu); /* on top of everything */
}

/* ---- field editing ----------------------------------------------------------------------- */

static void field_delete_sel(struct tk_widget *w)
{
    if (!w->has_sel || w->anchor == w->caret) {
        w->has_sel = false;
        return;
    }
    size_t a = MIN(w->anchor, w->caret), b = MAX(w->anchor, w->caret), len = strlen(w->text);
    memmove(w->text + a, w->text + b, len - b + 1);
    w->caret = a;
    w->has_sel = false;
}

static bool field_insert(struct tk_widget *w, const char *s)
{
    field_delete_sel(w);
    size_t len = strlen(w->text), n = strlen(s);
    if (len + n >= sizeof w->text)
        return false;
    memmove(w->text + w->caret + n, w->text + w->caret, len - w->caret + 1);
    memcpy(w->text + w->caret, s, n);
    w->caret += n;
    return true;
}

static bool field_key(struct tk_widget *w, const struct key_event *e)
{
    size_t len = strlen(w->text);
    bool shift = (e->mods & MOD_SHIFT) != 0;
    size_t before = w->caret;
    switch (e->key) {
    case KEY_LEFT: case KEY_RIGHT: case KEY_HOME: case KEY_END:
        if (shift && !w->has_sel) {
            w->anchor = w->caret;
            w->has_sel = true;
        } else if (!shift) {
            if (w->has_sel && e->key == KEY_LEFT)
                w->caret = MIN(w->anchor, w->caret);
            else if (w->has_sel && e->key == KEY_RIGHT)
                w->caret = MAX(w->anchor, w->caret);
            bool had = w->has_sel;
            w->has_sel = false;
            if (had && (e->key == KEY_LEFT || e->key == KEY_RIGHT))
                return true;
        }
        if (e->key == KEY_LEFT)
            w->caret = utf8_prev(w->text, w->caret);
        else if (e->key == KEY_RIGHT)
            w->caret = utf8_next(w->text, len, w->caret);
        else if (e->key == KEY_HOME)
            w->caret = 0;
        else
            w->caret = len;
        (void)before;
        return true;
    case KEY_BACKSPACE:
        if (w->has_sel && w->anchor != w->caret)
            field_delete_sel(w);
        else if (w->caret > 0) {
            size_t p = utf8_prev(w->text, w->caret);
            memmove(w->text + p, w->text + w->caret, len - w->caret + 1);
            w->caret = p;
        }
        w->has_sel = false;
        if (w->on_change)
            w->on_change(w, w->ctx);
        return true;
    case KEY_DELETE:
        if (w->has_sel && w->anchor != w->caret)
            field_delete_sel(w);
        else if (w->caret < len) {
            size_t n = utf8_next(w->text, len, w->caret);
            memmove(w->text + w->caret, w->text + n, len - n + 1);
        }
        if (w->on_change)
            w->on_change(w, w->ctx);
        return true;
    case KEY_ENTER:
        if (w->on_activate)
            w->on_activate(w, w->ctx);
        return true;
    case KEY_CHAR:
        if ((e->mods & MOD_CTRL) && (e->ascii | 0x20) == 'a') {
            tk_field_select_all(w);
            return true;
        }
        return false;
    default:
        return false;
    }
}

/* ---- events -------------------------------------------------------------------------------- */

static void move_focus(struct tk_panel *p, int dir)
{
    for (int k = 1; k <= p->n; k++) {
        int i = ((p->focus < 0 ? (dir > 0 ? -1 : 0) : p->focus) + dir * k + p->n * 2) % p->n;
        if (focusable(&p->w[i])) {
            tk_focus(p, &p->w[i]);
            if (p->w[i].kind == TK_FIELD)
                tk_field_select_all(&p->w[i]);
            return;
        }
    }
}

bool tk_key(struct tk_panel *p, const struct key_event *e)
{
    p->caret_on = true;
    for (int i = 0; i < p->n; i++) {
        struct tk_widget *m = &p->w[i];
        if (m->kind == TK_MENU && m->menu_open) {
            if (e->key == KEY_ESC)
                m->menu_open = false;
            else if (e->key == KEY_DOWN)
                m->menu_hover = (m->menu_hover + 1) % MAX(1, m->nitems);
            else if (e->key == KEY_UP)
                m->menu_hover = (m->menu_hover + m->nitems - 1) % MAX(1, m->nitems);
            else if (e->key == KEY_ENTER && m->menu_hover >= 0) {
                m->menu_open = false;
                m->selected = m->menu_hover;
                if (m->on_activate)
                    m->on_activate(m, m->ctx);
            }
            wm_invalidate(p->win);
            return true;
        }
    }
    if (e->key == KEY_TAB && !(e->mods & (MOD_CTRL | MOD_ALT))) {
        move_focus(p, (e->mods & MOD_SHIFT) ? -1 : 1);
        wm_invalidate(p->win);
        return true;
    }
    struct tk_widget *w = tk_focused(p);
    if (!w)
        return false;
    bool used = false;
    if (w->kind == TK_FIELD) {
        used = field_key(w, e);
    } else if (w->kind == TK_LIST) {
        int rows = list_rows(w);
        int s = w->selected;
        switch (e->key) {
        case KEY_UP: s = s > 0 ? s - 1 : 0; used = true; break;
        case KEY_DOWN: s = s < w->count - 1 ? s + 1 : s; used = true; break;
        case KEY_PGUP: s = MAX(0, s - rows); used = true; break;
        case KEY_PGDN: s = MIN(w->count - 1, s + rows); used = true; break;
        case KEY_HOME: s = 0; used = true; break;
        case KEY_END: s = w->count - 1; used = true; break;
        case KEY_ENTER:
            if (w->on_activate && w->selected >= 0)
                w->on_activate(w, w->ctx);
            used = true;
            break;
        default: break;
        }
        if (s != w->selected && w->count) {
            tk_list_select(w, s);
            if (w->on_change)
                w->on_change(w, w->ctx);
        }
    } else if (w->kind == TK_BUTTON && (e->key == KEY_ENTER || (e->key == KEY_CHAR && e->ascii == ' '))) {
        if (w->on_activate)
            w->on_activate(w, w->ctx);
        used = true;
    }
    if (used)
        wm_invalidate(p->win);
    return used;
}

bool tk_text(struct tk_panel *p, const char *utf8)
{
    struct tk_widget *w = tk_focused(p);
    if (!w)
        return false;
    if (w->kind == TK_BUTTON && !strcmp(utf8, " ")) {
        if (w->on_activate)
            w->on_activate(w, w->ctx);
        return true;
    }
    if (w->kind != TK_FIELD)
        return false;
    p->caret_on = true;
    char clean[16];
    size_t k = 0;
    for (const char *c = utf8; *c && k + 1 < sizeof clean; c++)
        if ((unsigned char)*c >= 32 && *c != 127)
            clean[k++] = *c;
    clean[k] = 0;
    if (k && field_insert(w, clean) && w->on_change)
        w->on_change(w, w->ctx);
    wm_invalidate(p->win);
    return true;
}

static struct tk_widget *widget_at(struct tk_panel *p, int32_t x, int32_t y)
{
    for (int i = p->n - 1; i >= 0; i--)
        if (p->w[i].visible && p->w[i].kind != TK_LABEL && rect_contains(p->w[i].r, x, y))
            return &p->w[i];
    return NULL;
}

bool tk_mouse(struct tk_panel *p, const struct win_mouse *m)
{
    /* An open menu takes the mouse first. */
    for (int i = 0; i < p->n; i++) {
        struct tk_widget *w = &p->w[i];
        if (w->kind != TK_MENU || !w->menu_open)
            continue;
        struct rect mr = menu_rect(w);
        int item = rect_contains(mr, m->x, m->y) ? (m->y - mr.y - 4) / 24 : -1;
        if (item >= w->nitems)
            item = -1;
        if (m->kind == WM_MOUSE_MOVE) {
            if (item != w->menu_hover) {
                w->menu_hover = item;
                wm_invalidate(p->win);
            }
            return true;
        }
        if (m->kind == WM_MOUSE_DOWN) {
            w->menu_open = false;
            if (item >= 0) {
                w->selected = item;
                if (w->on_activate)
                    w->on_activate(w, w->ctx);
            }
            wm_invalidate(p->win);
            return true;
        }
        return true;
    }
    if (m->kind == WM_MOUSE_WHEEL) {
        struct tk_widget *w = widget_at(p, m->x, m->y);
        if (w && w->kind == TK_LIST) {
            w->top = CLAMP(w->top - m->wheel * 3, 0, MAX(0, w->count - list_rows(w)));
            wm_invalidate(p->win);
            return true;
        }
        if (w && w->kind == TK_SCROLL) {
            w->offset = CLAMP(w->offset - m->wheel * 48, 0, MAX(0, w->content_h - w->r.h));
            wm_invalidate(p->win);
            return true;
        }
        return false;
    }
    if (m->kind == WM_MOUSE_MOVE) {
        bool changed = false;
        struct tk_widget *under = m->x < 0 ? NULL : widget_at(p, m->x, m->y);
        for (int i = 0; i < p->n; i++) {
            bool h = &p->w[i] == under;
            if (h != p->w[i].hover) {
                p->w[i].hover = h;
                changed = true;
            }
        }
        if (p->capture >= 0) {
            struct tk_widget *w = &p->w[p->capture];
            if (w->kind == TK_FIELD && (m->buttons & 1)) {
                size_t len = strlen(w->text);
                w->caret = text_index_at(FONT_UI, w->text, len, m->x - w->r.x - 8 + w->scroll_x);
                w->has_sel = w->caret != w->anchor;
                changed = true;
            }
        }
        if (changed)
            wm_invalidate(p->win);
        return under != NULL;
    }
    if (m->kind == WM_MOUSE_UP) {
        int c = p->capture;
        p->capture = -1;
        if (c >= 0) {
            struct tk_widget *w = &p->w[c];
            bool inside = rect_contains(w->r, m->x, m->y);
            if (w->kind == TK_BUTTON && w->pressed && inside && w->enabled && w->on_activate) {
                w->pressed = false;
                w->on_activate(w, w->ctx);
            }
            w->pressed = false;
            wm_invalidate(p->win);
            return true;
        }
        return false;
    }
    if (m->kind != WM_MOUSE_DOWN)
        return false;
    struct tk_widget *w = widget_at(p, m->x, m->y);
    if (!w || !w->enabled)
        return false;
    p->capture = (int)(w - p->w);
    if (focusable(w))
        tk_focus(p, w);
    switch (w->kind) {
    case TK_BUTTON:
        w->pressed = true;
        break;
    case TK_FIELD: {
        size_t len = strlen(w->text);
        w->caret = text_index_at(FONT_UI, w->text, len, m->x - w->r.x - 8 + w->scroll_x);
        w->anchor = w->caret;
        w->has_sel = false;
        if (m->clicks >= 2)
            tk_field_select_all(w);
        break;
    }
    case TK_LIST: {
        int i = w->top + (m->y - w->r.y - 1) / w->row_h;
        if (i >= 0 && i < w->count) {
            bool same = i == w->selected;
            tk_list_select(w, i);
            if (!same && w->on_change)
                w->on_change(w, w->ctx);
            if (m->clicks >= 2 && w->on_activate)
                w->on_activate(w, w->ctx);
        }
        break;
    }
    case TK_MENU:
        w->menu_open = true;
        w->menu_hover = -1;
        break;
    default:
        break;
    }
    wm_invalidate(p->win);
    return true;
}

void tk_tick(struct tk_panel *p, uint64_t now)
{
    bool on = (now / 500) % 2 == 0;
    struct tk_widget *f = tk_focused(p);
    if (f && f->kind == TK_FIELD && on != p->caret_on) {
        p->caret_on = on;
        wm_invalidate(p->win);
    }
}
