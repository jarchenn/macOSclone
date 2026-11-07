/* System call dispatch (int 0x80), 0.6 ABI (abi/aurora_abi.h). Every user
 * pointer is checked against the caller's address space before use, and
 * every result leaves as a value or -errno. Permission checks (mode bits,
 * owners, root) are made here: on the target and on its parent directory
 * for operations that change a directory. */
#include "core/process.h"
#include "core/file.h"
#include "core/pty.h"
#include "core/sched.h"
#include "core/spinlock.h"
#include "core/time.h"
#include "core/log.h"
#include "arch/cpu.h"
#include "arch/percpu.h"
#include "arch/x86_64/io.h"
#include "memory/physical.h"
#include "fs/vfs.h"
#include "net/net.h"
#include "kalloc.h"
#include "lib.h"

#ifndef AURORA_VERSION
#define AURORA_VERSION "0.6.0"
#endif

long errno_from(long e)
{
    if (e >= 0)
        return e;
    switch (e) {
    case VFS_ENOENT: return -AURORA_ENOENT;
    case VFS_EEXIST: return -AURORA_EEXIST;
    case VFS_ENOTDIR: return -AURORA_ENOTDIR;
    case VFS_EISDIR: return -AURORA_EISDIR;
    case VFS_EROFS: return -AURORA_EROFS;
    case VFS_ENOSPC: return -AURORA_ENOSPC;
    case VFS_EINVAL: return -AURORA_EINVAL;
    case VFS_EBADF: return -AURORA_EBADF;
    case VFS_ENAMETOOLONG: return -AURORA_ENAMETOOLONG;
    case VFS_EMFILE: return -AURORA_ENFILE;
    case VFS_ENOMEM: return -AURORA_ENOMEM;
    case VFS_EFBIG: return -AURORA_EFBIG;
    case VFS_EBADARCHIVE: return -AURORA_EIO;
    case VFS_EBUSY: return -AURORA_EBUSY;
    case VFS_ENOTEMPTY: return -AURORA_ENOTEMPTY;
    case VFS_EIO: return -AURORA_EIO;
    case VFS_EXDEV: return -AURORA_EXDEV;
    case AE_FAULT: return -AURORA_EFAULT;
    case AE_NOSYS: return -AURORA_ENOSYS;
    case AE_BADF: return -AURORA_EBADF;
    case AE_INVAL: return -AURORA_EINVAL;
    case AE_NOMEM: return -AURORA_ENOMEM;
    case AE_CHILD: return -AURORA_ECHILD;
    case AE_NOEXEC: return -AURORA_ENOEXEC;
    case AE_PROCLIM: return -AURORA_EAGAIN;
    case AE_TIMEDOUT: return -AURORA_ETIMEDOUT;
    case AE_CONNREFUSED: return -AURORA_ECONNREFUSED;
    case AE_HOSTUNREACH: return -AURORA_EHOSTUNREACH;
    case AE_NETDOWN: return -AURORA_ENETDOWN;
    case AE_NOTFOUND: return -AURORA_ENOENT;
    case AE_INTR: return -AURORA_EINTR;
    case AE_CONNRESET: return -AURORA_ECONNRESET;
    case AE_ADDRINUSE: return -AURORA_EADDRINUSE;
    case AE_PERM: return -AURORA_EPERM;
    case AE_ACCES: return -AURORA_EACCES;
    case AE_SRCH: return -AURORA_ESRCH;
    case AE_PIPE: return -AURORA_EPIPE;
    case AE_AGAIN: return -AURORA_EAGAIN;
    case AE_NOTTY: return -AURORA_ENOTTY;
    case AE_SPIPE: return -AURORA_ESPIPE;
    case AE_2BIG: return -AURORA_E2BIG;
    case AE_MFILE: return -AURORA_EMFILE;
    case AE_NOTSOCK: return -AURORA_ENOTSOCK;
    case AE_RANGE: return -AURORA_ERANGE;
    case AE_DEADLK: return -AURORA_EDEADLK;
    case AE_XDEV: return -AURORA_EXDEV;
    case AE_IO: return -AURORA_EIO;
    case AE_NOTSUP: return -AURORA_ENOTSUP;
    default: return -AURORA_EIO;
    }
}

/* ---- paths and permissions ----------------------------------------------- */

static int get_path(struct process *p, uint64_t upath, char *norm)
{
    char raw[VFS_PATH_MAX + 1];
    int r = copy_string_from_user(p, upath, raw, sizeof raw);
    if (r < 0)
        return r == AE_INVAL ? VFS_ENAMETOOLONG : r;
    if (!raw[0])
        return VFS_ENOENT;
    return vfs_normalize(p->cwd, raw, norm, VFS_PATH_MAX + 1);
}

/* want: AURORA_R_OK | W_OK | X_OK against the node's mode and owner. */
static bool may(const struct process *p, const struct vfs_stat *st, int want)
{
    if (p->uid == 0) {
        /* Root bypasses read/write bits; execute needs some x bit (files). */
        return !(want & AURORA_X_OK) || st->type == VFS_DIR || (st->mode & 0111);
    }
    unsigned bits = st->uid == p->uid ? (st->mode >> 6) & 7 : st->gid == p->gid ? (st->mode >> 3) & 7 : st->mode & 7;
    return (bits & (unsigned)want) == (unsigned)want;
}

static int parent_of(const char *norm, char *parent)
{
    char name[VFS_NAME_MAX + 1];
    return vfs_split(norm, parent, VFS_PATH_MAX + 1, name, sizeof name);
}

