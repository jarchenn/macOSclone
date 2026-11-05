#ifndef AURORA_INITRAMFS_H
#define AURORA_INITRAMFS_H
#include "types.h"

/* AURFS v1 archive parser (format documented in tools/make_initramfs.py).
 * Every size, offset and path is validated before the callback sees it. */
typedef int (*initramfs_cb)(void *ctx, uint32_t type, const char *path, const uint8_t *data, uint64_t len);

int initramfs_parse(const uint8_t *archive, size_t size, initramfs_cb cb, void *ctx);

#endif
