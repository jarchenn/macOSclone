/* Heap allocator core and bitmap page allocator in simulated memory. */
#include "harness.h"
#include "memory/heap.h"
#include "memory/physical.h"
#include <stdlib.h>

const char *t_suite = "memory";

/* ---- heap ------------------------------------------------------------------- */
#define ARENA (4u << 20)
static uint8_t *arena;
static int corruptions;
static const char *last_corruption;

static bool grow(void *ctx, uintptr_t end, size_t bytes)
{
    (void)ctx;
    return end + bytes <= (uintptr_t)arena + ARENA;
}

static void on_corrupt(const char *what, const void *p)
{
    (void)p;
    corruptions++;
    last_corruption = what;
}

static void new_heap(struct heap *h, size_t limit)
{
    static uint8_t *raw;
    if (!raw) {
        raw = malloc(ARENA + 4096);
        arena = (uint8_t *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
    }
    corruptions = 0;
    memset(h, 0, sizeof *h);
    h->on_corruption = on_corrupt;
    heap_init(h, (uintptr_t)arena, limit, grow, NULL);
}

TEST(heap_alloc_free_alignment)
{
    struct heap h;
    new_heap(&h, ARENA);
    CHECK(heap_alloc(&h, 0) == NULL);
    void *p[64];
    for (int i = 0; i < 64; i++) {
        p[i] = heap_alloc(&h, (size_t)(i * 37 + 1));
        CHECK(p[i] != NULL);
        CHECK(((uintptr_t)p[i] & 15) == 0);
        memset(p[i], i, (size_t)(i * 37 + 1));
    }
    CHECK_EQ(h.stats.alloc_count, 64);
    CHECK(heap_check(&h));
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < i * 37 + 1; k++)
            if (((uint8_t *)p[i])[k] != (uint8_t)i) {
                CHECK(false);
                break;
            }
    for (int i = 0; i < 64; i += 2)
        heap_free(&h, p[i]);
    CHECK(heap_check(&h));
    for (int i = 1; i < 64; i += 2)
        heap_free(&h, p[i]);
    CHECK_EQ(h.stats.alloc_count, 0);
    CHECK_EQ(h.stats.used_bytes, 0);
    CHECK(heap_check(&h));
    CHECK_EQ(corruptions, 0);
}

TEST(heap_coalescing_reuses_space)
{
    struct heap h;
    new_heap(&h, ARENA);
    void *a = heap_alloc(&h, 1000), *b = heap_alloc(&h, 1000), *c = heap_alloc(&h, 1000);
    size_t mapped = h.stats.mapped_bytes;
    heap_free(&h, a);
    heap_free(&h, c);
    heap_free(&h, b); /* merges with both neighbours */
    void *big = heap_alloc(&h, 3000);
    CHECK(big == a); /* the coalesced block is reused from the start */
    CHECK_EQ(h.stats.mapped_bytes, mapped);
    heap_free(&h, big);
    /* repeated alloc/free cycles do not grow the heap without bound */
    for (int round = 0; round < 1000; round++) {
        void *x = heap_alloc(&h, 50000);
        void *y = heap_alloc(&h, 123);
        heap_free(&h, x);
        heap_free(&h, y);
    }
    CHECK(h.stats.mapped_bytes <= mapped + 128 * 1024);
    CHECK(heap_check(&h));
}

TEST(heap_misuse_detected)
{
    struct heap h;
    new_heap(&h, ARENA);
    void *a = heap_alloc(&h, 64);
    heap_free(&h, a);
    heap_free(&h, a);
    CHECK_EQ(corruptions, 1);
    CHECK_STR(last_corruption, "double free");
    int local;
    heap_free(&h, &local);
    CHECK_EQ(corruptions, 2);
    void *b = heap_alloc(&h, 64);
    heap_free(&h, (uint8_t *)b + 16); /* interior pointer */
    CHECK_EQ(corruptions, 3);
    heap_free(&h, NULL); /* allowed */
    CHECK_EQ(corruptions, 3);
}

