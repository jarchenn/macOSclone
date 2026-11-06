/* newlib's OS hooks and the POSIX system interfaces Aurora provides
 * (file I/O, processes, memory, time, terminals). Each wrapper turns the
 * kernel's -errno result into -1 + errno. */
#include "sys.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <poll.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <reent.h>

/* ---- files ---------------------------------------------------------------- */

int open(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)__ret(__sys3(SYS_OPEN, path, flags, mode));
}
int _open(const char *path, int flags, int mode) { return (int)__ret(__sys3(SYS_OPEN, path, flags, mode)); }
int creat(const char *path, mode_t mode) { return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode); }

int close(int fd) { return (int)__ret(__sys1(SYS_CLOSE, fd)); }
int _close(int fd) { return close(fd); }

ssize_t read(int fd, void *buf, size_t n) { return __ret(__sys3(SYS_READ, fd, buf, n)); }
_ssize_t _read(int fd, void *buf, size_t n) { return read(fd, buf, n); }
ssize_t write(int fd, const void *buf, size_t n) { return __ret(__sys3(SYS_WRITE, fd, buf, n)); }
_ssize_t _write(int fd, const void *buf, size_t n) { return write(fd, buf, n); }

off_t lseek(int fd, off_t off, int whence) { return __ret(__sys3(SYS_LSEEK, fd, off, whence)); }
_off_t _lseek(int fd, _off_t off, int whence) { return lseek(fd, off, whence); }

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    off_t old = lseek(fd, 0, SEEK_CUR);
    if (old < 0 || lseek(fd, off, SEEK_SET) < 0)
        return -1;
    ssize_t r = read(fd, buf, n);
    int e = errno;
    lseek(fd, old, SEEK_SET);
    errno = e;
    return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    off_t old = lseek(fd, 0, SEEK_CUR);
    if (old < 0 || lseek(fd, off, SEEK_SET) < 0)
        return -1;
    ssize_t r = write(fd, buf, n);
    int e = errno;
    lseek(fd, old, SEEK_SET);
    errno = e;
    return r;
}

static void conv_stat(const struct aurora_stat *a, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = (dev_t)a->dev;
    st->st_ino = (ino_t)a->ino;
    st->st_mode = (mode_t)a->mode;
    st->st_nlink = (nlink_t)a->nlink;
    st->st_uid = (uid_t)a->uid;
    st->st_gid = (gid_t)a->gid;
    st->st_rdev = (dev_t)a->rdev;
    st->st_size = (off_t)a->size;
    st->st_blksize = (blksize_t)a->blksize;
    st->st_blocks = (blkcnt_t)a->blocks;
    st->st_atim.tv_sec = a->atime;
    st->st_mtim.tv_sec = a->mtime;
    st->st_ctim.tv_sec = a->ctime;
}

int fstat(int fd, struct stat *st)
{
    struct aurora_stat a;
    long r = __sys2(SYS_FSTAT, fd, &a);
    if (r < 0)
        return (int)__ret(r);
    conv_stat(&a, st);
    return 0;
}
int _fstat(int fd, struct stat *st) { return fstat(fd, st); }

int stat(const char *path, struct stat *st)
{
    struct aurora_stat a;
    long r = __sys2(SYS_STAT, path, &a);
    if (r < 0)
        return (int)__ret(r);
    conv_stat(&a, st);
    return 0;
}
int _stat(const char *path, struct stat *st) { return stat(path, st); }
int lstat(const char *path, struct stat *st) { return stat(path, st); } /* no symlinks */

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    (void)flags;
    if (dirfd != AT_FDCWD && path[0] != '/') {
        errno = ENOSYS;
        return -1;
    }
    return stat(path, st);
}

int getdents(int fd, void *buf, int len) { return (int)__ret(__sys3(SYS_GETDENTS, fd, buf, len)); }

