/* Pseudo-terminal and line discipline (core/pty.h). Runs under the BKL. */
#include "core/pty.h"
#include "core/process.h"
#include "core/file.h"
#include "core/sched.h"
#include "core/log.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "kalloc.h"
#include "lib.h"

static int next_index;

static void default_termios(struct aurora_termios *t)
{
    memset(t, 0, sizeof *t);
    t->iflag = AURORA_ICRNL;
    t->oflag = AURORA_OPOST | AURORA_ONLCR;
    t->cflag = 0x30 | 0x80; /* CS8 | CREAD (informational) */
    t->lflag = AURORA_ISIG | AURORA_ICANON | AURORA_ECHO | AURORA_ECHOE | AURORA_ECHOK | AURORA_IEXTEN;
    t->cc[AURORA_VINTR] = 0x03;  /* ^C */
    t->cc[AURORA_VQUIT] = 0x1C;  /* ^\ */
    t->cc[AURORA_VERASE] = 0x7F; /* DEL (the Terminal sends it for Backspace) */
    t->cc[AURORA_VKILL] = 0x15;  /* ^U */
    t->cc[AURORA_VEOF] = 0x04;   /* ^D */
    t->cc[AURORA_VSUSP] = 0x1A;  /* ^Z */
    t->cc[AURORA_VMIN] = 1;
    t->ispeed = t->ospeed = 38400;
}

struct pty *pty_create(const struct pty_master *m, uint16_t rows, uint16_t cols)
{
    struct pty *t = kzalloc(sizeof *t);
    if (!t)
        return NULL;
    t->refs = 1;
    t->master_open = true;
    t->index = next_index++;
    t->master = *m;
    default_termios(&t->tio);
    t->ws.rows = rows;
    t->ws.cols = cols;
    return t;
}

void pty_ref(struct pty *t) { t->refs++; }

void pty_put(struct pty *t)
{
    if (--t->refs == 0)
        kfree(t);
}

void pty_master_close(struct pty *t)
{
    if (!t->master_open)
        return;
    t->master_open = false;
    t->master.output = NULL;
    if (t->sid > 0)
        signal_send_group(t->sid, AURORA_SIGHUP); /* the session leader's group */
    if (t->fg_pgrp > 0 && t->fg_pgrp != t->sid)
        signal_send_group(t->fg_pgrp, AURORA_SIGHUP);
    sched_wake_all(t);
    file_poll_wake();
    pty_put(t);
}

void pty_set_winsize(struct pty *t, uint16_t rows, uint16_t cols)
{
    if (t->ws.rows == rows && t->ws.cols == cols)
        return;
    t->ws.rows = rows;
    t->ws.cols = cols;
    if (t->fg_pgrp > 0)
        signal_send_group(t->fg_pgrp, AURORA_SIGWINCH);
}

/* ---- output (slave -> master) ------------------------------------------- */

static void emit(struct pty *t, const char *s, size_t n)
{
    t->bytes_out += n;
    if (t->master_open && t->master.output && n)
        t->master.output(t->master.ctx, s, n);
}

/* Output processing (OPOST/ONLCR) into a small staging buffer. */
static void out_bytes(struct pty *t, const char *s, size_t n)
{
    char buf[256];
    size_t k = 0;
    bool onlcr = (t->tio.oflag & AURORA_OPOST) && (t->tio.oflag & AURORA_ONLCR);
    for (size_t i = 0; i < n; i++) {
        if (k + 2 > sizeof buf) {
            emit(t, buf, k);
            k = 0;
        }
        if (s[i] == '\n' && onlcr)
            buf[k++] = '\r';
        buf[k++] = s[i];
    }
    emit(t, buf, k);
}

static void echo_char(struct pty *t, char c)
{
    if (!(t->tio.lflag & AURORA_ECHO)) {
        if (c == '\n' && (t->tio.lflag & AURORA_ECHONL))
            out_bytes(t, "\n", 1);
        return;
    }
    if ((unsigned char)c < 32 && c != '\n' && c != '\t') {
        char cc[2] = { '^', (char)(c + '@') };
        out_bytes(t, cc, 2);
        return;
    }
    out_bytes(t, &c, 1);
}

