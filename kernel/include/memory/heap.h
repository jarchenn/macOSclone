#ifndef AURORA_HEAP_H
#define AURORA_HEAP_H
#include "types.h"

/* ---- Portable allocator core (also compiled into host tests) ----------- */

/* grow(ctx, bytes) must make [end, end + bytes) usable and return true, or
 * return false if the heap may not grow further. */
typedef bool (*heap_grow_fn)(void *ctx, uintptr_t end, size_t bytes);

struct heap_stats {
    size_t mapped_bytes;  /* bytes made available by grow() */
    size_t used_bytes;    /* payload bytes currently allocated */
    size_t peak_used_bytes;
    size_t alloc_count;   /* live allocations */
    size_t total_allocs;
    size_t total_frees;
    size_t failed_allocs;
    size_t limit_bytes;
};

struct heap {
    uintptr_t base, end, limit; /* [base, end) is usable, end <= limit */
    struct heap_block *free_list;
    heap_grow_fn grow;
    void *grow_ctx;
    struct heap_stats stats;
    void (*on_corruption)(const char *what, const void *ptr);
};

#define HEAP_ALIGN 16
#define HEAP_MAX_ALLOC (64u * 1024 * 1024)

void heap_init(struct heap *h, uintptr_t base, size_t limit_bytes, heap_grow_fn grow, void *ctx);
void *heap_alloc(struct heap *h, size_t n);
void heap_free(struct heap *h, void *p);
size_t heap_block_size(struct heap *h, const void *p);
/* Walks every block and verifies headers; returns false on corruption. */
bool heap_check(struct heap *h);

/* ---- Kernel global heap ------------------------------------------------- */
#ifndef AURORA_HOST
void kheap_init(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *kcalloc(size_t count, size_t size);
void kfree(void *p);
void kheap_stats(struct heap_stats *out);
bool kheap_check(void);
#endif

#endif
