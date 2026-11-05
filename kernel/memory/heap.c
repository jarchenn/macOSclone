/* Boundary-tag heap allocator (portable core).
 *
 * Each block starts with a 32-byte header; payloads are 16-byte aligned.
 * Blocks are kept in address order implicitly (size + prev_size) so neighbours
 * can be coalesced in O(1); free blocks are also on a doubly linked list.
 */
#include "memory/heap.h"
#include "lib.h"

#define MAGIC_USED 0xA110C8EDu
#define MAGIC_FREE 0xF4EEB10Cu
#define HDR 32u
#define MIN_BLOCK (HDR + 16u)
#define GROW_CHUNK (64u * 1024u)

struct heap_block {
    uint32_t magic;
    uint32_t reserved;
    uint64_t size;      /* whole block, header included */
    uint64_t prev_size; /* 0 for the first block */
    uint64_t requested; /* payload bytes requested by the caller */
    /* Free blocks only (overlays the payload): */
    struct heap_block *next;
    struct heap_block *prev;
};

_Static_assert(sizeof(struct heap_block) == HDR + 16, "heap header layout");

static void corrupt(struct heap *h, const char *what, const void *p)
{
    if (h->on_corruption)
        h->on_corruption(what, p);
}

static inline struct heap_block *next_phys(struct heap *h, struct heap_block *b)
{
    uintptr_t n = (uintptr_t)b + b->size;
    return n < h->end ? (struct heap_block *)n : NULL;
}

static inline struct heap_block *prev_phys(struct heap *h, struct heap_block *b)
{
    if (!b->prev_size)
        return NULL;
    return (struct heap_block *)((uintptr_t)b - b->prev_size);
}

static void list_remove(struct heap *h, struct heap_block *b)
{
    if (b->prev)
        b->prev->next = b->next;
    else
        h->free_list = b->next;
    if (b->next)
        b->next->prev = b->prev;
    b->next = b->prev = NULL;
}

static void list_push(struct heap *h, struct heap_block *b)
{
    b->magic = MAGIC_FREE;
    b->prev = NULL;
    b->next = h->free_list;
    if (h->free_list)
        h->free_list->prev = b;
    h->free_list = b;
}

void heap_init(struct heap *h, uintptr_t base, size_t limit_bytes, heap_grow_fn grow, void *ctx)
{
    void (*cb)(const char *, const void *) = h->on_corruption;
    memset(h, 0, sizeof *h);
    h->on_corruption = cb;
    h->base = h->end = ALIGN_UP(base, HEAP_ALIGN);
    h->limit = h->base + limit_bytes;
    h->grow = grow;
    h->grow_ctx = ctx;
    h->stats.limit_bytes = limit_bytes;
}

/* Last physical block (the one ending at h->end), or NULL if empty. */
static struct heap_block *last_block(struct heap *h)
{
    if (h->end == h->base)
        return NULL;
    struct heap_block *b = (struct heap_block *)h->base;
    for (;;) {
        struct heap_block *n = next_phys(h, b);
        if (!n)
            return b;
        b = n;
    }
}

static bool grow(struct heap *h, size_t need)
{
    size_t bytes = ALIGN_UP(MAX(need, (size_t)GROW_CHUNK), 4096);
    if (h->end + bytes > h->limit || h->end + bytes < h->end) {
        bytes = ALIGN_UP(need, 4096);
        if (h->end + bytes > h->limit || h->end + bytes < h->end)
            return false;
    }
    if (!h->grow || !h->grow(h->grow_ctx, h->end, bytes))
        return false;
    struct heap_block *last = last_block(h);
    struct heap_block *b = (struct heap_block *)h->end;
    b->size = bytes;
    b->prev_size = last ? last->size : 0;
    b->requested = 0;
    h->end += bytes;
    h->stats.mapped_bytes += bytes;
    if (last && last->magic == MAGIC_FREE) {
        list_remove(h, last);
        last->size += b->size;
        list_push(h, last);
    } else {
        list_push(h, b);
    }
    return true;
}

