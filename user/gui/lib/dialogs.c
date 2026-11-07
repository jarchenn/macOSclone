/* Open and save dialogs (0.7f), as sheets built from toolkit widgets. */
#include "gui/tk.h"
#include "fs/vfs.h"
#include "lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FD_MAX 256
#define BOX_W 480
#define BOX_H 360

enum { ST_BROWSE = 0, ST_MKDIR, ST_CONFIRM };

struct fd {
    int mode, state;
    char dir[VFS_PATH_MAX + 1];
    struct vfs_dirent ents[FD_MAX];
    int n;
    bool truncated;
    char error[120];
    char pending[VFS_PATH_MAX + 1]; /* path awaiting overwrite confirmation */
    fd_done_fn done;
    void *ctx;
    struct tk_panel p, sub;
    struct tk_widget *path_lbl, *up, *mkdir, *list, *name, *cancel, *ok, *err;
    struct tk_widget *sub_name, *sub_ok, *sub_cancel, *sub_msg;
    struct window *win;
};

static struct rect box_of(const struct window *w)
{
    int32_t bw = MIN(BOX_W, w->content.w - 20), bh = MIN(BOX_H, w->content.h - 16);
    return rect_make((w->content.w - bw) / 2, (w->content.h - bh) / 2, bw, bh);
}

static const char *item(void *ctx, int i, int *icon)
{
    struct fd *d = ctx;
    *icon = d->ents[i].type == VFS_DIR ? 1 : 2;
    return d->ents[i].name;
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static void load(struct fd *d)
{
    d->n = 0;
    d->truncated = false;
    struct vfs_dirent e;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; vfs_readdir(d->dir, i, &e) == VFS_OK; i++) {
            if ((pass == 0) != (e.type == VFS_DIR))
                continue;
            if (e.name[0] == '.')
                continue; /* temporary files of atomic saves */
            if (d->n >= FD_MAX) {
                d->truncated = true;
                break;
            }
            d->ents[d->n++] = e;
        }
    d->list->selected = -1;
    d->list->top = 0;
    tk_list_set(d->list, d->n, item, d);
    if (d->mode == FD_OPEN && d->n)
        tk_list_select(d->list, 0); /* Enter opens the first entry */
    tk_set_text(d->path_lbl, d->dir);
    d->up->enabled = strcmp(d->dir, "/") != 0;
    struct vfs_stat st;
    d->mkdir->enabled = vfs_stat(d->dir, &st) == VFS_OK && !st.readonly;
    TEST_EVENT("DIALOG_DIR path=%s entries=%d", d->dir, d->n);
}

static void go(struct fd *d, const char *path)
{
    char norm[VFS_PATH_MAX + 1];
    if (vfs_normalize("/", path, norm, sizeof norm) != VFS_OK)
        return;
    struct vfs_stat st;
    if (vfs_stat(norm, &st) != VFS_OK || st.type != VFS_DIR) {
        snprintf(d->error, sizeof d->error, "Cannot open %s", norm);
        tk_set_text(d->err, d->error);
        return;
    }
    str_copy(d->dir, norm, sizeof d->dir);
    d->error[0] = 0;
    tk_set_text(d->err, "");
    load(d);
}

static void finish(struct fd *d, const char *path)
{
    struct window *w = d->win;
    fd_done_fn done = d->done;
    void *ctx = d->ctx;
    TEST_EVENT("DIALOG_DONE result=%s path=%s", path ? "ok" : "cancel", path ? path : "");
    char copy[VFS_PATH_MAX + 1];
    if (path)
        str_copy(copy, path, sizeof copy);
    ui_sheet_close(w);
    free(d);
    if (done)
        done(ctx, path ? copy : NULL);
    wm_invalidate(w);
}

