/* User address spaces: regions, demand paging, copy-on-write (memory/uvm.h). */
#include "memory/uvm.h"
#include "memory/physical.h"
#include "memory/virtual.h"
#include "memory/tlb.h"
#include "arch/paging.h"
#include "arch/x86_64/io.h"
#include "abi/aurora_abi.h"
#include "core/panic.h"
#include "lib.h"

#define USER_TOP 0x0000800000000000ull
#define BATCH 64

static inline unsigned idx(uint64_t va, int level) { return (unsigned)((va >> (12 + 9 * level)) & 511); }
static inline uint64_t *tbl(uint64_t e) { return (uint64_t *)phys_to_virt(e & PTE_ADDR); }

/* Leaf PTE slot for va, optionally creating the intermediate tables. */
static uint64_t *pte_slot(uint64_t *pml4, uint64_t va, bool create)
{
    uint64_t *t = pml4;
    for (int level = 3; level >= 1; level--) {
        uint64_t *e = &t[idx(va, level)];
        if (!(*e & PTE_P)) {
            if (!create)
                return NULL;
            phys_addr_t p = pmm_alloc_page();
            if (!p)
                return NULL;
            *e = p | PTE_P | PTE_W | PTE_U;
        } else if (*e & PTE_PS) {
            return NULL; /* never in user space */
        }
        t = tbl(*e);
    }
    return &t[idx(va, 0)];
}

static uint64_t leaf_flags(uint8_t prot, bool writable)
{
    uint64_t f = PTE_P;
    if (prot & (VMA_R | VMA_W | VMA_X))
        f |= PTE_U;
    if ((prot & VMA_W) && writable)
        f |= PTE_W;
    if (!(prot & VMA_X) && g_vmm.nx)
        f |= PTE_NX;
    return f;
}

static bool is_loaded(const struct mm *m) { return read_cr3() == m->pml4_phys; }

/* Invalidates a range on this CPU (if loaded) and on every other CPU that
 * has the space loaded (threads of the process). */
static void flush_range(struct mm *m, uint64_t va, uint64_t pages)
{
    if (is_loaded(m)) {
        if (pages > 32)
            write_cr3(m->pml4_phys);
        else
            for (uint64_t i = 0; i < pages; i++)
                invlpg(va + i * PAGE_SIZE);
    }
    tlb_shootdown(va, pages, m->pml4_phys);
}

int mm_create(struct mm *m)
{
    memset(m, 0, sizeof *m);
    m->pml4 = vmm_space_create(&m->pml4_phys);
    return m->pml4 ? 0 : -1;
}

void mm_destroy(struct mm *m)
{
    if (!m->pml4)
        return;
    vmm_space_destroy(m->pml4, m->pml4_phys); /* releases shared pages by count */
    m->pml4 = NULL;
    m->pml4_phys = 0;
    m->nvma = 0;
}

struct vma *mm_find(struct mm *m, uint64_t addr)
{
    for (int i = 0; i < m->nvma; i++)
        if (addr >= m->vma[i].start && addr < m->vma[i].end)
            return &m->vma[i];
    return NULL;
}

/* Clears every present PTE in [start, end) and releases the pages, after
 * the TLBs no longer hold them. Skips absent tables quickly. */
static void release_range(struct mm *m, uint64_t start, uint64_t end)
{
    phys_addr_t batch[BATCH];
    int n = 0;
    uint64_t bstart = start, va = start;
    while (va < end) {
        uint64_t *t = m->pml4;
        bool hole = false;
        for (int level = 3; level >= 1; level--) {
            uint64_t e = t[idx(va, level)];
            if (!(e & PTE_P)) {
                uint64_t span = 1ull << (12 + 9 * level);
                va = ALIGN_DOWN(va, span) + span;
                hole = true;
                break;
            }
            t = tbl(e);
        }
        if (hole)
            continue;
        uint64_t *pte = &t[idx(va, 0)];
        if (*pte & PTE_P) {
            batch[n++] = *pte & PTE_ADDR;
            *pte = 0;
        }
        va += PAGE_SIZE;
        if (n == BATCH || va >= end) {
            if (n) {
                uint64_t stop = MIN(va, end);
                flush_range(m, bstart, (stop - bstart) / PAGE_SIZE);
                for (int i = 0; i < n; i++)
                    pmm_page_release(batch[i]);
                n = 0;
            }
            bstart = va;
        }
    }
}

static int vma_insert(struct mm *m, struct vma v)
{
    if (m->nvma >= UVM_MAX_VMAS)
        return -1;
    int i = m->nvma;
    while (i > 0 && m->vma[i - 1].start > v.start) {
        m->vma[i] = m->vma[i - 1];
        i--;
    }
    m->vma[i] = v;
    m->nvma++;
    return 0;
}

static void vma_remove_at(struct mm *m, int i)
{
    for (int k = i; k + 1 < m->nvma; k++)
        m->vma[k] = m->vma[k + 1];
    m->nvma--;
}

