/* wc [-lwc] [file...]: counts lines, words and bytes. */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static bool opt_l, opt_w, opt_c;

static void out(long l, long w, long c, const char *name)
{
    bool all = !opt_l && !opt_w && !opt_c;
    bool first = true;
    if (all || opt_l) {
        printf(first && !(all) ? "%ld" : "%7ld", l);
        first = false;
    }
    if (all || opt_w) {
        printf(first ? "%ld" : " %7ld", w);
        first = false;
    }
    if (all || opt_c) {
        printf(first ? "%ld" : " %7ld", c);
        first = false;
    }
    if (name)
        printf(" %s", name);
    printf("\n");
}

static void count(FILE *f, long *l, long *w, long *c)
{
    int ch;
    bool in_word = false;
    while ((ch = getc(f)) != EOF) {
        (*c)++;
        if (ch == '\n')
            (*l)++;
        if (isspace(ch)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            (*w)++;
        }
    }
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'l') opt_l = true;
            else if (*o == 'w') opt_w = true;
            else if (*o == 'c') opt_c = true;
            else {
                fprintf(stderr, "wc: unknown option -%c\n", *o);
                return 2;
            }
        }
    long tl = 0, tw = 0, tc = 0;
    int status = 0, files = argc - i;
    if (files == 0) {
        count(stdin, &tl, &tw, &tc);
        out(tl, tw, tc, NULL);
        return 0;
    }
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        long l = 0, w = 0, c = 0;
        count(f, &l, &w, &c);
        fclose(f);
        out(l, w, c, argv[i]);
        tl += l;
        tw += w;
        tc += c;
    }
    if (files > 1)
        out(tl, tw, tc, "total");
    return status;
}
