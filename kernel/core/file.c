/* Open file descriptions: VFS files, directories, pipes, sockets, devices
 * and pseudo-terminal slaves (core/file.h). Runs under the BKL. */
#include "core/file.h"
#include "core/process.h"
#include "core/pty.h"
#include "core/sched.h"
#include "core/time.h"
#include "core/log.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "net/net.h"
#include "kalloc.h"
#include "lib.h"

#define IO_CHUNK 4096

int g_poll_wq;
void file_poll_wake(void) { sched_wake_all(&g_poll_wq); }

static uint64_t next_ino = 1u << 30; /* inode numbers for non-VFS objects */

struct file *file_new(uint8_t kind, uint32_t flags)
{
    struct file *f = kzalloc(sizeof *f);
    if (!f)
        return NULL;
    f->refs = 1;
    f->kind = kind;
    f->flags = flags;
    f->handle = -1;
    f->ino = next_ino++;
    return f;
}

void file_ref(struct file *f) { f->refs++; }

void file_put(struct file *f)
{
    if (!f || --f->refs > 0)
        return;
    switch (f->kind) {
    case FK_VFS:
        vfs_close(f->handle);
        break;
    case FK_SOCKET:
        sock_close(f->handle);
        break;
    case FK_PIPE_R:
    case FK_PIPE_W: {
        struct pipe *pp = f->pipe;
        if (f->kind == FK_PIPE_R)
            pp->readers--;
        else
            pp->writers--;
        sched_wake_all(pp);
        if (pp->readers == 0 && pp->writers == 0)
            kfree(pp);
        break;
    }
    case FK_TTY:
        pty_put(f->pty);
        break;
    default:
        break;
    }
    file_poll_wake();
    kfree(f);
}

bool file_is_socket(const struct file *f) { return f && f->kind == FK_SOCKET; }

int file_pipe(struct file **rd, struct file **wr)
{
    struct pipe *pp = kzalloc(sizeof *pp);
    struct file *r = file_new(FK_PIPE_R, AURORA_O_RDONLY);
    struct file *w = file_new(FK_PIPE_W, AURORA_O_WRONLY);
    if (!pp || !r || !w) {
        if (pp) kfree(pp);
        if (r) kfree(r);
        if (w) kfree(w);
        return AE_NOMEM;
    }
    pp->readers = pp->writers = 1;
    r->pipe = w->pipe = pp;
    w->ino = r->ino;
    *rd = r;
    *wr = w;
    return 0;
}

/* ---- devices -------------------------------------------------------------- */

static uint64_t rng_state;

