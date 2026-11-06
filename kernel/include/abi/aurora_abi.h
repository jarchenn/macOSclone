#ifndef AURORA_ABI_H
#define AURORA_ABI_H
/* User/kernel ABI (0.6), shared by the kernel and Aurora's libc layer.
 *
 * System calls: `int $0x80` with the call number in rax and up to six
 * arguments in rdi, rsi, rdx, r10, r8, r9. The result is returned in rax;
 * values in [-4095, -1] are negated errno values (AURORA_E*, numerically the
 * same as newlib's <errno.h>). All other registers are preserved.
 *
 * The numbering is Aurora's own; the semantics follow POSIX where a call has
 * a POSIX counterpart. This is deliberately not the Linux ABI (decision
 * D-0.6-2). Only fixed-width types are used in structures. */
#include <stdint.h>

enum aurora_syscall {
    /* processes and threads */
    SYS_EXIT = 0,           /* (int status): ends every thread of the process */
    SYS_THREAD_EXIT = 1,    /* (int status): ends the calling thread */
    SYS_FORK = 2,           /* () -> child pid (parent), 0 (child) */
    SYS_EXECVE = 3,         /* (path, char *const argv[], char *const envp[]) */
    SYS_SPAWN = 4,          /* (path, argv, envp, const struct aurora_spawn *) -> pid */
    SYS_WAITPID = 5,        /* (pid, int *status, options) -> pid, 0 (WNOHANG) */
    SYS_GETPID = 6,
    SYS_GETPPID = 7,
    SYS_GETTID = 8,
    SYS_KILL = 9,           /* (pid, sig): pid > 0 process, 0 own group, < -1 group -pid */
    SYS_SIGACTION = 10,     /* (sig, const struct aurora_sigaction *, struct aurora_sigaction *) */
    SYS_SIGPROCMASK = 11,   /* (how, const uint64 *set, uint64 *old) */
    SYS_SIGRETURN = 12,     /* () only from the signal trampoline */
    SYS_SIGPENDING = 13,    /* (uint64 *set) */
    SYS_SIGSUSPEND = 14,    /* (const uint64 *mask) -> -EINTR */
    SYS_THREAD_CREATE = 15, /* (const struct aurora_thread *) -> tid */
    SYS_FUTEX = 16,         /* (uint32 *addr, op, uint32 val, int64 timeout_ns) */
    SYS_SET_FSBASE = 17,    /* (uint64 base): thread-local storage pointer */
    SYS_SETPGID = 18,       /* (pid, pgid) */
    SYS_GETPGID = 19,       /* (pid) */
    SYS_SETSID = 20,
    SYS_GETUID = 21,
    SYS_GETGID = 22,
    SYS_SETUID = 23,        /* (uid) root only, or to the current uid */
    SYS_SETGID = 24,
    SYS_YIELD = 25,
    SYS_NANOSLEEP = 26,     /* (uint64 ns) -> 0 or -EINTR */
    SYS_CLOCK_GETTIME = 27, /* (clock, struct aurora_timespec *) */
    SYS_UMASK = 28,         /* (mask) -> old mask */
    SYS_GETRANDOM = 29,     /* (void *buf, uint64 len) -> len */
    SYS_ALARM = 30,         /* (uint32 seconds) -> seconds left on the old alarm */

    /* memory */
    SYS_BRK = 32,           /* (uint64 addr) -> new break (0 queries) */
    SYS_MMAP = 33,          /* (addr, len, prot, flags, fd, offset) -> address */
    SYS_MUNMAP = 34,        /* (addr, len) */
    SYS_MPROTECT = 35,      /* (addr, len, prot) */

