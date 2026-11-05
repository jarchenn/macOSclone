/* Files: browses the real VFS. Re-reads the directory whenever the VFS
 * generation counter changes, so edits from Terminal/Notes show up. */
#include "desktop/desktop.h"
#include "apps/app_util.h"
#include "fs/vfs.h"
#include "core/log.h"
#include "lib.h"

#define MAX_ENTRIES 128
#define TOOLBAR_H 44
#define SIDEBAR_W 150
#define ROW_H 26
#define HEADER_H 26
#define FOOTER_H 26

static struct {
    uint32_t win_id;
    char path[VFS_PATH_MAX + 1];
    struct vfs_dirent ents[MAX_ENTRIES];
    int n;
    bool truncated;
    int sel;
    int scroll;
    uint32_t gen;
    int err;
    bool dir_ro;
    int hover_btn; /* 1 = up, 2 = refresh, 10+i sidebar */
    int hover_row;
} F;

static const struct {
    const char *label;
    const char *path;
} places[] = {
    { "Home", "/home" },
    { "Notes", "/home/notes" },
    { "System", "/system" },
    { "Root", "/" },
};

static struct rect up_btn(void) { return rect_make(10, 9, 30, 26); }
static struct rect refresh_btn(struct surface *s) { return rect_make(s->w - 90, 9, 80, 26); }
static struct rect place_rect(int i) { return rect_make(8, TOOLBAR_H + 30 + i * 28, SIDEBAR_W - 16, 26); }

static void reload(void)
{
    F.n = 0;
    F.truncated = false;
    struct vfs_stat st;
    F.err = vfs_stat(F.path, &st);
    if (F.err == VFS_OK && st.type != VFS_DIR)
        F.err = VFS_ENOTDIR;
    F.dir_ro = F.err == VFS_OK && st.readonly;
    if (F.err == VFS_OK) {
        /* Directories first, then files; both already sorted by name. */
        struct vfs_dirent d;
        for (int pass = 0; pass < 2; pass++) {
            for (uint32_t i = 0; vfs_readdir(F.path, i, &d) == VFS_OK; i++) {
                if ((pass == 0) != (d.type == VFS_DIR))
                    continue;
                if (F.n >= MAX_ENTRIES) {
                    F.truncated = true;
                    break;
                }
                F.ents[F.n++] = d;
            }
        }
    }
    if (F.sel >= F.n)
        F.sel = F.n - 1;
    F.gen = vfs_generation();
    TEST_EVENT("FILES path=%s entries=%d err=%d", F.path, F.n, F.err);
}

static void navigate(const char *path)
{
    char norm[VFS_PATH_MAX + 1];
    if (vfs_normalize("/", path, norm, sizeof norm) != VFS_OK)
        return;
    str_copy(F.path, norm, sizeof F.path);
    F.sel = -1;
    F.scroll = 0;
    reload();
    char title[80];
    k_snprintf(title, sizeof title, "Files - %s", F.path);
    struct window *w = wm_get(F.win_id);
    if (w) {
        wm_set_title(w, title);
        wm_invalidate(w);
    }
}

static void go_up(void)
{
    if (!strcmp(F.path, "/"))
        return;
    char parent[VFS_PATH_MAX + 1];
    char name[VFS_NAME_MAX + 1];
    if (vfs_split(F.path, parent, sizeof parent, name, sizeof name) == VFS_OK)
        navigate(parent);
}

static void open_entry(int i)
{
    if (i < 0 || i >= F.n)
        return;
    char full[VFS_PATH_MAX + 1];
    str_copy(full, F.path, sizeof full);
    if (strcmp(full, "/"))
        str_append(full, "/", sizeof full);
    str_append(full, F.ents[i].name, sizeof full);
    TEST_EVENT("FILES_OPEN path=%s", full);
    if (F.ents[i].type == VFS_DIR)
        navigate(full);
    else
        notes_open_path(full);
}

static int visible_rows(struct surface *s) { return (s->h - TOOLBAR_H - HEADER_H - FOOTER_H) / ROW_H; }