/* Splits regions so that `at` is a region boundary. */
static int split_at(struct mm *m, uint64_t at)
{
    for (int i = 0; i < m->nvma; i++) {
        struct vma *v = &m->vma[i];
        if (at > v->start && at < v->end) {
            struct vma hi = *v;
            hi.start = at;
            v->end = at;
            return vma_insert(m, hi);
        }
    }
    return 0;
}

int mm_unmap(struct mm *m, uint64_t start, uint64_t len)
{
    if (len == 0)
        return 0;
    uint64_t end = start + ALIGN_UP(len, PAGE_SIZE);
    if ((start & (PAGE_SIZE - 1)) || end > USER_TOP || end <= start)
        return -1;
    if (split_at(m, start) || split_at(m, end))
        return -2;
    for (int i = 0; i < m->nvma;) {
        if (m->vma[i].start >= start && m->vma[i].end <= end)
            vma_remove_at(m, i);
        else
            i++;
    }
    release_range(m, start, end);
    return 0;
}

int mm_map(struct mm *m, uint64_t start, uint64_t len, uint8_t prot, uint8_t kind)
{
    uint64_t end = start + ALIGN_UP(len, PAGE_SIZE);
    if ((start & (PAGE_SIZE - 1)) || start < PAGE_SIZE || end > USER_TOP || end <= start)
        return -1;
    int r = mm_unmap(m, start, end - start);
    if (r)
        return r;
    struct vma v = { start, end, prot, kind };
    return vma_insert(m, v) ? -2 : 0;
}

int mm_protect(struct mm *m, uint64_t start, uint64_t len, uint8_t prot)
{
    uint64_t end = start + ALIGN_UP(len, PAGE_SIZE);
    if ((start & (PAGE_SIZE - 1)) || end > USER_TOP || end <= start)
        return -1;
    /* The whole range must be mapped. */
    for (uint64_t va = start; va < end;) {
        struct vma *v = mm_find(m, va);
        if (!v)
            return -1;
        va = v->end;
    }
    if (split_at(m, start) || split_at(m, end))
        return -2;
    for (int i = 0; i < m->nvma; i++)
        if (m->vma[i].start >= start && m->vma[i].end <= end)
            m->vma[i].prot = prot;
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *pte = pte_slot(m->pml4, va, false);
        if (!pte || !(*pte & PTE_P))
            continue;
        phys_addr_t pa = *pte & PTE_ADDR;
        *pte = pa | leaf_flags(prot, pmm_page_owners(pa) == 1);
    }
    flush_range(m, start, (end - start) / PAGE_SIZE);
    return 0;
}

uint64_t mm_find_gap(struct mm *m, uint64_t hint, uint64_t len)
{
    len = ALIGN_UP(len, PAGE_SIZE);
    uint64_t cand = ALIGN_UP(MAX(hint, AURORA_MMAP_BASE), PAGE_SIZE);
    for (int i = 0; i < m->nvma; i++) {
        const struct vma *v = &m->vma[i];
        if (v->end <= cand)
            continue;
        if (cand + len <= v->start)
            return cand;
        cand = v->end;
    }
    return cand + len <= AURORA_USER_LIMIT ? cand : 0;
}

int mm_clone(struct mm *dst, struct mm *src)
{
    if (mm_create(dst))
        return -1;
    memcpy(dst->vma, src->vma, sizeof src->vma);
    dst->nvma = src->nvma;
    dst->brk_base = src->brk_base;
    dst->brk = src->brk;
    for (unsigned i3 = 0; i3 < 256; i3++) {
        uint64_t e3 = src->pml4[i3];
        if (!(e3 & PTE_P))
            continue;
        for (unsigned i2 = 0; i2 < 512; i2++) {
            uint64_t e2 = tbl(e3)[i2];
            if (!(e2 & PTE_P))
                continue;
            for (unsigned i1 = 0; i1 < 512; i1++) {
                uint64_t e1 = tbl(e2)[i1];
                if (!(e1 & PTE_P))
                    continue;
                uint64_t *spt = tbl(e1);
                for (unsigned i0 = 0; i0 < 512; i0++) {
                    if (!(spt[i0] & PTE_P))
                        continue;
                    uint64_t va = ((uint64_t)i3 << 39) | ((uint64_t)i2 << 30) | ((uint64_t)i1 << 21) | ((uint64_t)i0 << 12);
                    uint64_t *d = pte_slot(dst->pml4, va, true);
                    if (!d) {
                        mm_destroy(dst);
                        flush_range(src, 0, ~0ull >> 12);
                        return -1;
                    }
                    spt[i0] &= ~PTE_W; /* both copies are read-only until written */
                    *d = spt[i0];
                    pmm_page_share(spt[i0] & PTE_ADDR);
                }
            }
        }
    }
    flush_range(src, 0, ~0ull >> 12); /* full flush wherever src is loaded */
    return 0;
}

