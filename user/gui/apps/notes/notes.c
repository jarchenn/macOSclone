/* Notes: the plain-text editor (0.7: a user process on the toolkit). UTF-8
 * text in a monospaced grid, open/save through the file dialogs, cut/copy/
 * paste through the display server's clipboard. */
#include "desktop/desktop.h"
#include "gui/tk.h"
#include "apps/textbuf.h"
#include "apps/app_util.h"
#include "fs/vfs.h"
#include "lib.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TOOLBAR_H 44
#define FOOTER_H 26
#define PAD 14
#define LINE_H 18
#define TEXT_Y (TOOLBAR_H + 8)

enum dialog { DLG_NONE = 0, DLG_UNSAVED };
enum pending { PEND_NONE = 0, PEND_CLOSE, PEND_NEW, PEND_OPEN, PEND_OPEN_DIALOG };

static struct {
    uint32_t win_id;
    struct textbuf tb;
    char path[VFS_PATH_MAX + 1]; /* empty = untitled */
    bool dirty;
    bool readonly;
    char status[160];
    bool status_err;
    int scroll; /* first visible visual row */
    bool caret;
    int dialog;
    int pending;
    char pending_path[VFS_PATH_MAX + 1];
    bool save_then_pending; /* a Save As started from the unsaved dialog */
    int hover;
    bool selecting;
} N;

enum { B_NONE = 0, B_NEW, B_OPEN, B_SAVE, B_SAVEAS, B_DLG_SAVE, B_DLG_DISCARD, B_DLG_CANCEL };

static int32_t cell(void) { return text_cell_width(FONT_MONO); }

static struct rect btn_rect(int b, struct surface *s)
{
    switch (b) {
    case B_NEW: return rect_make(10, 9, 56, 26);
    case B_OPEN: return rect_make(72, 9, 60, 26);
    case B_SAVE: return rect_make(138, 9, 56, 26);
    case B_SAVEAS: return rect_make(200, 9, 80, 26);
    default: break;
    }
    struct rect box = rect_make((s->w - 380) / 2, (s->h - 150) / 2, 380, 150);
    switch (b) {
    case B_DLG_SAVE: return rect_make(box.x + box.w - 92, box.y + box.h - 42, 80, 28);
    case B_DLG_DISCARD: return rect_make(box.x + 12, box.y + box.h - 42, 104, 28);
    case B_DLG_CANCEL: return rect_make(box.x + box.w - 180, box.y + box.h - 42, 80, 28);
    default: return rect_make(0, 0, 0, 0);
    }
}

static const char *display_name(void)
{
    if (!N.path[0])
        return "Untitled";
    const char *slash = strrchr(N.path, '/');
    return slash ? slash + 1 : N.path;
}

