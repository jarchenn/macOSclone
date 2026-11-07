/* Client library: connection, windows, buffers and the event loop
 * (desktop/desktop.h). */
#include "desktop/desktop.h"
#include "lib.h"
#include <aurora.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAX_TIMERS 32
#define MAX_WATCH 8

static int conn = -1;
static int my_app = APP_OTHER; /* kept for diagnostics */
int ui_app(void) { return my_app; }
static struct window *windows;
static uint32_t next_win = 1, next_buf = 1;
static bool quit;
static int32_t scr_w = 1024, scr_h = 768;
static struct rect work = { 0, MENUBAR_H, 1024, 600 };

static struct {
    int id;
    uint64_t due;
    uint32_t period;
    ui_timer_fn fn;
    void *ctx;
} timers[MAX_TIMERS];
static int next_timer = 1;

static struct {
    int fd, events;
    ui_fd_fn fn;
    void *ctx;
} watches[MAX_WATCH];
static int nwatch;

static void (*idle_fn)(void);
static ui_settings_fn settings_cb;
static struct window *(*open_cb)(const char *arg);

void ui_on_open(struct window *(*fn)(const char *arg)) { open_cb = fn; }

/* Clipboard requests waiting for S_CLIPBOARD, oldest first. */
static struct {
    ui_clip_fn fn;
    void *ctx;
} clip_wait[8];
static int nclip;

uint64_t desktop_now_ms(void) { return gui_now_ms(); }
int32_t desktop_width(void) { return scr_w; }
int32_t desktop_height(void) { return scr_h; }
struct rect ui_work_area(void) { return work; }

static void send_msg(uint16_t type, uint32_t win, const void *p, size_t n)
{
    if (conn >= 0 && proto_send(conn, type, win, p, n, NULL, 0) < 0)
        quit = true;
}

bool ui_init(int app, const char *name)
{
    signal(SIGPIPE, SIG_IGN);
    text_init();
    my_app = app;
    for (int attempt = 0; attempt < 50; attempt++) {
        conn = aurora_local_connect(DISPLAY_SOCKET, O_CLOEXEC);
        if (conn >= 0 || errno != EAGAIN)
            break;
        usleep(20000); /* the server's backlog is full: retry briefly */
    }
    if (conn < 0)
        return false;
    struct m_hello h;
    memset(&h, 0, sizeof h);
    h.version = PROTO_VERSION;
    h.app = app;
    h.pid = (int32_t)getpid();
    str_copy(h.name, name, sizeof h.name);
    send_msg(C_HELLO, 0, &h, sizeof h);
    /* Wait for the welcome (screen size, theme). */
    static uint8_t buf[PROTO_MAX_MSG];
    for (;;) {
        unsigned nfds = 0;
        long n = proto_recv(conn, buf, sizeof buf, NULL, &nfds, false);
        if (n <= 0)
            return false;
        struct msg_hdr hd;
        memcpy(&hd, buf, sizeof hd);
        if (hd.type == S_WELCOME && n >= (long)(sizeof hd + sizeof(struct m_welcome))) {
            struct m_welcome w;
            memcpy(&w, buf + sizeof hd, sizeof w);
            scr_w = w.screen_w;
            scr_h = w.screen_h;
            work = rect_make(w.work.x, w.work.y, w.work.w, w.work.h);
            theme_set(w.dark != 0);
            return true;
        }
    }
}

void ui_quit(void) { quit = true; }

/* ---- windows ------------------------------------------------------------------ */

struct window *wm_first(void) { return windows; }

int wm_count(void)
{
    int n = 0;
    for (struct window *w = windows; w; w = w->next)
        n++;
    return n;
}

static struct window *by_id(uint32_t id)
{
    for (struct window *w = windows; w; w = w->next)
        if (w->id == id)
            return w;
    return NULL;
}

struct window *wm_get(uint32_t id) { return id ? by_id(id) : NULL; }

void ui_sheet_open(struct window *w, const struct window_ops *ops, void *data)
{
    w->sheet = ops;
    w->sheet_data = data;
    w->dirty = true;
}

void ui_sheet_close(struct window *w)
{
    w->sheet = NULL;
    w->sheet_data = NULL;
    w->dirty = true;
}

/* While a sheet is open it gets input; the window below only draws. */
static const struct window_ops *input_ops(struct window *w) { return w->sheet ? w->sheet : w->ops; }

