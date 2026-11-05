/* Kernel entry point and small CPU helpers.
 *
 * Limine (base revision 6) enters _start in 64-bit long mode with paging
 * enabled, interrupts disabled, DF clear, and rsp pointing at a 64 KiB stack
 * in bootloader-reclaimable memory with a 0 return address pushed. The
 * SysV ABI therefore already holds at entry (rsp + 8 is 16-byte aligned).
 */
    .section .text
    .global _start
    .type _start, @function
_start:
    xor %ebp, %ebp          /* terminate frame-pointer chains for backtraces */
    cld
    call kmain_early
1:  cli
    hlt
    jmp 1b
    .size _start, . - _start

/* void switch_stack_and_call(uint64_t stack_top, void (*fn)(void)) */
    .global switch_stack_and_call
    .type switch_stack_and_call, @function
switch_stack_and_call:
    mov %rdi, %rsp
    and $-16, %rsp
    xor %ebp, %ebp
    push $0                 /* fake return address keeps ABI alignment */
    jmp *%rsi
    .size switch_stack_and_call, . - switch_stack_and_call

/* void gdt_reload_segments(uint16_t code, uint16_t data) */
    .global gdt_reload_segments
    .type gdt_reload_segments, @function
gdt_reload_segments:
    mov %si, %ds
    mov %si, %es
    mov %si, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    /* Far return to reload CS. */
    pop %rax                /* return address */
    movzwq %di, %rdi
    push %rdi
    push %rax
    lretq
    .size gdt_reload_segments, . - gdt_reload_segments

    .section .note.GNU-stack, "", @progbits