int mm_fault(struct mm *m, uint64_t addr, bool write, bool exec)
{
    if (addr >= USER_TOP)
        return -1;
    struct vma *v = mm_find(m, addr);
    if (!v)
        return -1;
    if ((write && !(v->prot & VMA_W)) || (exec && !(v->prot & VMA_X)) || !(v->prot & (VMA_R | VMA_W | VMA_X)))
        return -1;
    uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE);
    uint64_t *pte = pte_slot(m->pml4, va, true);
    if (!pte)
        return -1; /* out of memory for page tables */
    m->faults++;
    if (!(*pte & PTE_P)) {
        phys_addr_t pa = pmm_alloc_page(); /* zeroed */
        if (!pa)
            return -1;
        *pte = pa | leaf_flags(v->prot, true);
        if (is_loaded(m))
            invlpg(va);
        return 0;
    }
    if (!write || (*pte & PTE_W)) {
        if (is_loaded(m))
            invlpg(va); /* stale translation from before an upgrade */
        return 0;
    }
    phys_addr_t old = *pte & PTE_ADDR;
    if (pmm_page_owners(old) == 1) {
        *pte |= PTE_W; /* last owner: take it over */
        if (is_loaded(m))
            invlpg(va);
        return 0;
    }
    phys_addr_t pa = pmm_alloc_page();
    if (!pa)
        return -1;
    memcpy(phys_to_virt(pa), phys_to_virt(old), PAGE_SIZE);
    *pte = pa | leaf_flags(v->prot, true);
    m->cow_copies++;
    flush_range(m, va, 1);
    pmm_page_release(old);
    return 0;
}

bool mm_prepare(struct mm *m, uint64_t addr, uint64_t len, bool write)
{
    if (len == 0)
        return true;
    if (addr < PAGE_SIZE || addr >= USER_TOP || len > USER_TOP - addr || len > (256ull << 20))
        return false;
    for (uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE); va < addr + len; va += PAGE_SIZE) {
        uint64_t *pte = pte_slot(m->pml4, va, false);
        uint64_t e = pte ? *pte : 0;
        if ((e & PTE_P) && (e & PTE_U) && (!write || (e & PTE_W)))
            continue;
        if (mm_fault(m, va, write, false))
            return false;
    }
    return true;
}

void *mm_kptr(struct mm *m, uint64_t va, bool write)
{
    if (!mm_prepare(m, va, 1, write))
        return NULL;
    uint64_t *pte = pte_slot(m->pml4, va, false);
    if (!pte || !(*pte & PTE_P))
        return NULL;
    return (uint8_t *)phys_to_virt(*pte & PTE_ADDR) + (va & (PAGE_SIZE - 1));
}

void *mm_populate(struct mm *m, uint64_t va)
{
    struct vma *v = mm_find(m, va);
    if (!v)
        return NULL;
    uint64_t *pte = pte_slot(m->pml4, ALIGN_DOWN(va, PAGE_SIZE), true);
    if (!pte)
        return NULL;
    if (!(*pte & PTE_P)) {
        phys_addr_t pa = pmm_alloc_page();
        if (!pa)
            return NULL;
        *pte = pa | leaf_flags(v->prot, true);
    }
    return (uint8_t *)phys_to_virt(*pte & PTE_ADDR) + (va & (PAGE_SIZE - 1));
}

uint64_t mm_brk(struct mm *m, uint64_t want)
{
    if (!m->brk_base || want < m->brk_base)
        return m->brk;
    uint64_t old_end = ALIGN_UP(m->brk, PAGE_SIZE), new_end = ALIGN_UP(want, PAGE_SIZE);
    if (new_end > old_end) {
        for (int i = 0; i < m->nvma; i++)
            if (m->vma[i].kind != VMA_HEAP && m->vma[i].start < new_end && m->vma[i].end > old_end)
                return m->brk; /* would run into another mapping */
        if (new_end > AURORA_MMAP_BASE)
            return m->brk;
        struct vma *h = NULL;
        for (int i = 0; i < m->nvma; i++)
            if (m->vma[i].kind == VMA_HEAP)
                h = &m->vma[i];
        if (h) {
            h->end = new_end;
        } else {
            struct vma v = { m->brk_base, new_end, VMA_R | VMA_W, VMA_HEAP };
            if (vma_insert(m, v))
                return m->brk;
        }
    } else if (new_end < old_end) {
        for (int i = 0; i < m->nvma; i++)
            if (m->vma[i].kind == VMA_HEAP) {
                if (new_end <= m->vma[i].start)
                    vma_remove_at(m, i);
                else
                    m->vma[i].end = new_end;
                break;
            }
        release_range(m, new_end, old_end);
    }
    m->brk = want;
    return m->brk;
}

uint64_t mm_resident_pages(struct mm *m)
{
    uint64_t n = 0;
    if (!m->pml4)
        return 0;
    for (unsigned i3 = 0; i3 < 256; i3++) {
        uint64_t e3 = m->pml4[i3];
        if (!(e3 & PTE_P))
            continue;
        for (unsigned i2 = 0; i2 < 512; i2++) {
            uint64_t e2 = tbl(e3)[i2];
            if (!(e2 & PTE_P))
                continue;
            for (unsigned i1 = 0; i1 < 512; i1++) {
                uint64_t e1 = tbl(e2)[i1];
                if (!(e1 & PTE_P))
                    continue;
                for (unsigned i0 = 0; i0 < 512; i0++)
                    n += (tbl(e1)[i0] & PTE_P) != 0;
            }
        }
    }
    return n;
}
