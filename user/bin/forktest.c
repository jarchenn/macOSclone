/* forktest [N]: N fork/exit cycles (default 1000). Each child touches some
 * memory (copy-on-write faults) and exits with i % 256; the parent checks
 * every status. The free page count is compared before and after: every
 * page of every child (data, stack, page tables, kernel stack) must come
 * back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <aurora.h>

static char data[16384] = "x";

static uint64_t free_pages(void)
{
    /* A dead task's kernel stack is freed by the next context switch on its
     * CPU; give every CPU a moment so the count is not caught mid-way. */
    usleep(100000);
    struct aurora_sysinfo si;
    aurora_sysinfo(&si);
    return si.free_pages;
}

static int cycle(int i)
{
    pid_t p = fork();
    if (p < 0) {
        perror("forktest: fork");
        return -1;
    }
    if (p == 0) {
        for (size_t k = 0; k < sizeof data; k += 4096)
            data[k] = (char)i; /* copy-on-write */
        volatile char stack[8192];
        stack[0] = (char)i;
        _exit(i & 0xff);
    }
    int st = 0;
    if (waitpid(p, &st, 0) != p || !WIFEXITED(st) || WEXITSTATUS(st) != (i & 0xff)) {
        printf("forktest: cycle %d: bad status %#x\n", i, st);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Warm up: stdio buffers, heap and page tables of the parent. */
    for (int i = 0; i < 3; i++)
        if (cycle(i))
            return 1;
    uint64_t before = free_pages();
    int bad = 0;
    for (int i = 0; i < n && !bad; i++)
        bad = cycle(i) != 0;
    uint64_t after = free_pages();
    printf("forktest: %d cycles, free pages before %llu after %llu (%s)\n", n, (unsigned long long)before,
           (unsigned long long)after, before == after ? "baseline restored" : "LEAK");
    return bad || before != after;
}
