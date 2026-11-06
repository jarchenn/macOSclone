/* Task context switching and the first entry into user mode. */
    .section .text

/* void context_switch(uint64_t *save_rsp, uint64_t new_rsp)
 * Saves the callee-saved registers and RFLAGS of the current task on its own
 * kernel stack, stores RSP, then restores the same set from the new stack.
 * Caller-saved registers need no saving: this is an ordinary C call. */
    .global context_switch
    .type context_switch, @function
context_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    pushfq
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    popfq
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
    .size context_switch, . - context_switch

/* First instruction of every new kernel task (reached through `ret` from
 * context_switch with RSP 16-byte aligned). */
    .global task_trampoline
    .type task_trampoline, @function
task_trampoline:
    xor %ebp, %ebp
    call task_start_c
    ud2
    .size task_trampoline, . - task_trampoline

/* void enter_user(uint64_t rip, uint64_t rsp, uint64_t arg0, uint64_t arg1)
 * Drops to ring 3 with interrupts enabled. User selectors: CS 0x23, SS 0x1B. */
    .global enter_user
    .type enter_user, @function
enter_user:
    cli
    mov $0x1B, %ax
    mov %ax, %ds
    mov %ax, %es
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    pushq $0x1B             /* SS */
    pushq %rsi              /* RSP */
    pushq $0x202            /* RFLAGS: IF set */
    pushq $0x23             /* CS */
    pushq %rdi              /* RIP */
    mov %rdx, %rdi          /* user arg0 */
    mov %rcx, %rsi          /* user arg1 */
    xor %eax, %eax
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %ebp, %ebp
    xor %r8d, %r8d
    xor %r9d, %r9d
    xor %r10d, %r10d
    xor %r11d, %r11d
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d
    iretq
    .size enter_user, . - enter_user

    .section .note.GNU-stack, "", @progbits
