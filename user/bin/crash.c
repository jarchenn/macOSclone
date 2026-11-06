/* crash [null|kernel|ud|stack|gp]: deliberately faults. The kernel must kill
 * only this process and keep running. */
#include "aurora.h"

__attribute__((noinline)) static int recurse(int depth)
{
    volatile char pad[512];
    pad[0] = (char)depth;
    if (depth > 1000000000)
        return 0; /* never true within a 128 KiB stack */
    return recurse(depth + 1) + pad[0];
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "null";
    printf("crash: about to fault (%s)\n", mode);
    if (!strcmp(mode, "kernel"))
        return *(volatile int *)0xffffffff80000000ull; /* supervisor-only page */
    if (!strcmp(mode, "ud"))
        __asm__ volatile("ud2");
    if (!strcmp(mode, "stack"))
        return recurse(0); /* runs into the guard page below the stack */
    if (!strcmp(mode, "gp"))
        __asm__ volatile("cli"); /* privileged instruction */
    *(volatile int *)0 = 42; /* NULL write */
    printf("crash: still alive?!\n");
    return 1;
}
