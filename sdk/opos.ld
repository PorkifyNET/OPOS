/*
 * Linker script for OPOS applications. The kernel maps each segment with
 * exactly one permission set (W^X: nothing is both writable and
 * executable), so every segment starts on its own page.
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    headers PT_LOAD FILEHDR PHDRS FLAGS(4);
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    note PT_NOTE FLAGS(4);
}

SECTIONS
{
    . = 0x00400000 + SIZEOF_HEADERS;
    .note.opos : { KEEP(*(.note.opos)) } :headers :note

    . = ALIGN(4096);
    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        /* Never empty: an empty segment would be placed at address 0. */
        LONG(0)
    } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.note.GNU-stack)
        *(.comment)
    }
}
