/* counter [count] [interval_ms] [label]: prints a numbered line every
 * interval, sleeping in between (other processes run meanwhile). */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 5;
    long ms = argc > 2 ? atol(argv[2]) : 200;
    const char *label = argc > 3 ? argv[3] : "count";
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (long i = 1; i <= n; i++) {
        printf("%s %ld\n", label, i);
        usleep((useconds_t)(ms * 1000));
    }
    return 0;
}
