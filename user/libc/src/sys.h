/* Internal helpers for Aurora's libc layer (not installed). */
#ifndef AURORA_LIBC_SYS_H
#define AURORA_LIBC_SYS_H
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <aurora_abi.h>

static inline long __sys6(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "memory");
    return r;
}

#define __sys0(n) __sys6((n), 0, 0, 0, 0, 0, 0)
#define __sys1(n, a) __sys6((n), (long)(a), 0, 0, 0, 0, 0)
#define __sys2(n, a, b) __sys6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define __sys3(n, a, b, c) __sys6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define __sys4(n, a, b, c, d) __sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define __sys5(n, a, b, c, d, e) __sys6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

/* Converts a raw result: negative errno -> -1 with errno set. */
static inline long __ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

/* Thread control block (x86-64 TLS variant II: %fs:0 points here, the TLS
 * block lies just below it). */
struct pthread;
struct aurora_tcb {
    struct aurora_tcb *self;
    struct pthread *thread;
    uint64_t reserved[3];
    uint64_t canary; /* %fs:0x28, the conventional stack-protector slot */
};

static inline struct aurora_tcb *__tcb(void)
{
    struct aurora_tcb *t;
    __asm__("mov %%fs:0, %0" : "=r"(t));
    return t;
}

/* TLS image from the program headers (start.c). */
extern uintptr_t __tls_image;
extern size_t __tls_filesz, __tls_memsz, __tls_align;
/* Allocates and initialises a TLS block + TCB in `mem` (size from
 * __tls_area_size()); returns the thread pointer. */
size_t __tls_area_size(void);
struct aurora_tcb *__tls_init_area(void *mem);

void __pthread_init_main(struct aurora_tcb *tcb);
void __pthread_after_fork_child(void);
void __libc_reset_handlers(void);

extern char **environ;

#endif
