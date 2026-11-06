/* rmdir dir... */
#include <unistd.h>
#include "fileutil.h"

int main(int argc, char **argv)
{
    int st = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: rmdir dir...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
        if (rmdir(argv[i]) < 0)
            st = fail("rmdir", argv[i]);
    return st;
}