/* ---- input (master -> slave) -------------------------------------------- */

static uint32_t in_count(const struct pty *t) { return t->in_w - t->in_r; }

static void in_push(struct pty *t, char c)
{
    if (in_count(t) < PTY_IN_CAP)
        t->in[t->in_w++ % PTY_IN_CAP] = c;
}

static void commit_line(struct pty *t)
{
    for (uint32_t i = 0; i < t->line_len; i++)
        in_push(t, t->line[i]);
    t->line_len = 0;
    sched_wake_all(t);
    file_poll_wake();
}

static void erase_one(struct pty *t)
{
    if (!t->line_len)
        return;
    char c = t->line[--t->line_len];
    if ((t->tio.lflag & AURORA_ECHO) && (t->tio.lflag & AURORA_ECHOE)) {
        out_bytes(t, "\b \b", 3);
        if ((unsigned char)c < 32 && c != '\t')
            out_bytes(t, "\b \b", 3); /* control characters were echoed as ^X */
    }
}

static void input_char(struct pty *t, char c)
{
    const struct aurora_termios *tio = &t->tio;
    t->bytes_in++;
    if (c == '\r' && (tio->iflag & AURORA_IGNCR))
        return;
    if (c == '\r' && (tio->iflag & AURORA_ICRNL))
        c = '\n';
    else if (c == '\n' && (tio->iflag & AURORA_INLCR))
        c = '\r';
    if (tio->lflag & AURORA_ISIG) {
        int sig = 0;
        if (c == (char)tio->cc[AURORA_VINTR])
            sig = AURORA_SIGINT;
        else if (c == (char)tio->cc[AURORA_VQUIT])
            sig = AURORA_SIGQUIT;
        else if (c == (char)tio->cc[AURORA_VSUSP])
            sig = AURORA_SIGTSTP;
        if (sig) {
            if (!(tio->lflag & AURORA_NOFLSH)) {
                t->line_len = 0;
                t->in_r = t->in_w;
            }
            echo_char(t, c);
            if (tio->lflag & AURORA_ECHO)
                out_bytes(t, "\n", 1);
            TEST_EVENT("PTY_SIGNAL pty=%d sig=%d pgrp=%d", t->index, sig, t->fg_pgrp);
            if (t->fg_pgrp > 0)
                signal_send_group(t->fg_pgrp, sig);
            return;
        }
    }
    if (tio->lflag & AURORA_ICANON) {
        if (c == (char)tio->cc[AURORA_VERASE] || c == '\b') {
            erase_one(t);
            return;
        }
        if (c == (char)tio->cc[AURORA_VKILL]) {
            while (t->line_len)
                erase_one(t);
            return;
        }
        if (c == (char)tio->cc[AURORA_VEOF]) {
            if (t->line_len)
                commit_line(t); /* the pending bytes become readable now */
            else {
                t->eofs++;
                sched_wake_all(t);
                file_poll_wake();
            }
            return;
        }
        if (c == '\n') {
            if (t->line_len < PTY_LINE_CAP)
                t->line[t->line_len++] = '\n';
            echo_char(t, '\n');
            commit_line(t);
            return;
        }
        if (t->line_len + 1 >= PTY_LINE_CAP)
            return; /* keep room for the newline */
        t->line[t->line_len++] = c;
        echo_char(t, c);
        return;
    }
    in_push(t, c);
    echo_char(t, c);
    sched_wake_all(t);
    file_poll_wake();
}

void pty_input(struct pty *t, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        input_char(t, data[i]);
}

size_t pty_take_unread(struct pty *t, char *buf, size_t cap)
{
    size_t n = 0;
    while (n < cap && in_count(t))
        buf[n++] = t->in[t->in_r++ % PTY_IN_CAP];
    for (uint32_t i = 0; i < t->line_len && n < cap; i++)
        buf[n++] = t->line[i];
    t->line_len = 0;
    t->eofs = 0;
    return n;
}

/* ---- slave operations ------------------------------------------------------ */

