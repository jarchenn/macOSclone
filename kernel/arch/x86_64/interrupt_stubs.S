/* 256 interrupt entry stubs, each exactly 16 bytes apart.
 *
 * Vectors 8, 10-14, 17, 21, 29 and 30 receive a CPU-pushed error code; all
 * others push a dummy 0 so every handler sees the same frame layout.
 * In 64-bit mode the CPU aligns RSP to 16 bytes before pushing SS..RIP, so
 * the frame layout is deterministic; the common path still re-aligns RSP
 * explicitly before calling C.
 */
    .section .text
    .align 16
    .global isr_stub_table
isr_stub_table:
    .set vec, 0
    .rept 256
    .align 16
    .if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
    .else
    pushq $0
    .endif
    pushq $vec
    jmp isr_common
    .set vec, vec + 1
    .endr

isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld                         /* SysV ABI requires DF = 0 on C entry */
    /* GS: switch to the kernel's per-CPU base when coming from ring 3
     * (see arch/percpu.h). r12 remembers whether SWAPGS ran; it is
     * callee-saved, so it survives a task switch inside the handler, and
     * every CPU is in the same GS state while in the kernel. */
    xor %r12d, %r12d
    mov 120(%rsp), %rax         /* vector */
    cmp $2, %rax
    je 2f
    cmp $8, %rax
    je 2f
    cmp $18, %rax
    je 2f
    testb $3, 144(%rsp)         /* saved CS: from user mode? */
    jz 3f
    swapgs
    mov $1, %r12d
    jmp 3f
2:  /* NMI, #DF, #MC: trust the MSR, not CS (kernel GS has the top bit set) */
    mov $0xC0000101, %ecx
    rdmsr
    test %edx, %edx
    js 3f
    swapgs
    mov $1, %r12d
3:
    mov %rsp, %rdi              /* struct interrupt_frame * */
    mov %rsp, %rbx              /* rbx is callee-saved: survives the call */
    and $-16, %rsp
    call interrupt_dispatch
    mov %rbx, %rsp
    test %r12d, %r12d
    jz 4f
    swapgs
4:
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp               /* vector + error code */
    iretq

    .section .note.GNU-stack, "", @progbits