static void set_status(bool err, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void set_status(bool err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(N.status, sizeof N.status, fmt, ap);
    va_end(ap);
    N.status_err = err;
    TEST_EVENT("NOTES_STATUS err=%d msg=%s", err, N.status);
}

static void update_title(void)
{
    struct window *w = wm_get(N.win_id);
    if (!w)
        return;
    char t[96];
    snprintf(t, sizeof t, "%s%s - Notes", display_name(), N.dirty ? " (edited)" : "");
    wm_set_title(w, t);
    wm_invalidate(w);
}

/* ---- Text layout (character wrap, one cell per code point) ---------------- */

static bool is_cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

static int wrap_cols(struct surface *s)
{
    int c = (s->w - 2 * PAD) / cell();
    return c < 8 ? 8 : c;
}

static void pos_to_rc(int cols, size_t pos, int *row, int *col)
{
    int r = 0, c = 0;
    for (size_t i = 0; i < pos && i < N.tb.len; i++) {
        if (is_cont(N.tb.data[i]))
            continue;
        if (N.tb.data[i] == '\n') {
            r++;
            c = 0;
        } else if (++c >= cols) {
            r++;
            c = 0;
        }
    }
    *row = r;
    *col = c;
}

static size_t rc_to_pos(int cols, int row, int col)
{
    int r = 0, c = 0;
    for (size_t i = 0; i < N.tb.len; i++) {
        if (is_cont(N.tb.data[i]))
            continue;
        if (r == row && c >= col)
            return i;
        if (N.tb.data[i] == '\n') {
            if (r == row)
                return i;
            r++;
            c = 0;
        } else if (++c >= cols) {
            r++;
            c = 0;
            if (r > row) {
                size_t k = i + 1;
                while (k < N.tb.len && is_cont(N.tb.data[k]))
                    k++;
                return k;
            }
        }
    }
    return N.tb.len;
}

static int text_rows(struct surface *s) { return (s->h - TOOLBAR_H - FOOTER_H - 2 * 8) / LINE_H; }

static void ensure_visible(struct surface *s)
{
    int row, col;
    pos_to_rc(wrap_cols(s), N.tb.cursor, &row, &col);
    int rows = text_rows(s);
    if (row < N.scroll)
        N.scroll = row;
    if (row >= N.scroll + rows)
        N.scroll = row - rows + 1;
    if (N.scroll < 0)
        N.scroll = 0;
}

static void report_layout(struct window *w)
{
    TEST_EVENT("NOTES_LAYOUT x=%d y=%d cell=%d line=%d w=%d h=%d", PAD, TEXT_Y, cell(), LINE_H, w->content.w,
               w->content.h);
}

/* ---- Rendering --------------------------------------------------------------- */

static void render(struct window *w, struct surface *s)
{
    const struct theme *t = g_theme;
    int32_t cw = cell();
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->field_bg);
    draw_fill(s, rect_make(0, 0, s->w, TOOLBAR_H), t->title_active);
    draw_hline(s, 0, TOOLBAR_H - 1, s->w, t->separator);
    widget_button(s, btn_rect(B_NEW, s), "New", BTN_NORMAL, N.hover == B_NEW);
    widget_button(s, btn_rect(B_OPEN, s), "Open", BTN_NORMAL, N.hover == B_OPEN);
    widget_button(s, btn_rect(B_SAVE, s), "Save", N.dirty ? BTN_PRIMARY : BTN_NORMAL, N.hover == B_SAVE);
    widget_button(s, btn_rect(B_SAVEAS, s), "Save As", BTN_NORMAL, N.hover == B_SAVEAS);
    char label[VFS_PATH_MAX + 16];
    snprintf(label, sizeof label, "%s%s", N.path[0] ? N.path : "Untitled", N.dirty ? "  - Edited" : "");
    int32_t lx = 296;
    int32_t avail = s->w - lx - 12;
    if (N.readonly) {
        widget_badge(s, s->w - 96, 13, "Read-only", t->warning_bg, t->warning_text);
        avail -= 96;
    }
    if (avail > 20)
        text_draw_fit(s, FONT_UI, lx, (TOOLBAR_H - text_height(FONT_UI)) / 2, label, avail,
                      N.dirty ? t->text : t->text_dim);

    int cols = wrap_cols(s);
    int rows = text_rows(s);
    int row = 0, col = 0;
    int crow, ccol;
    pos_to_rc(cols, N.tb.cursor, &crow, &ccol);
    size_t sel_a = 0, sel_b = 0;
    bool has_sel = tb_selection(&N.tb, &sel_a, &sel_b);
    struct rect area = rect_make(0, TOOLBAR_H, s->w, s->h - TOOLBAR_H - FOOTER_H);
    surface_set_clip(s, area);
    if (N.tb.len == 0 && !N.dialog)
        text_draw(s, FONT_UI, PAD, TEXT_Y, "Start typing...", t->text_dim);
    for (size_t i = 0; i < N.tb.len;) {
        size_t len;
        uint32_t cp = utf8_decode(N.tb.data + i, N.tb.len - i, &len);
        bool in_sel = has_sel && i >= sel_a && i < sel_b;
        if (cp == '\n') {
            if (in_sel && row >= N.scroll && row < N.scroll + rows)
                draw_fill(s, rect_make(PAD + col * cw, TEXT_Y + (row - N.scroll) * LINE_H - 1, cw - 2, LINE_H),
                          t->selection);
            row++;
            col = 0;
            i += len;
            continue;
        }
        if (row >= N.scroll && row < N.scroll + rows) {
            int32_t gy = TEXT_Y + (row - N.scroll) * LINE_H;
            if (in_sel)
                draw_fill(s, rect_make(PAD + col * cw, gy - 1, cw, LINE_H), t->selection);
            text_draw_cp(s, FONT_MONO, PAD + col * cw, gy, cp == '\t' ? ' ' : cp, in_sel ? t->sel_text : t->text);
        }
        i += len;
        if (++col >= cols) {
            row++;
            col = 0;
        }
        if (row >= N.scroll + rows)
            break;
    }
    if (w->focused && N.caret && !N.dialog && !w->sheet && crow >= N.scroll && crow < N.scroll + rows)
        draw_fill(s, rect_make(PAD + ccol * cw, TEXT_Y + (crow - N.scroll) * LINE_H - 1, 2, LINE_H), t->accent);
    surface_reset_clip(s);

    int32_t fy = s->h - FOOTER_H;
    draw_fill(s, rect_make(0, fy, s->w, FOOTER_H), t->win_bg);
    draw_hline(s, 0, fy, s->w, t->separator);
    char pos[48];
    size_t ls = tb_line_start(&N.tb, N.tb.cursor);
    size_t colno = 0;
    for (size_t i = ls; i < N.tb.cursor; i++)
        colno += !is_cont(N.tb.data[i]);
    snprintf(pos, sizeof pos, "Ln %zu, Col %zu  %zu B", tb_line_number(&N.tb, N.tb.cursor) + 1, colno + 1,
             N.tb.len);
    int32_t ty = fy + (FOOTER_H - text_height(FONT_UI)) / 2;
    int32_t status_w = s->w - 40 - text_width(FONT_UI, pos);
    if (status_w > 20) {
        if (N.status[0])
            text_draw_fit(s, FONT_UI, 12, ty, N.status, status_w, N.status_err ? t->danger : t->text_dim);
        else
            text_draw_fit(s, FONT_UI, 12, ty, app_storage_note(), status_w, t->text_dim);
    }
    text_draw(s, FONT_UI, s->w - 12 - text_width(FONT_UI, pos), ty, pos, t->text_dim);

    if (N.dialog == DLG_UNSAVED) {
        draw_blend(s, rect_make(0, 0, s->w, s->h), RGB(0, 0, 0), 70);
        struct rect box = rect_make((s->w - 380) / 2, (s->h - 150) / 2, 380, 150);
        draw_shadow(s, box, 10, 14, 90);
        draw_round_rect(s, box, 10, t->win_bg, 255, 0xF);
        draw_round_frame(s, box, 10, RGB(0, 0, 0), t->border_alpha + 20);
        char q[96];
        snprintf(q, sizeof q, "Save changes to \"%s\"?", display_name());
        text_draw_fit(s, FONT_UI_BOLD, box.x + 16, box.y + 18, q, box.w - 32, t->text);
        text_draw(s, FONT_UI, box.x + 16, box.y + 46, "Your changes will be lost if you", t->text_dim);
        text_draw(s, FONT_UI, box.x + 16, box.y + 64, "don't save them.", t->text_dim);
        widget_button(s, btn_rect(B_DLG_DISCARD, s), "Don't Save", BTN_NORMAL, N.hover == B_DLG_DISCARD);
        widget_button(s, btn_rect(B_DLG_CANCEL, s), "Cancel", BTN_NORMAL, N.hover == B_DLG_CANCEL);
        widget_button(s, btn_rect(B_DLG_SAVE, s), "Save", BTN_PRIMARY, N.hover == B_DLG_SAVE);
    }
}