    /* files */
    SYS_OPEN = 40,          /* (path, flags, mode) -> fd */
    SYS_CLOSE = 41,
    SYS_READ = 42,
    SYS_WRITE = 43,
    SYS_LSEEK = 44,         /* (fd, int64 offset, whence) -> new offset */
    SYS_FSTAT = 45,         /* (fd, struct aurora_stat *) */
    SYS_STAT = 46,          /* (path, struct aurora_stat *) */
    SYS_GETDENTS = 47,      /* (fd, void *buf, uint64 len) -> bytes (0 = end) */
    SYS_DUP = 48,
    SYS_DUP3 = 49,          /* (old, new, flags): flags 0 behaves like dup2 */
    SYS_FCNTL = 50,         /* (fd, cmd, arg) */
    SYS_PIPE2 = 51,         /* (int fds[2], flags) */
    SYS_POLL = 52,          /* (struct aurora_pollfd *, n, int64 timeout_ms) */
    SYS_IOCTL = 53,         /* (fd, request, arg) */
    SYS_CHDIR = 54,
    SYS_GETCWD = 55,        /* (buf, size) -> length including NUL */
    SYS_MKDIR = 56,         /* (path, mode) */
    SYS_RMDIR = 57,
    SYS_UNLINK = 58,
    SYS_RENAME = 59,        /* (from, to): atomic, replaces a file at `to` */
    SYS_TRUNCATE = 60,      /* (path, length) */
    SYS_FTRUNCATE = 61,     /* (fd, length) */
    SYS_CHMOD = 62,         /* (path, mode) */
    SYS_FCHMOD = 63,        /* (fd, mode) */
    SYS_CHOWN = 64,         /* (path, uid, gid) root only */
    SYS_ACCESS = 65,        /* (path, R_OK|W_OK|X_OK) using the real ids */
    SYS_SYNC = 66,
    SYS_FSYNC = 67,

    /* network (0.4 calls; sockets live in the descriptor table) */
    SYS_SOCKET = 80,        /* (uint32 type) -> fd */
    SYS_CONNECT = 81,       /* (fd, uint32 ipv4_be, uint16 port, uint64 timeout_ms) */
    SYS_SEND = 82,          /* (fd, buf, len) -> bytes */
    SYS_RECV = 83,          /* (fd, buf, len, uint64 timeout_ms) -> bytes (0 = closed) */
    SYS_RESOLVE = 84,       /* (name, uint32 *ipv4_be, timeout_ms, server_be, port) */
    SYS_BIND = 85,          /* (fd, uint16 port) UDP only */
    SYS_SENDTO = 86,        /* (fd, buf, len, uint32 ip, uint16 port) */
    SYS_RECVFROM = 87,      /* (fd, buf, len, struct aurora_sockaddr *, timeout_ms) */
    SYS_NETINFO = 88,       /* (struct aurora_netinfo *) */
    SYS_PING = 89,          /* (uint32 ipv4_be, uint16 seq, timeout_ms) -> round trip ms */

    /* system */
    SYS_UPTIME = 96,        /* () -> milliseconds since boot */
    SYS_SYSINFO = 97,       /* (struct aurora_sysinfo *) */
    SYS_UNAME = 98,         /* (struct aurora_utsname *) */
    SYS_COUNT = 99,
};

/* errno values (negated in system call results). Identical to newlib. */
#define AURORA_EPERM 1
#define AURORA_ENOENT 2
#define AURORA_ESRCH 3
#define AURORA_EINTR 4
#define AURORA_EIO 5
#define AURORA_ENXIO 6
#define AURORA_E2BIG 7
#define AURORA_ENOEXEC 8
#define AURORA_EBADF 9
#define AURORA_ECHILD 10
#define AURORA_EAGAIN 11
#define AURORA_ENOMEM 12
#define AURORA_EACCES 13
#define AURORA_EFAULT 14
#define AURORA_EBUSY 16
#define AURORA_EEXIST 17
#define AURORA_EXDEV 18
#define AURORA_ENODEV 19
#define AURORA_ENOTDIR 20
#define AURORA_EISDIR 21
#define AURORA_EINVAL 22
#define AURORA_ENFILE 23
#define AURORA_EMFILE 24
#define AURORA_ENOTTY 25
#define AURORA_EFBIG 27
#define AURORA_ENOSPC 28
#define AURORA_ESPIPE 29
#define AURORA_EROFS 30
#define AURORA_EMLINK 31
#define AURORA_EPIPE 32
#define AURORA_ERANGE 34
#define AURORA_EDEADLK 45
#define AURORA_ENOSYS 88
#define AURORA_ENOTEMPTY 90
#define AURORA_ENAMETOOLONG 91
#define AURORA_ECONNRESET 104
#define AURORA_ENOTSOCK 108
#define AURORA_ECONNREFUSED 111
#define AURORA_EADDRINUSE 112
#define AURORA_ENETDOWN 115
#define AURORA_ETIMEDOUT 116
#define AURORA_EHOSTUNREACH 118
#define AURORA_ENOTSUP 134
/* A name that does not resolve is reported as ENOENT. */