struct window *wm_create_ex(int app, const char *title, int32_t cw, int32_t ch, uint32_t flags, int32_t min_w,
                            int32_t min_h, int32_t max_w, int32_t max_h, const struct window_ops *ops, void *data)
{
    struct window *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->id = next_win++;
    w->app = app;
    str_copy(w->title, title, sizeof w->title);
    w->ops = ops;
    w->data = data;
    w->content.w = cw;
    w->content.h = ch;
    w->dirty = true;
    for (int i = 0; i < 2; i++)
        w->buf[i].fd = -1;
    w->next = windows;
    windows = w;
    struct m_create m;
    memset(&m, 0, sizeof m);
    m.w = cw;
    m.h = ch;
    m.min_w = min_w;
    m.min_h = min_h;
    m.max_w = max_w;
    m.max_h = max_h;
    m.flags = flags;
    str_copy(m.title, title, sizeof m.title);
    send_msg(C_CREATE, w->id, &m, sizeof m);
    return w;
}

struct window *wm_create(int app, const char *title, int32_t cw, int32_t ch, const struct window_ops *ops, void *data)
{
    return wm_create_ex(app, title, cw, ch, WINF_RESIZABLE, 0, 0, 0, 0, ops, data);
}

static void buffer_free(struct ui_buffer *b)
{
    if (b->px)
        munmap(b->px, b->bytes);
    if (b->fd >= 0)
        close(b->fd);
    memset(b, 0, sizeof *b);
    b->fd = -1;
}

void wm_destroy(struct window *w)
{
    struct window **pp = &windows;
    while (*pp && *pp != w)
        pp = &(*pp)->next;
    if (!*pp)
        return;
    *pp = w->next;
    send_msg(C_DESTROY, w->id, NULL, 0);
    if (w->ops && w->ops->destroyed)
        w->ops->destroyed(w);
    for (int i = 0; i < 2; i++)
        buffer_free(&w->buf[i]);
    free(w);
}

void wm_request_close(struct window *w)
{
    if (!w)
        return;
    if (w->ops && w->ops->close_request && !w->ops->close_request(w))
        return;
    wm_destroy(w);
}

void wm_invalidate(struct window *w)
{
    if (w)
        w->dirty = true;
}

void wm_set_title(struct window *w, const char *title)
{
    if (!strcmp(w->title, title))
        return;
    str_copy(w->title, title, sizeof w->title);
    struct m_title m;
    memset(&m, 0, sizeof m);
    str_copy(m.title, title, sizeof m.title);
    send_msg(C_SET_TITLE, w->id, &m, sizeof m);
}

static void window_op(struct window *w, uint32_t op)
{
    struct m_op m = { op };
    send_msg(C_WINDOW_OP, w->id, &m, sizeof m);
}

void wm_minimize(struct window *w) { window_op(w, OP_MINIMIZE); }
void wm_toggle_maximize(struct window *w) { window_op(w, OP_MAXIMIZE); }
void wm_focus(struct window *w) { window_op(w, OP_FOCUS); }

/* A free buffer of the window's current size (allocating if needed). */
static struct ui_buffer *free_buffer(struct window *w)
{
    struct ui_buffer *pick = NULL;
    for (int i = 0; i < 2 && !pick; i++)
        if (!w->buf[i].busy)
            pick = &w->buf[i];
    if (!pick)
        return NULL;
    if (pick->px && pick->w == w->content.w && pick->h == w->content.h)
        return pick;
    buffer_free(pick);
    size_t bytes;
    if (!surface_size_bytes(w->content.w, w->content.h, &bytes))
        return NULL;
    pick->fd = aurora_shm_create(bytes, O_CLOEXEC);
    if (pick->fd < 0)
        return NULL;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, pick->fd, 0);
    if (p == MAP_FAILED) {
        buffer_free(pick);
        return NULL;
    }
    pick->px = p;
    pick->bytes = bytes;
    pick->w = w->content.w;
    pick->h = w->content.h;
    pick->id = next_buf++;
    struct m_attach a = { pick->id, (uint32_t)pick->w, (uint32_t)pick->h, (uint32_t)pick->w };
    uint8_t msg[sizeof(struct msg_hdr) + sizeof a];
    struct msg_hdr h = { C_ATTACH, 0, w->id };
    memcpy(msg, &h, sizeof h);
    memcpy(msg + sizeof h, &a, sizeof a);
    if (aurora_sendmsg(conn, msg, sizeof msg, &pick->fd, 1, 0) < 0)
        quit = true;
    return pick;
}

