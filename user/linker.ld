/* Aurora user programs: static, non-PIE, loaded at 0x400000.
 * Separate segments keep text RX, rodata R and data/bss RW (the kernel's
 * loader rejects writable+executable segments). */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;
    .text : { *(.text._start) *(.text .text.*) } :text
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) *(.eh_frame*) } :rodata
    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.note .note.*) *(.comment) }
}
