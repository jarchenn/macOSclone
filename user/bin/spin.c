/* spin [seconds]: burns CPU without ever blocking. The scheduler preempts
 * it, so the desktop and other programs keep running. 0 = forever. */
#include "aurora.h"

int main(int argc, char **argv)
{
    long secs = argc > 1 ? atol(argv[1]) : 0;
    uint64_t start = uptime_ms(), next = start + 1000;
    volatile uint64_t x = 0;
    for (;;) {
        x++;
        if ((x & 0xFFFF) == 0) {
            uint64_t now = uptime_ms();
            if (now >= next) {
                printf("spin: %llu M iterations after %llu ms\n", (unsigned long long)(x >> 20),
                       (unsigned long long)(now - start));
                next += 1000;
                if (secs && now - start >= (uint64_t)secs * 1000)
                    return 0;
            }
        }
    }
}
