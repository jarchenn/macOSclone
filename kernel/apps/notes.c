/* Notes: plain-text editor that saves to the VFS (RAM-backed /home). */
#include "desktop/desktop.h"
#include "apps/textbuf.h"
#include "fs/vfs.h"
#include "core/log.h"
#include "lib.h"

#define TOOLBAR_H 44
#define FOOTER_H 26
#define PAD 14
#define LINE_H 18

enum dialog { DLG_NONE = 0, DLG_UNSAVED, DLG_SAVEAS };
enum pending { PEND_NONE = 0, PEND_CLOSE, PEND_NEW, PEND_OPEN };

static struct {
    uint32_t win_id;
    struct textbuf tb;
    char path[VFS_PATH_MAX + 1]; /* empty = untitled */
    bool dirty;
    bool readonly;
    char status[128];
    bool status_err;
    int scroll; /* first visible visual row */
    bool caret;
    int dialog;
    int pending;
    char pending_path[VFS_PATH_MAX + 1];
    char field[VFS_NAME_MAX + 1];
    char dlg_error[96];
    int hover;
} N;

/* Toolbar / dialog hit targets */
enum { B_NONE = 0, B_NEW, B_OPEN, B_SAVE, B_SAVEAS, B_DLG_SAVE, B_DLG_DISCARD, B_DLG_CANCEL, B_DLG_OK };

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
    case B_DLG_OK: return rect_make(box.x + box.w - 92, box.y + box.h - 42, 80, 28);
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
    k_vsnprintf(N.status, sizeof N.status, fmt, ap);
    va_end(ap);
    N.status_err = err;
    TEST_EVENT("NOTES_STATUS err=%d msg=%s", err, N.status);
}

static void update_title(void)
{
    struct window *w = wm_get(N.win_id);
    if (!w)
        return;
    char t[80];
    k_snprintf(t, sizeof t, "%s%s - Notes", display_name(), N.dirty ? " (edited)" : "");
    wm_set_title(w, t);
    wm_invalidate(w);
}

/* ---- Text layout (character wrap) ---------------------------------------- */

static int wrap_cols(struct surface *s)
{
    int c = (s->w - 2 * PAD) / 8;
    return c < 8 ? 8 : c;
}

