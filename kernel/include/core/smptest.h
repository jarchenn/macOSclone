#ifndef AURORA_SMPTEST_H
#define AURORA_SMPTEST_H
#include "types.h"

struct tlb_test_result {
    int unmap_cpu, reader_cpu;
    bool first_read_ok;       /* reader saw the page before the unmap */
    bool second_read_faulted; /* reader faulted after the unmap */
    bool second_value_stale;  /* reader still saw the old page (stale TLB) */
};

/* Returns 0 when the test ran (see the result), < 0 if it could not run
 * (one CPU, no memory, timeout). Call from a task holding the BKL. */
int smp_tlb_test(bool shootdown, struct tlb_test_result *out);

#endif
