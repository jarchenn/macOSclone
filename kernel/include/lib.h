#ifndef AURORA_LIB_H
#define AURORA_LIB_H

#include "types.h"

#ifdef AURORA_HOST
#include <string.h>
#include <stdlib.h>
#else
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
#endif

/* Aurora helpers available in both kernel and host builds. */
size_t str_copy(char *dst, const char *src, size_t cap);   /* like strlcpy */
size_t str_append(char *dst, const char *src, size_t cap); /* like strlcat */
bool str_starts_with(const char *s, const char *prefix);
void *memset32(uint32_t *dst, uint32_t v, size_t count);

int k_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int k_snprintf(char *buf, size_t cap, const char *fmt, ...) PRINTF_LIKE(3, 4);

uint32_t crc32_update(uint32_t crc, const void *data, size_t len);
static inline uint32_t crc32(const void *data, size_t len) { return crc32_update(0, data, len); }

#endif
