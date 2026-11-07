/* tee [-a] file...: copies standard input to standard output and files. */
#include <fcntl.h>
#include <unistd.h>
#include "fileutil.h"

int main(int argc, char **argv)
{
    int i = 1, append = 0, fds[16], n = 0, r = 0;
    if (i < argc && !strcmp(argv[i], "-a")) {
        append = 1;
        i++;
    }
    for (; i < argc && n < 16; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0)
            r |= fail("tee", argv[i]);
        else
            fds[n++] = fd;
    }
    char buf[4096];
    ssize_t got;
    while ((got = read(0, buf, sizeof buf)) > 0) {
        write(1, buf, (size_t)got);
        for (int k = 0; k < n; k++)
            write(fds[k], buf, (size_t)got);
    }
    for (int k = 0; k < n; k++)
        close(fds[k]);
    return r;
}
