#ifndef AURORA_INFLATE_H
#define AURORA_INFLATE_H
#include "types.h"

/* DEFLATE (RFC 1951) decoder for the compressed initramfs (0.7). Portable
 * and host-tested. Decodes a zlib stream (RFC 1950: 2-byte header, deflate
 * data, Adler-32 trailer, which is verified). Returns 0 and the output
 * length, or a negative error. Never writes past `cap`. */
enum {
    INFLATE_OK = 0,
    INFLATE_ETRUNC = -1,   /* input ended early */
    INFLATE_EFULL = -2,    /* output larger than cap */
    INFLATE_EDATA = -3,    /* invalid block type, code lengths or distance */
    INFLATE_EHEADER = -4,  /* not a zlib stream (or a preset dictionary) */
    INFLATE_ECHECK = -5,   /* Adler-32 mismatch */
};

int zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap, size_t *out_len);
uint32_t adler32(const uint8_t *p, size_t n);

#endif