/* The caller may add or remove entries in the directory holding `norm`. */
static int check_dir_write(struct process *p, const char *norm)
{
    char parent[VFS_PATH_MAX + 1];
    int r = parent_of(norm, parent);
    if (r)
        return r;
    struct vfs_stat st;
    r = vfs_stat(parent, &st);
    if (r)
        return r;
    if (st.type != VFS_DIR)
        return VFS_ENOTDIR;
    return may(p, &st, AURORA_W_OK | AURORA_X_OK) ? 0 : AE_ACCES;
}

/* Removing or renaming `norm`: directory write access plus the sticky rule
 * (in a sticky directory only the owner of the entry or of the directory,
 * or root, may remove it). */
static int check_remove(struct process *p, const char *norm)
{
    int r = check_dir_write(p, norm);
    if (r)
        return r;
    char parent[VFS_PATH_MAX + 1];
    struct vfs_stat dst, st;
    if (parent_of(norm, parent) || vfs_stat(parent, &dst) || vfs_stat(norm, &st))
        return 0;
    if ((dst.mode & 01000) && p->uid != 0 && p->uid != st.uid && p->uid != dst.uid)
        return AE_PERM;
    return 0;
}

static struct vfs_cred cred_for(struct process *p, uint32_t mode)
{
    struct vfs_cred c = { p->uid, p->gid, (uint16_t)(mode & 07777 & ~p->umask) };
    return c;
}

/* ---- files ----------------------------------------------------------------- */

static long sys_open(struct process *p, uint64_t upath, uint32_t flags, uint32_t mode)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    bool cloexec = flags & AURORA_O_CLOEXEC;
    uint32_t fflags = flags & (AURORA_O_ACCMODE | AURORA_O_APPEND | AURORA_O_NONBLOCK);
    uint32_t acc = flags & AURORA_O_ACCMODE;
    if (acc == 3)
        return AE_INVAL;
    struct file *f = NULL;
    if (str_starts_with(path, "/dev/") && file_is_device_path(path, NULL)) {
        f = file_open_device(p, path, fflags, &r);
        if (!f)
            return r;
        int fd = proc_fd_install(p, f, 0, cloexec);
        if (fd < 0)
            file_put(f);
        return fd;
    }
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r == VFS_OK) {
        if ((flags & AURORA_O_CREAT) && (flags & AURORA_O_EXCL))
            return VFS_EEXIST;
        int want = (acc != AURORA_O_WRONLY ? AURORA_R_OK : 0) | (acc != AURORA_O_RDONLY ? AURORA_W_OK : 0);
        if (flags & AURORA_O_TRUNC)
            want |= AURORA_W_OK;
        if (st.type == VFS_DIR) {
            if (acc != AURORA_O_RDONLY)
                return VFS_EISDIR;
            if (!may(p, &st, AURORA_R_OK))
                return AE_ACCES;
            f = file_new(FK_DIR, fflags);
            if (!f)
                return AE_NOMEM;
            str_copy(f->path, path, sizeof f->path);
            f->ino = st.ino;
            int fd = proc_fd_install(p, f, 0, cloexec);
            if (fd < 0)
                file_put(f);
            return fd;
        }
        if (flags & AURORA_O_DIRECTORY)
            return VFS_ENOTDIR;
        if (!may(p, &st, want))
            return AE_ACCES;
    } else if (r == VFS_ENOENT && (flags & AURORA_O_CREAT)) {
        r = check_dir_write(p, path);
        if (r)
            return r;
    } else {
        return r;
    }
    unsigned vf = 0;
    if (acc != AURORA_O_WRONLY) vf |= VFS_O_READ;
    if (acc != AURORA_O_RDONLY) vf |= VFS_O_WRITE;
    if (flags & AURORA_O_CREAT) vf |= VFS_O_CREATE;
    if (flags & AURORA_O_TRUNC) vf |= VFS_O_TRUNC | VFS_O_WRITE;
    if (flags & AURORA_O_EXCL) vf |= VFS_O_EXCL;
    if (flags & AURORA_O_APPEND) vf |= VFS_O_APPEND;
    if (!(vf & VFS_O_WRITE) && (vf & VFS_O_TRUNC))
        vf |= VFS_O_WRITE;
    struct vfs_cred c = cred_for(p, mode);
    int h = vfs_open_as(path, vf, &c);
    if (h < 0)
        return h;
    f = file_new(FK_VFS, fflags);
    if (!f) {
        vfs_close(h);
        return AE_NOMEM;
    }
    f->handle = h;
    str_copy(f->path, path, sizeof f->path);
    int fd = proc_fd_install(p, f, 0, cloexec);
    if (fd < 0)
        file_put(f);
    return fd;
}

static long sys_stat(struct process *p, uint64_t upath, uint64_t ust)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    struct aurora_stat st;
    if (!file_is_device_path(path, &st)) {
        struct vfs_stat v;
        r = vfs_stat(path, &v);
        if (r)
            return r;
        memset(&st, 0, sizeof st);
        st.dev = 1;
        st.ino = v.ino;
        st.mode = (v.type == VFS_DIR ? AURORA_S_IFDIR : AURORA_S_IFREG) | v.mode;
        st.nlink = 1;
        st.uid = v.uid;
        st.gid = v.gid;
        st.size = v.size;
        st.blksize = 4096;
        st.blocks = (v.size + 511) / 512;
        st.atime = st.mtime = st.ctime = v.mtime;
    }
    return copy_to_user(p, ust, &st, sizeof st);
}

