/* mv source target  |  mv source... directory  (rename(2): atomic) */
#include <libgen.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "fileutil.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mv source target | mv source... dir\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    struct stat st;
    int is_dir = stat(dst, &st) == 0 && S_ISDIR(st.st_mode);
    if (argc > 3 && !is_dir) {
        errno = ENOTDIR;
        return fail("mv", dst);
    }
    int r = 0;
    for (int i = 1; i < argc - 1; i++) {
        char target[512];
        if (is_dir) {
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%s", argv[i]);
            snprintf(target, sizeof target, "%s/%s", dst, basename(tmp));
        } else {
            snprintf(target, sizeof target, "%s", dst);
        }
        if (rename(argv[i], target) < 0)
            r |= fail("mv", argv[i]);
    }
    return r;
}