/* open() flags: newlib's <sys/_default_fcntl.h> values */
#define AURORA_O_RDONLY 0x0
#define AURORA_O_WRONLY 0x1
#define AURORA_O_RDWR 0x2
#define AURORA_O_ACCMODE 0x3
#define AURORA_O_APPEND 0x0008
#define AURORA_O_CREAT 0x0200
#define AURORA_O_TRUNC 0x0400
#define AURORA_O_EXCL 0x0800
#define AURORA_O_NONBLOCK 0x4000
#define AURORA_O_NOCTTY 0x8000
#define AURORA_O_CLOEXEC 0x40000
#define AURORA_O_DIRECTORY 0x200000

/* fcntl */
#define AURORA_F_DUPFD 0
#define AURORA_F_GETFD 1
#define AURORA_F_SETFD 2
#define AURORA_F_GETFL 3
#define AURORA_F_SETFL 4
#define AURORA_F_DUPFD_CLOEXEC 14
#define AURORA_FD_CLOEXEC 1

/* lseek */
#define AURORA_SEEK_SET 0
#define AURORA_SEEK_CUR 1
#define AURORA_SEEK_END 2

/* File types and permission bits (st_mode), as in newlib's <sys/stat.h>. */
#define AURORA_S_IFMT 0170000
#define AURORA_S_IFDIR 0040000
#define AURORA_S_IFCHR 0020000
#define AURORA_S_IFREG 0100000
#define AURORA_S_IFIFO 0010000
#define AURORA_S_IFSOCK 0140000

#define AURORA_R_OK 4
#define AURORA_W_OK 2
#define AURORA_X_OK 1

struct aurora_stat {
    uint64_t dev, ino;
    uint32_t mode, nlink, uid, gid;
    uint64_t rdev, size, blksize, blocks;
    int64_t atime, mtime, ctime; /* seconds since the epoch */
};

/* getdents record: reclen bytes, 8-byte aligned, NUL-terminated name. */
struct aurora_dirent {
    uint64_t ino;
    uint16_t reclen;
    uint8_t type; /* AURORA_DT_* */
    char name[];
};
#define AURORA_DT_UNKNOWN 0
#define AURORA_DT_CHR 2
#define AURORA_DT_DIR 4
#define AURORA_DT_REG 8

/* mmap */
#define AURORA_PROT_NONE 0
#define AURORA_PROT_READ 1
#define AURORA_PROT_WRITE 2
#define AURORA_PROT_EXEC 4
#define AURORA_MAP_SHARED 0x01
#define AURORA_MAP_PRIVATE 0x02
#define AURORA_MAP_FIXED 0x10
#define AURORA_MAP_ANONYMOUS 0x20

/* waitpid: status encoding matches newlib's <sys/wait.h>:
 * exited: code << 8; killed by a signal: the signal number. */
#define AURORA_WNOHANG 1

