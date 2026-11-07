/* memtest write <secret> [ms] | read | peek <hex-address>
 * Two instances of this program use the same virtual address for `shared`,
 * but each has its own physical page: a reader never sees a writer's data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char shared[64] = "untouched";

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "read";
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(mode, "write") && argc > 2) {
        snprintf(shared, sizeof shared, "%s", argv[2]);
        long ms = argc > 3 ? atol(argv[3]) : 3000;
        printf("memtest: pid %d wrote %s at %p\n", getpid(), shared, (void *)shared);
        usleep((useconds_t)(ms * 1000));
        printf("memtest: pid %d still sees %s\n", getpid(), shared);
        return strcmp(shared, argv[2]) == 0 ? 0 : 1;
    }
    if (!strcmp(mode, "peek") && argc > 2) {
        unsigned long a = strtoul(argv[2], NULL, 16);
        printf("memtest: reading %p\n", (void *)a);
        printf("memtest: value %x\n", *(volatile unsigned *)a);
        return 0;
    }
    printf("memtest: pid %d reads %s at %p\n", getpid(), shared, (void *)shared);
    return 0;
}
