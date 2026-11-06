/* Aurora user-space runtime. memcpy/strlen/... and the printf formatter are
 * the kernel's own freestanding implementations (kernel/lib), linked in. */
#include "aurora.h"

int k_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int main(int argc, char **argv);

#define MAX_ARGS 16
static char argbuf[AURORA_USER_ARGS_MAX];

__attribute__((noreturn)) void __aurora_start(const char *args, size_t len);
__attribute__((noreturn)) void __aurora_start(const char *args, size_t len)
{
    char *argv[MAX_ARGS + 1];
    int argc = 0;
    if (len >= sizeof argbuf)
        len = sizeof argbuf - 1;
    memcpy(argbuf, args, len);
    argbuf[len] = '\0';
    argv[argc++] = "prog";
    char *p = argbuf;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = '\0';
    }
    argv[argc] = NULL;
    exit(main(argc, argv));
}

void exit(int code)
{
    __syscall(SYS_EXIT, code, 0, 0, 0, 0);
    for (;;)
        ;
}

int getpid(void) { return (int)__syscall(SYS_GETPID, 0, 0, 0, 0, 0); }
int sleep_ms(uint64_t ms) { return (int)__syscall(SYS_SLEEP, (long)ms, 0, 0, 0, 0); }
void yield(void) { __syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
uint64_t uptime_ms(void) { return (uint64_t)__syscall(SYS_UPTIME, 0, 0, 0, 0, 0); }
int spawn(const char *path, const char *args) { return (int)__syscall(SYS_SPAWN, (long)path, (long)args, 0, 0, 0); }
int wait(int pid) { return (int)__syscall(SYS_WAIT, pid, 0, 0, 0, 0); }
long write(int fd, const void *buf, size_t len) { return __syscall(SYS_WRITE, fd, (long)buf, (long)len, 0, 0); }
long read(int fd, void *buf, size_t len) { return __syscall(SYS_READ, fd, (long)buf, (long)len, 0, 0); }
int open(const char *path, unsigned flags) { return (int)__syscall(SYS_OPEN, (long)path, flags, 0, 0, 0); }
int close(int fd) { return (int)__syscall(SYS_CLOSE, fd, 0, 0, 0, 0); }
int readdir(const char *path, uint32_t index, struct aurora_dirent *out) { return (int)__syscall(SYS_READDIR, (long)path, index, (long)out, 0, 0); }
int stat(const char *path, struct aurora_stat *out) { return (int)__syscall(SYS_STAT, (long)path, (long)out, 0, 0, 0); }
int mkdir(const char *path) { return (int)__syscall(SYS_MKDIR, (long)path, 0, 0, 0, 0); }
int unlink(const char *path) { return (int)__syscall(SYS_UNLINK, (long)path, 0, 0, 0, 0); }
int sync(void) { return (int)__syscall(SYS_SYNC, 0, 0, 0, 0, 0); }
int socket(uint32_t type) { return (int)__syscall(SYS_SOCKET, type, 0, 0, 0, 0); }
int connect(int s, uint32_t ip, uint16_t port, uint64_t t) { return (int)__syscall(SYS_CONNECT, s, ip, port, (long)t, 0); }
long send(int s, const void *buf, size_t len) { return __syscall(SYS_SEND, s, (long)buf, (long)len, 0, 0); }
long recv(int s, void *buf, size_t len, uint64_t t) { return __syscall(SYS_RECV, s, (long)buf, (long)len, (long)t, 0); }
int resolve(const char *name, uint32_t *ip, uint64_t t) { return (int)__syscall(SYS_RESOLVE, (long)name, (long)ip, (long)t, 0, 0); }
int bind(int s, uint16_t port) { return (int)__syscall(SYS_BIND, s, port, 0, 0, 0); }
long sendto(int s, const void *buf, size_t len, uint32_t ip, uint16_t port) { return __syscall(SYS_SENDTO, s, (long)buf, (long)len, ip, port); }
long recvfrom(int s, void *buf, size_t len, struct aurora_sockaddr *from, uint64_t t) { return __syscall(SYS_RECVFROM, s, (long)buf, (long)len, (long)from, (long)t); }

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = k_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = k_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = (size_t)r < sizeof buf ? (size_t)r : sizeof buf - 1;
    write(1, buf, n);
    return r;
}

int puts(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

long atol(const char *s)
{
    long v = 0, sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}

long readline(char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        long r = read(0, &c, 1);
        if (r <= 0) {
            if (n == 0)
                return -1;
            break;
        }
        if (c == '\n')
            break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return (long)n;
}

const char *strerror(long e)
{
    switch (e) {
    case 0: return "success";
    case -1: return "no such file or directory";
    case -2: return "file already exists";
    case -3: return "not a directory";
    case -4: return "is a directory";
    case -5: return "read-only file system";
    case -6: return "storage full";
    case -7: return "invalid path or argument";
    case -12: return "file too large";
    case -14: return "file is in use";
    case -15: return "directory not empty";
    case -16: return "disk I/O error";
    case AE_FAULT: return "bad address";
    case AE_NOSYS: return "no such system call";
    case AE_BADF: return "bad file descriptor";
    case AE_INVAL: return "invalid argument";
    case AE_NOMEM: return "out of memory";
    case AE_CHILD: return "no such child process";
    case AE_NOEXEC: return "not an executable";
    case AE_PROCLIM: return "too many processes";
    case AE_TIMEDOUT: return "timed out";
    case AE_CONNREFUSED: return "connection refused";
    case AE_HOSTUNREACH: return "host unreachable";
    case AE_NETDOWN: return "network is down";
    case AE_NOTFOUND: return "name not found";
    case AE_INTR: return "interrupted";
    case AE_CONNRESET: return "connection reset";
    case AE_ADDRINUSE: return "address in use";
    default: return "error";
    }
}

bool parse_ipv4(const char *s, uint32_t *ip)
{
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            v = v * 10 + (uint32_t)(*s++ - '0');
            digits++;
        }
        if (v > 255)
            return false;
        parts[i] = v;
        if (i < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *ip = parts[0] | parts[1] << 8 | parts[2] << 16 | parts[3] << 24; /* network order in memory */
    return true;
}

void format_ipv4(uint32_t ip, char *buf, size_t cap)
{
    snprintf(buf, cap, "%u.%u.%u.%u", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
}

/* Bump allocator over a static arena; free() only releases the most
 * recent allocation. Enough for the small programs shipped with Aurora. */
static uint8_t arena[128 * 1024] __attribute__((aligned(16)));
static size_t arena_used, last_off;

void *malloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (n == 0 || n > sizeof arena - arena_used)
        return NULL;
    last_off = arena_used;
    arena_used += n;
    return arena + last_off;
}

void free(void *p)
{
    if (p && (uint8_t *)p == arena + last_off)
        arena_used = last_off;
}