void *heap_alloc(struct heap *h, size_t n)
{
    if (n == 0)
        return NULL;
    if (n > HEAP_MAX_ALLOC) {
        h->stats.failed_allocs++;
        return NULL;
    }
    size_t need = ALIGN_UP(n, HEAP_ALIGN) + HDR;
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct heap_block *b = h->free_list; b; b = b->next) {
            if (b->magic != MAGIC_FREE) {
                corrupt(h, "free list entry has bad magic", b);
                return NULL;
            }
            if (b->size < need)
                continue;
            list_remove(h, b);
            if (b->size - need >= MIN_BLOCK) {
                struct heap_block *rest = (struct heap_block *)((uintptr_t)b + need);
                rest->size = b->size - need;
                rest->prev_size = need;
                rest->requested = 0;
                b->size = need;
                struct heap_block *after = next_phys(h, rest);
                if (after)
                    after->prev_size = rest->size;
                list_push(h, rest);
            }
            b->magic = MAGIC_USED;
            b->requested = n;
            h->stats.used_bytes += n;
            if (h->stats.used_bytes > h->stats.peak_used_bytes)
                h->stats.peak_used_bytes = h->stats.used_bytes;
            h->stats.alloc_count++;
            h->stats.total_allocs++;
            return (uint8_t *)b + HDR;
        }
        if (attempt == 0 && !grow(h, need))
            break;
    }
    h->stats.failed_allocs++;
    return NULL;
}

static struct heap_block *block_of(struct heap *h, const void *p)
{
    uintptr_t a = (uintptr_t)p;
    if (a < h->base + HDR || a >= h->end || (a & (HEAP_ALIGN - 1)))
        return NULL;
    return (struct heap_block *)(a - HDR);
}

void heap_free(struct heap *h, void *p)
{
    if (!p)
        return;
    struct heap_block *b = block_of(h, p);
    if (!b) {
        corrupt(h, "free of pointer outside the heap", p);
        return;
    }
    if (b->magic == MAGIC_FREE) {
        corrupt(h, "double free", p);
        return;
    }
    if (b->magic != MAGIC_USED) {
        corrupt(h, "free of invalid pointer (bad header)", p);
        return;
    }
    h->stats.used_bytes -= b->requested;
    h->stats.alloc_count--;
    h->stats.total_frees++;
    b->requested = 0;
    b->magic = MAGIC_FREE;
    b->next = b->prev = NULL;

    struct heap_block *n = next_phys(h, b);
    if (n && n->magic == MAGIC_FREE) {
        list_remove(h, n);
        b->size += n->size;
    }
    struct heap_block *pv = prev_phys(h, b);
    if (pv && pv->magic == MAGIC_FREE) {
        list_remove(h, pv);
        pv->size += b->size;
        b = pv;
    }
    struct heap_block *after = next_phys(h, b);
    if (after)
        after->prev_size = b->size;
    list_push(h, b);
}

size_t heap_block_size(struct heap *h, const void *p)
{
    struct heap_block *b = block_of(h, p);
    return (b && b->magic == MAGIC_USED) ? b->requested : 0;
}

bool heap_check(struct heap *h)
{
    uintptr_t a = h->base;
    uint64_t prev = 0;
    size_t used = 0, live = 0;
    while (a < h->end) {
        struct heap_block *b = (struct heap_block *)a;
        if ((b->magic != MAGIC_USED && b->magic != MAGIC_FREE) || b->size < MIN_BLOCK ||
            (b->size & (HEAP_ALIGN - 1)) || a + b->size > h->end || b->prev_size != prev) {
            corrupt(h, "heap_check: inconsistent block", b);
            return false;
        }
        if (b->magic == MAGIC_USED) {
            used += b->requested;
            live++;
        }
        prev = b->size;
        a += b->size;
    }
    if (used != h->stats.used_bytes || live != h->stats.alloc_count) {
        corrupt(h, "heap_check: statistics mismatch", NULL);
        return false;
    }
    return true;
}
