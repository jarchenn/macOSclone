#ifndef AURORA_TK_H
#define AURORA_TK_H
/* Toolkit (0.7d): retained widgets for the apps - label, button, text
 * field, list, scroll view, menu button - grouped in a panel that does hit
 * testing, keyboard focus (Tab / Shift+Tab), mouse capture and drawing.
 * Kept to what the core apps and the file dialogs need. Text is UTF-8. */
#include "desktop/desktop.h"

enum tk_kind { TK_LABEL = 1, TK_BUTTON, TK_FIELD, TK_LIST, TK_SCROLL, TK_MENU };

#define TK_TEXT_MAX 256
#define TK_MENU_ITEMS 12

struct tk_widget;
typedef void (*tk_cb)(struct tk_widget *w, void *ctx);
/* List items and scroll content come from the app. */
typedef const char *(*tk_item_fn)(void *ctx, int index, int *icon); /* icon: 0 none, 1 folder, 2 file */
typedef void (*tk_paint_fn)(void *ctx, struct surface *s, struct rect view, int32_t offset);

struct tk_widget {
    int kind;
    int id;
    struct rect r;
    bool visible, enabled;
    bool hover, pressed;
    int style;                 /* buttons: BTN_*; labels: 0 normal, 1 dim, 2 bold, 3 title, 4 danger */
    char text[TK_TEXT_MAX];
    /* field */
    size_t caret, anchor;
    bool has_sel;
    int32_t scroll_x;
    char placeholder[64];
    /* list */
    tk_item_fn item;
    int count, selected, top;
    int32_t row_h;
    /* scroll view */
    tk_paint_fn paint;
    int32_t content_h, offset;
    /* menu button */
    const char *items[TK_MENU_ITEMS];
    int nitems, menu_hover;
    bool menu_open;
    /* callbacks */
    tk_cb on_activate;         /* button/menu click, field Enter, list double-click/Enter */
    tk_cb on_change;           /* field edited, list selection moved */
    void *ctx;
};

#define TK_MAX_WIDGETS 40
struct tk_panel {
    struct window *win;
    struct tk_widget w[TK_MAX_WIDGETS];
    int n;
    int focus;                 /* index or -1 */
    int capture;               /* widget holding the mouse, -1 */
    bool caret_on;
};

void tk_init(struct tk_panel *p, struct window *win);
struct tk_widget *tk_add(struct tk_panel *p, int kind, struct rect r, const char *text);
void tk_set_text(struct tk_widget *w, const char *text);
void tk_focus(struct tk_panel *p, struct tk_widget *w);
struct tk_widget *tk_focused(struct tk_panel *p);
void tk_list_set(struct tk_widget *w, int count, tk_item_fn item, void *ctx);
void tk_list_select(struct tk_widget *w, int index); /* scrolls it into view */
void tk_field_select_all(struct tk_widget *w);

void tk_render(struct tk_panel *p, struct surface *s);
/* Each returns true if the panel used the event. */
bool tk_key(struct tk_panel *p, const struct key_event *e);
bool tk_text(struct tk_panel *p, const char *utf8);
bool tk_mouse(struct tk_panel *p, const struct win_mouse *m);
void tk_tick(struct tk_panel *p, uint64_t now);

/* ---- file dialogs (0.7f) ----------------------------------------------------
 * Modal sheets over a window: directory browsing (double-click or Enter
 * opens a folder, the Up button goes to the parent), New Folder, and for
 * Save a name field plus overwrite confirmation. `done` gets the chosen
 * absolute path, or NULL if cancelled. */
enum { FD_OPEN = 1, FD_SAVE };
typedef void (*fd_done_fn)(void *ctx, const char *path);
void file_dialog(struct window *w, int mode, const char *dir, const char *name, fd_done_fn done, void *ctx);
bool file_dialog_active(struct window *w);

/* Sheets: a modal layer with its own callbacks drawn over a window. The
 * client library routes input to the sheet while one is open. */
void ui_sheet_open(struct window *w, const struct window_ops *ops, void *data);
void ui_sheet_close(struct window *w);

#endif
