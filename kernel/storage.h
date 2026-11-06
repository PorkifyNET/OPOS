#ifndef OPOS_STORAGE_H
#define OPOS_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Every volume on every block disk, readable or not: the partitions of
 * each disk (MBR primaries, GPT entries), or the whole disk when it has no
 * partition table. Used by the Files "THIS PC" view and the shell
 * 'drives' command. Only FAT volumes can be opened today; the rest are
 * named so the user can see what is there.
 */
enum storage_filesystem {
    STORAGE_FS_UNKNOWN,
    STORAGE_FS_EMPTY,       /* all zero: blank disk or unused partition */
    STORAGE_FS_FAT12,
    STORAGE_FS_FAT16,
    STORAGE_FS_FAT32,
    STORAGE_FS_EXFAT,
    STORAGE_FS_NTFS,
    STORAGE_FS_EXT,         /* ext2, ext3 or ext4 */
    STORAGE_FS_BTRFS,
    STORAGE_FS_XFS,
    STORAGE_FS_BITLOCKER,
    STORAGE_FS_ISO9660,
    STORAGE_FS_SWAP,
    STORAGE_FS_HFS_PLUS,
    STORAGE_FS_APFS,
    STORAGE_FS_NO_PARTITIONS, /* a partition table with no entries */
    STORAGE_FS_READ_ERROR,
    STORAGE_FS_NO_DISC,       /* CD/DVD drive without a disc */
    STORAGE_FS_UDF,
};

#define STORAGE_NOT_MOUNTED ((size_t)-1)
#define STORAGE_MAX_VOLUMES 64
#define STORAGE_LABEL_CAPACITY 24

struct storage_volume {
    /* CD/DVD drive: `disk` is the optical drive index (optical.h) and
       there are no partitions. */
    bool optical;
    /* A USB stick or other removable disk. */
    bool removable;
    /* Removable and in an SD card slot (else a USB stick). */
    bool sd_card;
    size_t disk;            /* block disk index */
    uint32_t partition;     /* 1-based table position; 0 = whole disk */
    uint64_t first;         /* first sector */
    uint64_t sectors;
    uint8_t partition_type; /* MBR type (or the GPT stand-in); 0 if none */
    enum storage_filesystem filesystem;
    /* Volume label, trimmed; "" when it has none or it cannot be read. */
    char label[STORAGE_LABEL_CAPACITY];
    /* FAT volume index (0 = /disk) when mounted, else STORAGE_NOT_MOUNTED
       (also for discs). */
    size_t mount;
    /* Where it can be opened ("/disk2", "/cdrom"); "" when it cannot. */
    char path[16];
};

/* Fills volumes (disk order, then table order, then the CD/DVD drives,
   whose discs are checked again first); returns how many. */
size_t storage_list(struct storage_volume *volumes, size_t capacity);
const char *storage_filesystem_name(enum storage_filesystem filesystem);
/* "/disk" for FAT volume 0, "/disk2".."/disk8" for the others. Returns the
   length, 0 when it does not fit. */
size_t storage_mount_path(size_t mount, char *path, size_t capacity);
/* Short human size: "512 MB", "1.5 GB", "2 TB". Returns the length. */
size_t storage_format_size(uint64_t sectors, char *text, size_t capacity);

/* Starts USB (xHCI) and mounts what is plugged in; call once at boot. */
void storage_initialize(void);
/* Hot-plug: notices USB sticks plugged in or pulled out, mounts and
   unmounts their FAT volumes. Cheap enough to call twice a second. True
   when the set of disks changed. */
bool storage_poll(void);

#endif