static long sys_mkdir(struct process *p, uint64_t upath, uint32_t mode)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    struct vfs_stat st;
    if (vfs_stat(path, &st) == VFS_OK)
        return VFS_EEXIST;
    r = check_dir_write(p, path);
    if (r)
        return r;
    struct vfs_cred c = cred_for(p, mode);
    return vfs_mkdir_as(path, &c);
}

static long sys_remove(struct process *p, uint64_t upath, bool dir)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r)
        return r;
    if (dir && st.type != VFS_DIR)
        return VFS_ENOTDIR;
    if (!dir && st.type == VFS_DIR)
        return VFS_EISDIR;
    if (!strcmp(path, p->cwd) && dir)
        return VFS_EBUSY;
    r = check_remove(p, path);
    return r ? r : vfs_remove(path);
}

static long sys_rename(struct process *p, uint64_t ufrom, uint64_t uto)
{
    char from[VFS_PATH_MAX + 1], to[VFS_PATH_MAX + 1];
    int r = get_path(p, ufrom, from);
    if (r)
        return r;
    r = get_path(p, uto, to);
    if (r)
        return r;
    r = check_remove(p, from);
    if (r)
        return r;
    struct vfs_stat st;
    r = vfs_stat(to, &st) == VFS_OK ? check_remove(p, to) : check_dir_write(p, to);
    return r ? r : vfs_rename(from, to);
}

static long sys_chmod(struct process *p, uint64_t upath, uint32_t mode)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r)
        return r;
    if (p->uid != 0 && p->uid != st.uid)
        return AE_PERM;
    return vfs_chmod(path, (uint16_t)(mode & 07777));
}

static long sys_access(struct process *p, uint64_t upath, int mode)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    if (file_is_device_path(path, NULL))
        return 0;
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r)
        return r;
    if ((mode & AURORA_W_OK) && st.readonly)
        return VFS_EROFS;
    return may(p, &st, mode & 7) ? 0 : AE_ACCES;
}

static long sys_truncate(struct process *p, uint64_t upath, int64_t len)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    if (len < 0 || len > UINT32_MAX)
        return AE_INVAL;
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r)
        return r;
    if (!may(p, &st, AURORA_W_OK))
        return AE_ACCES;
    return vfs_truncate(path, (uint32_t)len);
}

static long sys_chdir(struct process *p, uint64_t upath)
{
    char path[VFS_PATH_MAX + 1];
    int r = get_path(p, upath, path);
    if (r)
        return r;
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r)
        return r;
    if (st.type != VFS_DIR)
        return VFS_ENOTDIR;
    if (!may(p, &st, AURORA_X_OK))
        return AE_ACCES;
    str_copy(p->cwd, path, sizeof p->cwd);
    return 0;
}

static long sys_getcwd(struct process *p, uint64_t ubuf, uint64_t size)
{
    size_t n = strlen(p->cwd) + 1;
    if (size < n)
        return AE_RANGE;
    int r = copy_to_user(p, ubuf, p->cwd, n);
    return r ? r : (long)n;
}

static long sys_dup3(struct process *p, int oldfd, int newfd, uint32_t flags)
{
    struct file *f = proc_fd_get(p, oldfd);
    if (!f || newfd < 0 || newfd >= PROC_MAX_FDS)
        return AE_BADF;
    if (oldfd == newfd)
        return flags ? AE_INVAL : newfd;
    file_ref(f);
    if (p->fds[newfd].f)
        proc_fd_close(p, newfd);
    p->fds[newfd].f = f;
    p->fds[newfd].cloexec = (flags & AURORA_O_CLOEXEC) != 0;
    return newfd;
}

static long sys_fcntl(struct process *p, int fd, int cmd, uint64_t arg)
{
    struct file *f = proc_fd_get(p, fd);
    if (!f)
        return AE_BADF;
    switch (cmd) {
    case AURORA_F_DUPFD:
    case AURORA_F_DUPFD_CLOEXEC: {
        if (arg >= PROC_MAX_FDS)
            return AE_INVAL;
        file_ref(f);
        int n = proc_fd_install(p, f, (int)arg, cmd == AURORA_F_DUPFD_CLOEXEC);
        if (n < 0)
            file_put(f);
        return n;
    }
    case AURORA_F_GETFD: return p->fds[fd].cloexec ? AURORA_FD_CLOEXEC : 0;
    case AURORA_F_SETFD: p->fds[fd].cloexec = (arg & AURORA_FD_CLOEXEC) != 0; return 0;
    case AURORA_F_GETFL: return f->flags;
    case AURORA_F_SETFL:
        f->flags = (f->flags & ~(uint32_t)(AURORA_O_APPEND | AURORA_O_NONBLOCK)) |
                   ((uint32_t)arg & (AURORA_O_APPEND | AURORA_O_NONBLOCK));
        return 0;
    default: return AE_INVAL;
    }
}

static long sys_pipe2(struct process *p, uint64_t ufds, uint32_t flags)
{
    if (!user_range_ok(p, ufds, 8, true))
        return AE_FAULT;
    struct file *r, *w;
    int e = file_pipe(&r, &w);
    if (e)
        return e;
    r->flags |= flags & AURORA_O_NONBLOCK;
    w->flags |= flags & AURORA_O_NONBLOCK;
    bool ce = flags & AURORA_O_CLOEXEC;
    int a = proc_fd_install(p, r, 0, ce);
    if (a < 0) {
        file_put(r);
        file_put(w);
        return a;
    }
    int b = proc_fd_install(p, w, 0, ce);
    if (b < 0) {
        proc_fd_close(p, a);
        file_put(w);
        return b;
    }
    int32_t fds[2] = { a, b };
    return copy_to_user(p, ufds, fds, sizeof fds);
}