static void render(struct window *w)
{
    if (!w->configured || w->state == WIN_MINIMIZED)
        return;
    struct ui_buffer *b = free_buffer(w);
    if (!b)
        return; /* both held by the server: draw after a RELEASE */
    w->dirty = false;
    w->content.px = b->px;
    w->content.stride = b->w;
    surface_reset_clip(&w->content);
    if (w->ops && w->ops->render)
        w->ops->render(w, &w->content);
    if (w->sheet && w->sheet->render) {
        surface_reset_clip(&w->content);
        w->sheet->render(w, &w->content);
    }
    w->content.px = NULL;
    struct m_commit m;
    memset(&m, 0, sizeof m);
    m.buffer = b->id;
    m.ack_serial = w->cfg_serial;
    m.n = 0; /* whole window: the toolkit redraws everything */
    w->acked = w->cfg_serial;
    b->busy = true;
    send_msg(C_COMMIT, w->id, &m, sizeof m);
}

/* ---- services --------------------------------------------------------------------- */

void ui_clipboard_set(const char *text, size_t len)
{
    static uint8_t buf[sizeof(struct m_clip) + CLIP_MAX];
    struct m_clip m;
    memset(&m, 0, sizeof m);
    str_copy(m.mime, "text/plain;charset=utf-8", sizeof m.mime);
    m.len = (uint32_t)MIN(len, (size_t)CLIP_MAX);
    memcpy(buf, &m, sizeof m);
    memcpy(buf + sizeof m, text, m.len);
    send_msg(C_SET_CLIPBOARD, 0, buf, sizeof m + m.len);
}

void ui_clipboard_get(ui_clip_fn fn, void *ctx)
{
    if (nclip >= 8)
        return;
    clip_wait[nclip].fn = fn;
    clip_wait[nclip].ctx = ctx;
    nclip++;
    struct m_clip m;
    memset(&m, 0, sizeof m);
    str_copy(m.mime, "text/plain;charset=utf-8", sizeof m.mime);
    send_msg(C_GET_CLIPBOARD, 0, &m, sizeof m);
}

void ui_launch(const char *program, const char *arg)
{
    struct m_launch m;
    memset(&m, 0, sizeof m);
    str_copy(m.program, program, sizeof m.program);
    if (arg)
        str_copy(m.arg, arg, sizeof m.arg);
    send_msg(C_LAUNCH, 0, &m, sizeof m);
}

void ui_set_theme(bool dark)
{
    struct m_theme m = { dark };
    send_msg(C_SET_THEME, 0, &m, sizeof m);
}

void ui_set_mode(int32_t w, int32_t h)
{
    struct m_size m = { w, h };
    send_msg(C_SET_MODE, 0, &m, sizeof m);
}

void ui_set_keymap(const char *name)
{
    struct m_name m;
    memset(&m, 0, sizeof m);
    str_copy(m.name, name, sizeof m.name);
    send_msg(C_SET_KEYMAP, 0, &m, sizeof m);
}

void ui_set_repeat(uint32_t delay_ms, uint32_t rate_hz)
{
    struct m_repeat m = { delay_ms, rate_hz };
    send_msg(C_SET_REPEAT, 0, &m, sizeof m);
}

void ui_power(int how)
{
    struct m_op m = { (uint32_t)how };
    send_msg(C_POWER, 0, &m, sizeof m);
}

void ui_on_settings(ui_settings_fn fn)
{
    settings_cb = fn;
    send_msg(C_GET_SETTINGS, 0, NULL, 0);
}

/* ---- timers and watches ----------------------------------------------------------------- */

int ui_timer_add(uint32_t ms, bool repeat, ui_timer_fn fn, void *ctx)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (!timers[i].id) {
            timers[i].id = next_timer++;
            timers[i].due = gui_now_ms() + ms;
            timers[i].period = repeat ? MAX(ms, 1u) : 0;
            timers[i].fn = fn;
            timers[i].ctx = ctx;
            return timers[i].id;
        }
    return -1;
}

void ui_timer_cancel(int id)
{
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].id == id && id > 0)
            timers[i].id = 0;
}

bool ui_watch_fd(int fd, int events, ui_fd_fn fn, void *ctx)
{
    ui_unwatch_fd(fd);
    if (nwatch >= MAX_WATCH)
        return false;
    watches[nwatch].fd = fd;
    watches[nwatch].events = events;
    watches[nwatch].fn = fn;
    watches[nwatch].ctx = ctx;
    nwatch++;
    return true;
}

void ui_unwatch_fd(int fd)
{
    for (int i = 0; i < nwatch; i++)
        if (watches[i].fd == fd) {
            watches[i] = watches[--nwatch];
            return;
        }
}