/* Visual (row, col) of buffer position pos. */
static void pos_to_rc(int cols, size_t pos, int *row, int *col)
{
    int r = 0, c = 0;
    for (size_t i = 0; i < pos && i < N.tb.len; i++) {
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
            if (r > row)
                return i + 1;
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
}

/* ---- Rendering --------------------------------------------------------------- */

static void render(struct window *w, struct surface *s)
{
    const struct theme *t = g_theme;
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->field_bg);
    /* Toolbar */
    draw_fill(s, rect_make(0, 0, s->w, TOOLBAR_H), t->title_active);
    draw_hline(s, 0, TOOLBAR_H - 1, s->w, t->separator);
    widget_button(s, btn_rect(B_NEW, s), "New", BTN_NORMAL, N.hover == B_NEW);
    widget_button(s, btn_rect(B_OPEN, s), "Open", BTN_NORMAL, N.hover == B_OPEN);
    widget_button(s, btn_rect(B_SAVE, s), "Save", N.dirty ? BTN_PRIMARY : BTN_NORMAL, N.hover == B_SAVE);
    widget_button(s, btn_rect(B_SAVEAS, s), "Save As", BTN_NORMAL, N.hover == B_SAVEAS);
    char label[96];
    k_snprintf(label, sizeof label, "%s%s", N.path[0] ? N.path : "Untitled", N.dirty ? "  - Edited" : "");
    int32_t lx = 296;
    int32_t avail = s->w - lx - 12;
    if (N.readonly) {
        widget_badge(s, s->w - 96, 13, "Read-only", t->warning_bg, t->warning_text);
        avail -= 96;
    }
    draw_text_fit(s, FONT, lx, 14, label, avail, N.dirty ? t->text : t->text_dim);

    /* Text */
    int cols = wrap_cols(s);
    int rows = text_rows(s);
    int row = 0, col = 0;
    int32_t ty = TOOLBAR_H + 8;
    int crow, ccol;
    pos_to_rc(cols, N.tb.cursor, &crow, &ccol);
    if (N.tb.len == 0 && !N.dialog)
        draw_text(s, FONT, PAD, ty, "Start typing...", t->text_dim);
    for (size_t i = 0; i < N.tb.len; i++) {
        char ch = N.tb.data[i];
        if (ch == '\n') {
            row++;
            col = 0;
            continue;
        }
        if (row >= N.scroll && row < N.scroll + rows) {
            char str[2] = { ch, 0 };
            draw_text(s, FONT, PAD + col * 8, ty + (row - N.scroll) * LINE_H, str, t->text);
        }
        if (++col >= cols) {
            row++;
            col = 0;
        }
        if (row >= N.scroll + rows)
            break;
    }
    bool focused = wm_focused() == w;
    if (focused && N.caret && !N.dialog && crow >= N.scroll && crow < N.scroll + rows)
        draw_fill(s, rect_make(PAD + ccol * 8, ty + (crow - N.scroll) * LINE_H - 1, 2, 18), t->accent);

    /* Footer */
    int32_t fy = s->h - FOOTER_H;
    draw_fill(s, rect_make(0, fy, s->w, FOOTER_H), t->win_bg);
    draw_hline(s, 0, fy, s->w, t->separator);
    char pos[48];
    k_snprintf(pos, sizeof pos, "Ln %zu, Col %zu  %zu B", tb_line_number(&N.tb, N.tb.cursor) + 1,
               N.tb.cursor - tb_line_start(&N.tb, N.tb.cursor) + 1, N.tb.len);
    int32_t status_w = s->w - 40 - text_width(FONT, pos);
    if (N.status[0])
        draw_text_fit(s, FONT, 12, fy + 5, N.status, status_w, N.status_err ? t->danger : t->text_dim);
    else
        draw_text_fit(s, FONT, 12, fy + 5, "Files are stored in memory and reset when Aurora restarts.", status_w, t->text_dim);
    draw_text(s, FONT, s->w - 12 - text_width(FONT, pos), fy + 5, pos, t->text_dim);

    /* Dialogs */
    if (N.dialog) {
        draw_blend(s, rect_make(0, 0, s->w, s->h), RGB(0, 0, 0), 70);
        struct rect box = rect_make((s->w - 380) / 2, (s->h - 150) / 2, 380, 150);
        draw_shadow(s, box, 10, 14, 90);
        draw_round_rect(s, box, 10, t->win_bg, 255, 0xF);
        draw_round_frame(s, box, 10, RGB(0, 0, 0), t->border_alpha + 20);
        if (N.dialog == DLG_UNSAVED) {
            char q[96];
            k_snprintf(q, sizeof q, "Save changes to \"%s\"?", display_name());
            draw_text_bold(s, FONT, box.x + 16, box.y + 18, q, t->text);
            draw_text(s, FONT, box.x + 16, box.y + 46, "Your changes will be lost if you", t->text_dim);
            draw_text(s, FONT, box.x + 16, box.y + 64, "don't save them.", t->text_dim);
            widget_button(s, btn_rect(B_DLG_DISCARD, s), "Don't Save", BTN_NORMAL, N.hover == B_DLG_DISCARD);
            widget_button(s, btn_rect(B_DLG_CANCEL, s), "Cancel", BTN_NORMAL, N.hover == B_DLG_CANCEL);
            widget_button(s, btn_rect(B_DLG_SAVE, s), "Save", BTN_PRIMARY, N.hover == B_DLG_SAVE);
        } else {
            draw_text_bold(s, FONT, box.x + 16, box.y + 16, "Save note as", t->text);
            draw_text(s, FONT, box.x + 16, box.y + 38, "Name (saved in /home/notes):", t->text_dim);
            widget_field(s, rect_make(box.x + 16, box.y + 58, box.w - 32, 28), N.field, true, N.caret);
            if (N.dlg_error[0])
                draw_text_fit(s, FONT, box.x + 16, box.y + 92, N.dlg_error, box.w - 32, t->danger);
            widget_button(s, btn_rect(B_DLG_CANCEL, s), "Cancel", BTN_NORMAL, N.hover == B_DLG_CANCEL);
            widget_button(s, btn_rect(B_DLG_OK, s), "Save", BTN_PRIMARY, N.hover == B_DLG_OK);
        }
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
    TEST_EVENT("NOTES_LOAD path=%s result=0 len=%zu crc=%08x", path, len, crc32(N.tb.data, N.tb.len));
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
    }
}

static int write_to(const char *path)
{
    int r = vfs_write_file(path, N.tb.data, N.tb.len);
    TEST_EVENT("NOTES_SAVE path=%s result=%d len=%zu crc=%08x", path, r, N.tb.len, crc32(N.tb.data, N.tb.len));
    if (r == VFS_OK) {
        str_copy(N.path, path, sizeof N.path);
        N.dirty = false;
        N.readonly = false;
        set_status(false, "Saved %s (%zu bytes, in memory)", path, N.tb.len);
        files_refresh_all();
    } else {
        /* The buffer is untouched: edits are preserved. */
        set_status(true, "Save failed: %s. Use Save As.", vfs_strerror(r));
    }
    update_title();
    return r;
}

