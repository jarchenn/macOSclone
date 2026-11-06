/* Process start-up: arguments, environment, auxiliary vector, TLS for the
 * main thread, constructors, main(), exit(). */
#include "sys.h"
#include <stdlib.h>
#include <string.h>

uintptr_t __tls_image;
size_t __tls_filesz, __tls_memsz, __tls_align = 16;
unsigned long __aurora_auxv[32];

int main(int argc, char **argv, char **envp);
extern void __libc_init_array(void);
extern void __libc_fini_array(void);

#define PT_TLS_ 7

struct phdr64 {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static size_t round_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

size_t __tls_area_size(void)
{
    return __tls_align + round_up(__tls_memsz, __tls_align) + sizeof(struct aurora_tcb) + 64;
}

/* Variant II: [ ... TLS block (memsz, rounded) ][ TCB ]; tp = &TCB, which
 * must be aligned to the TLS segment's alignment. */
struct aurora_tcb *__tls_init_area(void *mem)
{
    size_t block = round_up(__tls_memsz, __tls_align);
    uintptr_t tp = round_up((uintptr_t)mem + block, __tls_align < 64 ? 64 : __tls_align);
    uint8_t *start = (uint8_t *)(tp - block);
    memset(start, 0, block);
    if (__tls_filesz)
        memcpy(start, (const void *)__tls_image, __tls_filesz);
    struct aurora_tcb *tcb = (struct aurora_tcb *)tp;
    memset(tcb, 0, sizeof *tcb);
    tcb->self = tcb;
    tcb->canary = 0x5a17c0de5a17c0deull;
    return tcb;
}

static _Alignas(64) uint8_t main_tls[16384];

void __aurora_start(uint64_t *sp);
void __aurora_start(uint64_t *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    environ = envp;
    char **e = envp;
    while (*e)
        e++;
    unsigned long *aux = (unsigned long *)(e + 1);
    for (; aux[0] != AURORA_AT_NULL; aux += 2)
        if (aux[0] < 32)
            __aurora_auxv[aux[0]] = aux[1];
    const struct phdr64 *ph = (const struct phdr64 *)__aurora_auxv[AURORA_AT_PHDR];
    size_t phnum = __aurora_auxv[AURORA_AT_PHNUM];
    for (size_t i = 0; ph && i < phnum; i++) {
        if (ph[i].type == PT_TLS_) {
            __tls_image = ph[i].vaddr;
            __tls_filesz = ph[i].filesz;
            __tls_memsz = ph[i].memsz;
            __tls_align = ph[i].align > 16 ? ph[i].align : 16;
        }
    }
    void *area = main_tls;
    if (__tls_area_size() > sizeof main_tls) {
        /* Large TLS segment: take it from the break (malloc needs TLS). */
        long cur = __sys1(SYS_BRK, 0);
        long want = cur + (long)__tls_area_size();
        if (__sys1(SYS_BRK, want) != want)
            __sys1(SYS_EXIT, 127);
        area = (void *)cur;
    }
    struct aurora_tcb *tcb = __tls_init_area(area);
    __sys1(SYS_SET_FSBASE, tcb);
    __pthread_init_main(tcb);
    atexit(__libc_fini_array);
    __libc_init_array();
    exit(main(argc, argv, envp));
}
