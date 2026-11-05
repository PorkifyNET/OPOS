OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

PHDRS
{
    text PT_LOAD FILEHDR PHDRS FLAGS(5);
    data PT_LOAD FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(6);
    tls PT_TLS FLAGS(4);
}

SECTIONS
{
    . = SIZEOF_HEADERS;
    .hash : { *(.hash) } :text
    .gnu.hash : { *(.gnu.hash) } :text
    .dynsym : { *(.dynsym) } :text
    .dynstr : { *(.dynstr) } :text
    .gnu.version : { *(.gnu.version) } :text
    .gnu.version_d : { *(.gnu.version_d) } :text
    .gnu.version_r : { *(.gnu.version_r) } :text
    .rela.dyn : { *(.rela .rela.dyn) } :text
    .opos.relr : {
        QUAD(opos_relr_slots)
        QUAD(15)
    } :text
    .rela.plt : { *(.rela.plt) } :text

    . = ALIGN(4096);
    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
    } :text

    . = 0x2000;
    .dynamic : {
        QUAD(36)
        QUAD(ADDR(.opos.relr))
        QUAD(35)
        QUAD(SIZEOF(.opos.relr))
        QUAD(37)
        QUAD(8)
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
    .relr_test : {
        opos_relr_slots = .;
        QUAD(opos_relr_target)
        QUAD(opos_relr_target)
        QUAD(opos_relr_target)
        QUAD(opos_relr_target)
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
