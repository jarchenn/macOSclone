/* User program entry. The kernel enters here in ring 3 with
 * rdi = pointer to the argument string, rsi = its length. */
    .section .text._start
    .global _start
    .type _start, @function
_start:
    xor %ebp, %ebp
    and $-16, %rsp
    call __aurora_start
    ud2
    .size _start, . - _start

    .section .note.GNU-stack, "", @progbits
