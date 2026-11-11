#ifndef OPOS_INSTALLER_H
#define OPOS_INSTALLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * OPOS installer engine. The live ISO carries an "install" Limine module
 * (a ustar archive with the Limine boot files); its presence means OPOS
 * runs from installation media. Installing erases the chosen disk, writes
 * an MBR with one FAT partition, Limine's BIOS boot code, and copies the
 * running kernel, the initramfs and the Limine files onto the partition,
 * so the disk boots on BIOS and UEFI firmware.
 */

/* Records the boot files; install_archive may be NULL (installed system). */
void installer_set_payload(const void *install_archive, size_t archive_size,
                           const void *kernel, size_t kernel_size,
                           const void *initramfs, size_t initramfs_size);

/* True when booted from installation media that can install OPOS. */
bool installer_available(void);
/* Populate/verify this kernel's generation from its full boot initramfs.
   Used after formatting a new install and on the first boot after an update. */
bool installer_stage_system_root(const char **error);

/* Called before each install step with a description and 0-100 percent. */
typedef void (*installer_progress)(const char *step, unsigned percent);

/* What a disk holds now, so the user can tell disks apart before erasing
   one. Partition sizes are in 512-byte sectors. */
struct installer_disk_summary {
    bool readable;
    /* The first 128 KiB and the 1 MiB sector are all zero. */
    bool blank;
    bool gpt;
    /* MBR: primary partitions in use (type != 0); GPT: entries in use. */
    size_t partition_count;
    uint8_t mbr_types[4];
    uint32_t mbr_sectors[4];
    /* A FAT filesystem starts at sector 0 (no partition table). */
    bool whole_disk_fat;
};

/* Reads block disk `disk` (see block_disk_count) without mounting it. */
bool installer_disk_summary(size_t disk, struct installer_disk_summary *summary);
/* Short upper-case name of an MBR partition type, e.g. "FAT32", "NTFS". */
const char *installer_partition_type_name(uint8_t type);
/* Why OPOS cannot be installed on `disk`, or NULL when it can. */
const char *installer_disk_unusable(size_t disk);

/*
 * Manual partitioning (MBR and GPT disks). A layout is a planned partition
 * table, edited in memory; nothing is written until
 * installer_install_partition. On MBR disks slot i is table entry i + 1
 * (four slots); on GPT disks slots hold the partitions in table order.
 */
#define INSTALLER_SLOTS 16
#define INSTALLER_MBR_SLOTS 4
#define INSTALLER_NO_SLOT ((size_t)-1)
/* OPOS needs room for itself plus user files. */
#define INSTALLER_MIN_SECTORS (64u * 2048u)

struct installer_slot {
    /* MBR type byte (GPT: the closest MBR type, see partition.h);
       0 = unused */
    uint8_t type;
    uint8_t status;      /* boot flag byte, kept as found */
    uint32_t first;
    uint32_t sectors;
    bool created;        /* added in this plan */
    /* Partition number as Limine and GRUB count (MBR 1-4, GPT entry);
       for a new GPT partition the entry it is expected to get. */
    uint32_t number;
    /* What the slot held on disk, for the summary of changes. */
    uint8_t original_type;
    uint32_t original_first;
    uint32_t original_sectors;
    uint32_t original_number;
    uint8_t original_guid[16];   /* GPT type GUID */
};

struct installer_layout {
    size_t disk;
    uint64_t disk_sectors;
    bool gpt;
    /* Where partitions may go: [usable_first, usable_end). */
    uint64_t usable_first;
    uint64_t usable_end;
    struct installer_slot slots[INSTALLER_SLOTS];
    size_t target;       /* slot OPOS goes into, or INSTALLER_NO_SLOT */
};

struct installer_region {
    uint32_t first;
    uint32_t sectors;
};

/* Reads the disk's MBR or GPT into *layout. Fails with a reason for
   damaged tables and unreadable disks. A blank disk gives an empty MBR. */
bool installer_layout_load(size_t disk, struct installer_layout *layout,
                           const char **error);
/* Free space between partitions in disk order, each starting on a 1 MiB
   boundary. Returns the region count. */
size_t installer_layout_free(const struct installer_layout *layout,
                             struct installer_region *regions,
                             size_t capacity);
/* Adds a partition of `sectors` (rounded down to 1 MiB) at the start of
   free region `region`; it becomes the install target. */
bool installer_layout_create(struct installer_layout *layout,
                             const struct installer_region *region,
                             uint32_t sectors, const char **error);
bool installer_layout_delete(struct installer_layout *layout, size_t slot);
/* Why OPOS cannot go into this slot, or NULL. */
const char *installer_layout_slot_problem(const struct installer_layout *layout,
                                          size_t slot);
