#ifndef AURORA_KALLOC_H
#define AURORA_KALLOC_H
/* Allocation interface used by portable modules (VFS, surfaces, apps).
 * Kernel builds use the kernel heap; host test builds use libc. */
#ifdef AURORA_HOST
#include <stdlib.h>
#define kmalloc(n) malloc(n)
#define kzalloc(n) calloc(1, (n))
#define kcalloc(c, s) calloc((c), (s))
#define kfree(p) free(p)
#else
#include "memory/heap.h"
#endif
#endif
