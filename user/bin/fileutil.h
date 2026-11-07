/* Shared helpers for the small file utilities (header-only). */
#ifndef AURORA_FILEUTIL_H
#define AURORA_FILEUTIL_H
#include <errno.h>
#include <stdio.h>
#include <string.h>

static inline int fail(const char *prog, const char *what)
{
    fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(errno));
    return 1;
}

#endif
