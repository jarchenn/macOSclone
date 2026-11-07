/* Application-processor startup trampoline.
 *
 * Copied by smp.c to a page below 1 MiB; the SIPI starts the AP in real mode
 * at CS = page >> 4, IP = 0. The code is position independent: %ebx holds the
 * page's linear address throughout, and the data block at TR_DATA (filled in
 * by the boot CPU) supplies everything else. Path: real mode -> 32-bit
 * protected mode (flat GDT in this page) -> PAE + EFER.LME (+NXE) -> paging on
 * a temporary PML4 that identity-maps the first 2 MiB and shares the kernel
 * half -> 64-bit code segment -> jump to ap_entry64 in the kernel on the AP's
 * own stack. */
#define TR_DATA 0xF00
#define TR_CR3 (TR_DATA + 0x00)      /* u32 temporary PML4 (below 4 GiB) */
#define TR_EFER (TR_DATA + 0x04)     /* u32 EFER bits to set (LME, NXE) */
#define TR_STACK (TR_DATA + 0x08)    /* u64 kernel stack top */
#define TR_ENTRY (TR_DATA + 0x10)    /* u64 64-bit entry point */
#define TR_ARG (TR_DATA + 0x18)      /* u64 argument (CPU index) */
#define TR_GDTR (TR_DATA + 0x20)     /* u16 limit, u32 base */
#define TR_PM_PTR (TR_DATA + 0x28)   /* u32 offset, u16 selector (32-bit far jump) */
#define TR_LM_PTR (TR_DATA + 0x30)   /* u32 offset, u16 selector (64-bit far jump) */
#define TR_STARTED (TR_DATA + 0x38)  /* u32 set to 1 by the AP in real mode */
#define TR_GDT (TR_DATA + 0x40)      /* 4 descriptors */

    .section .rodata
    .global ap_trampoline_start, ap_trampoline_end
    .code16
ap_trampoline_start:
    cli
    cld
    mov %cs, %ax
    mov %ax, %ds
    xor %ebx, %ebx
    mov %cs, %bx
    shl $4, %ebx                     /* linear base of this page */
    movl $1, TR_STARTED              /* ds-relative: tells the BSP we woke up */
    lgdtl TR_GDTR
    mov %cr0, %eax
    or $1, %eax                      /* PE */
    mov %eax, %cr0
    ljmpl *TR_PM_PTR

    .code32
ap_pm32:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %cr4, %eax
    or $(1 << 5), %eax               /* PAE */
    mov %eax, %cr4
    mov TR_CR3(%ebx), %eax
    mov %eax, %cr3
    mov $0xC0000080, %ecx            /* EFER */
    rdmsr
    or TR_EFER(%ebx), %eax
    wrmsr
    mov %cr0, %eax
    or $0x80010001, %eax             /* PG | WP | PE */
    mov %eax, %cr0
    ljmpl *TR_LM_PTR(%ebx)

    .code64
ap_lm64:
    mov %ebx, %ebx                   /* zero-extend the base */
    mov TR_STACK(%rbx), %rsp
    mov TR_ARG(%rbx), %rdi
    mov TR_ENTRY(%rbx), %rax
    xor %ebp, %ebp
    jmp *%rax
ap_trampoline_end:

    .global ap_tr_pm32_off, ap_tr_lm64_off
ap_tr_pm32_off: .long ap_pm32 - ap_trampoline_start
ap_tr_lm64_off: .long ap_lm64 - ap_trampoline_start

    .section .note.GNU-stack, "", @progbits