TEST(heap_limits)
{
    struct heap h;
    new_heap(&h, 256 * 1024);
    CHECK(heap_alloc(&h, (size_t)HEAP_MAX_ALLOC + 1) == NULL);
    CHECK(heap_alloc(&h, SIZE_MAX) == NULL);           /* no overflow in rounding */
    CHECK(heap_alloc(&h, SIZE_MAX - 8) == NULL);
    void *a = heap_alloc(&h, 200 * 1024);
    CHECK(a != NULL);
    CHECK(heap_alloc(&h, 100 * 1024) == NULL);         /* limit reached */
    CHECK(h.stats.failed_allocs >= 4);
    heap_free(&h, a);
    CHECK(heap_alloc(&h, 100 * 1024) != NULL);         /* recovered after free */
    CHECK(heap_check(&h));
}

/* ---- physical allocator ----------------------------------------------------- */

TEST(pmm_alloc_free)
{
    struct boot_mem_region map[] = {
        { 0x0, 0x9F000, BOOT_MEM_USABLE },
        { 0x9F000, 0x61000, BOOT_MEM_RESERVED },
        { 0x100000, 0x300000, BOOT_MEM_USABLE },
        { 0x400000, 0x100000, BOOT_MEM_BOOTLOADER_RECLAIMABLE },
        { 0x500000, 0x80000, BOOT_MEM_KERNEL_AND_MODULES },
        { 0x580800, 0x1000, BOOT_MEM_USABLE }, /* unaligned: only whole pages used */
    };
    size_t n = sizeof map / sizeof map[0];
    size_t bytes = pmm_bitmap_bytes_needed(map, n);
    CHECK(bytes >= (0x581800 / 4096 + 7) / 8);
    phys_addr_t home = pmm_find_bitmap_home(map, n, bytes);
    CHECK(home != 0);
    uint64_t *bm = malloc(bytes);
    struct pmm p;
    pmm_core_init(&p, map, n, bm, home, bytes);
    uint64_t usable_pages = 0x9F + 0x300 - 1 /* page 0 */ - bytes / 4096;
    CHECK_EQ(p.free_pages, usable_pages);
    CHECK(pmm_core_is_used(&p, 0));
    CHECK(pmm_core_is_used(&p, home));
    CHECK(pmm_core_is_used(&p, 0x9F000));          /* reserved */
    CHECK(pmm_core_is_used(&p, 0x400000));         /* reclaimable until released */
    CHECK(pmm_core_is_used(&p, 0x580000));         /* partial page never usable */
    /* allocate everything, every page unique and in a usable region */
    uint64_t count = 0;
    phys_addr_t a;
    static uint8_t seen[0x600];
    memset(seen, 0, sizeof seen);
    while ((a = pmm_core_alloc(&p)) != 0) {
        CHECK((a & 0xFFF) == 0);
        CHECK(!seen[a / 4096]);
        seen[a / 4096] = 1;
        CHECK((a < 0x9F000) || (a >= 0x100000 && a < 0x400000));
        count++;
    }
    CHECK_EQ(count, usable_pages);
    CHECK_EQ(p.free_pages, 0);
    CHECK_EQ(pmm_core_free(&p, 0x1000 * 5 + 1), PMM_INVALID);  /* unaligned */
    CHECK_EQ(pmm_core_free(&p, 0), PMM_INVALID);               /* page 0 */
    CHECK_EQ(pmm_core_free(&p, 0x10000000), PMM_INVALID);      /* beyond bitmap */
    CHECK_EQ(pmm_core_free(&p, home), PMM_INVALID);            /* bitmap itself */
    CHECK_EQ(pmm_core_free(&p, 0x200000), PMM_OK);
    CHECK_EQ(pmm_core_free(&p, 0x200000), PMM_DOUBLE_FREE);
    CHECK_EQ(p.double_frees, 1);
    CHECK_EQ(pmm_core_alloc(&p), 0x200000);
    /* release bootloader-reclaimable memory */
    uint64_t gained = pmm_core_release_type(&p, map, n, BOOT_MEM_BOOTLOADER_RECLAIMABLE);
    CHECK_EQ(gained, 0x100);
    CHECK(!pmm_core_is_used(&p, 0x400000));
    phys_addr_t run = pmm_core_alloc_contig(&p, 16);
    CHECK(run >= 0x400000 && run + 16 * 4096 <= 0x500000);
    CHECK_EQ(pmm_core_alloc_contig(&p, 0), 0);
    CHECK_EQ(pmm_core_alloc_contig(&p, 1000), 0);
    free(bm);
}

void run_tests(void)
{
    RUN(heap_alloc_free_alignment);
    RUN(heap_coalescing_reuses_space);
    RUN(heap_misuse_detected);
    RUN(heap_limits);
    RUN(pmm_alloc_free);
}
