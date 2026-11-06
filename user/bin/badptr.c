/* badptr: passes invalid pointers and lengths to system calls. Each call
 * must fail with an error instead of crashing the kernel or the process. */
#include "aurora.h"

static int failures, checks;

static void expect(const char *what, long got, long want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("badptr: FAIL %s: got %ld, want %ld\n", what, got, want);
    } else {
        printf("badptr: ok   %s -> %s\n", what, strerror(got));
    }
}

int main(void)
{
    struct aurora_stat st;
    expect("write(kernel address)", write(1, (void *)0xffffffff80000000ull, 16), AE_FAULT);
    expect("write(unmapped address)", write(1, (void *)0x1000, 16), AE_FAULT);
    expect("write(length overflow)", write(1, "x", (size_t)-1), AE_FAULT);
    expect("stat(into read-only text)", stat("/system/about.txt", (struct aurora_stat *)(void *)main), AE_FAULT);
    expect("open(kernel path pointer)", open((const char *)0xffff800000001000ull, AURORA_O_READ), AE_FAULT);
    expect("spawn(unmapped path)", spawn((const char *)0x7000, ""), AE_FAULT);
    expect("read(bad fd)", read(9, &st, sizeof st), AE_BADF);
    expect("unknown system call", __syscall(999, 0, 0, 0, 0, 0), AE_NOSYS);
    expect("open(/system for writing)", open("/system/about.txt", AURORA_O_WRITE), -5);
    expect("stat(valid buffer)", stat("/system/about.txt", &st), 0);
    printf("badptr: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
