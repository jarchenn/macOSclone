/* _setjmp/_longjmp (BSD names, used by Lua's POSIX configuration): no
 * signal mask handling, exactly like newlib's setjmp/longjmp. */
    .text
    .global _setjmp, _longjmp
    .type _setjmp, @function
_setjmp:
    jmp setjmp
    .size _setjmp, . - _setjmp
    .type _longjmp, @function
_longjmp:
    jmp longjmp
    .size _longjmp, . - _longjmp
    .section .note.GNU-stack, "", @progbits
