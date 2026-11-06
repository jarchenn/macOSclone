/* mkdir [-p] [-m mode] dir... */
#include <stdlib.h>
#include <sys/stat.h>
#include "fileutil.h"

static int make(const char *path, mode_t mode, int parents)
{
    if (!parents)
        return mkdir(path, mode) < 0 ? fail("mkdir", path) : 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0755) < 0 && errno != EEXIST)
                return fail("mkdir", buf);
            *p = '/';
        }
    }
    if (mkdir(buf, mode) < 0 && errno != EEXIST)
        return fail("mkdir", buf);
    return 0;
}

int main(int argc, char **argv)
{
    int parents = 0, i = 1, st = 0;
    mode_t mode = 0777;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-p"))
            parents = 1;
        else if (!strcmp(argv[i], "-m") && i + 1 < argc)
            mode = (mode_t)strtol(argv[++i], NULL, 8);
    }
    if (i >= argc) {
        fprintf(stderr, "usage: mkdir [-p] [-m mode] dir...\n");
        return 2;
    }
    for (; i < argc; i++)
        st |= make(argv[i], mode, parents);
    return st;
}