/* True when the disk uses an MBR and sectors 1-62 are outside every
   partition, so Limine's BIOS boot code fits. */
bool installer_layout_bios_bootable(const struct installer_layout *layout);
/*
 * Other operating systems. Before installing, the installer looks at what
 * boots the computer now: the boot code in the MBR of the OPOS disk, the
 * boot sectors of the partitions OPOS keeps, UEFI boot loaders on their FAT
 * partitions, and the MBRs of the other disks. Every system it can start
 * gets an entry in OPOS's Limine boot menu.
 */
enum installer_boot_code {
    INSTALLER_BOOT_CODE_NONE,     /* all zero: the MBR boots nothing */
    INSTALLER_BOOT_CODE_LIMINE,   /* Limine, e.g. an earlier OPOS */
    INSTALLER_BOOT_CODE_GRUB,     /* GNU GRUB (usually Linux) */
    /* Windows, DOS or similar code that boots the active partition. */
    INSTALLER_BOOT_CODE_STANDARD,
    INSTALLER_BOOT_CODE_UNKNOWN,
};

/* One boot menu entry for another system. */
struct installer_boot_entry {
    char title[48];
    bool uefi;
    /* BIOS: 1-4 chainloads that partition's boot sector; 0 the MBR.
       UEFI: the partition holding `path`. */
    uint8_t partition;
    /* Another disk (BIOS only), found by its MBR signature or GPT GUID;
       both empty for the OPOS disk. */
    uint32_t mbr_id;
    char gpt_guid[40];
    char path[160];
};

#define INSTALLER_BOOT_ENTRIES_MAX 12

struct installer_boot_scan {
    /* MBR boot code of the OPOS disk (partition installs only). */
    enum installer_boot_code code;
    /* Sectors 1-62 of the OPOS disk hold data that is not Limine's (GRUB
       keeps most of itself there); Limine in the MBR overwrites it. */
    bool gap_used;
    /* A kept EFI system partition on the OPOS disk that Limine can read
       (number 0 when there is none). */
    uint32_t esp_number;
    uint32_t esp_first;
    size_t entry_count;
    struct installer_boot_entry entries[INSTALLER_BOOT_ENTRIES_MAX];
};

/* How a partition install starts OPOS on BIOS computers. The OPOS
   partition always gets a boot sector that starts Limine, so any boot
   manager can chainload it (GRUB: "chainloader +1"). */
enum installer_boot_mode {
    /* Limine replaces the MBR boot code and shows the boot menu. The old
       sectors 0-62 are saved to /disk/OPOS/BOOTBAK.BIN. */
    INSTALLER_BOOT_LIMINE_MBR,
    /* The standard MBR code stays and starts the OPOS partition, which is
       marked active; Limine's menu then offers the other systems. */
    INSTALLER_BOOT_ACTIVE,
    /* Nothing outside the OPOS partition changes. */
    INSTALLER_BOOT_KEEP,
    /* UEFI: Limine becomes the fallback loader \EFI\BOOT\BOOTX64.EFI on
       the disk's EFI system partition, with the boot menu next to it; the
       old fallback loader is kept as BOOTX64.OLD. */
    INSTALLER_BOOT_UEFI_DEFAULT,
};
#define INSTALLER_BOOT_MODES 4

/* Scans for other systems. `layout` is the planned partition table for a
   partition install (its kept partitions are examined), or NULL when the
   whole disk is erased (then only the other disks count). */
void installer_boot_scan(size_t disk, const struct installer_layout *layout,
                         struct installer_boot_scan *scan);
const char *installer_boot_code_name(enum installer_boot_code code);
/* Why `mode` cannot be used, or NULL. */
const char *installer_boot_mode_problem(const struct installer_layout *layout,
                                        const struct installer_boot_scan *scan,
                                        enum installer_boot_mode mode);
enum installer_boot_mode installer_boot_default_mode(
    const struct installer_layout *layout,
    const struct installer_boot_scan *scan);

/* Formats the target slot, installs OPOS into it, then writes the new
   partition table and, as `mode` says, boot code. Partitions not deleted
   keep their data. */
bool installer_install_partition(const struct installer_layout *layout,
                                 enum installer_boot_mode mode,
                                 installer_progress progress,
                                 const char **error);

/* Erases block disk `disk`, makes it the active disk and installs OPOS
   onto it; /disk then shows the new installation. Systems on other disks
   get boot menu entries. On failure returns false and points *error at a
   short upper-case reason. */
bool installer_install(size_t disk, installer_progress progress,
                       const char **error);

#endif
