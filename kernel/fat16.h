#ifndef OPOS_FAT16_H
#define OPOS_FAT16_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct fat16_directory_entry {
    uint8_t name[11];
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
};

struct fat16_node {
    uint8_t name[11];
    uint8_t attributes;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t entry_sector;
    uint16_t entry_offset;
    /* NT case flags (byte 12): lower-case base/extension of an 8.3 name. */
    uint8_t case_flags;
    /* Number of VFAT long-name entries directly before this entry. */
    uint8_t long_entries;
    /* Directory (0 = root) and entry index the node was read from. */
    uint32_t directory_cluster;
    uint32_t entry_index;
};

/* Longest VFAT long file name, in characters. */
#define FAT16_LONG_NAME_MAX 255

#define FAT16_ATTRIBUTE_DIRECTORY 0x10
#define FAT16_ATTRIBUTE_LONG_NAME 0x0f
#define FAT16_ATTRIBUTE_VOLUME_ID 0x08

/* The fat16_* interface serves both FAT16 and FAT32 volumes. Directory
   cluster 0 always names the root directory, including on FAT32 where the
   root is an ordinary cluster chain. */
enum fat_volume_type {
    FAT_VOLUME_NONE,
    FAT_VOLUME_FAT16,
    FAT_VOLUME_FAT32,
};

enum fat_disk_state {
    FAT_DISK_NO_DEVICE,
    FAT_DISK_READ_ERROR,
    FAT_DISK_BLANK,
    FAT_DISK_GPT,
    FAT_DISK_UNRECOGNIZED,
    FAT_DISK_MOUNTED,
};

bool fat16_initialize(void);
bool fat16_mounted(void);
enum fat_volume_type fat16_volume_type(void);
enum fat_disk_state fat16_disk_state(void);
const char *fat16_disk_state_description(enum fat_disk_state state);
uint64_t fat16_volume_sectors(void);
bool fat16_format_plan(uint64_t disk_sectors, enum fat_volume_type *type,
                       uint32_t *partition_sectors);
bool fat16_format_disk(void);
/* True when a FAT16/FAT32 volume of this many sectors can be made. */
bool fat16_volume_size_ok(uint32_t sectors);
/* Formats a FAT16 or FAT32 volume at first_sector on the active disk
   without touching the partition table and without mounting it. The
   reserved area gets at least reserved_minimum sectors (0: the usual
   size), e.g. to hold boot code. *partition_type gets the MBR type byte
   for the new volume. */
bool fat16_format_volume(uint32_t first_sector, uint32_t sectors,
                         uint16_t reserved_minimum, uint8_t *partition_type);
/* Mounts the FAT volume at this location, ignoring the partition table. */
bool fat16_mount_volume(uint32_t first_sector, uint32_t sectors);
/* Partition number (MBR 1-4, GPT entry number) fat16_initialize tries
   first; 0 for none. */
void fat16_prefer_partition(size_t index);

/* Several mounted volumes. Volume 0 is the system volume at /disk; the
   others (1..FAT16_MAX_VOLUMES-1) are every other FAT volume on any disk,
   shown as /disk2, /disk3 and so on. All fat16_* calls act on the current
   volume; fat16_use switches it (and the active block disk). */
#define FAT16_MAX_VOLUMES 8
struct fat16_volume_info {
    size_t disk;
    uint32_t first_sector;
    uint32_t sectors;
    bool fat32;
    char label[12];
};
bool fat16_use(size_t volume);
size_t fat16_current_volume(void);
/* Mounts every FAT volume except the system volume (MBR partitions and
   whole-disk volumes; GPT is not read). Replaces earlier extra mounts. */
void fat16_mount_others(void);
/* Drops extra volumes on `disk` (BLOCK_NO_DISK: all of them). */
void fat16_unmount_others(size_t disk);
/* Mounts the FAT volumes of one disk (a USB stick that was plugged in)
   into free slots; the other mounts keep their slots. */
void fat16_mount_disk(size_t disk);
/* Changes whenever a slot is mounted again; 0 for slot 0 and unused
   slots. */