/* ---- Actions ------------------------------------------------------------------- */

static void do_load(const char *path)
{
    struct vfs_stat st;
    int r = vfs_stat(path, &st);
    if (r == VFS_OK && st.type == VFS_DIR)
        r = VFS_EISDIR;
    if (r == VFS_OK && st.size > TEXTBUF_CAP)
        r = VFS_EFBIG;
    static char tmp[TEXTBUF_CAP];
    size_t len = 0;
    if (r == VFS_OK)
        r = vfs_read_file(path, tmp, sizeof tmp, &len);
    if (r) {
        set_status(true, "Cannot open %s: %s", path, r == VFS_EFBIG ? "file larger than 16 KiB" : vfs_strerror(r));
        TEST_EVENT("NOTES_LOAD path=%s result=%d", path, r);
        update_title();
        return;
    }
    tb_set(&N.tb, tmp, len);
    str_copy(N.path, path, sizeof N.path);
    N.readonly = st.readonly;
    N.dirty = false;
    N.scroll = 0;
    set_status(false, "%s %s (%zu bytes)", N.readonly ? "Opened read-only" : "Opened", path, len);
    TEST_EVENT("NOTES_LOAD path=%s result=0 len=%zu crc=%08x", path, len, crc32_update(0, N.tb.data, N.tb.len));
    update_title();
}