void ui_set_idle(void (*fn)(void)) { idle_fn = fn; }

static void run_timers(uint64_t now)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].id || now < timers[i].due)
            continue;
        ui_timer_fn fn = timers[i].fn;
        void *ctx = timers[i].ctx;
        if (timers[i].period)
            timers[i].due = now + timers[i].period;
        else
            timers[i].id = 0;
        fn(ctx);
    }
}

static int next_timeout(uint64_t now)
{
    int64_t t = 250 - (int64_t)(now % 250); /* tick() for the windows */
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].id)
            t = MIN(t, MAX(0, (int64_t)timers[i].due - (int64_t)now));
    return (int)t;
}

/* ---- events -------------------------------------------------------------------------------- */

static void deliver_text(struct window *w, const char *utf8)
{
    const struct window_ops *ops = input_ops(w);
    if (ops && ops->text) {
        ops->text(w, utf8);
        return;
    }
    if (!ops || !ops->key)
        return;
    for (const char *p = utf8; *p; p++) {
        if ((unsigned char)*p < 32 || (unsigned char)*p > 126)
            continue;
        struct key_event e = { KEY_CHAR, (uint8_t)*p, 0, true };
        ops->key(w, &e);
    }
}

static void handle(uint8_t *buf, long n, int *fds, unsigned nfds)
{
    for (unsigned i = 0; i < nfds; i++)
        close(fds[i]); /* the server never sends descriptors */
    if (n < (long)sizeof(struct msg_hdr))
        return;
    struct msg_hdr h;
    memcpy(&h, buf, sizeof h);
    uint8_t *p = buf + sizeof h;
    size_t len = (size_t)n - sizeof h;
    struct window *w = h.win ? by_id(h.win) : NULL;
    switch (h.type) {
    case S_CONFIGURE: {
        if (!w || len < sizeof(struct m_configure))
            break;
        struct m_configure m;
        memcpy(&m, p, sizeof m);
        bool size = m.w != w->content.w || m.h != w->content.h;
        w->cfg_serial = m.serial;
        w->state = m.state == WS_MAXIMIZED ? WIN_MAXIMIZED : m.state == WS_MINIMIZED ? WIN_MINIMIZED : WIN_NORMAL;
        w->content.w = MAX(m.w, 1);
        w->content.h = MAX(m.h, 1);
        bool first = !w->configured;
        w->configured = true;
        if (size && !first && w->ops && w->ops->resized)
            w->ops->resized(w);
        if (size && !first && w->sheet && w->sheet->resized)
            w->sheet->resized(w);
        w->dirty = true;
        break;
    }
    case S_RELEASE: {
        if (!w || len < sizeof(struct m_release))
            break;
        struct m_release m;
        memcpy(&m, p, sizeof m);
        for (int i = 0; i < 2; i++)
            if (w->buf[i].id == m.buffer)
                w->buf[i].busy = false;
        break;
    }
    case S_CLOSE_REQUEST:
        if (w && w->sheet) {
            /* A dialog is open: closing means Escape on it first. */
            struct key_event esc = { KEY_ESC, 0, 0, true };
            if (w->sheet->key)
                w->sheet->key(w, &esc);
        } else if (w) {
            wm_request_close(w);
        }
        break;
    case S_FOCUS:
        if (w && len >= 1) {
            w->focused = p[0] != 0;
            w->dirty = true;
        }
        break;
    case S_KEY: {
        if (!w || len < sizeof(struct m_key))
            break;
        struct m_key m;
        memcpy(&m, p, sizeof m);
        if (!m.pressed)
            break;
        if (m.key == KEY_CHAR && !(m.mods & (MOD_CTRL | MOD_ALT)))
            break; /* the characters follow as TEXT */
        struct key_event e = { m.key, m.ascii, m.mods, true };
        const struct window_ops *ops = input_ops(w);
        if (ops && ops->key)
            ops->key(w, &e);
        break;
    }
    case S_TEXT: {
        if (!w || len < sizeof(struct m_text))
            break;
        struct m_text m;
        memcpy(&m, p, sizeof m);
        m.utf8[sizeof m.utf8 - 1] = 0;
        deliver_text(w, m.utf8);
        break;
    }
    case S_MOUSE: {
        if (!w || len < sizeof(struct m_mouse) || !input_ops(w) || !input_ops(w)->mouse)
            break;
        struct m_mouse m;
        memcpy(&m, p, sizeof m);
        struct win_mouse wm = { m.x, m.y, 0, m.buttons, m.clicks, m.wheel };
        switch (m.kind) {
        case MK_DOWN: wm.kind = WM_MOUSE_DOWN; break;
        case MK_UP: wm.kind = WM_MOUSE_UP; break;
        case MK_WHEEL: wm.kind = WM_MOUSE_WHEEL; break;
        case MK_LEAVE: wm.kind = WM_MOUSE_MOVE; wm.x = wm.y = -1; wm.buttons = 0; break;
        default: wm.kind = WM_MOUSE_MOVE; break;
        }
        input_ops(w)->mouse(w, &wm);
        break;
    }
    case S_CLIPBOARD: {
        if (len < sizeof(struct m_clip) || nclip == 0)
            break;
        struct m_clip m;
        memcpy(&m, p, sizeof m);
        size_t dl = MIN((size_t)m.len, len - sizeof m);
        ui_clip_fn fn = clip_wait[0].fn;
        void *ctx = clip_wait[0].ctx;
        for (int i = 1; i < nclip; i++)
            clip_wait[i - 1] = clip_wait[i];
        nclip--;
        fn(ctx, (const char *)p + sizeof m, dl);
        break;
    }
    case S_THEME:
        if (len >= 1) {
            theme_set(p[0] != 0);
            for (struct window *x = windows; x; x = x->next)
                x->dirty = true;
        }
        break;
    case S_SCREEN: {
        if (len < sizeof(struct m_screen))
            break;
        struct m_screen m;
        memcpy(&m, p, sizeof m);
        scr_w = m.w;
        scr_h = m.h;
        work = rect_make(m.work.x, m.work.y, m.work.w, m.work.h);
        for (struct window *x = windows; x; x = x->next)
            x->dirty = true;
        break;
    }
    case S_SETTINGS:
        if (len >= sizeof(struct m_settings) && settings_cb) {
            struct m_settings s;
            memcpy(&s, p, sizeof s);
            settings_cb(&s);
        }
        break;
    case S_OPEN: {
        if (len < sizeof(struct m_launch) || !open_cb)
            break;
        struct m_launch m;
        memcpy(&m, p, sizeof m);
        m.arg[sizeof m.arg - 1] = 0;
        open_cb(m.arg[0] ? m.arg : NULL);
        break;
    }
    case S_ERROR:
        fprintf(stderr, "ui: the display server refused a request\n");
        if (w && !w->configured)
            quit = true; /* window could not be created */
        break;
    default:
        break;
    }
}