long pty_read(struct process *p, struct pty *t, uint64_t ubuf, uint64_t len, bool nonblock)
{
    if (len == 0)
        return 0;
    for (;;) {
        bool canon = (t->tio.lflag & AURORA_ICANON) != 0;
        if (in_count(t)) {
            if (!user_range_ok(p, ubuf, len, true))
                return AE_FAULT;
            uint64_t n = 0;
            char *dst = (char *)(uintptr_t)ubuf;
            while (n < len && in_count(t)) {
                char c = t->in[t->in_r++ % PTY_IN_CAP];
                dst[n++] = c;
                if (canon && c == '\n')
                    break; /* one line per read */
            }
            file_poll_wake();
            return (long)n;
        }
        if (t->eofs) {
            t->eofs--;
            return 0;
        }
        if (!t->master_open)
            return 0;
        if (nonblock)
            return AE_AGAIN;
        if (task_interrupted(sched_current()))
            return AE_INTR;
        uint64_t f = irq_save();
        if (!in_count(t) && !t->eofs && t->master_open)
            sched_wait_on(t, 0);
        irq_restore(f);
    }
}

long pty_write(struct process *p, struct pty *t, uint64_t ubuf, uint64_t len)
{
    if (!t->master_open)
        return AE_IO;
    if (!user_range_ok(p, ubuf, len, false))
        return AE_FAULT;
    const char *src = (const char *)(uintptr_t)ubuf;
    /* Copy through a kernel buffer: the output callback may run long. */
    char buf[512];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = MIN(len - done, (uint64_t)sizeof buf);
        memcpy(buf, src + done, n);
        out_bytes(t, buf, n);
        done += n;
    }
    sched_wake(sched_desktop());
    return (long)len;
}

int pty_poll(struct pty *t, int events)
{
    int r = 0;
    if ((events & AURORA_POLLIN) && (in_count(t) || t->eofs))
        r |= AURORA_POLLIN;
    if ((events & AURORA_POLLOUT) && t->master_open)
        r |= AURORA_POLLOUT;
    if (!t->master_open)
        r |= AURORA_POLLHUP | (events & AURORA_POLLIN);
    return r;
}

long pty_ioctl(struct process *p, struct pty *t, uint64_t req, uint64_t arg)
{
    switch (req) {
    case AURORA_TCGETS:
        return copy_to_user(p, arg, &t->tio, sizeof t->tio);
    case AURORA_TCSETS:
    case AURORA_TCSETSW:
    case AURORA_TCSETSF: {
        struct aurora_termios nt;
        int r = copy_from_user(p, &nt, arg, sizeof nt);
        if (r)
            return r;
        bool was_canon = (t->tio.lflag & AURORA_ICANON) != 0;
        t->tio = nt;
        if (req == AURORA_TCSETSF) {
            t->in_r = t->in_w;
            t->line_len = 0;
        }
        if (was_canon && !(nt.lflag & AURORA_ICANON))
            commit_line(t); /* pending edit text becomes raw input */
        return 0;
    }
    case AURORA_TIOCGPGRP: {
        int32_t g = t->fg_pgrp;
        return copy_to_user(p, arg, &g, sizeof g);
    }
    case AURORA_TIOCSPGRP: {
        int32_t g;
        int r = copy_from_user(p, &g, arg, sizeof g);
        if (r)
            return r;
        if (p->tty != t || g <= 0)
            return AE_NOTTY;
        t->fg_pgrp = g;
        return 0;
    }
    case AURORA_TIOCGWINSZ:
        return copy_to_user(p, arg, &t->ws, sizeof t->ws);
    case AURORA_TIOCSWINSZ: {
        struct aurora_winsize w;
        int r = copy_from_user(p, &w, arg, sizeof w);
        if (r)
            return r;
        pty_set_winsize(t, w.rows, w.cols);
        return 0;
    }
    case AURORA_FIONREAD: {
        int32_t n = (int32_t)in_count(t);
        return copy_to_user(p, arg, &n, sizeof n);
    }
    default:
        return AE_NOTTY;
    }
}
