#ifndef AURORA_ABI_H
#define AURORA_ABI_H
/* User/kernel ABI shared by the kernel and the user-space library.
 *
 * System calls: `int $0x80` with the call number in rax and up to five
 * arguments in rdi, rsi, rdx, r10, r8. The result is returned in rax; values
 * in [-4095, -1] are errors. All other registers are preserved.
 *
 * Errors -1 .. -99 are VFS errors (see fs/vfs.h); kernel-wide errors start at
 * -100. Only fixed-width types are used in structures. */
#include <stdint.h>

enum aurora_syscall {
    SYS_EXIT = 0,      /* (int code) -> never returns */
    SYS_WRITE = 1,     /* (int fd, const void *buf, uint64 len) -> bytes */
    SYS_READ = 2,      /* (int fd, void *buf, uint64 len) -> bytes (0 = EOF) */
    SYS_OPEN = 3,      /* (const char *path, uint32 flags) -> fd */
    SYS_CLOSE = 4,     /* (int fd) */
    SYS_GETPID = 5,    /* () -> pid */
    SYS_SLEEP = 6,     /* (uint64 ms) */
    SYS_YIELD = 7,     /* () */
    SYS_UPTIME = 8,    /* () -> milliseconds since boot */
    SYS_SPAWN = 9,     /* (const char *path, const char *args) -> pid */
    SYS_WAIT = 10,     /* (int pid) -> exit status */
    SYS_READDIR = 11,  /* (const char *path, uint32 index, struct aurora_dirent *) */
    SYS_STAT = 12,     /* (const char *path, struct aurora_stat *) */
    SYS_MKDIR = 13,    /* (const char *path) */
    SYS_UNLINK = 14,   /* (const char *path) */
    SYS_SYNC = 15,     /* () flush persistent storage */
    SYS_SOCKET = 20,   /* (uint32 type) -> socket handle */
    SYS_CONNECT = 21,  /* (int sock, uint32 ipv4_be, uint16 port, uint64 timeout_ms) */
    SYS_SEND = 22,     /* (int sock, const void *buf, uint64 len) -> bytes */
    SYS_RECV = 23,     /* (int sock, void *buf, uint64 len, uint64 timeout_ms) -> bytes (0 = closed) */
    SYS_SOCKCLOSE = 24,/* (int sock) */
    SYS_RESOLVE = 25,  /* (const char *name, uint32 *ipv4_be, uint64 timeout_ms, uint32 server_be, uint16 port)
                        * server 0 = the DHCP-provided DNS server */
    SYS_BIND = 26,     /* (int sock, uint16 port) UDP only */
    SYS_SENDTO = 27,   /* (int sock, const void *buf, uint64 len, uint32 ip, uint16 port) */
    SYS_RECVFROM = 28, /* (int sock, void *buf, uint64 len, struct aurora_sockaddr *, uint64 timeout_ms) */
    SYS_NETINFO = 29,  /* (struct aurora_netinfo *) */
    SYS_PING = 30,     /* (uint32 ipv4_be, uint16 seq, uint64 timeout_ms) -> round trip ms */
    SYS_COUNT = 31,
};

#define AURORA_O_READ 1u
#define AURORA_O_WRITE 2u
#define AURORA_O_CREATE 4u
#define AURORA_O_TRUNC 8u

#define AURORA_SOCK_TCP 1u
#define AURORA_SOCK_UDP 2u

/* Kernel-wide error codes (VFS errors -1..-13 pass through unchanged). */
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
#define AE_INTR (-113)         /* interrupted (process being killed) */
#define AE_CONNRESET (-114)
#define AE_ADDRINUSE (-115)

struct aurora_dirent {
    char name[64];
    uint32_t type; /* 1 = file, 2 = directory */
    uint32_t size;
    uint32_t readonly;
    uint32_t reserved;
};

struct aurora_stat {
    uint32_t type;
    uint32_t size;
    uint32_t readonly;
    uint32_t reserved;
};

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

/* User address space layout */
#define AURORA_USER_BASE 0x0000000000400000ull   /* lowest loadable address */
#define AURORA_USER_LIMIT 0x0000700000000000ull  /* exclusive upper bound for segments */
#define AURORA_USER_STACK_TOP 0x00007fffffff0000ull
#define AURORA_USER_STACK_SIZE (128u * 1024u)
#define AURORA_USER_ARGS_MAX 256u

#endif
