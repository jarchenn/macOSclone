#ifndef AURORA_UI_H
#define AURORA_UI_H
/* App side of the desktop (0.7): the client library every app links. It
 * keeps the 0.1 window model (a window with render/key/mouse callbacks)
 * but each window now lives in the display server: content is drawn into
 * shared-memory buffers and handed over with an explicit commit, and an
 * event loop with timers and descriptor watches drives the callbacks. */
#include "gui/desktop.h"
#include "gui/proto.h"
#include "drivers/input.h"

enum win_state { WIN_NORMAL = 0, WIN_MINIMIZED, WIN_MAXIMIZED };
enum win_mouse_kind { WM_MOUSE_DOWN = 1, WM_MOUSE_UP, WM_MOUSE_MOVE, WM_MOUSE_WHEEL };

struct win_mouse {
    int32_t x, y; /* content-relative; -1,-1 when the pointer left */
    uint8_t kind;
    uint8_t buttons;
    uint8_t clicks; /* 2 for a double click */
    int8_t wheel;   /* WM_MOUSE_WHEEL: + = up */
};

struct window;
struct window_ops {
    void (*render)(struct window *w, struct surface *s);
    /* Key presses (and repeats). Plain printable keys arrive as text
     * instead; KEY_CHAR events come only with Ctrl or Alt held. */
    void (*key)(struct window *w, const struct key_event *e);
    /* Characters typed (UTF-8, dead keys composed). Without this callback
     * ASCII characters are delivered to key() as KEY_CHAR events. */
    void (*text)(struct window *w, const char *utf8);
    void (*mouse)(struct window *w, const struct win_mouse *m);
    /* Return true if the window may close now; false to veto (e.g. show an
     * unsaved-changes dialog and close later via wm_destroy). */
    bool (*close_request)(struct window *w);
    void (*destroyed)(struct window *w);
    void (*tick)(struct window *w, uint64_t now_ms); /* every 250 ms */
    void (*resized)(struct window *w);
};

struct ui_buffer {
    uint32_t id;
    int fd;
    uint32_t *px;
    size_t bytes;
    int32_t w, h;
    bool busy; /* held by the server */
};

struct window {
    uint32_t id;
    int app;
    char title[64];
    struct surface content; /* size is current; px valid inside render() */
    enum win_state state;
    bool focused;
    bool dirty;
    const struct window_ops *ops;
    void *data;
    /* library state */
    struct ui_buffer buf[2];
    uint32_t cfg_serial, acked;
    bool configured, closing;
    const struct window_ops *sheet; /* modal layer (gui/tk.h), or NULL */
    void *sheet_data;
    struct window *next;
};

/* Connects to the display server; false if it is not running. */
bool ui_init(int app, const char *name);
/* Runs until the last window closes or the server goes away. */
int ui_run(void);
void ui_quit(void);

struct window *wm_create(int app, const char *title, int32_t content_w, int32_t content_h,
                         const struct window_ops *ops, void *data);
/* flags: WINF_RESIZABLE, WINF_DIALOG; min/max content size (0 = none). */
struct window *wm_create_ex(int app, const char *title, int32_t content_w, int32_t content_h, uint32_t flags,
                            int32_t min_w, int32_t min_h, int32_t max_w, int32_t max_h,
                            const struct window_ops *ops, void *data);
void wm_request_close(struct window *w);
void wm_destroy(struct window *w);
void wm_invalidate(struct window *w);
void wm_set_title(struct window *w, const char *title);
void wm_minimize(struct window *w);
void wm_toggle_maximize(struct window *w);
void wm_focus(struct window *w);
int wm_count(void);
struct window *wm_first(void);
struct window *wm_get(uint32_t id);

/* Event loop services. */
typedef void (*ui_timer_fn)(void *ctx);
int ui_timer_add(uint32_t ms, bool repeat, ui_timer_fn fn, void *ctx); /* -> id */
void ui_timer_cancel(int id);
typedef void (*ui_fd_fn)(void *ctx, int revents);
bool ui_watch_fd(int fd, int events, ui_fd_fn fn, void *ctx);
void ui_unwatch_fd(int fd);
/* Called once per loop iteration after events (child exits, etc.). */
void ui_set_idle(void (*fn)(void));

/* Desktop services through the server. */
void ui_clipboard_set(const char *text, size_t len);
typedef void (*ui_clip_fn)(void *ctx, const char *data, size_t len);
void ui_clipboard_get(ui_clip_fn fn, void *ctx);
void ui_launch(const char *program, const char *arg);
void ui_set_theme(bool dark);
void ui_set_mode(int32_t w, int32_t h);
void ui_set_keymap(const char *name);
void ui_set_repeat(uint32_t delay_ms, uint32_t rate_hz);
void ui_power(int how);
typedef void (*ui_settings_fn)(const struct m_settings *s);
void ui_on_settings(ui_settings_fn fn); /* also requests the current values */

/* Single-instance apps: called when the app is launched again (arg may be
 * NULL). */
void ui_on_open(struct window *(*fn)(const char *arg));

uint64_t desktop_now_ms(void);
int32_t desktop_width(void);
int32_t desktop_height(void);
struct rect ui_work_area(void);

#endif