int ui_run(void)
{
    static uint8_t buf[PROTO_MAX_MSG];
    uint64_t last_tick = 0;
    while (!quit && windows) {
        for (struct window *w = windows; w; w = w->next)
            if (w->dirty)
                render(w);
        struct pollfd pfd[1 + MAX_WATCH];
        pfd[0].fd = conn;
        pfd[0].events = POLLIN;
        int n = 1;
        for (int i = 0; i < nwatch; i++) {
            pfd[n].fd = watches[i].fd;
            pfd[n].events = (short)watches[i].events;
            n++;
        }
        uint64_t now = gui_now_ms();
        poll(pfd, (nfds_t)n, next_timeout(now));
        if (pfd[0].revents & (POLLIN | POLLHUP)) {
            for (int k = 0; k < 64; k++) {
                int fds[AURORA_MSG_FDS];
                unsigned nfds = AURORA_MSG_FDS;
                long r = proto_recv(conn, buf, sizeof buf, fds, &nfds, true);
                if (r == 0) {
                    quit = true; /* the display server went away */
                    break;
                }
                if (r < 0)
                    break;
                handle(buf, r, fds, nfds);
                if (!windows)
                    break;
            }
        }
        for (int i = 1; i < n; i++) {
            if (!pfd[i].revents)
                continue;
            for (int k = 0; k < nwatch; k++)
                if (watches[k].fd == pfd[i].fd) {
                    watches[k].fn(watches[k].ctx, pfd[i].revents);
                    break;
                }
        }
        now = gui_now_ms();
        run_timers(now);
        if (now - last_tick >= 250) {
            last_tick = now;
            for (struct window *w = windows, *next; w; w = next) {
                next = w->next;
                /* The tick may destroy w: touch nothing of it afterwards. */
                if (w->sheet) {
                    if (w->sheet->tick)
                        w->sheet->tick(w, now);
                } else if (w->ops && w->ops->tick) {
                    w->ops->tick(w, now);
                }
            }
        }
        if (idle_fn)
            idle_fn();
    }
    return 0;
}
