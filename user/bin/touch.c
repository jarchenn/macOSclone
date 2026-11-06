/* touch file...: creates files that do not exist. */
#include <fcntl.h>
#include <unistd.h>
#include "fileutil.h"

int main(int argc, char **argv)
{
    int r = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0)
            r |= fail("touch", argv[i]);
        else
            close(fd);
    }
    return r;
}
