#include "harness.h"
#include "types.h"
#include "core/boot.h"

int t_failures, t_checks;
const char *t_name = "";

/* Symbols the portable kernel modules expect. */
struct boot_info g_boot;

void *memset32(uint32_t *dst, uint32_t v, size_t count)
{
    for (size_t i = 0; i < count; i++)
        dst[i] = v;
    return dst;
}

int main(void)
{
    run_tests();
    printf("%-16s %4d checks, %d failure(s)\n", t_suite, t_checks, t_failures);
    return t_failures ? 1 : 0;
}