#define POLL_MAX 64

static long sys_poll(struct process *p, uint64_t ufds, uint64_t n, int64_t timeout_ms)
{
    if (n > POLL_MAX)
        return AE_INVAL;
    struct aurora_pollfd fds[POLL_MAX];
    if (n && copy_from_user(p, fds, ufds, n * sizeof fds[0]))
        return AE_FAULT;
    uint64_t deadline = timeout_ms < 0 ? 0 : time_ms() + (uint64_t)timeout_ms;
    for (;;) {
        int ready = 0;
        bool sockets = false;
        for (uint64_t i = 0; i < n; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            struct file *f = proc_fd_get(p, fds[i].fd);
            if (!f) {
                fds[i].revents = AURORA_POLLNVAL;
            } else {
                sockets |= file_is_socket(f);
                fds[i].revents = (int16_t)file_poll(f, fds[i].events);
            }
            ready += fds[i].revents != 0;
        }
        uint64_t now = time_ms();
        if (ready || timeout_ms == 0 || (timeout_ms > 0 && now >= deadline)) {
            if (n && copy_to_user(p, ufds, fds, n * sizeof fds[0]))
                return AE_FAULT;
            return ready;
        }
        if (task_interrupted(sched_current()))
            return AE_INTR;
        /* Sockets have no poll wakeup: re-check them every 10 ms. */
        uint64_t wait = timeout_ms < 0 ? 0 : deadline - now;
        if (sockets && (wait == 0 || wait > 10))
            wait = 10;
        uint64_t fl = irq_save();
        sched_wait_on(&g_poll_wq, wait);
        irq_restore(fl);
    }
}

/* ---- memory ------------------------------------------------------------------ */

static uint8_t prot_of(uint64_t prot)
{
    return (uint8_t)(((prot & AURORA_PROT_READ) ? VMA_R : 0) | ((prot & AURORA_PROT_WRITE) ? VMA_W : 0) |
                     ((prot & AURORA_PROT_EXEC) ? VMA_X : 0));
}

static long sys_mmap(struct process *p, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int fd,
                     int64_t off)
{
    if (len == 0 || len > (1ull << 36))
        return AE_INVAL;
    if (flags & AURORA_MAP_SHARED)
        return AE_NOTSUP; /* shared memory is deferred (roadmap 0.7) */
    if (!(flags & AURORA_MAP_PRIVATE))
        return AE_INVAL;
    if ((prot & AURORA_PROT_WRITE) && (prot & AURORA_PROT_EXEC))
        return AE_ACCES; /* W^X for user mappings too */
    len = ALIGN_UP(len, PAGE_SIZE);
    struct file *f = NULL;
    if (!(flags & AURORA_MAP_ANONYMOUS)) {
        f = proc_fd_get(p, fd);
        if (!f)
            return AE_BADF;
        if (f->kind != FK_VFS || off < 0 || (off & (PAGE_SIZE - 1)))
            return AE_INVAL;
    }
    uint64_t at;
    if (flags & AURORA_MAP_FIXED) {
        if ((addr & (PAGE_SIZE - 1)) || addr < AURORA_USER_BASE || addr + len > AURORA_USER_LIMIT)
            return AE_INVAL;
        at = addr;
    } else {
        at = mm_find_gap(&p->mm, addr ? ALIGN_UP(addr, PAGE_SIZE) : AURORA_MMAP_BASE, len);
        if (!at)
            return AE_NOMEM;
    }
    if (mm_map(&p->mm, at, len, prot_of(prot), VMA_ANON))
        return AE_NOMEM;
    if (f) {
        /* Private file mapping: copy the file's bytes in now. */
        struct vfs_stat st;
        if (vfs_fstat(f->handle, &st) == VFS_OK && (uint64_t)off < st.size) {
            uint64_t n = MIN(len, st.size - (uint64_t)off);
            int save = vfs_tell(f->handle);
            char *tmp = kmalloc(4096);
            if (!tmp) {
                mm_unmap(&p->mm, at, len);
                return AE_NOMEM;
            }
            vfs_seek(f->handle, (uint32_t)off);
            for (uint64_t done = 0; done < n;) {
                int got = vfs_read(f->handle, tmp, MIN(4096u, n - done));
                if (got <= 0)
                    break;
                for (int k = 0; k < got;) {
                    uint8_t *d = mm_populate(&p->mm, at + done + (uint64_t)k);
                    size_t room = PAGE_SIZE - ((at + done + (uint64_t)k) & (PAGE_SIZE - 1));
                    size_t c = MIN(room, (size_t)(got - k));
                    if (!d)
                        break;
                    memcpy(d, tmp + k, c);
                    k += (int)c;
                }
                done += (uint64_t)got;
            }
            vfs_seek(f->handle, (uint32_t)save);
            kfree(tmp);
        }
    }
    return (long)at;
}

/* ---- signals -------------------------------------------------------------- */

#define BIT(sig) (1ull << ((sig) - 1))

