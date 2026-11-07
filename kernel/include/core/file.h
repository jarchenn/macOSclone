#ifndef AURORA_FILE_H
#define AURORA_FILE_H
#include "types.h"
#include "abi/aurora_abi.h"
#include "fs/vfs.h"

/* Open file descriptions (0.6). A process's descriptor table points at
 * reference-counted `struct file`s; dup, fork and descriptor passing to a
 * spawned child share one description (and so one offset). The last
 * reference closes the underlying object.
 *
 * All operations run under the BKL. Blocking operations drop it while they
 * sleep (the scheduler does that) and re-check everything afterwards,
 * including that the user buffer is still mapped. */

#define PIPE_SIZE 4096u /* also PIPE_BUF: writes up to this size are atomic */

enum file_kind {
    FK_NONE = 0,
    FK_VFS,     /* regular file (VFS handle) */
    FK_DIR,     /* directory opened for getdents */
    FK_PIPE_R,
    FK_PIPE_W,
    FK_SOCKET,
    FK_NULL,    /* /dev/null */
    FK_ZERO,    /* /dev/zero */
    FK_RANDOM,  /* /dev/urandom */
    FK_TTY,     /* pseudo-terminal slave */
    FK_SHM,     /* shared-memory object (0.7) */
    FK_LOCAL,   /* connected local message socket (0.7) */
    FK_LISTEN,  /* listening local socket (0.7) */
    FK_DISPLAY, /* /dev/display (0.7) */
    FK_INPUT,   /* /dev/input (0.7) */
    FK_PTM,     /* pseudo-terminal master from /dev/ptmx (0.7) */
    FK_CONSOLE, /* /dev/console: writes go to the kernel log (0.7) */
};

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint32_t r, w; /* free-running indexes */
    int readers, writers;
};

struct pty;
struct process;
struct shm_obj;
struct local_end;
struct local_listener;

struct file {
    int refs;
    uint8_t kind;
    uint32_t flags; /* AURORA_O_ACCMODE | O_APPEND | O_NONBLOCK */
    int handle;     /* VFS handle or socket */
    struct pipe *pipe;
    struct pty *pty;               /* FK_TTY, FK_PTM */
    struct shm_obj *shm;           /* FK_SHM */
    struct local_end *lend;        /* FK_LOCAL */
    struct local_listener *llisten;/* FK_LISTEN */
    uint32_t dir_index;
    uint64_t ino;
    char path[VFS_PATH_MAX + 1]; /* directory path (FK_DIR) / file path */
};

struct file *file_new(uint8_t kind, uint32_t flags);
void file_ref(struct file *f);
void file_put(struct file *f);
int file_pipe(struct file **rd, struct file **wr);
/* Opens a device node under /dev; returns NULL (and *err) if `path` is not
 * a device. */
struct file *file_open_device(struct process *p, const char *path, uint32_t flags, int *err);
bool file_is_device_path(const char *path, struct aurora_stat *st);

long file_read(struct process *p, struct file *f, uint64_t ubuf, uint64_t len);
long file_write(struct process *p, struct file *f, uint64_t ubuf, uint64_t len);
long file_lseek(struct file *f, int64_t off, int whence);
int file_stat(struct file *f, struct aurora_stat *st);
long file_getdents(struct process *p, struct file *f, uint64_t ubuf, uint64_t len);
long file_ioctl(struct process *p, struct file *f, uint64_t req, uint64_t arg);
/* Readiness without blocking: returns AURORA_POLL* bits from `events`
 * (plus HUP/ERR, which are always reported). */
int file_poll(struct file *f, int events);
bool file_is_socket(const struct file *f);
struct file *file_shm_create(uint64_t bytes, int *err);

/* Woken whenever a pipe, pty or descriptor changes state (poll/select). */
extern int g_poll_wq;
void file_poll_wake(void);

uint64_t file_random64(void);

#endif
