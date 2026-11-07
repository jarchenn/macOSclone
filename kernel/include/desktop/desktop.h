#ifndef AURORA_DESKTOP_H
#define AURORA_DESKTOP_H
#include "types.h"
#include "graphics/surface.h"
#include "drivers/input.h"

/* ---- Layout constants ---------------------------------------------------- */
#define MENUBAR_H 26
#define TITLE_H 30
#define WIN_RADIUS 10
#define DOCK_ICON 52
#define DOCK_GAP 12
#define DOCK_PAD 10
#define DOCK_MARGIN 8
#define MAX_WINDOWS 16

/* Title-bar controls: centres relative to the window frame origin. */
#define CTRL_Y 15
#define CTRL_CLOSE_X 20
#define CTRL_MIN_X 40
#define CTRL_MAX_X 60
#define CTRL_R 6

enum app_id { APP_TERMINAL = 0, APP_FILES, APP_NOTES, APP_SETTINGS, APP_WELCOME, APP_COUNT };
extern const char *const app_names[APP_COUNT];

/* ---- Theme ------------------------------------------------------------- */
struct theme {
    bool dark;
    uint32_t win_bg, win_border, title_active, title_inactive, title_text, title_text_inactive;
    uint32_t text, text_dim, accent, accent_text, selection, sel_text;
    uint32_t field_bg, field_border, button_bg, button_border, button_text, button_hover;
    uint32_t menubar_bg, menubar_text;
    uint32_t dock_bg, dock_border;
    uint32_t term_bg, term_text, term_prompt, term_error;
    uint32_t separator, sidebar_bg, warning_bg, warning_text, danger;
    uint32_t ctrl_inactive;
    uint8_t menubar_alpha, dock_alpha, border_alpha;
};
extern const struct theme *g_theme;
void theme_set(bool dark);
void wallpaper_generate(struct surface *s);
void icon_draw(struct surface *s, int app, int32_t x, int32_t y, bool pressed);
void logo_draw(struct surface *s, int32_t cx, int32_t cy);

/* ---- Windows ------------------------------------------------------------- */
enum win_state { WIN_NORMAL = 0, WIN_MINIMIZED, WIN_MAXIMIZED };
enum win_mouse_kind { WM_MOUSE_DOWN = 1, WM_MOUSE_UP, WM_MOUSE_MOVE };

struct win_mouse {
    int32_t x, y; /* content-relative */
    uint8_t kind;
    uint8_t buttons;
    uint8_t clicks; /* 2 for a double click */
};

struct window;
struct window_ops {
    void (*render)(struct window *w, struct surface *s);
    void (*key)(struct window *w, const struct key_event *e);
    void (*mouse)(struct window *w, const struct win_mouse *m);
    /* Return true if the window may close now; false to veto (e.g. show an
     * unsaved-changes dialog and close later via wm_destroy). */
    bool (*close_request)(struct window *w);
    void (*destroyed)(struct window *w);
    void (*tick)(struct window *w, uint64_t now_ms);
    void (*resized)(struct window *w);
};

struct window {
    uint32_t id;
    int app;
    char title[64];
    struct rect frame; /* outer rectangle, title bar included */
    struct rect restore_frame;
    enum win_state state, prev_state;
    struct surface content;
    bool dirty;
    const struct window_ops *ops;
    void *data;
};

struct window *wm_create(int app, const char *title, int32_t content_w, int32_t content_h,
                         const struct window_ops *ops, void *data);
void wm_request_close(struct window *w);
void wm_destroy(struct window *w);
void wm_focus(struct window *w);
void wm_minimize(struct window *w);
void wm_restore(struct window *w);
void wm_toggle_maximize(struct window *w);
void wm_cycle(void);
struct window *wm_find_app(int app);
struct window *wm_get(uint32_t id);
struct window *wm_focused(void);
struct window *wm_top_at(int32_t x, int32_t y);
int wm_count(void);
struct window *wm_at_z(int i); /* 0 = bottom */
void wm_invalidate(struct window *w);
void wm_invalidate_all(void);
void wm_set_title(struct window *w, const char *title);
void wm_move(struct window *w, int32_t x, int32_t y);
/* Resizes a normal window (frame size, title bar included). */
bool wm_resize(struct window *w, int32_t width, int32_t height);
#define WM_GRIP 16
#define WM_MIN_W 320
#define WM_MIN_H 180
struct rect wm_content_rect(const struct window *w);
struct rect wm_damage_rect(const struct window *w);
void wm_render_dirty(void);
void wm_paint(struct surface *s, struct window *w, bool focused);
void wm_set_hover_controls(struct window *w);
uint32_t wm_hover_controls(void);
struct rect wm_work_area(void);

/* ---- Desktop services ---------------------------------------------------- */
void desktop_damage(struct rect r);
void desktop_damage_all(void);
void desktop_theme_changed(void);
uint64_t desktop_now_ms(void);
int32_t desktop_width(void);
int32_t desktop_height(void);
struct rect dock_rect(void);
void desktop_request_reboot(void);
void desktop_request_poweroff(void);
void desktop_input_stats(uint32_t *dropped, uint32_t *coalesced);
void desktop_main(void);

/* ---- Widgets ------------------------------------------------------------- */
enum button_style { BTN_NORMAL = 0, BTN_PRIMARY, BTN_DANGER };
void widget_button(struct surface *s, struct rect r, const char *label, int style, bool hover);
void widget_field(struct surface *s, struct rect r, const char *text, bool focused, bool caret);
void widget_separator(struct surface *s, int32_t x, int32_t y, int32_t w);
void widget_badge(struct surface *s, int32_t x, int32_t y, const char *text, uint32_t bg, uint32_t fg);

/* ---- Apps ------------------------------------------------------------------ */
struct window *app_launch(int app);
struct window *terminal_open(void);
struct window *files_open(const char *path);
struct window *notes_open(void);
struct window *notes_open_path(const char *path);
struct window *settings_open(int tab);
struct window *welcome_open(void);
void files_refresh_all(void);

#endif
