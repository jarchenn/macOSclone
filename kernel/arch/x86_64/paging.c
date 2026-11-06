/* x86-64 4-level page table manipulation. Tables are accessed through the
 * HHDM; physical addresses are never dereferenced directly. */
#include "arch/paging.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "memory/physical.h"
#include "core/boot.h"

uint64_t paging_table_pages;

static uint64_t fix_flags(uint64_t f)
{
    if (!g_cpu.nx)
        f &= ~PTE_NX;
    return f;
}

static inline uint64_t *table_virt(uint64_t entry) { return (uint64_t *)phys_to_virt(entry & PTE_ADDR); }

/* Returns the next-level table for entry *e, allocating it if needed. */
static uint64_t *next_table(uint64_t *e, bool create, bool user)
{
    if (*e & PTE_P) {
        if (*e & PTE_PS)
            return NULL; /* large page in the way */
        if (user)
            *e |= PTE_U; /* user mappings need U at every level */
        return table_virt(*e);
    }
    if (!create)
        return NULL;
    phys_addr_t p = pmm_alloc_page();
    if (!p)
        return NULL;
    paging_table_pages++;
    /* Intermediate entries are permissive; leaf entries carry the real
     * permissions (W and NX restrict at every level, so leave them open). */
    *e = p | PTE_P | PTE_W | (user ? PTE_U : 0);
    return table_virt(*e);
}

static inline unsigned idx(uint64_t va, int level) { return (unsigned)((va >> (12 + 9 * level)) & 511); }

bool paging_map_4k(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags)
{
    bool user = (flags & PTE_U) != 0;
    if (user && va >= 0x0000800000000000ull)
        return false; /* never expose the kernel half to ring 3 */
    uint64_t *pdpt = next_table(&pml4[idx(va, 3)], true, user);
    if (!pdpt)
        return false;
    uint64_t *pd = next_table(&pdpt[idx(va, 2)], true, user);
    if (!pd)
        return false;
    uint64_t *pt = next_table(&pd[idx(va, 1)], true, user);
    if (!pt)
        return false;
    pt[idx(va, 0)] = (pa & PTE_ADDR) | fix_flags(flags) | PTE_P;
    return true;
}

bool paging_map_2m(uint64_t *pml4, uint64_t va, uint64_t pa, uint64_t flags)
{
    if ((va | pa) & 0x1FFFFF)
        return false;
    uint64_t *pdpt = next_table(&pml4[idx(va, 3)], true, false);
    if (!pdpt)
        return false;
    uint64_t *pd = next_table(&pdpt[idx(va, 2)], true, false);
    if (!pd)
        return false;
    uint64_t *e = &pd[idx(va, 1)];
    if ((*e & PTE_P) && !(*e & PTE_PS))
        return false; /* a 4 KiB table already lives here */
    *e = (pa & PTE_ADDR) | fix_flags(flags) | PTE_P | PTE_PS;
    return true;
}

static uint64_t *pd_entry(uint64_t *pml4, uint64_t va)
{
    uint64_t *pdpt = next_table(&pml4[idx(va, 3)], false, false);
    if (!pdpt)
        return NULL;
    uint64_t *pd = next_table(&pdpt[idx(va, 2)], false, false);
    if (!pd)
        return NULL;
    return &pd[idx(va, 1)];
}

bool paging_is_2m(uint64_t *pml4, uint64_t va)
{
    uint64_t *e = pd_entry(pml4, va);
    return e && (*e & PTE_P) && (*e & PTE_PS);
}

bool paging_pd_slot_free(uint64_t *pml4, uint64_t va)
{
    uint64_t *e = pd_entry(pml4, va);
    return !e || !(*e & PTE_P);
}

uint64_t paging_unmap_4k(uint64_t *pml4, uint64_t va)
{
    uint64_t *e = pd_entry(pml4, va);
    if (!e || !(*e & PTE_P) || (*e & PTE_PS))
        return 0;
    uint64_t *pt = table_virt(*e);
    uint64_t old = pt[idx(va, 0)];
    if (!(old & PTE_P))
        return 0;
    pt[idx(va, 0)] = 0;
    invlpg(va);
    return old & PTE_ADDR;
}

uint64_t paging_translate(uint64_t *pml4, uint64_t va)
{
    uint64_t e = pml4[idx(va, 3)];
    if (!(e & PTE_P))
        return 0;
    e = table_virt(e)[idx(va, 2)];
    if (!(e & PTE_P))
        return 0;
    if (e & PTE_PS)
        return (e & PTE_ADDR & ~0x3FFFFFFFull) | (va & 0x3FFFFFFF);
    e = table_virt(e)[idx(va, 1)];
    if (!(e & PTE_P))
        return 0;
    if (e & PTE_PS)
        return (e & PTE_ADDR & ~0x1FFFFFull) | (va & 0x1FFFFF);
    e = table_virt(e)[idx(va, 0)];
    if (!(e & PTE_P))
        return 0;
    return (e & PTE_ADDR) | (va & 0xFFF);
}

uint64_t paging_get_pte(uint64_t *pml4, uint64_t va)
{
    uint64_t *e = pd_entry(pml4, va);
    if (!e || !(*e & PTE_P) || (*e & PTE_PS))
        return 0;
    return table_virt(*e)[idx(va, 0)];
}

bool paging_ensure_pml4_entry(uint64_t *pml4, unsigned index)
{
    return next_table(&pml4[index & 511], true, false) != NULL;
}

/* Frees every page and page table reachable from the lower half (user
 * space) of pml4. The upper half is shared with the kernel and left alone. */
uint64_t paging_free_user_half(uint64_t *pml4)
{
    uint64_t pages = 0;
    for (unsigned i = 0; i < 256; i++) {
        uint64_t e3 = pml4[i];
        if (!(e3 & PTE_P))
            continue;
        uint64_t *pdpt = table_virt(e3);
        for (unsigned j = 0; j < 512; j++) {
            uint64_t e2 = pdpt[j];
            if (!(e2 & PTE_P) || (e2 & PTE_PS))
                continue;
            uint64_t *pd = table_virt(e2);
            for (unsigned k = 0; k < 512; k++) {
                uint64_t e1 = pd[k];
                if (!(e1 & PTE_P) || (e1 & PTE_PS))
                    continue;
                uint64_t *pt = table_virt(e1);
                for (unsigned l = 0; l < 512; l++) {
                    if (pt[l] & PTE_P) {
                        pmm_free_page(pt[l] & PTE_ADDR);
                        pages++;
                    }
                }
                pmm_free_page(e1 & PTE_ADDR);
            }
            pmm_free_page(e2 & PTE_ADDR);
        }
        pmm_free_page(e3 & PTE_ADDR);
        pml4[i] = 0;
    }
    return pages;
}