static void do_new(void)
{
    tb_init(&N.tb);
    N.path[0] = '\0';
    N.readonly = false;
    N.dirty = false;
    N.scroll = 0;
    set_status(false, "New note");
    update_title();
}

static void open_dialog_now(void);

static void run_pending(void)
{
    int p = N.pending;
    N.pending = PEND_NONE;
    if (p == PEND_CLOSE) {
        struct window *w = wm_get(N.win_id);
        if (w)
            wm_destroy(w);
    } else if (p == PEND_NEW) {
        do_new();
    } else if (p == PEND_OPEN) {
        do_load(N.pending_path);
    } else if (p == PEND_OPEN_DIALOG) {
        open_dialog_now();
    }
}

static int write_to(const char *path)
{
    int r = vfs_write_file(path, N.tb.data, N.tb.len);
    TEST_EVENT("NOTES_SAVE path=%s result=%d len=%zu crc=%08x", path, r, N.tb.len,
               crc32_update(0, N.tb.data, N.tb.len));
    if (r == VFS_OK) {
        str_copy(N.path, path, sizeof N.path);
        N.dirty = false;
        N.readonly = false;
        set_status(false, "Saved %s (%zu bytes, %s)", path, N.tb.len,
                   vfs_home_persistent() && str_starts_with(path, "/home") ? "on disk" : "in memory");
    } else {
        set_status(true, "Save failed: %s. Use Save As.", vfs_strerror(r));
    }
    update_title();
    return r;
}

static void saveas_done(void *ctx, const char *path)
{
    (void)ctx;
    bool then = N.save_then_pending;
    N.save_then_pending = false;
    if (!path) {
        N.pending = PEND_NONE;
        return;
    }
    if (write_to(path) == VFS_OK && then)
        run_pending();
    else if (then)
        N.pending = PEND_NONE;
}

static void open_saveas(bool then_pending)
{
    struct window *w = wm_get(N.win_id);
    if (!w || w->sheet)
        return;
    char dir[VFS_PATH_MAX + 1] = "/home/notes";
    struct vfs_stat st;
    if (vfs_stat("/home/notes", &st) != VFS_OK)
        vfs_mkdir("/home/notes");
    if (N.path[0] && !N.readonly) {
        char name[VFS_NAME_MAX + 1];
        vfs_split(N.path, dir, sizeof dir, name, sizeof name);
    }
    N.save_then_pending = then_pending;
    TEST_EVENT("NOTES_DIALOG saveas");
    file_dialog(w, FD_SAVE, dir, N.path[0] ? display_name() : "untitled.txt", saveas_done, NULL);
}

static void open_done(void *ctx, const char *path)
{
    (void)ctx;
    if (path)
        do_load(path);
}

