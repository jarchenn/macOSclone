/* sleep seconds (fractions allowed) */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 2;
    }
    double s = strtod(argv[1], NULL);
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };
    while (nanosleep(&ts, &ts) < 0)
        ;
    return 0;
}
