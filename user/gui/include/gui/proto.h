#ifndef AURORA_GUI_PROTO_H
#define AURORA_GUI_PROTO_H
/* Display protocol (0.7a). Apps connect to the local socket
 * DISPLAY_SOCKET; every message is one local-socket message starting with
 * struct msg_hdr. Pixels never travel in messages: a window's content lives
 * in shared-memory buffers the app attaches (the descriptor rides along
 * with ATTACH). The handoff is explicit:
 *
 *   app draws into a free buffer -> COMMIT(buffer, damage)
 *   server composites from that buffer and sends RELEASE for the one it
 *   showed before; the app may draw into a buffer only while it holds it.
 *
 * Resizing is a configure/redraw/acknowledge exchange: the server sends
 * CONFIGURE(w, h, serial); the app attaches buffers of that size, draws and
 * commits with ack_serial = serial. Until then the server keeps showing the
 * old buffer (clipped, the rest filled with the window background).
 *
 * Decorations (title bar, controls, shadow, border) are drawn by the server.
 * Keys arrive twice by design: KEY events for every press/release (shortcuts,
 * navigation) and TEXT events with the characters the keymap produced
 * (dead keys already composed), so an input method can sit in between later. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DISPLAY_SOCKET "system.display"
#define PROTO_VERSION 1
#define PROTO_MAX_MSG 65536
#define CLIP_MAX 60000 /* clipboard payload per message */

enum msg_type {
    /* app -> server */
    C_HELLO = 1,
    C_CREATE,        /* struct m_create */
    C_DESTROY,
    C_ATTACH,        /* struct m_attach + 1 descriptor */
    C_COMMIT,        /* struct m_commit */
    C_SET_TITLE,     /* struct m_title */
    C_WINDOW_OP,     /* struct m_op: minimize, maximize, close, focus */
    C_SET_CLIPBOARD, /* struct m_clip + data */
    C_GET_CLIPBOARD, /* struct m_clip (mime only) */
    C_LAUNCH,        /* struct m_launch */
    C_SET_THEME,     /* struct m_theme */
    C_SET_MODE,      /* struct m_size */
    C_SET_KEYMAP,    /* struct m_name */
    C_SET_REPEAT,    /* struct m_repeat */
    C_GET_SETTINGS,
    C_POWER,         /* struct m_op (op = 1 restart, 2 power off) */

    /* server -> app */
    S_WELCOME = 64,  /* struct m_welcome */
    S_CONFIGURE,     /* struct m_configure */
    S_CLOSE_REQUEST,
    S_FOCUS,         /* struct m_focus */
    S_KEY,           /* struct m_key */
    S_TEXT,          /* struct m_text */
    S_MOUSE,         /* struct m_mouse */
    S_RELEASE,       /* struct m_release */
    S_CLIPBOARD,     /* struct m_clip + data (reply to C_GET_CLIPBOARD) */
    S_THEME,         /* struct m_theme */
    S_SCREEN,        /* struct m_screen */
    S_SETTINGS,      /* struct m_settings (reply to C_GET_SETTINGS, and on change) */
    S_ERROR,         /* struct m_name: what was refused */
    S_OPEN,          /* struct m_launch: a launch for a single-instance app that is running */
};

struct msg_hdr {
    uint16_t type;
    uint16_t reserved;
    uint32_t win; /* the app's window id (0 = none) */
};

#define WINF_RESIZABLE 1u
#define WINF_DIALOG 2u     /* centred, not counted for the dock indicator */

struct m_hello {
    uint32_t version;
    int32_t app;           /* enum app_id */
    int32_t pid;
    char name[32];
};
struct m_welcome {
    uint32_t version;
    int32_t screen_w, screen_h;
    struct { int32_t x, y, w, h; } work;
    uint8_t dark;
    uint8_t reserved[3];
};
struct m_create {
    int32_t w, h;          /* content size */
    int32_t min_w, min_h, max_w, max_h; /* 0 = no limit */
    uint32_t flags;
    char title[64];
};
struct m_attach {
    uint32_t buffer;       /* app-chosen id */
    uint32_t w, h, stride; /* stride in pixels */
};
#define COMMIT_RECTS 16
struct m_commit {
    uint32_t buffer;
    uint32_t ack_serial;   /* last CONFIGURE handled (0 if none) */
    uint32_t n;            /* damage rectangles in content coordinates; 0 = all */
    uint32_t reserved;
    struct { int32_t x, y, w, h; } r[COMMIT_RECTS];
};
struct m_release {
    uint32_t buffer;
};
struct m_title {
    char title[64];
};
enum win_op { OP_MINIMIZE = 1, OP_MAXIMIZE, OP_CLOSE, OP_FOCUS, OP_RESTORE };
struct m_op {
    uint32_t op;
};
enum win_state_proto { WS_NORMAL = 0, WS_MINIMIZED, WS_MAXIMIZED };
struct m_configure {
    int32_t w, h;
    uint32_t serial;
    uint32_t state;
};
struct m_focus {
    uint8_t focused;
};
struct m_key {
    uint16_t key;          /* enum keycode (gui/keys.h) */
    uint16_t scancode;
    uint8_t ascii;         /* the unmodified-layout ASCII character for KEY_CHAR, or 0 */
    uint8_t mods;          /* MOD_* */
    uint8_t pressed;
    uint8_t repeat;
};
struct m_text {
    char utf8[16];         /* NUL-terminated */
};
enum mouse_kind { MK_DOWN = 1, MK_UP, MK_MOVE, MK_WHEEL, MK_LEAVE };
struct m_mouse {
    int32_t x, y;          /* content coordinates */
    uint8_t kind;
    uint8_t buttons;
    uint8_t clicks;
    int8_t wheel;          /* + = up */
};
struct m_clip {
    char mime[32];         /* "text/plain;charset=utf-8" */
    uint32_t len;
    /* followed by len bytes */
};
struct m_launch {
    char program[32];      /* name in /system/bin */
    char arg[200];         /* one argument, may be empty */
};
struct m_theme {
    uint8_t dark;
};
struct m_size {
    int32_t w, h;
};
struct m_screen {
    int32_t w, h;
    struct { int32_t x, y, w, h; } work;
    uint32_t generation;
};
struct m_name {
    char name[32];
};
struct m_repeat {
    uint32_t delay_ms, rate_hz; /* rate 0 = no repeat */
};
#define SETTINGS_MODES 16
struct m_settings {
    char keymap[32];
    char keymaps[8][32];   /* available layouts */
    uint32_t nkeymaps;
    uint32_t delay_ms, rate_hz;
    int32_t screen_w, screen_h;
    uint32_t can_set_mode, nmodes;
    struct { int32_t w, h; } modes[SETTINGS_MODES];
    uint8_t dark;
    uint8_t reserved[3];
};

/* Helpers (common/proto.c). send: -1 on error (peer gone). */
int proto_send(int fd, uint16_t type, uint32_t win, const void *payload, size_t len, const int *fds, unsigned nfds);
/* Receives one message into buf (>= PROTO_MAX_MSG); returns its length
 * (0 = peer closed, -1 = error/would block). Received descriptors land in
 * fds (room *nfds). */
long proto_recv(int fd, void *buf, size_t cap, int *fds, unsigned *nfds, bool nonblock);

#endif