static void open_dialog_now(void)
{
    struct window *w = wm_get(N.win_id);
    if (!w || w->sheet)
        return;
    struct vfs_stat st;
    const char *dir = vfs_stat("/home/notes", &st) == VFS_OK ? "/home/notes" : "/home";
    TEST_EVENT("NOTES_DIALOG open");
    file_dialog(w, FD_OPEN, dir, NULL, open_done, NULL);
}

static bool do_save(void)
{
    if (!N.path[0] || N.readonly) {
        if (!N.path[0]) {
            open_saveas(false);
            return false;
        }
    }
    return write_to(N.path) == VFS_OK;
}

static void request(int action, const char *path)
{
    if (path)
        str_copy(N.pending_path, path, sizeof N.pending_path);
    N.pending = action;
    if (N.dirty) {
        N.dialog = DLG_UNSAVED;
        TEST_EVENT("NOTES_DIALOG unsaved");
        wm_invalidate(wm_get(N.win_id));
        return;
    }
    run_pending();
}

static void dialog_button(int b)
{
    if (N.dialog != DLG_UNSAVED)
        return;
    if (b == B_DLG_SAVE) {
        N.dialog = DLG_NONE;
        if (!N.path[0]) {
            open_saveas(true); /* the pending action runs after the save */
        } else if (write_to(N.path) == VFS_OK) {
            run_pending();
        } else {
            N.pending = PEND_NONE; /* save failed: stay, keep edits */
        }
    } else if (b == B_DLG_DISCARD) {
        N.dialog = DLG_NONE;
        N.dirty = false;
        run_pending();
    } else if (b == B_DLG_CANCEL) {
        N.dialog = DLG_NONE;
        N.pending = PEND_NONE;
    }
    update_title();
}

/* ---- Input ------------------------------------------------------------------- */

static void changed_now(bool changed, bool ok)
{
    if (!changed)
        return;
    if (!N.dirty) {
        N.dirty = true;
        update_title();
    }
    if (ok && N.status_err)
        N.status[0] = 0;
}

static void paste_cb(void *ctx, const char *data, size_t n)
{
    (void)ctx;
    struct window *w = wm_get(N.win_id);
    if (!w || !n)
        return;
    bool ok = tb_insert_text(&N.tb, data, n);
    if (!ok)
        set_status(true, "Paste refused: the note would exceed %d bytes.", TEXTBUF_CAP);
    TEST_EVENT("NOTES_PASTE len=%zu ok=%d", n, ok);
    changed_now(ok, ok);
    ensure_visible(&w->content);
    wm_invalidate(w);
}

static void text(struct window *w, const char *utf8)
{
    N.caret = true;
    if (N.dialog == DLG_UNSAVED) {
        if (utf8[0] == 'd' || utf8[0] == 'D')
            dialog_button(B_DLG_DISCARD);
        wm_invalidate(w);
        return;
    }
    char clean[16];
    size_t k = 0;
    for (const char *p = utf8; *p && k + 1 < sizeof clean; p++)
        if ((unsigned char)*p >= 32 && *p != 127)
            clean[k++] = *p;
    if (!k)
        return;
    bool ok = tb_insert_text(&N.tb, clean, k);
    if (!ok) {
        set_status(true, "Note is full (%d bytes max). Nothing was lost.", TEXTBUF_CAP);
        TEST_EVENT("NOTES_FULL len=%zu", N.tb.len);
    }
    changed_now(ok, ok);
    ensure_visible(&w->content);
    wm_invalidate(w);
}

