#ifndef OPOS_PARTITION_H
#define OPOS_PARTITION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Partition tables: MBR (four primary entries) and GPT. Partition numbers
 * are 1-based table positions, as Limine numbers them: MBR slots 1-4, GPT
 * entry numbers 1-n.
 */
#define PARTITION_MAX 16

/* Stand-in MBR types for GPT partitions without an MBR equivalent. */
#define PARTITION_TYPE_MS_RESERVED 0xe3
#define PARTITION_TYPE_BIOS_BOOT 0xe4
#define PARTITION_TYPE_GPT_OTHER 0xda

enum partition_scheme {
    PARTITION_SCHEME_NONE,   /* no table (blank, whole-disk filesystem...) */
    PARTITION_SCHEME_MBR,
    PARTITION_SCHEME_GPT,
};

struct partition_entry {
    uint32_t number;
    uint64_t first;
    uint64_t sectors;
    /* MBR type byte; for GPT the closest MBR type, so code can name and
       classify both the same way (0x0c FAT32 for basic data holding FAT,
       0x07 for other basic data, 0xef EFI system, 0x83 Linux...). */
    uint8_t type;
    uint8_t status;          /* MBR boot flag */
    uint8_t type_guid[16];   /* GPT only, as stored on disk */
    uint8_t unique_guid[16];
};

struct partition_table {
    enum partition_scheme scheme;
    size_t count;
    /* More partitions than PARTITION_MAX: the rest are not listed. */
    bool truncated;
    struct partition_entry entries[PARTITION_MAX];
    /* GPT only. */
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t disk_guid[16];
};

/* Reads the table of block disk `disk`, entries in table order. False when
   the disk cannot be read; a disk without a table gives SCHEME_NONE. */
bool partition_read(size_t disk, struct partition_table *table);

/* GPT type GUIDs (on-disk byte order). */
extern const uint8_t partition_guid_basic_data[16];
extern const uint8_t partition_guid_efi_system[16];

/*
 * Changes to the GPT of the active block disk; both the primary and the
 * backup table are rewritten with fresh checksums. Existing entries are
 * removed or retyped by number; number 0 adds an entry (named "OPOS",
 * with a new unique GUID) in the first free slot and reports its number.
 */
struct partition_gpt_edit {
    uint32_t number;
    bool remove;
    uint8_t type_guid[16];
    uint64_t first;          /* new entries */
    uint64_t sectors;        /* new entries */
    uint32_t assigned;       /* out: the new entry's number */
};
bool partition_gpt_apply(struct partition_gpt_edit *edits, size_t count);

#endif
