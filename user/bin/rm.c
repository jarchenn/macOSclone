/* rm [-rf] path...   (also installed as rmdir semantics with -d) */
#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fileutil.h"

static int force, recursive;

static int remove_path(const char *p)
{
    struct stat st;
    if (stat(p, &st) < 0) {
        if (force && errno == ENOENT)
            return 0;
        return fail("rm", p);
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            errno = EISDIR;
            return fail("rm", p);
        }
        DIR *d = opendir(p);
        if (!d)
            return fail("rm", p);
        struct dirent *e;
        int r = 0;
        /* Collect names first: removing while iterating shifts indexes. */
        char **names = NULL;
        size_t n = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            names = realloc(names, (n + 1) * sizeof *names);
            names[n++] = strdup(e->d_name);
        }
        closedir(d);
        for (size_t i = 0; i < n; i++) {
            char child[512];
            snprintf(child, sizeof child, "%s/%s", p, names[i]);
            r |= remove_path(child);
            free(names[i]);
        }
        free(names);
        if (rmdir(p) < 0)
            return fail("rm", p);
        return r;
    }
    return unlink(p) < 0 ? fail("rm", p) : 0;
}

int main(int argc, char **argv)
{
    int i = 1, st = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'f') force = 1;
            else if (*o == 'r' || *o == 'R') recursive = 1;
        }
    if (i >= argc && !force) {
        fprintf(stderr, "usage: rm [-rf] path...\n");
        return 2;
    }
    for (; i < argc; i++)
        st |= remove_path(argv[i]);
    return st;
}