static void sub_layout(struct fd *d)
{
    struct rect b = box_of(d->win);
    struct rect s = rect_make(b.x + 40, b.y + (b.h - 150) / 2, b.w - 80, 150);
    tk_init(&d->sub, d->win);
    d->sub_msg = tk_add(&d->sub, TK_LABEL, rect_make(s.x + 16, s.y + 14, s.w - 32, 22), "");
    d->sub_msg->style = 2;
    d->sub_name = NULL;
    if (d->state == ST_MKDIR) {
        tk_set_text(d->sub_msg, "New folder");
        d->sub_name = tk_add(&d->sub, TK_FIELD, rect_make(s.x + 16, s.y + 48, s.w - 32, 28), "untitled-folder");
        tk_field_select_all(d->sub_name);
        tk_focus(&d->sub, d->sub_name);
    } else {
        char msg[TK_TEXT_MAX];
        const char *slash = strrchr(d->pending, '/');
        snprintf(msg, sizeof msg, "\"%s\" already exists. Replace it?", slash ? slash + 1 : d->pending);
        tk_set_text(d->sub_msg, msg);
        struct tk_widget *hint = tk_add(&d->sub, TK_LABEL, rect_make(s.x + 16, s.y + 44, s.w - 32, 22),
                                        "Replacing it overwrites its current contents.");
        hint->style = 1;
    }
    d->sub_cancel = tk_add(&d->sub, TK_BUTTON, rect_make(s.x + s.w - 196, s.y + s.h - 42, 84, 28), "Cancel");
    d->sub_ok = tk_add(&d->sub, TK_BUTTON, rect_make(s.x + s.w - 102, s.y + s.h - 42, 86, 28),
                       d->state == ST_MKDIR ? "Create" : "Replace");
    d->sub_ok->style = d->state == ST_MKDIR ? BTN_PRIMARY : BTN_DANGER;
    if (d->state == ST_CONFIRM)
        tk_focus(&d->sub, d->sub_ok);
}

static void sub_done(struct fd *d, bool ok)
{
    int st = d->state;
    d->state = ST_BROWSE;
    if (!ok) {
        wm_invalidate(d->win);
        return;
    }
    if (st == ST_MKDIR && d->sub_name) {
        char path[VFS_PATH_MAX + 1];
        join(path, sizeof path, d->dir, d->sub_name->text);
        char norm[VFS_PATH_MAX + 1];
        int r = vfs_normalize("/", path, norm, sizeof norm);
        if (r == VFS_OK)
            r = vfs_mkdir(norm);
        TEST_EVENT("DIALOG_MKDIR path=%s result=%d", norm, r);
        if (r) {
            snprintf(d->error, sizeof d->error, "Cannot create folder: %s", vfs_strerror(r));
            tk_set_text(d->err, d->error);
        } else {
            go(d, norm);
        }
    } else if (st == ST_CONFIRM) {
        finish(d, d->pending);
        return;
    }
    wm_invalidate(d->win);
}

static void on_sub_ok(struct tk_widget *w, void *ctx) { (void)w; sub_done(ctx, true); }
static void on_sub_cancel(struct tk_widget *w, void *ctx) { (void)w; sub_done(ctx, false); }

static void open_sub(struct fd *d, int state)
{
    d->state = state;
    sub_layout(d);
    d->sub_ok->on_activate = on_sub_ok;
    d->sub_ok->ctx = d;
    d->sub_cancel->on_activate = on_sub_cancel;
    d->sub_cancel->ctx = d;
    if (d->sub_name) {
        d->sub_name->on_activate = on_sub_ok;
        d->sub_name->ctx = d;
    }
    if (state == ST_CONFIRM)
        TEST_EVENT("DIALOG_CONFIRM overwrite path=%s", d->pending);
    wm_invalidate(d->win);
}

/* OK / Enter: open the selected folder, or choose the file. */
static void accept(struct fd *d)
{
    int sel = d->list->selected;
    bool list_focus = tk_focused(&d->p) == d->list;
    if (sel >= 0 && d->ents[sel].type == VFS_DIR && (d->mode == FD_OPEN || list_focus)) {
        char path[VFS_PATH_MAX + 1];
        join(path, sizeof path, d->dir, d->ents[sel].name);
        go(d, path);
        return;
    }
    if (d->mode == FD_OPEN) {
        if (sel < 0)
            return;
        char path[VFS_PATH_MAX + 1];
        join(path, sizeof path, d->dir, d->ents[sel].name);
        finish(d, path);
        return;
    }
    const char *name = d->name->text;
    if (!name[0]) {
        tk_set_text(d->err, "Type a name for the file.");
        return;
    }
    char raw[VFS_PATH_MAX + 1], path[VFS_PATH_MAX + 1];
    if (name[0] == '/')
        str_copy(raw, name, sizeof raw);
    else
        join(raw, sizeof raw, d->dir, name);
    int r = vfs_normalize("/", raw, path, sizeof path);
    if (r) {
        snprintf(d->error, sizeof d->error, "Invalid name: %s", vfs_strerror(r));
        tk_set_text(d->err, d->error);
        return;
    }
    struct vfs_stat st;
    if (vfs_stat(path, &st) == VFS_OK) {
        if (st.type == VFS_DIR) {
            go(d, path);
            return;
        }
        str_copy(d->pending, path, sizeof d->pending);
        open_sub(d, ST_CONFIRM);
        return;
    }
    finish(d, path);
}