/* Signals: newlib's default numbering. */
#define AURORA_SIGHUP 1
#define AURORA_SIGINT 2
#define AURORA_SIGQUIT 3
#define AURORA_SIGILL 4
#define AURORA_SIGTRAP 5
#define AURORA_SIGABRT 6
#define AURORA_SIGFPE 8
#define AURORA_SIGKILL 9
#define AURORA_SIGBUS 10
#define AURORA_SIGSEGV 11
#define AURORA_SIGSYS 12
#define AURORA_SIGPIPE 13
#define AURORA_SIGALRM 14
#define AURORA_SIGTERM 15
#define AURORA_SIGURG 16
#define AURORA_SIGSTOP 17
#define AURORA_SIGTSTP 18
#define AURORA_SIGCONT 19
#define AURORA_SIGCHLD 20
#define AURORA_SIGTTIN 21
#define AURORA_SIGTTOU 22
#define AURORA_SIGIO 23
#define AURORA_SIGWINCH 28
#define AURORA_SIGUSR1 30
#define AURORA_SIGUSR2 31
#define AURORA_NSIG 32

#define AURORA_SIG_DFL 0ull
#define AURORA_SIG_IGN 1ull
#define AURORA_SA_RESTART 0x10000000u
#define AURORA_SA_NODEFER 0x40000000u
#define AURORA_SA_RESETHAND 0x80000000u
#define AURORA_SA_SIGINFO 0x2u
#define AURORA_SIG_BLOCK 1
#define AURORA_SIG_UNBLOCK 2
#define AURORA_SIG_SETMASK 0

struct aurora_sigaction {
    uint64_t handler;  /* SIG_DFL, SIG_IGN or a function (int sig) */
    uint64_t restorer; /* code that calls SYS_SIGRETURN; required for handlers */
    uint64_t mask;     /* bit (sig - 1) blocked while the handler runs */
    uint32_t flags;
    uint32_t reserved;
};

/* Signal frame pushed on the user stack, below the 128-byte red zone and
 * 64-byte aligned. The handler is entered with RSP pointing 8 bytes below
 * the frame, at a return address that is the action's restorer (which calls
 * SYS_SIGRETURN with RSP at the frame), as if the handler had been called.
 * rdi = signal, rsi = 0, rdx = frame. */
struct aurora_sigframe {
    uint64_t sig;
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
    uint64_t mask;     /* signal mask to restore */
    uint64_t fsbase;
    uint64_t reserved;
    uint8_t fxsave[512] __attribute__((aligned(16)));
};

/* Thread creation */
struct aurora_thread {
    uint64_t entry;     /* void entry(void *arg) - must not return */
    uint64_t stack;     /* initial RSP (16-byte aligned, minus 8 as after a call) */
    uint64_t arg;
    uint64_t fsbase;    /* TLS block of the new thread */
    uint64_t clear_tid; /* uint32 *: set to 0 and futex-woken when the thread exits */
};

#define AURORA_FUTEX_WAIT 0
#define AURORA_FUTEX_WAKE 1

/* Native process creation (posix_spawn). fd_map[i] is the parent descriptor
 * that becomes descriptor i in the child, or -1 to leave i closed; entries
 * past nfds are closed. */
#define AURORA_SPAWN_FDS 16
#define AURORA_SPAWN_SETPGROUP 1
#define AURORA_SPAWN_SETSIGMASK 2
#define AURORA_SPAWN_SETSIGDEF 4
struct aurora_spawn {
    uint32_t flags;
    int32_t pgroup;
    uint32_t nfds;
    int32_t fd_map[AURORA_SPAWN_FDS];
    uint64_t sigmask;
    uint64_t sigdefault;
};

struct aurora_pollfd {
    int32_t fd;
    int16_t events, revents;
};
#define AURORA_POLLIN 0x0001
#define AURORA_POLLPRI 0x0002
#define AURORA_POLLOUT 0x0004
#define AURORA_POLLERR 0x0008
#define AURORA_POLLHUP 0x0010
#define AURORA_POLLNVAL 0x0020