static void render(struct window *w, struct surface *s)
{
    UNUSED(w);
    const struct theme *t = g_theme;
    if (F.gen != vfs_generation())
        reload();
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->win_bg);
    /* Toolbar */
    draw_fill(s, rect_make(0, 0, s->w, TOOLBAR_H), t->title_active);
    draw_hline(s, 0, TOOLBAR_H - 1, s->w, t->separator);
    widget_button(s, up_btn(), "<", BTN_NORMAL, F.hover_btn == 1);
    widget_field(s, rect_make(48, 9, s->w - 48 - 100, 26), F.path, false, false);
    widget_button(s, refresh_btn(s), "Refresh", BTN_NORMAL, F.hover_btn == 2);
    /* Sidebar */
    draw_fill(s, rect_make(0, TOOLBAR_H, SIDEBAR_W, s->h - TOOLBAR_H), t->sidebar_bg);
    draw_vline(s, SIDEBAR_W - 1, TOOLBAR_H, s->h - TOOLBAR_H, t->separator);
    draw_text(s, FONT, 16, TOOLBAR_H + 10, "Locations", t->text_dim);
    for (int i = 0; i < (int)ARRAY_LEN(places); i++) {
        struct rect r = place_rect(i);
        bool cur = !strcmp(F.path, places[i].path);
        if (cur)
            draw_round_rect(s, r, 6, t->selection, 255, 0xF);
        else if (F.hover_btn == 10 + i)
            draw_round_rect(s, r, 6, t->button_hover, 255, 0xF);
        app_draw_folder_icon(s, r.x + 8, r.y + 5);
        draw_text(s, FONT, r.x + 34, r.y + 5, places[i].label, cur ? t->sel_text : t->text);
    }
    /* List */
    int32_t lx = SIDEBAR_W;
    int32_t lw = s->w - SIDEBAR_W;
    int32_t y = TOOLBAR_H;
    draw_text(s, FONT, lx + 40, y + 5, "Name", t->text_dim);
    draw_text(s, FONT, lx + lw - 200, y + 5, "Size", t->text_dim);
    draw_text(s, FONT, lx + lw - 110, y + 5, "Kind", t->text_dim);
    draw_hline(s, lx, y + HEADER_H - 1, lw, t->separator);
    y += HEADER_H;
    if (F.err) {
        char msg[96];
        k_snprintf(msg, sizeof msg, "Cannot open %s: %s", F.path, vfs_strerror(F.err));
        draw_text(s, FONT, lx + 20, y + 20, msg, t->danger);
    } else if (F.n == 0) {
        const char *m = "This folder is empty";
        draw_text(s, FONT, lx + (lw - text_width(FONT, m)) / 2, y + 60, m, t->text_dim);
    }
    int rows = visible_rows(s);
    for (int i = F.scroll; i < F.n && i < F.scroll + rows; i++) {
        struct rect r = rect_make(lx + 6, y, lw - 12, ROW_H);
        bool sel = i == F.sel;
        if (sel)
            draw_round_rect(s, r, 6, t->accent, 255, 0xF);
        else if (i == F.hover_row)
            draw_round_rect(s, r, 6, t->button_hover, 255, 0xF);
        uint32_t fg = sel ? t->accent_text : t->text;
        uint32_t dim = sel ? t->accent_text : t->text_dim;
        if (F.ents[i].type == VFS_DIR)
            app_draw_folder_icon(s, r.x + 8, r.y + 5);
        else
            app_draw_file_icon(s, r.x + 8, r.y + 5);
        draw_text_fit(s, FONT, r.x + 34, r.y + 5, F.ents[i].name, lw - 260, fg);
        char size[24] = "--";
        if (F.ents[i].type == VFS_FILE)
            k_snprintf(size, sizeof size, "%u B", F.ents[i].size);
        draw_text(s, FONT, lx + lw - 200, r.y + 5, size, dim);
        draw_text(s, FONT, lx + lw - 110, r.y + 5, F.ents[i].type == VFS_DIR ? "Folder" : "Text", dim);
        y += ROW_H;
    }
    /* Footer */
    int32_t fy = s->h - FOOTER_H;
    draw_fill(s, rect_make(lx, fy, lw, FOOTER_H), t->win_bg);
    draw_hline(s, lx, fy, lw, t->separator);
    char foot[96];
    k_snprintf(foot, sizeof foot, "%d item%s%s", F.n, F.n == 1 ? "" : "s", F.truncated ? " (list truncated)" : "");
    draw_text(s, FONT, lx + 12, fy + 5, foot, t->text_dim);
    const char *loc = F.dir_ro ? "Read-only" : "RAM storage - resets on restart";
    if (F.err == VFS_OK)
        widget_badge(s, lx + lw - 16 - text_width(FONT, loc) - 12, fy + 4, loc,
                     F.dir_ro ? t->warning_bg : t->selection, F.dir_ro ? t->warning_text : t->sel_text);
}

