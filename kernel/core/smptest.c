/* SMP diagnostics: the TLB shootdown test used by the guest test suite.
 *
 * A kernel task pinned to another CPU reads a test page (caching the
 * translation in that CPU's TLB) and waits. This CPU unmaps the page, with
 * or without a shootdown, then lets the reader read again through a
 * fault-tolerant probe. With a shootdown, the second read must fault. */
#include "core/smptest.h"
#include "core/sched.h"
#include "core/time.h"
#include "arch/percpu.h"
#include "arch/x86_64/io.h"
#include "memory/virtual.h"
#include "lib.h"

int probe_read64(const uint64_t *addr, uint64_t *out);

#define MAGIC 0xA0A0C0DE5AFE1234ull

static volatile int phase;
static volatile int res1, res2;
static volatile uint64_t val1, val2;
static volatile int reader_cpu;
static uint64_t test_va;

static void reader(void *arg)
{
    UNUSED(arg);
    uint64_t v = 0;
    reader_cpu = (int)this_cpu()->index;
    res1 = probe_read64((const uint64_t *)test_va, &v);
    val1 = v;
    __atomic_store_n(&phase, 1, __ATOMIC_RELEASE);
    uint64_t deadline = time_ms() + 5000;
    while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != 2 && time_ms() < deadline)
        cpu_pause(); /* stay on this CPU with the translation cached */
    v = 0;
    res2 = probe_read64((const uint64_t *)test_va, &v);
    val2 = v;
    __atomic_store_n(&phase, 3, __ATOMIC_RELEASE);
}

static bool wait_phase(int want, uint64_t ms)
{
    uint64_t deadline = time_ms() + ms;
    while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != want) {
        if (time_ms() >= deadline)
            return false;
        sched_sleep_ms(1);
    }
    return true;
}

int smp_tlb_test(bool shootdown, struct tlb_test_result *out)
{
    memset(out, 0, sizeof *out);
    int target = -1;
    for (uint32_t i = 0; i < g_ncpus; i++)
        if (g_cpus[i].online && &g_cpus[i] != this_cpu()) {
            target = (int)i;
            break;
        }
    if (target < 0)
        return -1; /* needs two CPUs */
    test_va = vmm_test_page_map();
    if (!test_va)
        return -2;
    *(volatile uint64_t *)test_va = MAGIC;
    phase = 0;
    if (!task_create_pinned("tlbtest", reader, NULL, target))
        return -3;
    if (!wait_phase(1, 3000))
        return -4;
    out->unmap_cpu = (int)this_cpu()->index;
    vmm_test_page_unmap(shootdown);
    __atomic_store_n(&phase, 2, __ATOMIC_RELEASE);
    if (!wait_phase(3, 6000))
        return -5;
    out->reader_cpu = reader_cpu;
    out->first_read_ok = res1 == 0 && val1 == MAGIC;
    out->second_read_faulted = res2 != 0;
    out->second_value_stale = res2 == 0 && val2 == MAGIC;
    return 0;
}
