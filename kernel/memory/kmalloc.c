/* Kernel global heap: the portable allocator core backed by on-demand page
 * mappings in the VMM heap region. Main-loop (non-IRQ) context only. */
#include "memory/heap.h"
#include "memory/physical.h"
#include "memory/virtual.h"
#include "core/log.h"
#include "core/panic.h"
#include "lib.h"

static struct heap kheap;
static bool ready;

static bool kheap_grow(void *ctx, uintptr_t end, size_t bytes)
{
    UNUSED(ctx);
    return vmm_map_fresh(end, bytes);
}

static void kheap_corruption(const char *what, const void *p)
{
    panic("heap: %s (%p)", what, p);
}

void kheap_init(void)
{
    const struct pmm *p = pmm_state();
    uint64_t usable = p->usable_pages * PAGE_SIZE;
    uint64_t limit = usable / 3;
    if (limit < 8ull << 20)
        limit = 8ull << 20;
    if (limit > 96ull << 20)
        limit = 96ull << 20;
    kheap.on_corruption = kheap_corruption;
    heap_init(&kheap, VMM_HEAP_BASE, (size_t)limit, kheap_grow, NULL);
    ready = true;
    klog("heap: base %llx, limit %llu MiB", (unsigned long long)VMM_HEAP_BASE, (unsigned long long)(limit >> 20));
}

void *kmalloc(size_t n)
{
    if (!ready)
        panic("kmalloc before kheap_init");
    return heap_alloc(&kheap, n);
}

void *kzalloc(size_t n)
{
    void *p = kmalloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void *kcalloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size)
        return NULL;
    return kzalloc(count * size);
}

void kfree(void *p) { heap_free(&kheap, p); }
void kheap_stats(struct heap_stats *out) { *out = kheap.stats; }
bool kheap_check(void) { return heap_check(&kheap); }