int mkdir(const char *path, mode_t mode) { return (int)__ret(__sys2(SYS_MKDIR, path, mode)); }
int _mkdir(const char *path, int mode) { return mkdir(path, (mode_t)mode); }
int rmdir(const char *path) { return (int)__ret(__sys1(SYS_RMDIR, path)); }
int unlink(const char *path) { return (int)__ret(__sys1(SYS_UNLINK, path)); }
int _unlink(const char *path) { return unlink(path); }
int _rename(const char *from, const char *to) { return (int)__ret(__sys2(SYS_RENAME, from, to)); }
int link(const char *a, const char *b)
{
    (void)a;
    (void)b;
    errno = EMLINK; /* the file systems have no hard links */
    return -1;
}
int _link(const char *a, const char *b) { return link(a, b); }
int symlink(const char *a, const char *b)
{
    (void)a;
    (void)b;
    errno = ENOSYS;
    return -1;
}
ssize_t readlink(const char *path, char *buf, size_t n)
{
    (void)buf;
    (void)n;
    struct stat st;
    if (stat(path, &st) == 0)
        errno = EINVAL; /* not a symbolic link: there are none */
    return -1;
}

int truncate(const char *path, off_t len) { return (int)__ret(__sys2(SYS_TRUNCATE, path, len)); }
int ftruncate(int fd, off_t len) { return (int)__ret(__sys2(SYS_FTRUNCATE, fd, len)); }
int chmod(const char *path, mode_t mode) { return (int)__ret(__sys2(SYS_CHMOD, path, mode)); }
int fchmod(int fd, mode_t mode) { return (int)__ret(__sys2(SYS_FCHMOD, fd, mode)); }
int chown(const char *path, uid_t uid, gid_t gid) { return (int)__ret(__sys3(SYS_CHOWN, path, uid, gid)); }
int lchown(const char *path, uid_t uid, gid_t gid) { return chown(path, uid, gid); }
int access(const char *path, int mode) { return (int)__ret(__sys2(SYS_ACCESS, path, mode)); }
mode_t umask(mode_t m) { return (mode_t)__sys1(SYS_UMASK, m); }
void sync(void) { __sys0(SYS_SYNC); }
int fsync(int fd) { return (int)__ret(__sys1(SYS_FSYNC, fd)); }
int fdatasync(int fd) { return fsync(fd); }

int chdir(const char *path) { return (int)__ret(__sys1(SYS_CHDIR, path)); }
int fchdir(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

char *getcwd(char *buf, size_t size)
{
    char tmp[260];
    long r = __sys2(SYS_GETCWD, tmp, sizeof tmp);
    if (r < 0) {
        __ret(r);
        return NULL;
    }
    if (!buf) {
        size_t n = size && size > (size_t)r ? size : (size_t)r;
        buf = malloc(n);
        if (!buf) {
            errno = ENOMEM;
            return NULL;
        }
    } else if (size < (size_t)r) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buf, tmp, (size_t)r);
    return buf;
}

char *realpath(const char *path, char *resolved)
{
    /* No symbolic links: resolving is normalising against the cwd. */
    char cwd[260], out[260];
    size_t n = 0;
    if (path[0] != '/') {
        if (!getcwd(cwd, sizeof cwd))
            return NULL;
        n = strlen(cwd);
        memcpy(out, cwd, n);
    }
    out[n] = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        const char *s = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t)(p - s);
        if (len == 0 || (len == 1 && s[0] == '.'))
            continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;
            out[n] = 0;
            continue;
        }
        if (n + 1 + len >= sizeof out) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        out[n++] = '/';
        memcpy(out + n, s, len);
        n += len;
        out[n] = 0;
    }
    if (n == 0) {
        out[0] = '/';
        out[1] = 0;
    }
    struct stat st;
    if (stat(out, &st) < 0)
        return NULL;
    if (!resolved) {
        resolved = malloc(strlen(out) + 1);
        if (!resolved) {
            errno = ENOMEM;
            return NULL;
        }
    }
    strcpy(resolved, out);
    return resolved;
}

/* ---- descriptors -------------------------------------------------------- */

int dup(int fd) { return (int)__ret(__sys1(SYS_DUP, fd)); }
int dup2(int a, int b) { return (int)__ret(__sys3(SYS_DUP3, a, b, 0)); }
int dup3(int a, int b, int flags) { return (int)__ret(__sys3(SYS_DUP3, a, b, flags ? flags : O_CLOEXEC)); }
int pipe(int fds[2]) { return (int)__ret(__sys2(SYS_PIPE2, fds, 0)); }
int pipe2(int fds[2], int flags) { return (int)__ret(__sys2(SYS_PIPE2, fds, flags)); }

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    return (int)__ret(__sys3(SYS_FCNTL, fd, cmd, arg));
}
int _fcntl(int fd, int cmd, int arg) { return (int)__ret(__sys3(SYS_FCNTL, fd, cmd, arg)); }

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return (int)__ret(__sys3(SYS_IOCTL, fd, req, arg));
}