static int row_at(struct surface *s, int32_t x, int32_t y)
{
    if (x < SIDEBAR_W || y < TOOLBAR_H + HEADER_H || y >= s->h - FOOTER_H)
        return -1;
    int i = F.scroll + (y - TOOLBAR_H - HEADER_H) / ROW_H;
    return i < F.n ? i : -1;
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    struct surface *s = &w->content;
    if (m->kind == WM_MOUSE_MOVE) {
        int hb = 0;
        if (rect_contains(up_btn(), m->x, m->y))
            hb = 1;
        else if (rect_contains(refresh_btn(s), m->x, m->y))
            hb = 2;
        for (int i = 0; i < (int)ARRAY_LEN(places); i++)
            if (rect_contains(place_rect(i), m->x, m->y))
                hb = 10 + i;
        int hr = row_at(s, m->x, m->y);
        if (hb != F.hover_btn || hr != F.hover_row) {
            F.hover_btn = hb;
            F.hover_row = hr;
            wm_invalidate(w);
        }
        return;
    }
    if (m->kind != WM_MOUSE_DOWN)
        return;
    if (rect_contains(up_btn(), m->x, m->y)) {
        go_up();
        return;
    }
    if (rect_contains(refresh_btn(s), m->x, m->y)) {
        reload();
        wm_invalidate(w);
        return;
    }
    for (int i = 0; i < (int)ARRAY_LEN(places); i++)
        if (rect_contains(place_rect(i), m->x, m->y)) {
            navigate(places[i].path);
            return;
        }
    int r = row_at(s, m->x, m->y);
    if (r >= 0) {
        F.sel = r;
        TEST_EVENT("FILES_SELECT name=%s clicks=%u", F.ents[r].name, m->clicks);
        if (m->clicks >= 2)
            open_entry(r);
        wm_invalidate(w);
    }
}

static void key(struct window *w, const struct key_event *e)
{
    int rows = visible_rows(&w->content);
    switch (e->key) {
    case KEY_UP: if (F.sel > 0) F.sel--; else F.sel = 0; break;
    case KEY_DOWN: if (F.sel < F.n - 1) F.sel++; break;
    case KEY_ENTER: open_entry(F.sel); return;
    case KEY_BACKSPACE: go_up(); return;
    default: return;
    }
    if (F.sel >= 0 && F.sel < F.scroll)
        F.scroll = F.sel;
    if (F.sel >= F.scroll + rows)
        F.scroll = F.sel - rows + 1;
    wm_invalidate(w);
}

static void tick(struct window *w, uint64_t now)
{
    UNUSED(now);
    if (F.gen != vfs_generation())
        wm_invalidate(w);
}

static void destroyed(struct window *w)
{
    UNUSED(w);
    F.win_id = 0;
}

static void resized(struct window *w) { wm_invalidate(w); }

static const struct window_ops ops = {
    .render = render, .key = key, .mouse = mouse, .tick = tick, .destroyed = destroyed, .resized = resized,
};

void files_refresh_all(void)
{
    struct window *w = wm_get(F.win_id);
    if (w)
        wm_invalidate(w);
}

struct window *files_open(const char *path)
{
    struct window *w = wm_get(F.win_id);
    if (!w) {
        w = wm_create(APP_FILES, "Files", 640, 420, &ops, NULL);
        if (!w)
            return NULL;
        F.win_id = w->id;
        F.hover_row = -1;
        navigate(path ? path : "/home");
    } else if (path) {
        navigate(path);
    }
    wm_focus(w);
    return w;
}
