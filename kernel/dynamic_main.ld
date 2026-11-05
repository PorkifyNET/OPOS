OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
    interp PT_INTERP FLAGS(4);
    headers PT_LOAD FILEHDR PHDRS FLAGS(4);
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
    tls PT_TLS FLAGS(4);
}

SECTIONS
{
    . = 0x02000000 + SIZEOF_HEADERS;
    .interp : {
        *(.interp)
    } :headers :interp
    .hash : { *(.hash) } :headers
    .gnu.hash : { *(.gnu.hash) } :headers
    .dynsym : { *(.dynsym) } :headers
    .dynstr : { *(.dynstr) } :headers
    .gnu.version : { *(.gnu.version) } :headers
    .gnu.version_d : { *(.gnu.version_d) } :headers
    .gnu.version_r : { *(.gnu.version_r) } :headers
    .rela.dyn : { *(.rela .rela.dyn) } :headers
    .rela.plt : { *(.rela.plt) } :headers

    . = ALIGN(4096);
    .text : {
        *(.text .text.*)
        *(.plt .plt.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = 0x02100020;
    .dynamic : {
        *(.dynamic)
    } :data :dynamic
    .init_array : {
        KEEP(*(.init_array .init_array.*))
    } :data
    .fini_array : {
        KEEP(*(.fini_array .fini_array.*))
    } :data
    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls
    .tbss (NOLOAD) : {
        *(.tbss .tbss.*)
    } :data :tls
    .data : {
        *(.data .data.*)
        *(.got .got.*)
    } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