static void key(struct window *w, const struct key_event *e)
{
    N.caret = true;
    if (N.dialog == DLG_UNSAVED) {
        if (e->key == KEY_ENTER)
            dialog_button(B_DLG_SAVE);
        else if (e->key == KEY_ESC)
            dialog_button(B_DLG_CANCEL);
        wm_invalidate(w);
        return;
    }
    if ((e->mods & MOD_CTRL) && e->key == KEY_CHAR) {
        char c = (char)(e->ascii | 0x20);
        if (c == 's') {
            if (e->mods & MOD_SHIFT)
                open_saveas(false);
            else
                do_save();
        } else if (c == 'n') {
            request(PEND_NEW, NULL);
        } else if (c == 'o') {
            request(PEND_OPEN_DIALOG, NULL);
        } else if (c == 'a') {
            tb_select_all(&N.tb);
        } else if (c == 'c' || c == 'x') {
            size_t a, b;
            if (tb_selection(&N.tb, &a, &b)) {
                ui_clipboard_set(N.tb.data + a, b - a);
                TEST_EVENT("NOTES_COPY len=%zu crc=%08x", b - a, crc32_update(0, N.tb.data + a, b - a));
                if (c == 'x')
                    changed_now(tb_delete_selection(&N.tb), true);
            }
        } else if (c == 'v') {
            ui_clipboard_get(paste_cb, NULL);
        }
        ensure_visible(&w->content);
        wm_invalidate(w);
        return;
    }
    bool changed = false, ok = true;
    bool moving = e->key == KEY_LEFT || e->key == KEY_RIGHT || e->key == KEY_UP || e->key == KEY_DOWN ||
                  e->key == KEY_HOME || e->key == KEY_END || e->key == KEY_PGUP || e->key == KEY_PGDN;
    if (moving) {
        if (e->mods & MOD_SHIFT)
            tb_select_begin(&N.tb);
        else
            tb_select_clear(&N.tb);
    }
    switch (e->key) {
    case KEY_LEFT: tb_left(&N.tb); break;
    case KEY_RIGHT: tb_right(&N.tb); break;
    case KEY_UP: tb_up(&N.tb); break;
    case KEY_DOWN: tb_down(&N.tb); break;
    case KEY_PGUP:
        for (int i = 0; i < text_rows(&w->content); i++)
            tb_up(&N.tb);
        break;
    case KEY_PGDN:
        for (int i = 0; i < text_rows(&w->content); i++)
            tb_down(&N.tb);
        break;
    case KEY_HOME: tb_home(&N.tb); break;
    case KEY_END: tb_end(&N.tb); break;
    case KEY_BACKSPACE: changed = tb_backspace(&N.tb); break;
    case KEY_DELETE: changed = tb_delete(&N.tb); break;
    case KEY_ENTER: ok = changed = tb_insert(&N.tb, '\n'); break;
    case KEY_TAB:
        for (int i = 0; i < 4 && ok; i++)
            ok = changed = tb_insert(&N.tb, ' ');
        break;
    default: break;
    }
    if (!ok) {
        set_status(true, "Note is full (%d bytes max). Nothing was lost.", TEXTBUF_CAP);
        TEST_EVENT("NOTES_FULL len=%zu", N.tb.len);
    }
    changed_now(changed, ok);
    ensure_visible(&w->content);
    wm_invalidate(w);
}

static int hit(struct surface *s, int32_t x, int32_t y)
{
    static const int dlg[] = { B_DLG_SAVE, B_DLG_DISCARD, B_DLG_CANCEL };
    static const int tool[] = { B_NEW, B_OPEN, B_SAVE, B_SAVEAS };
    const int *set = N.dialog ? dlg : tool;
    int n = N.dialog ? 3 : 4;
    for (int i = 0; i < n; i++)
        if (rect_contains(btn_rect(set[i], s), x, y))
            return set[i];
    return B_NONE;
}

static size_t text_pos_at(struct surface *s, int32_t x, int32_t y)
{
    int row = N.scroll + (y - TEXT_Y) / LINE_H;
    int col = (x - PAD + cell() / 2) / cell();
    if (y < TEXT_Y)
        row = N.scroll - (y < TOOLBAR_H ? 1 : 0);
    if (row < 0)
        return 0;
    if (col < 0)
        col = 0;
    return rc_to_pos(wrap_cols(s), row, col);
}

static bool word_char(char c) { return c != ' ' && c != '\n' && c != '\t'; }

