/*
 * Boot code for the boot sector of an OPOS partition (16-bit real mode).
 * The installer copies these bytes to offset 0x5a of the partition's FAT
 * boot sector, after the BPB (FAT32's BPB ends at 0x5a, FAT16's earlier),
 * and points the jump at byte 0 here.
 *
 * Whatever starts the partition (a standard MBR booting the active
 * partition, GRUB's "chainloader +1", Limine's bios chainload) loads the
 * sector to 0x7c00 with the BIOS drive number in DL. The code moves itself
 * to 0x0600, reads Limine's boot sector from partition sector
 * OPOS_VBR_BOOT_AREA (the BPB's hidden-sectors field holds the partition
 * start) to 0x7c00 and jumps to it, keeping DL. Limine's boot sector then
 * loads its stage 2 from the absolute disk offset the installer stored in
 * it. On a read error it prints "E" and halts.
 *
 * Only label differences are used, so the bytes need no relocation.
 */
#define VBR_CODE_OFFSET 0x5a
#define OPOS_VBR_BOOT_AREA 8
#define LOAD 0x7c00
#define MOVED 0x0600
/* Address of label x after the move. */
#define AT(x) (MOVED + VBR_CODE_OFFSET + ((x) - opos_vbr_stub))

    .section .rodata
    .global opos_vbr_stub
    .global opos_vbr_stub_end
    .code16
opos_vbr_stub:
    cli
    xorw %ax, %ax
    movw %ax, %ss
    movw $LOAD, %sp
    movw %ax, %ds
    movw %ax, %es
    sti
    cld
    movw $LOAD, %si
    movw $MOVED, %di
    movw $256, %cx
    rep movsw
    ljmp $0, $AT(moved)
moved:
    movl MOVED + 0x1c, %eax
    addl $OPOS_VBR_BOOT_AREA, %eax
    movl %eax, AT(dap_lba)
    movw $AT(dap), %si
    movb $0x42, %ah
    int $0x13
    jc read_error
    ljmp $0, $LOAD
read_error:
    movw $0x0e45, %ax
    xorw %bx, %bx
    int $0x10
halt:
    hlt
    jmp halt
dap:
    .byte 0x10, 0
    .word 1
    .word LOAD, 0
dap_lba:
    .long 0, 0
opos_vbr_stub_end:
    .code64
