/* env [NAME=value...] [command [args...]]: prints or modifies the
 * environment for a command. */
#include <stdlib.h>
#include <unistd.h>
#include "fileutil.h"

extern char **environ;

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && strchr(argv[i], '='); i++)
        putenv(argv[i]);
    if (i >= argc) {
        for (char **e = environ; *e; e++)
            printf("%s\n", *e);
        return 0;
    }
    execvp(argv[i], argv + i);
    fail("env", argv[i]);
    return errno == ENOENT ? 127 : 126;
}