static long sys_sigaction(struct process *p, int sig, uint64_t uact, uint64_t uold)
{
    if (sig < 1 || sig >= AURORA_NSIG)
        return AE_INVAL;
    struct aurora_sigaction old = { p->sig[sig].handler, p->sig[sig].restorer, p->sig[sig].mask, p->sig[sig].flags, 0 };
    if (uact) {
        if (sig == AURORA_SIGKILL || sig == AURORA_SIGSTOP)
            return AE_INVAL;
        struct aurora_sigaction a;
        int r = copy_from_user(p, &a, uact, sizeof a);
        if (r)
            return r;
        if (a.handler > AURORA_SIG_IGN && (a.handler >= 0x0000800000000000ull || !a.restorer))
            return AE_INVAL;
        p->sig[sig].handler = a.handler;
        p->sig[sig].restorer = a.restorer;
        p->sig[sig].mask = a.mask;
        p->sig[sig].flags = a.flags;
        /* Setting SIG_IGN discards a pending instance. */
        if (a.handler == AURORA_SIG_IGN) {
            __atomic_and_fetch(&p->sigpending, ~BIT(sig), __ATOMIC_SEQ_CST);
            for (int i = 0; i < p->nthreads; i++)
                __atomic_and_fetch(&p->threads[i]->sigpending, ~BIT(sig), __ATOMIC_SEQ_CST);
        }
    }
    if (uold)
        return copy_to_user(p, uold, &old, sizeof old);
    return 0;
}

static long sys_sigprocmask(struct process *p, int how, uint64_t uset, uint64_t uold)
{
    struct task *t = sched_current();
    uint64_t old = t->sigmask;
    if (uset) {
        uint64_t set;
        int r = copy_from_user(p, &set, uset, 8);
        if (r)
            return r;
        if (how == AURORA_SIG_BLOCK)
            t->sigmask |= set;
        else if (how == AURORA_SIG_UNBLOCK)
            t->sigmask &= ~set;
        else if (how == AURORA_SIG_SETMASK)
            t->sigmask = set;
        else
            return AE_INVAL;
        t->sigmask &= ~(BIT(AURORA_SIGKILL) | BIT(AURORA_SIGSTOP));
    }
    if (uold)
        return copy_to_user(p, uold, &old, 8);
    return 0;
}

static long sys_sigsuspend(struct process *p, uint64_t umask)
{
    uint64_t mask;
    int r = copy_from_user(p, &mask, umask, 8);
    if (r)
        return r;
    struct task *t = sched_current();
    t->saved_mask = t->sigmask;
    t->sigmask = mask & ~(BIT(AURORA_SIGKILL) | BIT(AURORA_SIGSTOP));
    t->in_sigsuspend = true;
    while (!task_interrupted(t)) {
        uint64_t f = irq_save();
        sched_wait_on(t, 0);
        irq_restore(f);
    }
    return AE_INTR; /* the mask is restored after the handler (signal.c) */
}

static bool may_signal(const struct process *p, const struct process *target)
{
    return p->uid == 0 || p->uid == target->uid;
}

static long sys_kill(struct process *p, int pid, int sig)
{
    if (sig < 0 || sig >= AURORA_NSIG)
        return AE_INVAL;
    if (pid > 0) {
        struct process *t = process_get(pid);
        if (!t || t->state != PROC_RUNNING)
            return AE_SRCH;
        if (!may_signal(p, t))
            return AE_PERM;
        return signal_send(t, sig);
    }
    int group = pid == 0 ? p->pgid : -pid;
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct process *t = process_at(i);
        if (!t || t->state != PROC_RUNNING)
            continue;
        if (pid == -1 ? (t->pid == 1 || t == p) : t->pgid != group)
            continue;
        if (!may_signal(p, t))
            continue;
        signal_send(t, sig);
        n++;
    }
    return n ? 0 : AE_SRCH;
}

static long sys_setpgid(struct process *p, int pid, int pgid)
{
    struct process *t = pid ? process_get(pid) : p;
    if (!t || (t != p && t->parent != p->pid))
        return AE_SRCH;
    if (pgid < 0)
        return AE_INVAL;
    if (t->sid != p->sid || t->pid == t->sid)
        return AE_PERM;
    t->pgid = pgid ? pgid : t->pid;
    return 0;
}

static long sys_setsid(struct process *p)
{
    if (p->pgid == p->pid)
        return AE_PERM;
    p->sid = p->pgid = p->pid;
    if (p->tty) {
        pty_put(p->tty);
        p->tty = NULL;
    }
    return p->sid;
}

static long sys_clock_gettime(struct process *p, int clock, uint64_t uts)
{
    struct aurora_timespec ts;
    uint64_t ms;
    switch (clock) {
    case AURORA_CLOCK_REALTIME: {
        uint64_t up = time_ms();
        ts.sec = (int64_t)(process_boot_epoch() + up / 1000);
        ts.nsec = (int64_t)(up % 1000) * 1000000;
        return copy_to_user(p, uts, &ts, sizeof ts);
    }
    case AURORA_CLOCK_MONOTONIC: ms = time_ms(); break;
    case AURORA_CLOCK_PROCESS_CPUTIME: ms = process_cpu_ticks(p) * 1000 / PIT_TARGET_HZ; break;
    case AURORA_CLOCK_THREAD_CPUTIME: ms = sched_current()->cpu_ticks * 1000 / PIT_TARGET_HZ; break;
    default: return AE_INVAL;
    }
    ts.sec = (int64_t)(ms / 1000);
    ts.nsec = (int64_t)(ms % 1000) * 1000000;
    return copy_to_user(p, uts, &ts, sizeof ts);
}