static void on_ok(struct tk_widget *w, void *ctx) { (void)w; accept(ctx); }
static void on_cancel(struct tk_widget *w, void *ctx) { (void)w; finish(ctx, NULL); }

static void on_up(struct tk_widget *w, void *ctx)
{
    (void)w;
    struct fd *d = ctx;
    char parent[VFS_PATH_MAX + 1], name[VFS_NAME_MAX + 1];
    if (strcmp(d->dir, "/") && vfs_split(d->dir, parent, sizeof parent, name, sizeof name) == VFS_OK)
        go(d, parent);
}

static void on_mkdir(struct tk_widget *w, void *ctx) { (void)w; open_sub(ctx, ST_MKDIR); }

static void on_select(struct tk_widget *w, void *ctx)
{
    struct fd *d = ctx;
    if (d->mode == FD_SAVE && w->selected >= 0 && d->ents[w->selected].type == VFS_FILE)
        tk_set_text(d->name, d->ents[w->selected].name);
}

static void on_list_activate(struct tk_widget *w, void *ctx)
{
    (void)w;
    struct fd *d = ctx;
    int sel = d->list->selected;
    if (sel < 0)
        return;
    if (d->ents[sel].type == VFS_DIR) {
        char path[VFS_PATH_MAX + 1];
        join(path, sizeof path, d->dir, d->ents[sel].name);
        go(d, path);
    } else {
        accept(d);
    }
}

static void layout(struct fd *d)
{
    struct rect b = box_of(d->win);
    tk_init(&d->p, d->win);
    struct tk_widget *title = tk_add(&d->p, TK_LABEL, rect_make(b.x + 16, b.y + 12, b.w - 32, 22),
                                     d->mode == FD_OPEN ? "Open" : "Save As");
    title->style = 2;
    d->up = tk_add(&d->p, TK_BUTTON, rect_make(b.x + 16, b.y + 42, 36, 26), "Up");
    d->path_lbl = tk_add(&d->p, TK_LABEL, rect_make(b.x + 60, b.y + 44, b.w - 196, 22), d->dir);
    d->path_lbl->style = 1;
    d->mkdir = tk_add(&d->p, TK_BUTTON, rect_make(b.x + b.w - 128, b.y + 42, 112, 26), "New Folder");
    int32_t list_h = b.h - 42 - 34 - (d->mode == FD_SAVE ? 40 : 0) - 82;
    d->list = tk_add(&d->p, TK_LIST, rect_make(b.x + 16, b.y + 76, b.w - 32, list_h), NULL);
    int32_t y = b.y + 76 + list_h + 8;
    d->name = NULL;
    if (d->mode == FD_SAVE) {
        struct tk_widget *nl = tk_add(&d->p, TK_LABEL, rect_make(b.x + 16, y + 3, 50, 22), "Name:");
        nl->style = 1;
        d->name = tk_add(&d->p, TK_FIELD, rect_make(b.x + 70, y, b.w - 86, 28), "");
        y += 36;
    }
    d->err = tk_add(&d->p, TK_LABEL, rect_make(b.x + 16, y, b.w - 32, 20), "");
    d->err->style = 4;
    d->cancel = tk_add(&d->p, TK_BUTTON, rect_make(b.x + b.w - 196, b.y + b.h - 42, 84, 28), "Cancel");
    d->ok = tk_add(&d->p, TK_BUTTON, rect_make(b.x + b.w - 102, b.y + b.h - 42, 86, 28),
                   d->mode == FD_OPEN ? "Open" : "Save");
    d->ok->style = BTN_PRIMARY;
    d->up->on_activate = on_up;
    d->mkdir->on_activate = on_mkdir;
    d->list->on_change = on_select;
    d->list->on_activate = on_list_activate;
    d->cancel->on_activate = on_cancel;
    d->ok->on_activate = on_ok;
    struct tk_widget *ws[] = { d->up, d->mkdir, d->list, d->cancel, d->ok, d->name };
    for (size_t i = 0; i < ARRAY_LEN(ws); i++)
        if (ws[i])
            ws[i]->ctx = d;
    if (d->name)
        d->name->on_activate = on_ok;
}

/* ---- sheet callbacks ---------------------------------------------------------------- */