struct aurora_timespec {
    int64_t sec, nsec;
};
#define AURORA_CLOCK_REALTIME 1
#define AURORA_CLOCK_MONOTONIC 4
#define AURORA_CLOCK_PROCESS_CPUTIME 2
#define AURORA_CLOCK_THREAD_CPUTIME 3

/* Terminal ioctls and termios (Aurora layout; libc's <termios.h> uses it
 * directly). */
#define AURORA_TCGETS 0x5401
#define AURORA_TCSETS 0x5402
#define AURORA_TCSETSW 0x5403
#define AURORA_TCSETSF 0x5404
#define AURORA_TIOCGPGRP 0x540F
#define AURORA_TIOCSPGRP 0x5410
#define AURORA_TIOCGWINSZ 0x5413
#define AURORA_TIOCSWINSZ 0x5414
#define AURORA_FIONREAD 0x541B

#define AURORA_NCCS 16
struct aurora_termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[AURORA_NCCS];
    uint32_t ispeed, ospeed;
};
/* iflag */
#define AURORA_ICRNL 0x0100
#define AURORA_INLCR 0x0040
#define AURORA_IGNCR 0x0080
/* oflag */
#define AURORA_OPOST 0x0001
#define AURORA_ONLCR 0x0004
/* lflag */
#define AURORA_ISIG 0x0001
#define AURORA_ICANON 0x0002
#define AURORA_ECHO 0x0008
#define AURORA_ECHOE 0x0010
#define AURORA_ECHOK 0x0020
#define AURORA_ECHONL 0x0040
#define AURORA_NOFLSH 0x0080
#define AURORA_IEXTEN 0x8000
/* cc indexes */
#define AURORA_VINTR 0
#define AURORA_VQUIT 1
#define AURORA_VERASE 2
#define AURORA_VKILL 3
#define AURORA_VEOF 4
#define AURORA_VTIME 5
#define AURORA_VMIN 6
#define AURORA_VSUSP 10

struct aurora_winsize {
    uint16_t rows, cols, xpixel, ypixel;
};

#define AURORA_SOCK_TCP 1u
#define AURORA_SOCK_UDP 2u

struct aurora_sockaddr {
    uint32_t ipv4_be;
    uint16_t port;
    uint16_t reserved;
};

struct aurora_netinfo {
    uint8_t up, configured, reserved[2];
    uint8_t mac[6];
    uint16_t reserved2;
    uint32_t ip, netmask, gateway, dns; /* network byte order */
    uint64_t rx_packets, tx_packets;
};

struct aurora_sysinfo {
    uint64_t uptime_ms;
    uint64_t total_pages, free_pages;
    uint32_t procs, threads;
    uint32_t cpus, reserved;
};

struct aurora_utsname {
    char sysname[32], nodename[32], release[32], version[64], machine[32];
};

/* Initial process stack (System V style), from low to high addresses:
 *   argc, argv[0..argc-1], NULL, envp[...], NULL, auxv pairs, AT_NULL,
 *   then the strings. RSP points at argc and is 16-byte aligned. */
#define AURORA_AT_NULL 0
#define AURORA_AT_PHDR 3
#define AURORA_AT_PHENT 4
#define AURORA_AT_PHNUM 5
#define AURORA_AT_PAGESZ 6
#define AURORA_AT_ENTRY 9
#define AURORA_AT_UID 11
#define AURORA_AT_GID 13
#define AURORA_AT_RANDOM 25

/* User address space layout */
#define AURORA_USER_BASE 0x0000000000400000ull   /* lowest loadable address */
#define AURORA_USER_LIMIT 0x0000700000000000ull  /* exclusive upper bound for segments */
#define AURORA_MMAP_BASE 0x0000100000000000ull   /* mmap search starts here */
#define AURORA_USER_STACK_TOP 0x00007fffffff0000ull
#define AURORA_USER_STACK_SIZE (8u * 1024u * 1024u) /* reserved; pages appear on first touch */
#define AURORA_ARG_MAX (128u * 1024u)               /* argv + envp strings */

#endif