/* ---- network (descriptor based) ------------------------------------------- */

static int sock_of(struct process *p, uint64_t fd)
{
    struct file *f = proc_fd_get(p, (int)fd);
    if (!f)
        return AE_BADF;
    return f->kind == FK_SOCKET ? f->handle : AE_NOTSOCK;
}

static long sys_socket(struct process *p, uint64_t type)
{
    int s = sock_open((uint32_t)type);
    if (s < 0)
        return s;
    struct file *f = file_new(FK_SOCKET, AURORA_O_RDWR);
    if (!f) {
        sock_close(s);
        return AE_NOMEM;
    }
    f->handle = s;
    int fd = proc_fd_install(p, f, 0, false);
    if (fd < 0)
        file_put(f);
    return fd;
}

static long sys_resolve(struct process *p, uint64_t uname, uint64_t uip, uint64_t timeout, uint64_t server, uint64_t port)
{
    char name[128];
    int r = copy_string_from_user(p, uname, name, sizeof name);
    if (r < 0)
        return r;
    uint32_t ip = 0;
    if (server)
        r = net_resolve_via(name, (uint32_t)server, port ? (uint16_t)port : 53, &ip, timeout ? timeout : 3000);
    else
        r = net_resolve(name, &ip, timeout ? timeout : 3000);
    if (r)
        return r;
    return copy_to_user(p, uip, &ip, 4);
}

static long net_buffer_call(struct process *p, int s, uint64_t ubuf, uint64_t len, bool in,
                            long (*fn)(int s, void *buf, uint64_t len, void *ctx), void *ctx)
{
    uint64_t n = MIN(len, 65536ull);
    char *tmp = kmalloc(n ? n : 1);
    if (!tmp)
        return AE_NOMEM;
    long r = 0;
    if (!in && n)
        r = copy_from_user(p, tmp, ubuf, n);
    if (r == 0)
        r = fn(s, tmp, n, ctx);
    if (in && r > 0 && copy_to_user(p, ubuf, tmp, (uint64_t)r))
        r = AE_FAULT;
    kfree(tmp);
    return r;
}

struct netargs { uint64_t a, b, c; };
static long do_send(int s, void *buf, uint64_t len, void *ctx) { UNUSED(ctx); return sock_send(s, buf, len); }
static long do_recv(int s, void *buf, uint64_t len, void *ctx) { return sock_recv(s, buf, len, ((struct netargs *)ctx)->a); }
static long do_sendto(int s, void *buf, uint64_t len, void *ctx)
{
    struct netargs *n = ctx;
    return sock_sendto(s, buf, len, (uint32_t)n->a, (uint16_t)n->b);
}
static long do_recvfrom(int s, void *buf, uint64_t len, void *ctx)
{
    struct netargs *n = ctx;
    uint32_t ip = 0;
    uint16_t port = 0;
    long r = sock_recvfrom(s, buf, len, &ip, &port, n->c);
    n->a = ip;
    n->b = port;
    return r;
}

/* ---- system -------------------------------------------------------------------- */

static long sys_uname(struct process *p, uint64_t u)
{
    struct aurora_utsname n;
    memset(&n, 0, sizeof n);
    str_copy(n.sysname, "Aurora", sizeof n.sysname);
    str_copy(n.nodename, "aurora", sizeof n.nodename);
    str_copy(n.release, AURORA_VERSION, sizeof n.release);
    str_copy(n.version, "Aurora OS " AURORA_VERSION " (SMP, POSIX-ish userland)", sizeof n.version);
    str_copy(n.machine, "x86_64", sizeof n.machine);
    return copy_to_user(p, u, &n, sizeof n);
}

static long sys_sysinfo(struct process *p, uint64_t u)
{
    struct aurora_sysinfo si;
    memset(&si, 0, sizeof si);
    si.uptime_ms = time_ms();
    si.total_pages = pmm_state()->usable_pages;
    si.free_pages = pmm_free_count();
    si.procs = (uint32_t)process_count();
    si.threads = (uint32_t)process_thread_count();
    si.cpus = g_ncpus_online;
    return copy_to_user(p, u, &si, sizeof si);
}