static void select_word(void)
{
    size_t a = N.tb.cursor, b = N.tb.cursor;
    while (a > 0 && word_char(N.tb.data[a - 1]))
        a--;
    while (b < N.tb.len && word_char(N.tb.data[b]))
        b++;
    N.tb.anchor = a;
    N.tb.cursor = b;
    N.tb.has_sel = a != b;
    N.selecting = false;
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    struct surface *s = &w->content;
    int b = hit(s, m->x, m->y);
    if (m->kind == WM_MOUSE_WHEEL) {
        int max = 0, c;
        pos_to_rc(wrap_cols(s), N.tb.len, &max, &c);
        N.scroll = CLAMP(N.scroll - m->wheel * 3, 0, MAX(0, max - text_rows(s) + 1));
        wm_invalidate(w);
        return;
    }
    if (m->kind == WM_MOUSE_MOVE) {
        if (b != N.hover) {
            N.hover = b;
            wm_invalidate(w);
        }
        if (N.selecting && (m->buttons & 1) && !N.dialog) {
            size_t before = N.tb.cursor;
            N.tb.cursor = text_pos_at(s, m->x, m->y);
            if (N.tb.cursor != before) {
                ensure_visible(s);
                wm_invalidate(w);
            }
        }
        return;
    }
    if (m->kind == WM_MOUSE_UP) {
        N.selecting = false;
        return;
    }
    if (m->kind != WM_MOUSE_DOWN)
        return;
    if (N.dialog) {
        if (b)
            dialog_button(b);
        wm_invalidate(w);
        return;
    }
    switch (b) {
    case B_NEW: request(PEND_NEW, NULL); break;
    case B_OPEN: request(PEND_OPEN_DIALOG, NULL); break;
    case B_SAVE: do_save(); break;
    case B_SAVEAS: open_saveas(false); break;
    default:
        if (m->y > TOOLBAR_H && m->y < s->h - FOOTER_H) {
            if (m->clicks == 2) {
                select_word();
                break;
            }
            tb_select_clear(&N.tb);
            N.tb.cursor = text_pos_at(s, m->x, m->y);
            tb_select_begin(&N.tb);
            N.selecting = true;
        }
        break;
    }
    wm_invalidate(w);
}

static void tick(struct window *w, uint64_t now)
{
    bool on = (now / 500) % 2 == 0 && w->focused;
    if (on != N.caret) {
        N.caret = on;
        wm_invalidate(w);
    }
}

static bool close_request(struct window *w)
{
    (void)w;
    if (!N.dirty)
        return true;
    request(PEND_CLOSE, NULL);
    return false;
}

static void destroyed(struct window *w)
{
    (void)w;
    N.win_id = 0;
    N.dialog = DLG_NONE;
    N.pending = PEND_NONE;
}

static void resized(struct window *w)
{
    ensure_visible(&w->content);
    report_layout(w);
    wm_invalidate(w);
}

static const struct window_ops ops = {
    .render = render, .key = key, .text = text, .mouse = mouse, .tick = tick,
    .close_request = close_request, .destroyed = destroyed, .resized = resized,
};

static struct window *notes_open(void)
{
    struct window *w = wm_get(N.win_id);
    if (w) {
        wm_focus(w);
        return w;
    }
    w = wm_create_ex(APP_NOTES, "Untitled - Notes", 600, 420, WINF_RESIZABLE, 200, 150, 0, 0, &ops, NULL);
    if (!w)
        return NULL;
    N.win_id = w->id;
    do_new();
    N.status[0] = '\0';
    report_layout(w);
    return w;
}

static struct window *open_arg(const char *path)
{
    struct window *w = notes_open();
    if (!w || !path)
        return w;
    wm_focus(w);
    if (N.path[0] && !strcmp(N.path, path) && !N.dirty) {
        do_load(path); /* re-read: the file may have changed */
        return w;
    }
    request(PEND_OPEN, path);
    wm_invalidate(w);
    return w;
}

int main(int argc, char **argv) { return app_main(APP_NOTES, "Notes", open_arg, argc, argv); }