int poll(struct pollfd *fds, nfds_t n, int timeout) { return (int)__ret(__sys3(SYS_POLL, fds, n, timeout)); }

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct pollfd p[64];
    int n = 0;
    if (nfds > 64)
        nfds = 64;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (rd && FD_ISSET(fd, rd))
            ev |= POLLIN;
        if (wr && FD_ISSET(fd, wr))
            ev |= POLLOUT;
        if (ex && FD_ISSET(fd, ex))
            ev |= POLLPRI;
        if (ev) {
            p[n].fd = fd;
            p[n].events = ev;
            p[n].revents = 0;
            n++;
        }
    }
    int timeout = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    int r = poll(p, (nfds_t)n, timeout);
    if (r < 0)
        return -1;
    if (rd)
        FD_ZERO(rd);
    if (wr)
        FD_ZERO(wr);
    if (ex)
        FD_ZERO(ex);
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (rd && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(p[i].fd, rd);
            count++;
        }
        if (wr && (p[i].revents & (POLLOUT | POLLERR))) {
            FD_SET(p[i].fd, wr);
            count++;
        }
        if (ex && (p[i].revents & POLLPRI)) {
            FD_SET(p[i].fd, ex);
            count++;
        }
        if (p[i].revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
    }
    return count;
}

int _isatty(int fd)
{
    struct termios t;
    long r = __sys3(SYS_IOCTL, fd, TCGETS, &t);
    if (r < 0) {
        errno = r == -EBADF ? EBADF : ENOTTY;
        return 0;
    }
    return 1;
}

static char ttyname_buf[32];
int ttyname_r(int fd, char *buf, size_t n)
{
    if (!_isatty(fd))
        return errno;
    if (n < 9)
        return ERANGE;
    strcpy(buf, "/dev/tty");
    return 0;
}
char *ttyname(int fd)
{
    int r = ttyname_r(fd, ttyname_buf, sizeof ttyname_buf);
    if (r) {
        errno = r;
        return NULL;
    }
    return ttyname_buf;
}

/* ---- terminals ------------------------------------------------------------- */

int tcgetattr(int fd, struct termios *t) { return (int)__ret(__sys3(SYS_IOCTL, fd, TCGETS, t)); }
int tcsetattr(int fd, int when, const struct termios *t)
{
    unsigned long req = when == TCSAFLUSH ? TCSETSF : when == TCSADRAIN ? TCSETSW : TCSETS;
    return (int)__ret(__sys3(SYS_IOCTL, fd, req, t));
}
int tcdrain(int fd) { return _isatty(fd) ? 0 : -1; }
int tcflow(int fd, int action)
{
    (void)action;
    return _isatty(fd) ? 0 : -1;
}
int tcsendbreak(int fd, int d)
{
    (void)d;
    return _isatty(fd) ? 0 : -1;
}
int tcflush(int fd, int q)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return -1;
    if (q == TCIFLUSH || q == TCIOFLUSH)
        return tcsetattr(fd, TCSAFLUSH, &t);
    return 0;
}
pid_t tcgetpgrp(int fd)
{
    int g;
    return __ret(__sys3(SYS_IOCTL, fd, TIOCGPGRP, &g)) < 0 ? -1 : g;
}
int tcsetpgrp(int fd, pid_t pg)
{
    int g = pg;
    return (int)__ret(__sys3(SYS_IOCTL, fd, TIOCSPGRP, &g));
}
pid_t tcgetsid(int fd)
{
    if (!_isatty(fd))
        return -1;
    return getsid(0);
}
void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}
speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
int cfsetispeed(struct termios *t, speed_t s)
{
    t->c_ispeed = s;
    return 0;
}
int cfsetospeed(struct termios *t, speed_t s)
{
    t->c_ospeed = s;
    return 0;
}

/* ---- processes -------------------------------------------------------------- */

void _exit(int code)
{
    __sys1(SYS_EXIT, code);
    for (;;)
        ;
}

