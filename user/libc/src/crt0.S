/* Process entry (System V initial stack, see aurora_abi.h): RSP points at
 * argc. __aurora_start(sp) never returns. */
    .section .text._start
    .global _start
    .type _start, @function
_start:
    xor %ebp, %ebp
    mov %rsp, %rdi
    and $-16, %rsp
    call __aurora_start
    ud2
    .size _start, . - _start

/* Signal return trampoline: a handler's `ret` lands here with RSP at the
 * signal frame. */
    .global __aurora_sigreturn
    .type __aurora_sigreturn, @function
__aurora_sigreturn:
    mov $12, %eax           /* SYS_SIGRETURN */
    int $0x80
    ud2
    .size __aurora_sigreturn, . - __aurora_sigreturn

/* First instruction of a new thread: the kernel enters with RDI = the
 * thread descriptor and RSP 16-byte aligned minus 8. */
    .global __aurora_thread_entry
    .type __aurora_thread_entry, @function
__aurora_thread_entry:
    xor %ebp, %ebp
    and $-16, %rsp
    call __aurora_thread_main
    ud2
    .size __aurora_thread_entry, . - __aurora_thread_entry

    .section .note.GNU-stack, "", @progbits