void syscall_dispatch(struct interrupt_frame *f)
{
    struct process *p = process_current();
    if (!p) {
        f->rax = (uint64_t)(int64_t)-AURORA_ENOSYS;
        return;
    }
    p->syscalls++;
    cpu_sti(); /* kernel code is not preempted, but IRQs keep flowing */
    bkl_lock(); /* system calls run under the big kernel lock (0.5) */
    struct task *t = sched_current();
    uint64_t nr = f->rax;
    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8, g = f->r9;
    long r;
    switch (nr) {
    case SYS_EXIT: proc_exit((int)((a & 0xff) << 8), "");
    case SYS_THREAD_EXIT:
        if (p->nthreads == 1)
            proc_exit((int)((a & 0xff) << 8), "");
        proc_thread_exit();
    case SYS_FORK: r = proc_fork(p, f); break;
    case SYS_EXECVE: r = proc_execve(p, f, a, b, c); break;
    case SYS_SPAWN: r = proc_spawn(p, a, b, c, d); break;
    case SYS_WAITPID: r = proc_waitpid(p, (int)a, b, (int)c); break;
    case SYS_GETPID: r = p->pid; break;
    case SYS_GETPPID: r = p->parent; break;
    case SYS_GETTID: r = t->tid; break;
    case SYS_KILL: r = sys_kill(p, (int)a, (int)b); break;
    case SYS_SIGACTION: r = sys_sigaction(p, (int)a, b, c); break;
    case SYS_SIGPROCMASK: r = sys_sigprocmask(p, (int)a, b, c); break;
    case SYS_SIGRETURN:
        signal_sigreturn(f);
        bkl_unlock();
        return; /* every register comes from the frame */
    case SYS_SIGPENDING: {
        uint64_t s = (t->sigpending | p->sigpending) & t->sigmask;
        r = copy_to_user(p, a, &s, 8);
        break;
    }
    case SYS_SIGSUSPEND: r = sys_sigsuspend(p, a); break;
    case SYS_THREAD_CREATE: r = proc_thread_create(p, a); break;
    case SYS_FUTEX: r = proc_futex(p, a, (int)b, (uint32_t)c, (int64_t)d); break;
    case SYS_SET_FSBASE:
        if (a >= 0x0000800000000000ull) {
            r = AE_INVAL;
        } else {
            t->fs_base = a;
            wrmsr(MSR_FS_BASE, a);
            r = 0;
        }
        break;
    case SYS_SETPGID: r = sys_setpgid(p, (int)a, (int)b); break;
    case SYS_GETPGID: {
        struct process *q = a ? process_get((int)a) : p;
        r = q ? q->pgid : AE_SRCH;
        break;
    }
    case SYS_SETSID: r = sys_setsid(p); break;
    case SYS_GETUID: r = p->uid; break;
    case SYS_GETGID: r = p->gid; break;
    case SYS_SETUID:
        if (p->uid == 0 || a == p->uid) {
            p->uid = (uint32_t)a;
            r = 0;
        } else {
            r = AE_PERM;
        }
        break;
    case SYS_SETGID:
        if (p->uid == 0 || a == p->gid) {
            p->gid = (uint32_t)a;
            r = 0;
        } else {
            r = AE_PERM;
        }
        break;
    case SYS_YIELD: sched_yield(); r = 0; break;
    case SYS_NANOSLEEP: {
        uint64_t ms = (a + 999999) / 1000000;
        sched_sleep_ms(MIN(ms, 3600000ull * 24));
        r = task_interrupted(t) ? AE_INTR : 0;
        break;
    }
    case SYS_CLOCK_GETTIME: r = sys_clock_gettime(p, (int)a, b); break;
    case SYS_UMASK: r = p->umask; p->umask = (uint32_t)a & 0777; break;
    case SYS_GETRANDOM: {
        uint64_t n = MIN(b, 256ull);
        uint8_t buf[256];
        for (uint64_t i = 0; i < n; i++)
            buf[i] = (uint8_t)file_random64();
        r = copy_to_user(p, a, buf, n);
        if (!r)
            r = (long)n;
        break;
    }
    case SYS_ALARM: {
        uint64_t now = time_ms();
        r = p->alarm_ms > now ? (long)((p->alarm_ms - now + 999) / 1000) : 0;
        p->alarm_ms = a ? now + a * 1000 : 0;
        break;
    }
    case SYS_BRK: r = (long)mm_brk(&p->mm, a); break;
    case SYS_MMAP: r = sys_mmap(p, a, b, c, d, (int)e, (int64_t)g); break;
    case SYS_MUNMAP:
        r = ((a & (PAGE_SIZE - 1)) || !b) ? AE_INVAL : (mm_unmap(&p->mm, a, b) ? AE_INVAL : 0);
        break;
    case SYS_MPROTECT:
        if ((c & AURORA_PROT_WRITE) && (c & AURORA_PROT_EXEC))
            r = AE_ACCES;
        else
            r = mm_protect(&p->mm, a, b, prot_of(c)) ? AE_NOMEM : 0;
        break;
    case SYS_OPEN: r = sys_open(p, a, (uint32_t)b, (uint32_t)c); break;
    case SYS_CLOSE: r = proc_fd_close(p, (int)a); break;
    case SYS_READ: {
        struct file *fl = proc_fd_get(p, (int)a);
        r = fl ? file_read(p, fl, b, c) : AE_BADF;
        break;
    }
    case SYS_WRITE: {
        struct file *fl = proc_fd_get(p, (int)a);
        r = fl ? file_write(p, fl, b, c) : AE_BADF;
        break;
    }
    case SYS_LSEEK: {
        struct file *fl = proc_fd_get(p, (int)a);
        r = fl ? file_lseek(fl, (int64_t)b, (int)c) : AE_BADF;
        break;
    }
    case SYS_FSTAT: {
        struct file *fl = proc_fd_get(p, (int)a);
        struct aurora_stat st;
        r = fl ? file_stat(fl, &st) : AE_BADF;
        if (!r)
            r = copy_to_user(p, b, &st, sizeof st);
        break;
    }
    case SYS_STAT: r = sys_stat(p, a, b); break;
    case SYS_GETDENTS: {
        struct file *fl = proc_fd_get(p, (int)a);
        r = fl ? file_getdents(p, fl, b, c) : AE_BADF;
        break;
    }
    case SYS_DUP: {
        struct file *fl = proc_fd_get(p, (int)a);
        if (!fl) {
            r = AE_BADF;
        } else {
            file_ref(fl);
            r = proc_fd_install(p, fl, 0, false);
            if (r < 0)
                file_put(fl);
        }
        break;
    }
    case SYS_DUP3: r = sys_dup3(p, (int)a, (int)b, (uint32_t)c); break;
    case SYS_FCNTL: r = sys_fcntl(p, (int)a, (int)b, c); break;
    case SYS_PIPE2: r = sys_pipe2(p, a, (uint32_t)b); break;
    case SYS_POLL: r = sys_poll(p, a, b, (int64_t)c); break;
    case SYS_IOCTL: {
        struct file *fl = proc_fd_get(p, (int)a);
        r = fl ? file_ioctl(p, fl, b, c) : AE_BADF;
        break;
    }
    case SYS_CHDIR: r = sys_chdir(p, a); break;
    case SYS_GETCWD: r = sys_getcwd(p, a, b); break;
    case SYS_MKDIR: r = sys_mkdir(p, a, (uint32_t)b); break;
    case SYS_RMDIR: r = sys_remove(p, a, true); break;
    case SYS_UNLINK: r = sys_remove(p, a, false); break;
    case SYS_RENAME: r = sys_rename(p, a, b); break;
    case SYS_TRUNCATE: r = sys_truncate(p, a, (int64_t)b); break;
    case SYS_FTRUNCATE: {
        struct file *fl = proc_fd_get(p, (int)a);
        if (!fl)
            r = AE_BADF;
        else if (fl->kind != FK_VFS || (int64_t)b < 0 || b > UINT32_MAX)
            r = AE_INVAL;
        else
            r = vfs_ftruncate(fl->handle, (uint32_t)b);
        break;
    }
    case SYS_CHMOD: r = sys_chmod(p, a, (uint32_t)b); break;
    case SYS_FCHMOD: {
        struct file *fl = proc_fd_get(p, (int)a);
        if (!fl)
            r = AE_BADF;
        else if (fl->kind != FK_VFS && fl->kind != FK_DIR)
            r = AE_INVAL;
        else {
            struct vfs_stat st;
            r = vfs_stat(fl->path, &st);
            if (!r)
                r = (p->uid == 0 || p->uid == st.uid) ? vfs_chmod(fl->path, (uint16_t)(b & 07777)) : AE_PERM;
        }
        break;
    }
    case SYS_CHOWN: {
        char path[VFS_PATH_MAX + 1];
        r = get_path(p, a, path);
        if (!r)
            r = p->uid == 0 ? vfs_chown(path, (uint32_t)b, (uint32_t)c) : AE_PERM;
        break;
    }
    case SYS_ACCESS: r = sys_access(p, a, (int)b); break;
    case SYS_SYNC: vfs_sync(); r = 0; break;
    case SYS_FSYNC: r = proc_fd_get(p, (int)a) ? vfs_sync() : AE_BADF; break;
    case SYS_SOCKET: r = sys_socket(p, a); break;
    case SYS_CONNECT: {
        int s = sock_of(p, a);
        r = s < 0 ? s : sock_connect(s, (uint32_t)b, (uint16_t)c, d ? d : 5000);
        break;
    }
    case SYS_SEND: {
        int s = sock_of(p, a);
        r = s < 0 ? s : net_buffer_call(p, s, b, c, false, do_send, NULL);
        break;
    }
    case SYS_RECV: {
        int s = sock_of(p, a);
        struct netargs na = { d, 0, 0 };
        r = s < 0 ? s : net_buffer_call(p, s, b, c, true, do_recv, &na);
        break;
    }
    case SYS_RESOLVE: r = sys_resolve(p, a, b, c, d, e); break;
    case SYS_BIND: {
        int s = sock_of(p, a);
        r = s < 0 ? s : sock_bind(s, (uint16_t)b);
        break;
    }
    case SYS_SENDTO: {
        int s = sock_of(p, a);
        struct netargs na = { d, e, 0 };
        r = s < 0 ? s : net_buffer_call(p, s, b, c, false, do_sendto, &na);
        break;
    }
    case SYS_RECVFROM: {
        int s = sock_of(p, a);
        struct netargs na = { 0, 0, e };
        r = s < 0 ? s : net_buffer_call(p, s, b, c, true, do_recvfrom, &na);
        if (r >= 0 && d) {
            struct aurora_sockaddr from = { (uint32_t)na.a, (uint16_t)na.b, 0 };
            if (copy_to_user(p, d, &from, sizeof from))
                r = AE_FAULT;
        }
        break;
    }
    case SYS_NETINFO: {
        const struct net_config *nc = net_info();
        struct aurora_netinfo ni;
        memset(&ni, 0, sizeof ni);
        ni.up = nc->up;
        ni.configured = nc->configured;
        memcpy(ni.mac, nc->mac, 6);
        ni.ip = nc->ip;
        ni.netmask = nc->netmask;
        ni.gateway = nc->gateway;
        ni.dns = nc->dns;
        ni.rx_packets = nc->rx_packets;
        ni.tx_packets = nc->tx_packets;
        r = copy_to_user(p, a, &ni, sizeof ni);
        break;
    }
    case SYS_PING: {
        uint64_t rtt = 0;
        r = net_ping((uint32_t)a, (uint16_t)b, c ? MIN(c, 30000ull) : 2000, &rtt);
        if (r == 0)
            r = (long)rtt;
        break;
    }
    case SYS_UPTIME: r = (long)time_ms(); break;
    case SYS_SYSINFO: r = sys_sysinfo(p, a); break;
    case SYS_UNAME: r = sys_uname(p, a); break;
    default: r = AE_NOSYS; break;
    }
    r = errno_from(r);
    f->rax = (uint64_t)(int64_t)r;
    t->sys_restart = r == -AURORA_EINTR ? (uint32_t)nr + 1 : 0;
    bkl_unlock();
}