static void render(struct window *w, struct surface *s)
{
    struct fd *d = w->sheet_data;
    const struct theme *t = g_theme;
    draw_blend(s, rect_make(0, 0, s->w, s->h), RGB(0, 0, 0), 70);
    struct rect b = box_of(w);
    draw_shadow(s, b, 10, 14, 90);
    draw_round_rect(s, b, 10, t->win_bg, 255, 0xF);
    draw_round_frame(s, b, 10, RGB(0, 0, 0), t->border_alpha + 20);
    tk_render(&d->p, s);
    if (d->state != ST_BROWSE) {
        struct rect sb = rect_make(b.x + 40, b.y + (b.h - 150) / 2, b.w - 80, 150);
        draw_blend(s, b, RGB(0, 0, 0), 40);
        draw_shadow(s, sb, 10, 12, 90);
        draw_round_rect(s, sb, 10, t->win_bg, 255, 0xF);
        draw_round_frame(s, sb, 10, RGB(0, 0, 0), t->border_alpha + 20);
        tk_render(&d->sub, s);
    }
}

static void key(struct window *w, const struct key_event *e)
{
    struct fd *d = w->sheet_data;
    if (d->state != ST_BROWSE) {
        if (e->key == KEY_ESC)
            sub_done(d, false);
        else if (!tk_key(&d->sub, e) && e->key == KEY_ENTER)
            sub_done(d, true);
        wm_invalidate(w);
        return;
    }
    if (e->key == KEY_ESC) {
        finish(d, NULL);
        return;
    }
    if (e->key == KEY_BACKSPACE && tk_focused(&d->p) == d->list) {
        on_up(NULL, d);
        wm_invalidate(w);
        return;
    }
    if (!tk_key(&d->p, e) && e->key == KEY_ENTER)
        accept(d);
    wm_invalidate(w);
}

static void text(struct window *w, const char *utf8)
{
    struct fd *d = w->sheet_data;
    tk_text(d->state != ST_BROWSE ? &d->sub : &d->p, utf8);
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    struct fd *d = w->sheet_data;
    tk_mouse(d->state != ST_BROWSE ? &d->sub : &d->p, m);
}

static void tick(struct window *w, uint64_t now)
{
    struct fd *d = w->sheet_data;
    tk_tick(d->state != ST_BROWSE ? &d->sub : &d->p, now);
}

static void resized(struct window *w)
{
    struct fd *d = w->sheet_data;
    char name[TK_TEXT_MAX] = "";
    if (d->name)
        str_copy(name, d->name->text, sizeof name);
    layout(d);
    if (d->name)
        tk_set_text(d->name, name);
    load(d);
    if (d->state != ST_BROWSE)
        open_sub(d, d->state);
}

static const struct window_ops sheet_ops = {
    .render = render, .key = key, .text = text, .mouse = mouse, .tick = tick, .resized = resized,
};

void file_dialog(struct window *w, int mode, const char *dir, const char *name, fd_done_fn done, void *ctx)
{
    struct fd *d = calloc(1, sizeof *d);
    if (!d) {
        if (done)
            done(ctx, NULL);
        return;
    }
    d->mode = mode;
    d->done = done;
    d->ctx = ctx;
    d->win = w;
    struct vfs_stat st;
    str_copy(d->dir, dir && vfs_stat(dir, &st) == VFS_OK && st.type == VFS_DIR ? dir : "/home", sizeof d->dir);
    layout(d);
    load(d);
    if (d->name) {
        tk_set_text(d->name, name ? name : "");
        tk_field_select_all(d->name);
        tk_focus(&d->p, d->name);
    } else {
        tk_focus(&d->p, d->list);
        if (d->n)
            tk_list_select(d->list, 0);
    }
    ui_sheet_open(w, &sheet_ops, d);
    TEST_EVENT("DIALOG kind=%s dir=%s", mode == FD_OPEN ? "open" : "save", d->dir);
#define C(wd) (wd)->r.x + (wd)->r.w / 2, (wd)->r.y + (wd)->r.h / 2
    TEST_EVENT("DIALOG_LAYOUT up=%d,%d newfolder=%d,%d ok=%d,%d cancel=%d,%d name=%d,%d list=%d,%d,%d,%d row=%d",
               C(d->up), C(d->mkdir), C(d->ok), C(d->cancel), d->name ? d->name->r.x + 20 : -1,
               d->name ? d->name->r.y + d->name->r.h / 2 : -1, d->list->r.x, d->list->r.y, d->list->r.w,
               d->list->r.h, d->list->row_h);
#undef C
}

bool file_dialog_active(struct window *w) { return w->sheet == &sheet_ops; }