uint32_t fat16_volume_generation(size_t volume);
bool fat16_volume_info(size_t volume, struct fat16_volume_info *info);
/* Makes the system volume (/disk) current and reports whether it is
   mounted. Code outside the VFS calls this before other fat16_* calls. */
bool fat16_system_mounted(void);
size_t fat16_root_entry_count(void);
bool fat16_root_entry_at(size_t index, struct fat16_directory_entry *entry);
bool fat16_read_root_file(const uint8_t name[11], uint8_t *buffer,
                          size_t buffer_capacity, size_t *file_size);
bool fat16_write_root_file(const uint8_t name[11], const uint8_t *buffer,
                           size_t file_size);
bool fat16_root_file_size(const uint8_t name[11], size_t *file_size);
bool fat16_read_root_file_at(const uint8_t name[11], size_t offset,
                             uint8_t *buffer, size_t length,
                             size_t *bytes_read);
bool fat16_write_root_file_at(const uint8_t name[11], size_t offset,
                              const uint8_t *buffer, size_t length,
                              size_t *bytes_written);
bool fat16_lookup_node(uint32_t directory_cluster, const uint8_t name[11],
                       struct fat16_node *node);
bool fat16_directory_capacity(uint32_t directory_cluster, size_t *capacity);
bool fat16_directory_node_at(uint32_t directory_cluster, size_t index,
                             struct fat16_node *node);
bool fat16_create_file_node(uint32_t directory_cluster, const uint8_t name[11],
                            struct fat16_node *node);
bool fat16_create_directory_node(uint32_t parent_cluster,
                                 const uint8_t name[11],
                                 struct fat16_node *node);
bool fat16_directory_is_empty(uint32_t directory_cluster, bool *empty);
bool fat16_directory_is_descendant(uint32_t directory_cluster,
                                   uint32_t ancestor_cluster,
                                   bool *descendant);
bool fat16_unlink_node(struct fat16_node *node);
bool fat16_release_node(const struct fat16_node *node);
bool fat16_delete_node(struct fat16_node *node);
bool fat16_move_node(struct fat16_node *source, uint32_t source_parent,
                     uint32_t destination_parent,
                     const uint8_t destination_name[11],
                     struct fat16_node *replacement);
bool fat16_truncate_node(struct fat16_node *node);

/*
 * Long-file-name (VFAT) interface. Names are ASCII, case-preserving and
 * matched case-insensitively; an 8.3 alias also matches.
 */
/* Next entry at or after *cursor (advanced past it); false at the end.
   name receives the long name, or the 8.3 name when there is none. */
bool fat16_directory_next(uint32_t directory_cluster, size_t *cursor,
                          struct fat16_node *node, char *name,
                          size_t name_capacity, size_t *name_length);
bool fat16_lookup_name(uint32_t directory_cluster, const char *name,
                       size_t length, struct fat16_node *node);
/* Allowed: 1-255 printable ASCII characters except < > : " / \ | ? *,
   not "." or "..", not ending in a space or dot. */
bool fat16_long_name_valid(const char *name, size_t length);
bool fat16_create_named(uint32_t directory_cluster, const char *name,
                        size_t length, bool directory,
                        struct fat16_node *node);
/* Moves/renames source; replacement (if any) is unlinked, not released. */
bool fat16_rename_named(struct fat16_node *source, uint32_t source_parent,
                        uint32_t destination_parent, const char *name,
                        size_t length, struct fat16_node *replacement);
bool fat16_resize_node(struct fat16_node *node, uint32_t new_size);
bool fat16_resize_detached_node(struct fat16_node *node, uint32_t new_size);
bool fat16_read_node_at(const struct fat16_node *node, size_t offset,
                        uint8_t *buffer, size_t length, size_t *bytes_read);
bool fat16_write_node_at(struct fat16_node *node, size_t offset,
                         const uint8_t *buffer, size_t length,
                         size_t *bytes_written);
bool fat16_write_detached_node_at(struct fat16_node *node, size_t offset,
                                  const uint8_t *buffer, size_t length,
                                  size_t *bytes_written);

#endif