static void open_saveas(void)
{
    N.dialog = DLG_SAVEAS;
    N.dlg_error[0] = '\0';
    if (N.path[0] && !N.readonly)
        str_copy(N.field, display_name(), sizeof N.field);
    else if (N.path[0])
        str_copy(N.field, display_name(), sizeof N.field);
    else
        str_copy(N.field, "untitled.txt", sizeof N.field);
    TEST_EVENT("NOTES_DIALOG saveas");
}

/* Save to the current path, or ask for a name. Returns true if saved now. */
static bool do_save(void)
{
    if (!N.path[0]) {
        open_saveas();
        return false;
    }
    return write_to(N.path) == VFS_OK;
}

static void confirm_saveas(void)
{
    char path[VFS_PATH_MAX + 1];
    int r;
    if (N.field[0] == '/')
        r = vfs_normalize("/", N.field, path, sizeof path);
    else
        r = vfs_normalize("/home/notes", N.field, path, sizeof path);
    if (r == VFS_OK) {
        struct vfs_stat st;
        if (vfs_stat("/home/notes", &st) != VFS_OK)
            vfs_mkdir("/home/notes");
        r = write_to(path);
    }
    if (r) {
        k_snprintf(N.dlg_error, sizeof N.dlg_error, "Cannot save: %s", vfs_strerror(r));
        return;
    }
    N.dialog = DLG_NONE;
    run_pending();
}

static void request(int action, const char *path)
{
    if (N.dirty) {
        N.pending = action;
        if (path)
            str_copy(N.pending_path, path, sizeof N.pending_path);
        N.dialog = DLG_UNSAVED;
        TEST_EVENT("NOTES_DIALOG unsaved");
        wm_invalidate(wm_get(N.win_id));
        return;
    }
    N.pending = action;
    if (path)
        str_copy(N.pending_path, path, sizeof N.pending_path);
    run_pending();
}

static void dialog_button(int b)
{
    if (N.dialog == DLG_UNSAVED) {
        if (b == B_DLG_SAVE) {
            N.dialog = DLG_NONE;
            if (do_save())
                run_pending();
            else if (N.dialog != DLG_SAVEAS)
                N.pending = PEND_NONE; /* save failed: stay, keep edits */
        } else if (b == B_DLG_DISCARD) {
            N.dialog = DLG_NONE;
            N.dirty = false;
            run_pending();
        } else if (b == B_DLG_CANCEL) {
            N.dialog = DLG_NONE;
            N.pending = PEND_NONE;
        }
    } else if (N.dialog == DLG_SAVEAS) {
        if (b == B_DLG_OK)
            confirm_saveas();
        else if (b == B_DLG_CANCEL) {
            N.dialog = DLG_NONE;
            N.pending = PEND_NONE;
        }
    }
    update_title();
}

/* ---- Input ------------------------------------------------------------------- */

static void key(struct window *w, const struct key_event *e)
{
    N.caret = true;
    if (N.dialog == DLG_SAVEAS) {
        size_t l = strlen(N.field);
        if (e->key == KEY_ENTER)
            dialog_button(B_DLG_OK);
        else if (e->key == KEY_ESC)
            dialog_button(B_DLG_CANCEL);
        else if (e->key == KEY_BACKSPACE && l > 0)
            N.field[l - 1] = '\0';
        else if (e->key == KEY_CHAR && !(e->mods & MOD_CTRL) && l + 1 < sizeof N.field && e->ascii != ' ')
            N.field[l] = (char)e->ascii, N.field[l + 1] = '\0';
        wm_invalidate(w);
        return;
    }
    if (N.dialog == DLG_UNSAVED) {
        if (e->key == KEY_ENTER)
            dialog_button(B_DLG_SAVE);
        else if (e->key == KEY_ESC)
            dialog_button(B_DLG_CANCEL);
        else if (e->key == KEY_CHAR && (e->ascii == 'd' || e->ascii == 'D'))
            dialog_button(B_DLG_DISCARD);
        wm_invalidate(w);
        return;
    }
    if ((e->mods & MOD_CTRL) && e->key == KEY_CHAR) {
        char c = (char)(e->ascii | 0x20);
        if (c == 's') {
            if (e->mods & MOD_SHIFT)
                open_saveas();
            else
                do_save();
        } else if (c == 'n') {
            request(PEND_NEW, NULL);
        } else if (c == 'o') {
            files_open("/home/notes");
        }
        wm_invalidate(w);
        return;
    }
    bool changed = false, ok = true;
    switch (e->key) {
    case KEY_LEFT: tb_left(&N.tb); break;
    case KEY_RIGHT: tb_right(&N.tb); break;
    case KEY_UP: tb_up(&N.tb); break;
    case KEY_DOWN: tb_down(&N.tb); break;
    case KEY_HOME: tb_home(&N.tb); break;
    case KEY_END: tb_end(&N.tb); break;
    case KEY_BACKSPACE: changed = tb_backspace(&N.tb); break;
    case KEY_DELETE: changed = tb_delete(&N.tb); break;
    case KEY_ENTER: ok = changed = tb_insert(&N.tb, '\n'); break;
    case KEY_TAB:
        for (int i = 0; i < 4 && ok; i++)
            ok = changed = tb_insert(&N.tb, ' ');
        break;
    case KEY_CHAR:
        if (e->ascii)
            ok = changed = tb_insert(&N.tb, (char)e->ascii);
        break;
    default: break;
    }
    if (!ok) {
        set_status(true, "Note is full (%d bytes max). Nothing was lost.", TEXTBUF_CAP);
        TEST_EVENT("NOTES_FULL len=%zu", N.tb.len);
    }
    if (changed) {
        if (!N.dirty) {
            N.dirty = true;
            update_title();
        }
        if (ok && N.status_err)
            N.status[0] = '\0';
    }
    ensure_visible(&w->content);
    wm_invalidate(w);
}

