#ifndef AURORA_TYPES_H
#define AURORA_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* Physical addresses are plain integers and must never be dereferenced
 * directly; convert with phys_to_virt() (HHDM) first. */
typedef uint64_t phys_addr_t;
typedef uint64_t virt_addr_t;

#define PAGE_SIZE 4096ull
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define UNUSED(x) ((void)(x))
#define NORETURN __attribute__((noreturn))
#define PACKED __attribute__((packed))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))

#define compiler_barrier() __asm__ volatile("" ::: "memory")

#endif