uint64_t file_random64(void)
{
    /* xorshift64* seeded from the TSC; mixed with the TSC on every call.
     * Not a cryptographic generator (documented in docs/userland.md). */
    if (!rng_state)
        rng_state = rdtsc() | 1;
    rng_state ^= rdtsc() << 7;
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

static const struct { const char *path; uint8_t kind; uint32_t ino; } devices[] = {
    { "/dev/null", FK_NULL, 1 },
    { "/dev/zero", FK_ZERO, 2 },
    { "/dev/urandom", FK_RANDOM, 3 },
    { "/dev/random", FK_RANDOM, 4 },
    { "/dev/tty", FK_TTY, 5 },
};

bool file_is_device_path(const char *path, struct aurora_stat *st)
{
    for (size_t i = 0; i < ARRAY_LEN(devices); i++) {
        if (!strcmp(path, devices[i].path)) {
            if (st) {
                memset(st, 0, sizeof *st);
                st->dev = 2;
                st->ino = devices[i].ino;
                st->mode = AURORA_S_IFCHR | 0666;
                st->nlink = 1;
                st->rdev = devices[i].ino;
                st->blksize = 4096;
            }
            return true;
        }
    }
    return false;
}

struct file *file_open_device(struct process *p, const char *path, uint32_t flags, int *err)
{
    for (size_t i = 0; i < ARRAY_LEN(devices); i++) {
        if (strcmp(path, devices[i].path))
            continue;
        if (devices[i].kind == FK_TTY) {
            if (!p->tty) {
                *err = AE_NOTTY; /* reported as ENXIO-like "no controlling terminal" */
                return NULL;
            }
            struct file *f = file_new(FK_TTY, flags);
            if (!f) {
                *err = AE_NOMEM;
                return NULL;
            }
            f->pty = p->tty;
            pty_ref(p->tty);
            return f;
        }
        struct file *f = file_new(devices[i].kind, flags);
        if (!f)
            *err = AE_NOMEM;
        else
            f->ino = devices[i].ino;
        return f;
    }
    *err = VFS_ENOENT;
    return NULL;
}

/* ---- read/write ----------------------------------------------------------- */

static bool can_read(const struct file *f) { return (f->flags & AURORA_O_ACCMODE) != AURORA_O_WRONLY; }
static bool can_write(const struct file *f) { return (f->flags & AURORA_O_ACCMODE) != AURORA_O_RDONLY; }

static long pipe_read(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    struct pipe *pp = f->pipe;
    if (len == 0)
        return 0;
    for (;;) {
        uint32_t avail = pp->w - pp->r;
        if (avail) {
            if (!user_range_ok(p, ubuf, len, true))
                return AE_FAULT;
            uint64_t n = MIN(len, (uint64_t)avail);
            uint8_t *dst = (uint8_t *)(uintptr_t)ubuf;
            for (uint64_t i = 0; i < n; i++)
                dst[i] = pp->buf[pp->r++ % PIPE_SIZE];
            sched_wake_all(pp);
            file_poll_wake();
            return (long)n;
        }
        if (pp->writers == 0)
            return 0; /* EOF: every writer has closed its end */
        if (f->flags & AURORA_O_NONBLOCK)
            return AE_AGAIN;
        if (task_interrupted(sched_current()))
            return AE_INTR;
        uint64_t fl = irq_save();
        if (pp->w == pp->r && pp->writers)
            sched_wait_on(pp, 0);
        irq_restore(fl);
    }
}

static long pipe_write(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    struct pipe *pp = f->pipe;
    uint64_t done = 0;
    while (done < len) {
        if (pp->readers == 0) {
            signal_send_thread(sched_current(), AURORA_SIGPIPE);
            return done ? (long)done : AE_PIPE;
        }
        uint32_t space = PIPE_SIZE - (pp->w - pp->r);
        uint64_t left = len - done;
        /* Writes of at most PIPE_SIZE bytes are atomic: wait for room for all. */
        bool fits = left <= PIPE_SIZE ? space >= left : space > 0;
        if (fits) {
            if (!user_range_ok(p, ubuf + done, left, false))
                return done ? (long)done : AE_FAULT;
            uint64_t n = MIN(left, (uint64_t)space);
            const uint8_t *src = (const uint8_t *)(uintptr_t)(ubuf + done);
            for (uint64_t i = 0; i < n; i++)
                pp->buf[pp->w++ % PIPE_SIZE] = src[i];
            done += n;
            sched_wake_all(pp);
            file_poll_wake();
            continue;
        }
        if (f->flags & AURORA_O_NONBLOCK)
            return done ? (long)done : AE_AGAIN;
        if (task_interrupted(sched_current()))
            return done ? (long)done : AE_INTR;
        uint64_t fl = irq_save();
        if (PIPE_SIZE - (pp->w - pp->r) < MIN(left, (uint64_t)PIPE_SIZE) && pp->readers)
            sched_wait_on(pp, 0);
        irq_restore(fl);
    }
    return (long)done;
}

static long vfs_read_user(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    if (!user_range_ok(p, ubuf, len, true))
        return AE_FAULT;
    char *tmp = kmalloc(IO_CHUNK);
    if (!tmp)
        return AE_NOMEM;
    uint64_t done = 0;
    long r = 0;
    while (done < len) {
        uint64_t n = MIN(len - done, (uint64_t)IO_CHUNK);
        r = vfs_read(f->handle, tmp, n);
        if (r <= 0)
            break;
        memcpy((void *)(uintptr_t)(ubuf + done), tmp, (size_t)r);
        done += (uint64_t)r;
        if ((uint64_t)r < n)
            break;
    }
    kfree(tmp);
    return done ? (long)done : r;
}

static long vfs_write_user(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    if (!user_range_ok(p, ubuf, len, false))
        return AE_FAULT;
    char *tmp = kmalloc(IO_CHUNK);
    if (!tmp)
        return AE_NOMEM;
    uint64_t done = 0;
    long r = 0;
    while (done < len) {
        uint64_t n = MIN(len - done, (uint64_t)IO_CHUNK);
        memcpy(tmp, (const void *)(uintptr_t)(ubuf + done), n);
        r = vfs_write(f->handle, tmp, n);
        if (r < 0)
            break;
        done += (uint64_t)r;
    }
    kfree(tmp);
    return done ? (long)done : r;
}

static long sock_read_user(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    /* Receive into a kernel buffer: the socket call may sleep, and the user
     * buffer is only validated once it returns. */
    uint64_t n = MIN(len, (uint64_t)IO_CHUNK * 4);
    char *tmp = kmalloc(n ? n : 1);
    if (!tmp)
        return AE_NOMEM;
    long r = sock_recv(f->handle, tmp, n, 0);
    if (r > 0 && copy_to_user(p, ubuf, tmp, (uint64_t)r))
        r = AE_FAULT;
    kfree(tmp);
    return r;
}

long file_read(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    if (!can_read(f))
        return AE_BADF;
    switch (f->kind) {
    case FK_VFS: return vfs_read_user(p, f, ubuf, len);
    case FK_DIR: return VFS_EISDIR;
    case FK_PIPE_R: return pipe_read(p, f, ubuf, len);
    case FK_SOCKET: return sock_read_user(p, f, ubuf, len);
    case FK_NULL: return 0;
    case FK_ZERO:
    case FK_RANDOM: {
        if (!user_range_ok(p, ubuf, len, true))
            return AE_FAULT;
        uint8_t *d = (uint8_t *)(uintptr_t)ubuf;
        for (uint64_t i = 0; i < len; i++)
            d[i] = f->kind == FK_ZERO ? 0 : (uint8_t)file_random64();
        return (long)len;
    }
    case FK_TTY: return pty_read(p, f->pty, ubuf, len, (f->flags & AURORA_O_NONBLOCK) != 0);
    default: return AE_BADF;
    }
}

long file_write(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    if (!can_write(f))
        return AE_BADF;
    switch (f->kind) {
    case FK_VFS: return vfs_write_user(p, f, ubuf, len);
    case FK_PIPE_W: return pipe_write(p, f, ubuf, len);
    case FK_SOCKET: {
        uint64_t n = MIN(len, (uint64_t)IO_CHUNK * 4);
        char *tmp = kmalloc(n ? n : 1);
        if (!tmp)
            return AE_NOMEM;
        long r = copy_from_user(p, tmp, ubuf, n);
        if (r == 0)
            r = sock_send(f->handle, tmp, n);
        kfree(tmp);
        return r;
    }
    case FK_NULL:
    case FK_ZERO:
    case FK_RANDOM:
        return user_range_ok(p, ubuf, len, false) ? (long)len : AE_FAULT;
    case FK_TTY: return pty_write(p, f->pty, ubuf, len);
    default: return AE_BADF;
    }
}

long file_lseek(struct file *f, int64_t off, int whence)
{
    if (f->kind == FK_DIR) {
        if (whence != AURORA_SEEK_SET || off < 0)
            return AE_INVAL;
        f->dir_index = (uint32_t)off; /* rewinddir/seekdir use entry indexes */
        return off;
    }
    if (f->kind == FK_NULL || f->kind == FK_ZERO || f->kind == FK_RANDOM)
        return 0;
    if (f->kind != FK_VFS)
        return AE_SPIPE;
    struct vfs_stat st;
    int r = vfs_fstat(f->handle, &st);
    if (r)
        return r;
    int64_t base = 0;
    if (whence == AURORA_SEEK_CUR)
        base = vfs_tell(f->handle);
    else if (whence == AURORA_SEEK_END)
        base = st.size;
    else if (whence != AURORA_SEEK_SET)
        return AE_INVAL;
    int64_t pos = base + off;
    if (pos < 0 || pos > (int64_t)UINT32_MAX)
        return AE_INVAL;
    vfs_seek(f->handle, (uint32_t)pos);
    return pos;
}

static void stat_from_vfs(const struct vfs_stat *v, struct aurora_stat *st)
{
    memset(st, 0, sizeof *st);
    st->dev = 1;
    st->ino = v->ino;
    st->mode = (v->type == VFS_DIR ? AURORA_S_IFDIR : AURORA_S_IFREG) | v->mode;
    st->nlink = 1;
    st->uid = v->uid;
    st->gid = v->gid;
    st->size = v->size;
    st->blksize = 4096;
    st->blocks = (v->size + 511) / 512;
    st->atime = st->mtime = st->ctime = v->mtime;
}

int file_stat(struct file *f, struct aurora_stat *st)
{
    struct vfs_stat v;
    switch (f->kind) {
    case FK_VFS: {
        int r = vfs_fstat(f->handle, &v);
        if (r)
            return r;
        stat_from_vfs(&v, st);
        return 0;
    }
    case FK_DIR: {
        int r = vfs_stat(f->path, &v);
        if (r)
            return r;
        stat_from_vfs(&v, st);
        return 0;
    }
    default:
        memset(st, 0, sizeof *st);
        st->dev = 2;
        st->ino = f->ino;
        st->nlink = 1;
        st->blksize = 4096;
        if (f->kind == FK_PIPE_R || f->kind == FK_PIPE_W)
            st->mode = AURORA_S_IFIFO | 0600;
        else if (f->kind == FK_SOCKET)
            st->mode = AURORA_S_IFSOCK | 0600;
        else
            st->mode = AURORA_S_IFCHR | 0620;
        return 0;
    }
}

/* Directory entries as aurora_dirent records, starting at f->dir_index. */
long file_getdents(struct process *p, struct file *f, uint64_t ubuf, uint64_t len)
{
    if (f->kind != FK_DIR)
        return VFS_ENOTDIR;
    if (!user_range_ok(p, ubuf, len, true))
        return AE_FAULT;
    uint8_t *out = (uint8_t *)(uintptr_t)ubuf;
    uint64_t used = 0;
    for (;;) {
        struct vfs_dirent d;
        int r;
        const char *name;
        uint8_t type;
        uint64_t ino;
        /* Entries 0 and 1 are "." and "..". */
        if (f->dir_index == 0) {
            name = ".";
            type = AURORA_DT_DIR;
            ino = 1;
        } else if (f->dir_index == 1) {
            name = "..";
            type = AURORA_DT_DIR;
            ino = 1;
        } else {
            r = vfs_readdir(f->path, f->dir_index - 2, &d);
            if (r == VFS_ENOENT)
                break;
            if (r)
                return used ? (long)used : r;
            name = d.name;
            type = d.type == VFS_DIR ? AURORA_DT_DIR : AURORA_DT_REG;
            ino = d.ino ? d.ino : 1;
        }
        size_t nl = strlen(name);
        uint64_t reclen = ALIGN_UP(sizeof(struct aurora_dirent) + nl + 1, 8);
        if (used + reclen > len) {
            if (!used)
                return AE_INVAL; /* buffer too small for one entry */
            break;
        }
        struct aurora_dirent *e = (struct aurora_dirent *)(out + used);
        e->ino = ino;
        e->reclen = (uint16_t)reclen;
        e->type = type;
        memcpy(e->name, name, nl + 1);
        used += reclen;
        f->dir_index++;
    }
    return (long)used;
}

long file_ioctl(struct process *p, struct file *f, uint64_t req, uint64_t arg)
{
    if (f->kind == FK_TTY)
        return pty_ioctl(p, f->pty, req, arg);
    if (req == AURORA_FIONREAD && (f->kind == FK_PIPE_R)) {
        int32_t n = (int32_t)(f->pipe->w - f->pipe->r);
        return copy_to_user(p, arg, &n, sizeof n);
    }
    return AE_NOTTY;
}

int file_poll(struct file *f, int events)
{
    int r = 0;
    switch (f->kind) {
    case FK_PIPE_R: {
        struct pipe *pp = f->pipe;
        if (pp->w != pp->r)
            r |= AURORA_POLLIN;
        if (pp->writers == 0)
            r |= AURORA_POLLHUP | AURORA_POLLIN;
        break;
    }
    case FK_PIPE_W: {
        struct pipe *pp = f->pipe;
        if (pp->readers == 0)
            r |= AURORA_POLLERR;
        else if (PIPE_SIZE - (pp->w - pp->r) > 0)
            r |= AURORA_POLLOUT;
        break;
    }
    case FK_TTY:
        r = pty_poll(f->pty, events);
        break;
    case FK_SOCKET: {
        int s = sock_poll(f->handle);
        if (s & 1)
            r |= AURORA_POLLIN;
        if (s & 2)
            r |= AURORA_POLLOUT;
        if (s & 4)
            r |= AURORA_POLLHUP;
        break;
    }
    default:
        r = AURORA_POLLIN | AURORA_POLLOUT; /* files and devices never block */
        break;
    }
    return r & (events | AURORA_POLLHUP | AURORA_POLLERR);
}