pid_t getpid(void) { return (pid_t)__sys0(SYS_GETPID); }
int _getpid(void) { return getpid(); }
pid_t getppid(void) { return (pid_t)__sys0(SYS_GETPPID); }
pid_t gettid(void) { return (pid_t)__sys0(SYS_GETTID); }

pid_t fork(void)
{
    long r = __sys0(SYS_FORK);
    if (r == 0)
        __pthread_after_fork_child();
    return (pid_t)__ret(r);
}
int _fork(void) { return fork(); }
pid_t vfork(void) { return fork(); }

int _execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)__ret(__sys3(SYS_EXECVE, path, argv, envp));
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    int st = 0;
    long r = __sys3(SYS_WAITPID, pid, &st, options);
    if (r > 0 && status)
        *status = st;
    return (pid_t)__ret(r);
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }
int _wait(int *status) { return wait(status); }

int kill(pid_t pid, int sig) { return (int)__ret(__sys2(SYS_KILL, pid, sig)); }
int _kill(int pid, int sig) { return kill(pid, sig); }
int killpg(pid_t pg, int sig) { return kill(-pg, sig); }

pid_t getpgrp(void) { return (pid_t)__sys1(SYS_GETPGID, 0); }
pid_t getpgid(pid_t pid) { return (pid_t)__ret(__sys1(SYS_GETPGID, pid)); }
int setpgid(pid_t pid, pid_t pg) { return (int)__ret(__sys2(SYS_SETPGID, pid, pg)); }
int setpgrp(void) { return setpgid(0, 0); }
pid_t setsid(void) { return (pid_t)__ret(__sys0(SYS_SETSID)); }
pid_t getsid(pid_t pid)
{
    (void)pid;
    return getpgid(0); /* approximation: sessions are not queryable */
}

uid_t getuid(void) { return (uid_t)__sys0(SYS_GETUID); }
uid_t geteuid(void) { return getuid(); }
gid_t getgid(void) { return (gid_t)__sys0(SYS_GETGID); }
gid_t getegid(void) { return getgid(); }
int setuid(uid_t u) { return (int)__ret(__sys1(SYS_SETUID, u)); }
int seteuid(uid_t u) { return setuid(u); }
int setgid(gid_t g) { return (int)__ret(__sys1(SYS_SETGID, g)); }
int setegid(gid_t g) { return setgid(g); }
int getgroups(int n, gid_t *list)
{
    if (n > 0)
        list[0] = getgid();
    return 1;
}

int sched_yield(void)
{
    __sys0(SYS_YIELD);
    return 0;
}

/* ---- memory ------------------------------------------------------------ */

static uintptr_t cur_brk;

void *sbrk(ptrdiff_t inc)
{
    if (!cur_brk)
        cur_brk = (uintptr_t)__sys1(SYS_BRK, 0);
    uintptr_t old = cur_brk;
    if (inc == 0)
        return (void *)old;
    uintptr_t want = old + (uintptr_t)inc;
    uintptr_t got = (uintptr_t)__sys1(SYS_BRK, want);
    if (got != want) {
        errno = ENOMEM;
        return (void *)-1;
    }
    cur_brk = want;
    return (void *)old;
}
void *_sbrk(ptrdiff_t inc) { return sbrk(inc); }
int brk(void *addr) { return (uintptr_t)__sys1(SYS_BRK, addr) == (uintptr_t)addr ? 0 : (errno = ENOMEM, -1); }

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    long r = __sys6(SYS_MMAP, (long)addr, (long)len, prot, flags, fd, off);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void *)r;
}
int munmap(void *addr, size_t len) { return (int)__ret(__sys2(SYS_MUNMAP, addr, len)); }
int mprotect(void *addr, size_t len, int prot) { return (int)__ret(__sys3(SYS_MPROTECT, addr, len, prot)); }

int getpagesize(void) { return 4096; }

/* ---- time --------------------------------------------------------------------- */

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    struct aurora_timespec a;
    long r = __sys2(SYS_CLOCK_GETTIME, clk, &a);
    if (r < 0)
        return (int)__ret(r);
    ts->tv_sec = (time_t)a.sec;
    ts->tv_nsec = (long)a.nsec;
    return 0;
}

