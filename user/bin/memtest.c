/* memtest write <secret> [ms] | read | peek <hex-address>
 * Two instances of this program use the same virtual address for `shared`,
 * but each has its own physical page: a reader never sees a writer's data. */
#include "aurora.h"

static char shared[64] = "untouched";

static uint64_t parse_hex(const char *s)
{
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    for (; *s; s++) {
        char c = *s;
        int d = -1;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        if (d < 0)
            break;
        v = v * 16 + (uint64_t)d;
    }
    return v;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "read";
    if (!strcmp(mode, "write") && argc > 2) {
        snprintf(shared, sizeof shared, "%s", argv[2]);
        long ms = argc > 3 ? atol(argv[3]) : 3000;
        printf("memtest: pid %d wrote %s at %p\n", getpid(), shared, (void *)shared);
        sleep_ms((uint64_t)ms);
        printf("memtest: pid %d still sees %s\n", getpid(), shared);
        return strcmp(shared, argv[2]) == 0 ? 0 : 1;
    }
    if (!strcmp(mode, "peek") && argc > 2) {
        uint64_t a = parse_hex(argv[2]);
        printf("memtest: reading %p\n", (void *)a);
        printf("memtest: value %x\n", *(volatile unsigned *)a);
        return 0;
    }
    printf("memtest: pid %d reads %s at %p\n", getpid(), shared, (void *)shared);
    return 0;
}
