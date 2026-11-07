#ifndef AURORA_PTY_H
#define AURORA_PTY_H
#include "types.h"
#include "abi/aurora_abi.h"

/* Pseudo-terminals (0.6). The master side is kernel code (the Terminal
 * app): it feeds keystrokes with pty_input() and receives program output
 * through its output callback. The slave side is an open file (FK_TTY) that
 * processes read and write.
 *
 * Line discipline: canonical mode (line editing with VERASE/VKILL, a line
 * becomes readable at '\n' or VEOF), echo, ICRNL on input, ONLCR on output,
 * and ISIG: VINTR (Ctrl-C) sends SIGINT and VQUIT sends SIGQUIT to the
 * foreground process group. Non-canonical mode returns bytes as they come
 * (VMIN/VTIME are not implemented beyond "at least one byte"). */

#define PTY_IN_CAP 4096u
#define PTY_LINE_CAP 1024u

struct pty_master {
    void (*output)(void *ctx, const char *data, size_t len);
    void *ctx;
};

struct pty {
    int refs;           /* slave descriptions + 1 while the master is open */
    bool master_open;
    int index;
    struct pty_master master;
    struct aurora_termios tio;
    struct aurora_winsize ws;
    char line[PTY_LINE_CAP];
    uint32_t line_len;
    char in[PTY_IN_CAP];
    uint32_t in_r, in_w;
    uint32_t eofs;      /* pending end-of-file marks (VEOF on an empty line) */
    int fg_pgrp, sid;
    uint64_t bytes_in, bytes_out;
};

struct pty *pty_create(const struct pty_master *m, uint16_t rows, uint16_t cols);
/* Hang up: slaves read EOF, writes fail with EIO; SIGHUP to the session. */
void pty_master_close(struct pty *t);
void pty_input(struct pty *t, const char *data, size_t len);
void pty_set_winsize(struct pty *t, uint16_t rows, uint16_t cols);
/* Input typed but not read (completed lines, then the line being edited),
 * removed from the pty: the Terminal takes back type-ahead when the
 * foreground program exits. Returns the byte count. */
size_t pty_take_unread(struct pty *t, char *buf, size_t cap);
void pty_ref(struct pty *t);
void pty_put(struct pty *t);

struct process;
long pty_read(struct process *p, struct pty *t, uint64_t ubuf, uint64_t len, bool nonblock);
long pty_write(struct process *p, struct pty *t, uint64_t ubuf, uint64_t len);
long pty_ioctl(struct process *p, struct pty *t, uint64_t req, uint64_t arg);
int pty_poll(struct pty *t, int events);

#endif
