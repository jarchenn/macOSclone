/* Helpers the shared kernel drawing code expects (kernel/lib/memory.c is
 * not built for user space: newlib provides memcpy and friends). */
#include "lib.h"

void *memset32(uint32_t *dst, uint32_t v, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = v;
    return dst;
}
