#ifndef AURORA_USER_H
#define AURORA_USER_H
/* Aurora user-space library: system call wrappers and a small libc subset.
 * Programs are single-threaded, statically linked and use no FPU/SSE. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "abi/aurora_abi.h"

static inline long __syscall(long n, long a, long b, long c, long d, long e)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "memory");
    return r;
}

/* Processes and time */
__attribute__((noreturn)) void exit(int code);
int getpid(void);
int sleep_ms(uint64_t ms);
void yield(void);
uint64_t uptime_ms(void);
int spawn(const char *path, const char *args);
int wait(int pid);

/* Files (fd 0 = console input, 1 and 2 = console output) */
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);
int open(const char *path, unsigned flags);
int close(int fd);
int readdir(const char *path, uint32_t index, struct aurora_dirent *out);
int stat(const char *path, struct aurora_stat *out);
int mkdir(const char *path);
int unlink(const char *path);
int sync(void);

/* Network */
int socket(uint32_t type);
int connect(int s, uint32_t ip_be, uint16_t port, uint64_t timeout_ms);
long send(int s, const void *buf, size_t len);
long recv(int s, void *buf, size_t len, uint64_t timeout_ms);
int resolve(const char *name, uint32_t *ip_be, uint64_t timeout_ms);
int bind(int s, uint16_t port);
long sendto(int s, const void *buf, size_t len, uint32_t ip_be, uint16_t port);
long recvfrom(int s, void *buf, size_t len, struct aurora_sockaddr *from, uint64_t timeout_ms);
bool parse_ipv4(const char *s, uint32_t *ip_be);
void format_ipv4(uint32_t ip_be, char *buf, size_t cap);

/* libc subset */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int puts(const char *s);
const char *strerror(long err);
long atol(const char *s);
void *malloc(size_t n);
void free(void *p);

/* Reads one line from fd 0 (without the newline). Returns length, or -1 at EOF. */
long readline(char *buf, size_t cap);

#endif
