/* grep [-ivcnqEF] pattern [file...]: prints matching lines (POSIX regular
 * expressions through newlib's regcomp; -F matches a fixed string). */
#include <errno.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool opt_i, opt_v, opt_c, opt_n, opt_q, opt_E, opt_F;
static regex_t re;
static const char *fixed;

static bool matches(const char *line)
{
    bool m;
    if (opt_F) {
        if (opt_i) {
            size_t n = strlen(fixed);
            m = false;
            for (const char *p = line; *p && !m; p++)
                m = strncasecmp(p, fixed, n) == 0;
        } else {
            m = strstr(line, fixed) != NULL;
        }
    } else {
        m = regexec(&re, line, 0, NULL, 0) == 0;
    }
    return m != opt_v;
}

static long scan(FILE *f, const char *name, bool show_name)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    long count = 0, lineno = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        if (len && line[len - 1] == '\n')
            line[len - 1] = 0;
        if (!matches(line))
            continue;
        count++;
        if (opt_q)
            break;
        if (opt_c)
            continue;
        if (show_name)
            printf("%s:", name);
        if (opt_n)
            printf("%ld:", lineno);
        printf("%s\n", line);
    }
    free(line);
    if (opt_c && !opt_q) {
        if (show_name)
            printf("%s:", name);
        printf("%ld\n", count);
    }
    return count;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) {
            i++;
            break;
        }
        for (const char *o = argv[i] + 1; *o; o++) {
            switch (*o) {
            case 'i': opt_i = true; break;
            case 'v': opt_v = true; break;
            case 'c': opt_c = true; break;
            case 'n': opt_n = true; break;
            case 'q': opt_q = true; break;
            case 'E': opt_E = true; break;
            case 'F': opt_F = true; break;
            default:
                fprintf(stderr, "grep: unknown option -%c\n", *o);
                return 2;
            }
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: grep [-ivcnqEF] pattern [file...]\n");
        return 2;
    }
    const char *pat = argv[i++];
    if (opt_F) {
        fixed = pat;
    } else {
        int fl = REG_NOSUB | (opt_E ? REG_EXTENDED : 0) | (opt_i ? REG_ICASE : 0);
        int r = regcomp(&re, pat, fl);
        if (r) {
            char msg[128];
            regerror(r, &re, msg, sizeof msg);
            fprintf(stderr, "grep: %s\n", msg);
            return 2;
        }
    }
    long total = 0;
    int status = 0;
    if (i >= argc) {
        total = scan(stdin, "(standard input)", false);
    } else {
        bool many = argc - i > 1;
        for (; i < argc; i++) {
            FILE *f = !strcmp(argv[i], "-") ? stdin : fopen(argv[i], "r");
            if (!f) {
                fprintf(stderr, "grep: %s: %s\n", argv[i], strerror(errno));
                status = 2;
                continue;
            }
            total += scan(f, argv[i], many);
            if (f != stdin)
                fclose(f);
        }
    }
    if (status)
        return status;
    return total ? 0 : 1;
}
