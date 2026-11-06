/* printf format [args...]: a subset of POSIX printf(1) - %s %d %i %u %x %o
 * %c %% and the escapes \n \r \t \b \a \e \\ \0NNN. */
#include <stdio.h>
#include <stdlib.h>

static void escape(const char **pp)
{
    const char *p = *pp;
    switch (*p) {
    case 'n': putchar('\n'); break;
    case 'r': putchar('\r'); break;
    case 't': putchar('\t'); break;
    case 'b': putchar('\b'); break;
    case 'a': putchar('\a'); break;
    case 'e': putchar(0x1b); break;
    case '\\': putchar('\\'); break;
    case '0': {
        int v = 0, k = 0;
        while (k < 3 && p[1] >= '0' && p[1] <= '7') {
            v = v * 8 + (*++p - '0');
            k++;
        }
        putchar(v);
        break;
    }
    default: putchar('\\'); putchar(*p); break;
    }
    *pp = p;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: printf format [args...]\n");
        return 2;
    }
    int a = 2;
    for (const char *p = argv[1]; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            escape(&p);
            continue;
        }
        if (*p != '%' || !p[1]) {
            putchar(*p);
            continue;
        }
        p++;
        const char *arg = a < argc ? argv[a] : "";
        switch (*p) {
        case 's': fputs(arg, stdout); a++; break;
        case 'd':
        case 'i': printf("%ld", strtol(arg, NULL, 0)); a++; break;
        case 'u': printf("%lu", strtoul(arg, NULL, 0)); a++; break;
        case 'x': printf("%lx", strtoul(arg, NULL, 0)); a++; break;
        case 'o': printf("%lo", strtoul(arg, NULL, 0)); a++; break;
        case 'c': putchar(arg[0]); a++; break;
        case '%': putchar('%'); break;
        default: putchar('%'); putchar(*p); break;
        }
    }
    return 0;
}
