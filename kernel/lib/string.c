/* Bounded string helpers shared by kernel and host builds. */
#include "lib.h"

size_t str_copy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t c = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, c);
        dst[c] = '\0';
    }
    return n;
}

size_t str_append(char *dst, const char *src, size_t cap)
{
    size_t d = strlen(dst);
    if (d >= cap)
        return d + strlen(src);
    return d + str_copy(dst + d, src, cap - d);
}

bool str_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return false;
    }
    return true;
}
