/* cat <file>...: copies files to the console through open/read/write. */
#include "aurora.h"

int main(int argc, char **argv)
{
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], AURORA_O_READ);
        if (fd < 0) {
            printf("cat: %s: %s\n", argv[i], strerror(fd));
            status = 1;
            continue;
        }
        char buf[512];
        long n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            write(1, buf, (size_t)n);
        close(fd);
    }
    return status;
}
