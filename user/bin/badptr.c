/* badptr: passes invalid pointers and lengths to system calls. Each call
 * must fail with an error (errno) instead of crashing the kernel or the
 * process. */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <aurora_abi.h>

static int failures, checks;

static void expect(const char *what, long got, int want_errno)
{
    checks++;
    int e = got < 0 ? errno : 0;
    if ((want_errno == 0 && got < 0) || (want_errno && (got >= 0 || e != want_errno))) {
        failures++;
        printf("badptr: FAIL %s: got %ld (%s), want %s\n", what, got, e ? strerror(e) : "success",
               want_errno ? strerror(want_errno) : "success");
    } else {
        printf("badptr: ok   %s -> %s\n", what, want_errno ? strerror(want_errno) : "success");
    }
}

static long raw_syscall2(long n, long a, long b)
{
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "memory");
    return r;
}

/* Raw call: -errno -> -1 + errno, so expect() can check it. */
static long raw(long n, long a, long b)
{
    long r = raw_syscall2(n, a, b);
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

int main(void)
{
    struct stat st;
    setvbuf(stdout, NULL, _IOLBF, 0);
    expect("write(kernel address)", write(1, (void *)0xffffffff80000000ull, 16), EFAULT);
    expect("write(unmapped address)", write(1, (void *)0x1000, 16), EFAULT);
    expect("write(length overflow)", write(1, "x", (size_t)-1), EFAULT);
    /* libc converts struct stat itself, so test the kernel's check with the
     * raw call: the kernel must refuse to write into read-only text. */
    expect("stat(into read-only text)", raw(SYS_STAT, (long)"/system/about.txt", (long)(void *)main), EFAULT);
    expect("open(kernel path pointer)", open((const char *)0xffff800000001000ull, O_RDONLY), EFAULT);
    pid_t pid;
    int sr = posix_spawn(&pid, (const char *)0x7000, NULL, NULL, NULL, NULL);
    errno = sr;
    expect("spawn(unmapped path)", sr ? -1 : 0, EFAULT);
    expect("read(bad fd)", read(9, &st, sizeof st), EBADF);
    expect("unknown system call", raw(999, 0, 0), ENOSYS);
    expect("open(/system for writing)", open("/system/about.txt", O_WRONLY), EROFS);
    expect("stat(valid buffer)", stat("/system/about.txt", &st), 0);
    printf("badptr: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
