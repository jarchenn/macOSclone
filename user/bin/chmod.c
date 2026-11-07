/* chmod octal-mode file... */
#include <stdlib.h>
#include <sys/stat.h>
#include "fileutil.h"

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: chmod octal-mode file...\n");
        return 2;
    }
    char *end;
    long m = strtol(argv[1], &end, 8);
    if (*end || m < 0 || m > 07777) {
        fprintf(stderr, "chmod: invalid mode %s (octal only)\n", argv[1]);
        return 2;
    }
    int r = 0;
    for (int i = 2; i < argc; i++)
        if (chmod(argv[i], (mode_t)m) < 0)
            r |= fail("chmod", argv[i]);
    return r;
}
