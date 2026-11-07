#ifndef AURORA_KERR_H
#define AURORA_KERR_H
/* Kernel-internal error codes. VFS errors are -1..-99 (fs/vfs.h); these
 * start at -100. System calls translate both to -errno at the user
 * boundary (errno_from() in core/syscall.c). */
#define AE_FAULT (-100)        /* bad user pointer or length */
#define AE_NOSYS (-101)        /* unknown system call */
#define AE_BADF (-102)         /* bad process file descriptor */
#define AE_INVAL (-103)        /* invalid argument */
#define AE_NOMEM (-104)        /* out of memory */
#define AE_CHILD (-105)        /* no such child process */
#define AE_NOEXEC (-106)       /* not a valid Aurora executable */
#define AE_PROCLIM (-107)      /* process or task limit reached */
#define AE_TIMEDOUT (-108)
#define AE_CONNREFUSED (-109)
#define AE_HOSTUNREACH (-110)
#define AE_NETDOWN (-111)      /* no network interface / not configured */
#define AE_NOTFOUND (-112)     /* name did not resolve */
#define AE_INTR (-113)         /* interrupted by a signal or process exit */
#define AE_CONNRESET (-114)
#define AE_ADDRINUSE (-115)
#define AE_PERM (-116)         /* operation not permitted (credentials) */
#define AE_ACCES (-117)        /* permission bits deny access */
#define AE_SRCH (-118)         /* no such process */
#define AE_PIPE (-119)         /* write to a pipe with no readers */
#define AE_AGAIN (-120)        /* would block (O_NONBLOCK) */
#define AE_NOTTY (-121)
#define AE_SPIPE (-122)        /* seek on a pipe/terminal/socket */
#define AE_2BIG (-123)         /* argument list too long */
#define AE_MFILE (-124)        /* descriptor table full */
#define AE_NOTSOCK (-125)
#define AE_RANGE (-126)
#define AE_DEADLK (-127)
#define AE_XDEV (-128)         /* cross-filesystem rename */
#define AE_IO (-129)
#define AE_NOTSUP (-130)
#define AE_BUSY (-131)         /* device already in use */
#define AE_NODEV (-132)
#endif