static int hit(struct surface *s, int32_t x, int32_t y)
{
    static const int dlg_unsaved[] = { B_DLG_SAVE, B_DLG_DISCARD, B_DLG_CANCEL };
    static const int dlg_saveas[] = { B_DLG_OK, B_DLG_CANCEL };
    static const int tool[] = { B_NEW, B_OPEN, B_SAVE, B_SAVEAS };
    const int *set = tool;
    int n = 4;
    if (N.dialog == DLG_UNSAVED) {
        set = dlg_unsaved;
        n = 3;
    } else if (N.dialog == DLG_SAVEAS) {
        set = dlg_saveas;
        n = 2;
    }
    for (int i = 0; i < n; i++)
        if (rect_contains(btn_rect(set[i], s), x, y))
            return set[i];
    return B_NONE;
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    struct surface *s = &w->content;
    int b = hit(s, m->x, m->y);
    if (m->kind == WM_MOUSE_MOVE) {
        if (b != N.hover) {
            N.hover = b;
            wm_invalidate(w);
        }
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
    case B_OPEN: files_open("/home/notes"); break;
    case B_SAVE: do_save(); break;
    case B_SAVEAS: open_saveas(); break;
    default:
        if (m->y > TOOLBAR_H && m->y < s->h - FOOTER_H) {
            int row = N.scroll + (m->y - TOOLBAR_H - 8) / LINE_H;
            int col = (m->x - PAD + 4) / 8;
            if (row < 0)
                row = 0;
            if (col < 0)
                col = 0;
            N.tb.cursor = rc_to_pos(wrap_cols(s), row, col);
            N.tb.goal_col = N.tb.cursor - tb_line_start(&N.tb, N.tb.cursor);
        }
        break;
    }
    wm_invalidate(w);
}

static void tick(struct window *w, uint64_t now)
{
    bool on = (now / 500) % 2 == 0 && wm_focused() == w;
    if (on != N.caret) {
        N.caret = on;
        wm_invalidate(w);
    }
}

static bool close_request(struct window *w)
{
    UNUSED(w);
    if (!N.dirty)
        return true;
    request(PEND_CLOSE, NULL);
    return false;
}

static void destroyed(struct window *w)
{
    UNUSED(w);
    N.win_id = 0;
    N.dialog = DLG_NONE;
    N.pending = PEND_NONE;
}

static void resized(struct window *w)
{
    ensure_visible(&w->content);
    wm_invalidate(w);
}

static const struct window_ops ops = {
    .render = render, .key = key, .mouse = mouse, .tick = tick,
    .close_request = close_request, .destroyed = destroyed, .resized = resized,
};

struct window *notes_open(void)
{
    struct window *w = wm_get(N.win_id);
    if (w) {
        wm_focus(w);
        return w;
    }
    w = wm_create(APP_NOTES, "Untitled - Notes", 600, 420, &ops, NULL);
    if (!w)
        return NULL;
    N.win_id = w->id;
    do_new();
    N.status[0] = '\0';
    return w;
}

struct window *notes_open_path(const char *path)
{
    struct window *w = notes_open();
    if (!w)
        return NULL;
    wm_focus(w);
    if (N.path[0] && !strcmp(N.path, path) && !N.dirty) {
        do_load(path); /* re-read: the file may have changed */
        return w;
    }
    request(PEND_OPEN, path);
    wm_invalidate(w);
    return w;
}