int clock_getres(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 4000000; /* 250 Hz timer */
    }
    return 0;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts))
        return -1;
    if (tv) {
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    return 0;
}
int gettimeofday(struct timeval *tv, void *tz) { return _gettimeofday(tv, tz); }

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    uint64_t ns = (uint64_t)req->tv_sec * 1000000000ull + (uint64_t)req->tv_nsec;
    long r = __sys1(SYS_NANOSLEEP, ns);
    if (r < 0) {
        if (rem) {
            clock_gettime(CLOCK_MONOTONIC, &b);
            int64_t done = (b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec);
            int64_t left = (int64_t)ns - done;
            if (left < 0)
                left = 0;
            rem->tv_sec = (time_t)(left / 1000000000ll);
            rem->tv_nsec = (long)(left % 1000000000ll);
        }
        return (int)__ret(r);
    }
    return 0;
}

clock_t _times(struct tms *t)
{
    struct timespec p;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &p);
    clock_t ticks = (clock_t)(p.tv_sec * CLOCKS_PER_SEC + p.tv_nsec / (1000000000L / CLOCKS_PER_SEC));
    if (t) {
        t->tms_utime = ticks;
        t->tms_stime = 0;
        t->tms_cutime = t->tms_cstime = 0;
    }
    struct timespec m;
    clock_gettime(CLOCK_MONOTONIC, &m);
    return (clock_t)(m.tv_sec * CLOCKS_PER_SEC + m.tv_nsec / (1000000000L / CLOCKS_PER_SEC));
}
clock_t times(struct tms *t) { return _times(t); }

int getrusage(int who, struct rusage *ru)
{
    memset(ru, 0, sizeof *ru);
    if (who == RUSAGE_SELF) {
        struct timespec p;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &p);
        ru->ru_utime.tv_sec = p.tv_sec;
        ru->ru_utime.tv_usec = p.tv_nsec / 1000;
    }
    return 0;
}

unsigned alarm(unsigned seconds) { return (unsigned)__sys1(SYS_ALARM, seconds); }

int getentropy(void *buf, size_t n)
{
    if (n > 256) {
        errno = EIO;
        return -1;
    }
    return __ret(__sys2(SYS_GETRANDOM, buf, n)) < 0 ? -1 : 0;
}
int _getentropy(void *buf, size_t n) { return getentropy(buf, n); }

/* ---- system information ------------------------------------------------------ */

int uname(struct utsname *u)
{
    struct aurora_utsname a;
    long r = __sys1(SYS_UNAME, &a);
    if (r < 0)
        return (int)__ret(r);
    memset(u, 0, sizeof *u);
    strncpy(u->sysname, a.sysname, sizeof u->sysname - 1);
    strncpy(u->nodename, a.nodename, sizeof u->nodename - 1);
    strncpy(u->release, a.release, sizeof u->release - 1);
    strncpy(u->version, a.version, sizeof u->version - 1);
    strncpy(u->machine, a.machine, sizeof u->machine - 1);
    return 0;
}

int gethostname(char *name, size_t len)
{
    struct utsname u;
    if (uname(&u))
        return -1;
    if (strlen(u.nodename) + 1 > len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(name, u.nodename);
    return 0;
}

long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE: return 4096;
    case _SC_OPEN_MAX: return 64;
    case _SC_CLK_TCK: return CLOCKS_PER_SEC;
    case _SC_ARG_MAX: return AURORA_ARG_MAX;
    case _SC_CHILD_MAX: return 64;
    case _SC_NGROUPS_MAX: return 1;
    case _SC_LINE_MAX: return 1024;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        struct aurora_sysinfo si;
        return __sys1(SYS_SYSINFO, &si) < 0 ? 1 : (long)si.cpus;
    }
    case _SC_PHYS_PAGES: {
        struct aurora_sysinfo si;
        return __sys1(SYS_SYSINFO, &si) < 0 ? -1 : (long)si.total_pages;
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

long pathconf(const char *p, int name)
{
    (void)p;
    switch (name) {
    case _PC_NAME_MAX: return 63;
    case _PC_PATH_MAX: return 256;
    case _PC_PIPE_BUF: return 4096;
    default: errno = EINVAL; return -1;
    }
}
long fpathconf(int fd, int name)
{
    (void)fd;
    return pathconf("/", name);
}
