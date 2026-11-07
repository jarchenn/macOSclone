/* cat [file...]: concatenates files (or standard input, also for "-") to
 * standard output. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int copy(int fd, const char *name)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "cat: write: %s\n", strerror(errno));
                return 1;
            }
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int status = 0;
    if (argc < 2)
        return copy(0, "stdin");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-")) {
            status |= copy(0, "stdin");
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        status |= copy(fd, argv[i]);
        close(fd);
    }
    return status;
}
