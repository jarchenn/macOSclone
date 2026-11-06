/* cp source target  |  cp source... directory */
#include <fcntl.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fileutil.h"

static int copy(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return fail("cp", from);
    struct stat st;
    fstat(in, &st);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (out < 0) {
        close(in);
        return fail("cp", to);
    }
    char buf[4096];
    ssize_t n;
    int r = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, (size_t)n) != n) {
            r = fail("cp", to);
            break;
        }
    if (n < 0)
        r = fail("cp", from);
    close(in);
    close(out);
    return r;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: cp source target | cp source... dir\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    struct stat st;
    int is_dir = stat(dst, &st) == 0 && S_ISDIR(st.st_mode);
    int r = 0;
    for (int i = 1; i < argc - 1; i++) {
        char target[512], tmp[256];
        snprintf(tmp, sizeof tmp, "%s", argv[i]);
        if (is_dir)
            snprintf(target, sizeof target, "%s/%s", dst, basename(tmp));
        else
            snprintf(target, sizeof target, "%s", dst);
        r |= copy(argv[i], target);
    }
    return r;
}
