/* Aurora OS kernel linker script.
 * Higher-half, non-PIE image at 0xffffffff80000000 (-mcmodel=kernel).
 * Separate PT_LOAD segments keep text RX, rodata R and data/bss RW (W^X).
 */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    limine_requests PT_LOAD;
    text    PT_LOAD;
    rodata  PT_LOAD;
    data    PT_LOAD;
}

SECTIONS
{
    . = 0xffffffff80000000;
    __kernel_start = .;

    .limine_requests : {
        __limine_requests_start = .;
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
        __limine_requests_end = .;
    } :limine_requests

    . = ALIGN(4096);
    .text : {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    } :text

    . = ALIGN(4096);
    .rodata : {
        __rodata_start = .;
        *(.rodata .rodata.*)
        *(.eh_frame*)
    } :rodata
    __rodata_end = .;

    . = ALIGN(4096);
    .data : {
        __data_start = .;
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __data_end = .;
    } :data

    __kernel_end = .;

    /DISCARD/ : {
        *(.note .note.*)
        *(.comment)
    }
}
