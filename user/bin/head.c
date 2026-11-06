/* head [-n count] [file...] */
#include <stdlib.h>
#include "fileutil.h"

static void head(FILE *f, long n)
{
    int c;
    while (n > 0 && (c = getc(f)) != EOF) {
        putchar(c);
        if (c == '\n')
            n--;
    }
}

int main(int argc, char **argv)
{
    long n = 10;
    int i = 1, r = 0;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        n = atol(argv[i + 1]);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
        n = atol(argv[i] + 1);
        i++;
    }
    if (i >= argc) {
        head(stdin, n);
        return 0;
    }
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            r |= fail("head", argv[i]);
            continue;
        }
        head(f, n);
        fclose(f);
    }
    return r;
}
