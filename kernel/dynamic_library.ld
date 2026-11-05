OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

PHDRS
{
    headers PT_LOAD FILEHDR PHDRS FLAGS(4);
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
    tls PT_TLS FLAGS(4);
    relro PT_GNU_RELRO FLAGS(4);
}

SECTIONS
{
    . = SIZEOF_HEADERS;
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
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = DATA_SEGMENT_ALIGN(4096, 4096);
    .dynamic : {
        *(.dynamic)
    } :data :dynamic
    .init_array : {
        KEEP(*(.init_array .init_array.*))
    } :data
    .fini_array : {
        KEEP(*(.fini_array .fini_array.*))
    } :data
    .data.rel.ro : {
        *(.data.rel.ro .data.rel.ro.*)
        *(.got .got.*)
    } :data
    .tdata : {
        *(.tdata .tdata.*)
    } :data :tls
    .tbss (NOLOAD) : {
        *(.tbss .tbss.*)
    } :data :tls

    . = DATA_SEGMENT_RELRO_END(0, .);
    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data
    . = DATA_SEGMENT_END(.);

    /DISCARD/ : {
        *(.eh_frame .eh_frame.*)
        *(.note .note.*)
    }
}
