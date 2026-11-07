#ifndef AURORA_SERVER_H
#define AURORA_SERVER_H
/* Display server internals (0.7): windows owned by connected apps. */
#include "gui/desktop.h"
#include "gui/proto.h"
#include "drivers/input.h"

#define MAX_CLIENTS 24

struct buffer {
    uint32_t id;
    int fd;              /* shared-memory object from the app */
    uint32_t *px;        /* mapped read-only into the server */
    size_t bytes;
    int32_t w, h, stride;
};

struct client;

enum win_state { WIN_NORMAL = 0, WIN_MINIMIZED, WIN_MAXIMIZED };

struct window {
    uint32_t id;         /* server-wide, stable */
    struct client *c;
    uint32_t cid;        /* the app's id for it */
    int app;
    char title[64];
    uint32_t flags;
    struct rect frame;   /* outer rectangle, title bar included */
    struct rect restore_frame;
    enum win_state state, prev_state;
    int32_t min_w, min_h, max_w, max_h; /* content limits */
    struct buffer buf[2];
    int shown;           /* index of the committed buffer, -1 = none yet */
    uint32_t cfg_serial; /* last CONFIGURE sent */
    uint32_t acked;      /* last serial the app acknowledged */
    bool mapped;         /* first commit arrived */
};

struct client {
    int fd;
    int pid;
    int app;
    char name[32];
    bool alive;
};

/* wm.c */
struct window *wm_create(struct client *c, uint32_t cid, const struct m_create *m);
void wm_destroy(struct window *w);
void wm_destroy_client(struct client *c);
struct window *wm_lookup(struct client *c, uint32_t cid);
void wm_request_close(struct window *w);
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
void wm_set_title(struct window *w, const char *title);
void wm_move(struct window *w, int32_t x, int32_t y);
bool wm_resize(struct window *w, int32_t width, int32_t height); /* frame size */
void wm_attach(struct window *w, uint32_t id, int fd, const struct m_attach *a);
void wm_commit(struct window *w, const struct m_commit *m);
struct rect wm_content_rect(const struct window *w);
struct rect wm_damage_rect(const struct window *w);
void wm_paint(struct surface *s, struct window *w, bool focused);
void wm_set_hover_controls(struct window *w);
uint32_t wm_hover_controls(void);
struct rect wm_work_area(void);
void wm_screen_changed(void);
void wm_report(struct window *w);

/* main.c */
void desktop_damage(struct rect r);
int32_t desktop_width(void);
int32_t desktop_height(void);
struct rect dock_rect(void);
void send_to(struct client *c, uint16_t type, uint32_t win, const void *p, size_t n);
void send_focus_change(struct window *old, struct window *now);

/* keymap.c: scan codes to keys and text */
struct key_out {
    struct key_event key;    /* key, ascii, mods, pressed */
    char text[16];           /* characters to insert (may be empty) */
};
bool keymap_load(const char *name);       /* /system/keymaps/<name>.kmap */
const char *keymap_name(void);
int keymap_list(char names[][32], int max);
/* Feeds one scan code; returns true and fills *o when it yields a key. */
bool keymap_feed(uint16_t scancode, bool pressed, struct key_out *o);
bool keymap_caps(void);
uint8_t keymap_mods(void);
bool keymap_is_modifier(uint16_t key);

#endif
