#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "fat16.h"
#include "partition.h"

#define SECTOR_SIZE 512
#define MBR_PARTITION_TABLE_OFFSET 446
#define MBR_SIGNATURE_OFFSET 510
#define FAT16_PARTITION_TYPE_SMALL 0x04
#define FAT16_PARTITION_TYPE_LARGE 0x06
#define FAT32_PARTITION_TYPE_CHS 0x0b
#define FAT16_PARTITION_TYPE_LBA 0x0e
#define EFI_SYSTEM_PARTITION_TYPE 0xef
#define FAT32_PARTITION_TYPE_LBA 0x0c
#define GPT_PROTECTIVE_PARTITION_TYPE 0xee
#define FAT16_DIRECTORY_ENTRY_SIZE 32
#define FAT16_ATTRIBUTE_LONG_NAME 0x0f
#define FAT16_CLUSTER_COUNT_MIN 4085
#define FAT16_CLUSTER_COUNT_MAX 65525
#define FAT32_CLUSTER_COUNT_MAX 0x0ffffff5u
#define FAT32_ENTRY_MASK 0x0fffffffu
#define FAT_END_OF_CHAIN 0x0fffffffu
#define FAT32_FSINFO_LEAD_SIGNATURE 0x41615252u
#define FAT32_FSINFO_STRUCT_SIGNATURE 0x61417272u
#define FAT32_FSINFO_UNKNOWN 0xffffffffu

struct fat16_volume {
    uint32_t first_sector;
    uint32_t total_sectors;
    uint32_t first_root_sector;
    uint32_t root_sector_count;
    uint32_t first_data_sector;
    uint32_t cluster_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t fsinfo_sector;
    uint16_t reserved_sectors;
    uint16_t root_entry_count;
    uint8_t fat_count;
    uint8_t sectors_per_cluster;
    bool fat32;
};

static struct fat16_volume volume;
static bool volume_ready;
static enum fat_disk_state disk_state;
static bool fsinfo_invalidated;
/* MBR partition (1-4) to try first when mounting; 0 means none. */
static size_t preferred_partition;
static uint32_t cached_root_sector;
static bool cached_root_sector_valid;
static uint32_t cached_fat_sector;
static bool cached_fat_sector_valid;

/*
 * Reading or writing a file at an offset walks its cluster chain, and so
 * does finding its length. Copying a large file in chunks walked from the
 * first cluster every time, which grows with the square of the size. These
 * remember where a few chains were last walked to (cluster number index of
 * the chain starting at first). Any FAT change other than appending to a
 * chain forgets them. free_hint is where the last allocation ended; free
 * clusters are looked for after it first.
 */
#define CHAIN_POSITIONS 4
struct chain_position {
    uint32_t first;
    uint32_t index;
    uint32_t cluster;
};
static struct chain_position chain_positions[CHAIN_POSITIONS];
static size_t chain_position_next;
static uint32_t free_hint;

static void fat16_forget_positions(void) {
    for (size_t i = 0; i < CHAIN_POSITIONS; i++) {
        chain_positions[i].first = 0;
    }
    free_hint = 0;
}

/* The remembered position furthest along first's chain, at most index. */
static const struct chain_position *chain_position_find(uint32_t first,
                                                        uint32_t index) {
    const struct chain_position *best = NULL;
    for (size_t i = 0; i < CHAIN_POSITIONS; i++) {
        const struct chain_position *position = &chain_positions[i];
        if (position->first == first && first != 0 && position->index <= index
            && (best == NULL || position->index > best->index)) {
            best = position;
        }
    }
    return best;
}

static void chain_position_store(uint32_t first, uint32_t index,
                                 uint32_t cluster) {
    for (size_t i = 0; i < CHAIN_POSITIONS; i++) {
        if (chain_positions[i].first == first) {
            chain_positions[i].index = index;
            chain_positions[i].cluster = cluster;
            return;
        }
    }
    chain_positions[chain_position_next] = (struct chain_position){
        .first = first, .index = index, .cluster = cluster,
    };
    chain_position_next = (chain_position_next + 1) % CHAIN_POSITIONS;
}
static uint8_t root_sector_buffer[SECTOR_SIZE];
static uint8_t fat_sector_buffer[SECTOR_SIZE];
static uint8_t file_sector_buffer[SECTOR_SIZE];
/* Directory reads go through a one-sector cache (directory listings and
   lookups read entry by entry). Every write through fat_write_sectors
   drops it if it covers the cached sector. */
static uint8_t directory_sector_buffer[SECTOR_SIZE];
static uint32_t directory_cached_sector;
static bool directory_cache_valid;

static bool fat_write_sectors(uint32_t first_sector, uint8_t sector_count,
                              const uint8_t *buffer) {
    if (directory_cache_valid && directory_cached_sector >= first_sector
        && directory_cached_sector - first_sector < sector_count) {
        directory_cache_valid = false;
    }
    return block_write_sectors(first_sector, sector_count, buffer);
}

static bool directory_read_sector(uint32_t sector) {
    if (directory_cache_valid && directory_cached_sector == sector) {
        return true;
    }
    directory_cache_valid = false;
    if (!block_read_sectors(sector, 1, directory_sector_buffer)) {
        return false;
    }
    directory_cached_sector = sector;
    directory_cache_valid = true;
    return true;
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static void write_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

/* Directory entries keep the cluster's low half at offset 26; FAT32 stores
   the high half at offset 20, which FAT16 requires to be zero. */
static uint32_t read_cluster(const uint8_t *raw_entry) {
    uint32_t cluster = read_le16(raw_entry + 26);
    if (volume.fat32) {
        cluster |= (uint32_t)read_le16(raw_entry + 20) << 16;
    }
    return cluster;
}

static void write_cluster(uint8_t *raw_entry, uint32_t cluster) {
    write_le16(raw_entry + 26, (uint16_t)cluster);
    write_le16(raw_entry + 20, volume.fat32 ? (uint16_t)(cluster >> 16) : 0);
}

static uint32_t directory_start_cluster(uint32_t directory_cluster) {
    return directory_cluster == 0 && volume.fat32
        ? volume.root_cluster : directory_cluster;
}

static bool is_power_of_two(uint8_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

static bool supported_partition_type(uint8_t type) {
    return type == FAT16_PARTITION_TYPE_SMALL
        || type == FAT16_PARTITION_TYPE_LARGE
        || type == FAT32_PARTITION_TYPE_CHS
        || type == FAT32_PARTITION_TYPE_LBA
        || type == FAT16_PARTITION_TYPE_LBA;
}

static bool parse_volume(uint32_t first_sector, uint32_t partition_sectors,
                         struct fat16_volume *parsed) {
    uint8_t boot_sector[SECTOR_SIZE];
    if (parsed == NULL
        || !block_read_sectors(first_sector, 1, boot_sector)
        || boot_sector[510] != 0x55 || boot_sector[511] != 0xaa) {
        return false;
    }

    uint16_t bytes_per_sector = read_le16(boot_sector + 11);
    uint8_t sectors_per_cluster = boot_sector[13];
    uint16_t reserved_sectors = read_le16(boot_sector + 14);
    uint8_t fat_count = boot_sector[16];
    uint16_t root_entries = read_le16(boot_sector + 17);
    uint32_t total_sectors = read_le16(boot_sector + 19);
    if (total_sectors == 0) {
        total_sectors = read_le32(boot_sector + 32);
    }
    uint32_t sectors_per_fat = read_le16(boot_sector + 22);
    bool fat32 = root_entries == 0 && sectors_per_fat == 0;
    if (fat32) {
        sectors_per_fat = read_le32(boot_sector + 36);
        if (read_le16(boot_sector + 42) != 0) {
            return false;
        }
    }

    if (bytes_per_sector != SECTOR_SIZE
        || !is_power_of_two(sectors_per_cluster)
        || reserved_sectors == 0 || fat_count == 0 || fat_count > 2
        || (!fat32 && root_entries == 0) || sectors_per_fat == 0
        || total_sectors == 0 || total_sectors > partition_sectors) {
        return false;
    }

    uint32_t root_sectors =
        ((uint32_t)root_entries * FAT16_DIRECTORY_ENTRY_SIZE
         + SECTOR_SIZE - 1) / SECTOR_SIZE;
    uint64_t first_root_sector = (uint64_t)first_sector + reserved_sectors
        + (uint64_t)fat_count * sectors_per_fat;
    uint64_t first_data_sector = first_root_sector + root_sectors;
    if (first_root_sector > UINT32_MAX || first_data_sector >=
        (uint64_t)first_sector + total_sectors) {
        return false;
    }

    uint32_t data_sectors =
        (uint32_t)((uint64_t)first_sector + total_sectors - first_data_sector);
    uint32_t cluster_count = data_sectors / sectors_per_cluster;
    uint32_t entry_size = fat32 ? sizeof(uint32_t) : sizeof(uint16_t);
    if ((uint64_t)sectors_per_fat * SECTOR_SIZE / entry_size
        < (uint64_t)cluster_count + 2) {
        return false;
    }
    if (fat32) {
        uint32_t root_cluster = read_le32(boot_sector + 44);
        uint16_t fsinfo = read_le16(boot_sector + 48);
        /* The BPB decides, as for Linux and UEFI firmware: mkfs.fat
           makes small FAT32 volumes (e.g. 100 MB EFI system partitions)
           with fewer clusters than the specification's FAT32 minimum. */
        if (cluster_count == 0
            || cluster_count > FAT32_CLUSTER_COUNT_MAX
            || root_cluster < 2 || root_cluster > cluster_count + 1) {
            return false;
        }
        parsed->root_cluster = root_cluster;
        parsed->fsinfo_sector = fsinfo != 0 && fsinfo < reserved_sectors
            ? first_sector + fsinfo : 0;
    } else {
        if (cluster_count < FAT16_CLUSTER_COUNT_MIN
            || cluster_count >= FAT16_CLUSTER_COUNT_MAX) {
            return false;
        }
        parsed->root_cluster = 0;
        parsed->fsinfo_sector = 0;
    }

    parsed->first_sector = first_sector;
    parsed->total_sectors = total_sectors;
    parsed->first_root_sector = (uint32_t)first_root_sector;
    parsed->root_sector_count = root_sectors;
    parsed->first_data_sector = (uint32_t)first_data_sector;
    parsed->cluster_count = cluster_count;
    parsed->reserved_sectors = reserved_sectors;
    parsed->sectors_per_fat = sectors_per_fat;
    parsed->root_entry_count = root_entries;
    parsed->fat_count = fat_count;
    parsed->sectors_per_cluster = sectors_per_cluster;
    parsed->fat32 = fat32;
    return true;
}

static bool sector_is_zero(const uint8_t *sector) {
    for (size_t i = 0; i < SECTOR_SIZE; i++) {
        if (sector[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Sector 0 alone does not prove a disk is blank: whole-disk ext2/3/4, LVM,
   Linux swap, btrfs and ZFS leave it zero and keep their signatures in the
   first 128 KiB, and a partition usually starts at 1 MiB. The disk only
   counts as blank when all of those sectors are zero. */
#define BLANK_CHECK_SECTORS 256
#define BLANK_CHECK_PARTITION_SECTOR 2048

static bool disk_start_is_blank(uint64_t disk_sectors) {
    uint8_t sector[SECTOR_SIZE];
    for (uint32_t lba = 0; lba < BLANK_CHECK_SECTORS && lba < disk_sectors;
         lba++) {
        if (!block_read_sectors(lba, 1, sector) || !sector_is_zero(sector)) {
            return false;
        }
    }
    if (disk_sectors > BLANK_CHECK_PARTITION_SECTOR
        && (!block_read_sectors(BLANK_CHECK_PARTITION_SECTOR, 1, sector)
            || !sector_is_zero(sector))) {
        return false;
    }
    return true;
}

static void remember_label(void);

static bool mount_volume(uint32_t first_sector, uint32_t sectors) {
    if (!parse_volume(first_sector, sectors, &volume)) {
        volume = (struct fat16_volume){0};
        return false;
    }
    volume_ready = true;
    fat16_forget_positions();
    disk_state = FAT_DISK_MOUNTED;
    remember_label();
    return true;
}

bool fat16_initialize(void) {
    volume_ready = false;
    directory_cache_valid = false;
    volume = (struct fat16_volume){0};
    fsinfo_invalidated = false;
    cached_root_sector_valid = false;
    cached_fat_sector_valid = false;
    fat16_forget_positions();
    disk_state = FAT_DISK_NO_DEVICE;
    uint64_t disk_sectors = block_sector_count();
    uint8_t sector[SECTOR_SIZE];
    if (disk_sectors == 0) {
        return false;
    }
    if (!block_read_sectors(0, 1, sector)) {
        disk_state = FAT_DISK_READ_ERROR;
        return false;
    }

    static struct partition_table table;
    bool gpt = false;
    if (partition_read(block_active_disk(), &table)) {
        gpt = table.scheme == PARTITION_SCHEME_GPT;
        /* The preferred partition (the one OPOS booted or was installed
           on) first, then the others in table order. */
        for (size_t step = 0; step <= table.count; step++) {
            size_t i = step == 0 ? 0 : step - 1;
            const struct partition_entry *partition = &table.entries[i];
            if (step == 0) {
                for (i = 0; i < table.count; i++) {
                    if (table.entries[i].number == preferred_partition) {
                        break;
                    }
                }
                if (i == table.count) {
                    continue;
                }
                partition = &table.entries[i];
            } else if (partition->number == preferred_partition) {
                continue;
            }
            if (!supported_partition_type(partition->type)
                || partition->first == 0 || partition->sectors == 0
                || partition->first + partition->sectors > disk_sectors
                || partition->first + partition->sectors > UINT32_MAX) {
                continue;
            }
            if (mount_volume((uint32_t)partition->first,
                             (uint32_t)partition->sectors)) {
                return true;
            }
        }
    }

    if (disk_sectors <= UINT32_MAX
        && mount_volume(0, (uint32_t)disk_sectors)) {
        return true;
    }
    disk_state = sector_is_zero(sector) && disk_start_is_blank(disk_sectors)
        ? FAT_DISK_BLANK
        : gpt ? FAT_DISK_GPT : FAT_DISK_UNRECOGNIZED;
    return false;
}

bool fat16_mounted(void) {
    return volume_ready;
}

enum fat_volume_type fat16_volume_type(void) {
    if (!volume_ready) {
        return FAT_VOLUME_NONE;
    }
    return volume.fat32 ? FAT_VOLUME_FAT32 : FAT_VOLUME_FAT16;
}

enum fat_disk_state fat16_disk_state(void) {
    return disk_state;
}

const char *fat16_disk_state_description(enum fat_disk_state state) {
    switch (state) {
    case FAT_DISK_NO_DEVICE:
        return "no usable disk";
    case FAT_DISK_READ_ERROR:
        return "disk could not be read";
    case FAT_DISK_BLANK:
        return "blank disk (no partition table or filesystem)";
    case FAT_DISK_GPT:
        return "GPT disk without a FAT partition";
    case FAT_DISK_UNRECOGNIZED:
        return "no FAT16 or FAT32 filesystem recognized";
    case FAT_DISK_MOUNTED:
        return "FAT volume mounted at /disk";
    }
    return "unknown";
}

uint64_t fat16_volume_sectors(void) {
    return volume_ready ? volume.total_sectors : 0;
}

size_t fat16_root_entry_count(void) {
    return volume_ready ? volume.root_entry_count : 0;
}

bool fat16_root_entry_at(size_t index, struct fat16_directory_entry *entry) {
    if (!volume_ready || entry == NULL || index >= volume.root_entry_count) {
        return false;
    }

    uint32_t sector_number =
        volume.first_root_sector + (uint32_t)(index * FAT16_DIRECTORY_ENTRY_SIZE
                                               / SECTOR_SIZE);
    if (!cached_root_sector_valid || cached_root_sector != sector_number) {
        if (!block_read_sectors(sector_number, 1, root_sector_buffer)) {
            cached_root_sector_valid = false;
            return false;
        }
        cached_root_sector = sector_number;
        cached_root_sector_valid = true;
    }

    size_t entry_offset = index * FAT16_DIRECTORY_ENTRY_SIZE % SECTOR_SIZE;
    const uint8_t *raw_entry = root_sector_buffer + entry_offset;
    for (size_t i = 0; i < sizeof(entry->name); i++) {
        entry->name[i] = raw_entry[i];
    }
    entry->attributes = raw_entry[11];
    entry->first_cluster = read_cluster(raw_entry);
    entry->size = read_le32(raw_entry + 28);
    return true;
}

/* All FAT sector reads and writes go through fat_sector_buffer, which caches
   the most recently transferred FAT sector so that scans for free clusters
   read each sector once instead of once per entry. */
static bool fat16_load_fat_sector(uint32_t lba) {
    if (cached_fat_sector_valid && cached_fat_sector == lba) {
        return true;
    }
    cached_fat_sector_valid = false;
    if (!block_read_sectors(lba, 1, fat_sector_buffer)) {
        return false;
    }
    cached_fat_sector = lba;
    cached_fat_sector_valid = true;
    return true;
}

static bool fat16_fat_entry_location(uint32_t cluster, uint32_t *fat_sector,
                                     size_t *entry_offset) {
    uint32_t entry_size = volume.fat32 ? sizeof(uint32_t) : sizeof(uint16_t);
    uint64_t fat_byte_offset = (uint64_t)cluster * entry_size;
    if (cluster < 2 || cluster > volume.cluster_count + 1
        || fat_byte_offset / SECTOR_SIZE >= volume.sectors_per_fat) {
        return false;
    }
    *fat_sector = (uint32_t)(fat_byte_offset / SECTOR_SIZE);
    *entry_offset = (size_t)(fat_byte_offset % SECTOR_SIZE);
    return true;
}

static bool fat16_read_fat_entry(uint32_t cluster, uint32_t *value) {
    uint32_t fat_sector;
    size_t entry_offset;
    if (value == NULL
        || !fat16_fat_entry_location(cluster, &fat_sector, &entry_offset)) {
        return false;
    }

    uint32_t lba = volume.first_sector + volume.reserved_sectors + fat_sector;
    if (!fat16_load_fat_sector(lba)) {
        return false;
    }
    *value = volume.fat32
        ? read_le32(fat_sector_buffer + entry_offset) & FAT32_ENTRY_MASK
        : read_le16(fat_sector_buffer + entry_offset);
    return true;
}

/* A FAT32 volume's FSInfo free-cluster count and next-free hint become stale
   once OPOS changes the FAT, so mark both unknown before the first change. */
static bool fat16_invalidate_fsinfo(void) {
    if (!volume.fat32 || volume.fsinfo_sector == 0 || fsinfo_invalidated) {
        return true;
    }
    uint8_t fsinfo[SECTOR_SIZE];
    if (!block_read_sectors(volume.fsinfo_sector, 1, fsinfo)) {
        return false;
    }
    if (read_le32(fsinfo) == FAT32_FSINFO_LEAD_SIGNATURE
        && read_le32(fsinfo + 484) == FAT32_FSINFO_STRUCT_SIGNATURE
        && (read_le32(fsinfo + 488) != FAT32_FSINFO_UNKNOWN
            || read_le32(fsinfo + 492) != FAT32_FSINFO_UNKNOWN)) {
        write_le32(fsinfo + 488, FAT32_FSINFO_UNKNOWN);
        write_le32(fsinfo + 492, FAT32_FSINFO_UNKNOWN);
        if (!fat_write_sectors(volume.fsinfo_sector, 1, fsinfo)) {
            return false;
        }
    }
    fsinfo_invalidated = true;
    return true;
}

static bool fat16_write_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_sector;
    size_t entry_offset;
    for (size_t i = 0; i < CHAIN_POSITIONS; i++) {
        chain_positions[i].first = 0;
    }
    if (!fat16_fat_entry_location(cluster, &fat_sector, &entry_offset)
        || !fat16_invalidate_fsinfo()) {
        return false;
    }

    for (uint8_t copy = 0; copy < volume.fat_count; copy++) {
        uint32_t lba = volume.first_sector + volume.reserved_sectors
            + (uint32_t)copy * volume.sectors_per_fat + fat_sector;
        if (!fat16_load_fat_sector(lba)) {
            return false;
        }
        if (volume.fat32) {
            uint32_t preserved = read_le32(fat_sector_buffer + entry_offset)
                & ~FAT32_ENTRY_MASK;
            write_le32(fat_sector_buffer + entry_offset,
                       preserved | (value & FAT32_ENTRY_MASK));
        } else {
            write_le16(fat_sector_buffer + entry_offset, (uint16_t)value);
        }
        if (!fat_write_sectors(lba, 1, fat_sector_buffer)) {
            cached_fat_sector_valid = false;
            return false;
        }
    }
    return true;
}

/*
 * Changes to FAT entries that share a FAT sector, written together: one
 * read and one write per FAT copy instead of per entry. Allocating or
 * freeing a long chain changes thousands of neighbouring entries.
 */
#define FAT_BATCH_MAX 256
static struct {
    uint32_t sector;
    size_t count;
    uint32_t clusters[FAT_BATCH_MAX];
    uint32_t values[FAT_BATCH_MAX];
} fat_batch;

static bool fat_batch_flush(void) {
    if (fat_batch.count == 0) {
        return true;
    }
    for (size_t i = 0; i < CHAIN_POSITIONS; i++) {
        chain_positions[i].first = 0;
    }
    size_t count = fat_batch.count;
    fat_batch.count = 0;
    if (!fat16_invalidate_fsinfo()) {
        return false;
    }
    for (uint8_t copy = 0; copy < volume.fat_count; copy++) {
        uint32_t lba = volume.first_sector + volume.reserved_sectors
            + (uint32_t)copy * volume.sectors_per_fat + fat_batch.sector;
        if (!fat16_load_fat_sector(lba)) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            uint32_t fat_sector;
            size_t entry_offset;
            if (!fat16_fat_entry_location(fat_batch.clusters[i], &fat_sector,
                                          &entry_offset)) {
                return false;
            }
            uint32_t value = fat_batch.values[i];
            if (volume.fat32) {
                uint32_t preserved = read_le32(fat_sector_buffer
                                               + entry_offset)
                    & ~FAT32_ENTRY_MASK;
                write_le32(fat_sector_buffer + entry_offset,
                           preserved | (value & FAT32_ENTRY_MASK));
            } else {
                write_le16(fat_sector_buffer + entry_offset, (uint16_t)value);
            }
        }
        if (!fat_write_sectors(lba, 1, fat_sector_buffer)) {
            cached_fat_sector_valid = false;
            return false;
        }
    }
    return true;
}

/* Queues cluster's FAT entry; fat_batch_flush writes the queue. */
static bool fat_batch_add(uint32_t cluster, uint32_t value) {
    uint32_t fat_sector;
    size_t entry_offset;
    if (!fat16_fat_entry_location(cluster, &fat_sector, &entry_offset)) {
        return false;
    }
    if (fat_batch.count != 0
        && (fat_batch.sector != fat_sector
            || fat_batch.count == FAT_BATCH_MAX)
        && !fat_batch_flush()) {
        return false;
    }
    fat_batch.sector = fat_sector;
    fat_batch.clusters[fat_batch.count] = cluster;
    fat_batch.values[fat_batch.count] = value;
    fat_batch.count++;
    return true;
}

static bool fat16_get_next_cluster(uint32_t cluster, uint32_t *next_cluster,
                                   bool *end_of_chain) {
    uint32_t value;
    if (next_cluster == NULL || end_of_chain == NULL
        || !fat16_read_fat_entry(cluster, &value)) {
        return false;
    }
    uint32_t reserved_first = volume.fat32 ? 0x0ffffff0u : 0xfff0u;
    uint32_t end_first = volume.fat32 ? 0x0ffffff8u : 0xfff8u;
    if (value >= end_first) {
        *end_of_chain = true;
        *next_cluster = 0;
        return true;
    }
    if (value < 2 || value >= reserved_first
        || value > volume.cluster_count + 1) {
        return false;
    }
    *end_of_chain = false;
    *next_cluster = value;
    return true;
}

bool fat16_read_root_file(const uint8_t name[11], uint8_t *buffer,
                          size_t buffer_capacity, size_t *file_size) {
    if (file_size == NULL) {
        return false;
    }
    *file_size = 0;
    if (!volume_ready || name == NULL
        || (buffer == NULL && buffer_capacity != 0)) {
        return false;
    }

    struct fat16_directory_entry entry;
    bool found = false;
    for (size_t i = 0; i < volume.root_entry_count; i++) {
        if (!fat16_root_entry_at(i, &entry)) {
            return false;
        }
        if (entry.name[0] == 0) {
            break;
        }
        if (entry.name[0] == 0xe5
            || entry.attributes == FAT16_ATTRIBUTE_LONG_NAME
            || (entry.attributes & (FAT16_ATTRIBUTE_VOLUME_ID
                                    | FAT16_ATTRIBUTE_DIRECTORY)) != 0) {
            continue;
        }
        size_t name_index = 0;
        while (name_index < sizeof(entry.name)
               && entry.name[name_index] == name[name_index]) {
            name_index++;
        }
        if (name_index == sizeof(entry.name)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }

    *file_size = entry.size;
    if (entry.size > buffer_capacity || (entry.size != 0 && buffer == NULL)) {
        return false;
    }
    if (entry.size == 0) {
        return true;
    }
    if (entry.first_cluster < 2
        || (uint32_t)entry.first_cluster > volume.cluster_count + 1) {
        return false;
    }

    size_t remaining = entry.size;
    size_t output_offset = 0;
    uint32_t cluster = entry.first_cluster;
    for (uint32_t visited = 0; visited < volume.cluster_count; visited++) {
        if (cluster < 2 || (uint32_t)cluster > volume.cluster_count + 1) {
            return false;
        }
        uint64_t cluster_sector = (uint64_t)volume.first_data_sector
            + (uint64_t)(cluster - 2) * volume.sectors_per_cluster;
        uint64_t volume_end = (uint64_t)volume.first_sector + volume.total_sectors;
        if (cluster_sector + volume.sectors_per_cluster > volume_end) {
            return false;
        }

        for (uint8_t i = 0; i < volume.sectors_per_cluster && remaining != 0;
             i++) {
            if (!block_read_sectors((uint32_t)cluster_sector + i, 1,
                                  file_sector_buffer)) {
                return false;
            }
            size_t bytes_to_copy = remaining < SECTOR_SIZE
                ? remaining : SECTOR_SIZE;
            for (size_t j = 0; j < bytes_to_copy; j++) {
                buffer[output_offset + j] = file_sector_buffer[j];
            }
            output_offset += bytes_to_copy;
            remaining -= bytes_to_copy;
        }
        if (remaining == 0) {
            return true;
        }

        uint32_t next_cluster;
        bool end_of_chain;
        if (!fat16_get_next_cluster(cluster, &next_cluster, &end_of_chain)
            || end_of_chain) {
            return false;
        }
        cluster = next_cluster;
    }
    return false;
}

static bool names_equal(const uint8_t left[11], const uint8_t right[11]) {
    for (size_t i = 0; i < 11; i++) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

static bool fat16_find_free_cluster(uint32_t after, uint32_t *cluster) {
    if (cluster == NULL) {
        return false;
    }
    uint32_t first = after < 2 ? 2 : (uint32_t)after + 1;
    for (uint32_t candidate = first;
         candidate <= volume.cluster_count + 1; candidate++) {
        uint32_t value;
        if (!fat16_read_fat_entry((uint32_t)candidate, &value)) {
            return false;
        }
        if (value == 0) {
            *cluster = (uint32_t)candidate;
            return true;
        }
    }
    return false;
}

static bool fat16_chain_length(uint32_t first_cluster, uint32_t *length,
                               uint32_t *last_cluster) {
    if (length == NULL || last_cluster == NULL) {
        return false;
    }
    *length = 0;
    *last_cluster = 0;
    if (first_cluster == 0) {
        return true;
    }
    uint32_t cluster = first_cluster;
    const struct chain_position *known =
        chain_position_find(first_cluster, UINT32_MAX);
    if (known != NULL) {
        cluster = known->cluster;
        *length = known->index;
    }
    for (uint32_t visited = 0; visited < volume.cluster_count; visited++) {
        if (cluster < 2 || (uint32_t)cluster > volume.cluster_count + 1) {
            return false;
        }
        (*length)++;
        *last_cluster = cluster;
        uint32_t next;
        bool end_of_chain;
        if (!fat16_get_next_cluster(cluster, &next, &end_of_chain)) {
            return false;
        }
        if (end_of_chain) {
            chain_position_store(first_cluster, *length - 1, cluster);
            return true;
        }
        cluster = next;
    }
    return false;
}

bool fat16_write_root_file(const uint8_t name[11], const uint8_t *buffer,
                           size_t file_size) {
    if (!volume_ready || name == NULL || file_size > UINT32_MAX
        || (file_size != 0 && buffer == NULL)) {
        return false;
    }

    struct fat16_directory_entry entry;
    size_t entry_index = 0;
    size_t available_index = SIZE_MAX;
    bool found = false;
    for (; entry_index < volume.root_entry_count; entry_index++) {
        if (!fat16_root_entry_at(entry_index, &entry)) {
            return false;
        }
        if (entry.name[0] == 0) {
            if (available_index == SIZE_MAX) {
                available_index = entry_index;
            }
            break;
        }
        if (entry.name[0] == 0xe5 && available_index == SIZE_MAX) {
            available_index = entry_index;
        }
        if (entry.name[0] != 0xe5
            && entry.attributes != FAT16_ATTRIBUTE_LONG_NAME
            && (entry.attributes & (FAT16_ATTRIBUTE_VOLUME_ID
                                    | FAT16_ATTRIBUTE_DIRECTORY)) == 0
            && names_equal(entry.name, name)) {
            found = true;
            break;
        }
    }
    if (!found) {
        if (available_index == SIZE_MAX) {
            return false;
        }
        entry_index = available_index;
        entry = (struct fat16_directory_entry){0};
    }
    if (entry.first_cluster != 0
        && (entry.first_cluster < 2
            || (uint32_t)entry.first_cluster > volume.cluster_count + 1)) {
        return false;
    }

    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t required_clusters = file_size == 0 ? 0
        : (uint32_t)((file_size + cluster_bytes - 1) / cluster_bytes);
    uint32_t existing_clusters;
    uint32_t existing_tail;
    if (!fat16_chain_length(entry.first_cluster, &existing_clusters,
                            &existing_tail)) {
        return false;
    }
    uint32_t additional_clusters = required_clusters > existing_clusters
        ? required_clusters - existing_clusters : 0;

    uint32_t free_cursor = 1;
    for (uint32_t i = 0; i < additional_clusters; i++) {
        if (!fat16_find_free_cluster(free_cursor, &free_cursor)) {
            return false;
        }
    }

    size_t remaining = file_size;
    size_t input_offset = 0;
    uint32_t cluster = entry.first_cluster;
    free_cursor = 1;
    for (uint32_t position = 0; position < required_clusters; position++) {
        bool existing = position < existing_clusters;
        if (!existing && !fat16_find_free_cluster(free_cursor, &cluster)) {
            return false;
        }
        if (!existing) {
            free_cursor = cluster;
        }
        uint64_t cluster_sector = (uint64_t)volume.first_data_sector
            + (uint64_t)(cluster - 2) * volume.sectors_per_cluster;
        uint64_t volume_end = (uint64_t)volume.first_sector + volume.total_sectors;
        if (cluster_sector + volume.sectors_per_cluster > volume_end) {
            return false;
        }
        for (uint8_t i = 0; i < volume.sectors_per_cluster && remaining != 0;
             i++) {
            size_t bytes_to_copy = remaining < SECTOR_SIZE
                ? remaining : SECTOR_SIZE;
            for (size_t j = 0; j < SECTOR_SIZE; j++) {
                file_sector_buffer[j] = j < bytes_to_copy
                    ? buffer[input_offset + j] : 0;
            }
            if (!fat_write_sectors((uint32_t)cluster_sector + i, 1,
                                     file_sector_buffer)) {
                return false;
            }
            input_offset += bytes_to_copy;
            remaining -= bytes_to_copy;
        }
        if (position + 1 < existing_clusters) {
            uint32_t next_cluster;
            bool end_of_chain;
            if (!fat16_get_next_cluster(cluster, &next_cluster, &end_of_chain)
                || end_of_chain) {
                return false;
            }
            cluster = next_cluster;
        }
    }

    uint32_t first_new_cluster = 0;
    if (additional_clusters != 0) {
        free_cursor = 1;
        uint32_t current_new;
        if (!fat16_find_free_cluster(free_cursor, &current_new)) {
            return false;
        }
        first_new_cluster = current_new;
        for (uint32_t i = 0; i < additional_clusters; i++) {
            uint32_t next_new = FAT_END_OF_CHAIN;
            if (i + 1 < additional_clusters
                && !fat16_find_free_cluster(current_new, &next_new)) {
                return false;
            }
            if (!fat16_write_fat_entry(current_new, next_new)) {
                return false;
            }
            current_new = next_new;
        }
        if (existing_clusters != 0
            && !fat16_write_fat_entry(existing_tail, first_new_cluster)) {
            return false;
        }
    }

    uint32_t root_sector = volume.first_root_sector
        + (uint32_t)(entry_index * FAT16_DIRECTORY_ENTRY_SIZE / SECTOR_SIZE);
    if (!cached_root_sector_valid || cached_root_sector != root_sector) {
        if (!block_read_sectors(root_sector, 1, root_sector_buffer)) {
            return false;
        }
        cached_root_sector = root_sector;
        cached_root_sector_valid = true;
    }
    size_t entry_offset = entry_index * FAT16_DIRECTORY_ENTRY_SIZE % SECTOR_SIZE;
    uint8_t *raw_entry = root_sector_buffer + entry_offset;
    if (!found) {
        for (size_t i = 0; i < FAT16_DIRECTORY_ENTRY_SIZE; i++) {
            raw_entry[i] = 0;
        }
        for (size_t i = 0; i < 11; i++) {
            raw_entry[i] = name[i];
        }
        raw_entry[11] = 0x20;
    }
    if (entry.first_cluster == 0 && first_new_cluster != 0) {
        write_cluster(raw_entry, first_new_cluster);
    }
    write_le32(raw_entry + 28, (uint32_t)file_size);
    if (!fat_write_sectors(root_sector, 1, root_sector_buffer)) {
        cached_root_sector_valid = false;
        return false;
    }
    return true;
}

static bool fat16_find_root_file(const uint8_t name[11],
                                 struct fat16_directory_entry *entry,
                                 size_t *entry_index) {
    if (!volume_ready || name == NULL || entry == NULL) {
        return false;
    }
    for (size_t i = 0; i < volume.root_entry_count; i++) {
        if (!fat16_root_entry_at(i, entry)) {
            return false;
        }
        if (entry->name[0] == 0) {
            break;
        }
        if (entry->name[0] != 0xe5
            && entry->attributes != FAT16_ATTRIBUTE_LONG_NAME
            && (entry->attributes & (FAT16_ATTRIBUTE_VOLUME_ID
                                     | FAT16_ATTRIBUTE_DIRECTORY)) == 0
            && names_equal(entry->name, name)) {
            if (entry_index != NULL) {
                *entry_index = i;
            }
            return true;
        }
    }
    return false;
}

bool fat16_root_file_size(const uint8_t name[11], size_t *file_size) {
    struct fat16_directory_entry entry;
    if (file_size == NULL || !fat16_find_root_file(name, &entry, NULL)) {
        return false;
    }
    *file_size = entry.size;
    return true;
}

static bool fat16_cluster_at(uint32_t first_cluster, uint32_t index,
                             uint32_t *cluster) {
    if (cluster == NULL || first_cluster < 2
        || (uint32_t)first_cluster > volume.cluster_count + 1) {
        return false;
    }
    uint32_t current = first_cluster;
    uint32_t start = 0;
    const struct chain_position *known =
        chain_position_find(first_cluster, index);
    if (known != NULL) {
        current = known->cluster;
        start = known->index;
    }
    for (uint32_t i = start; i < index; i++) {
        uint32_t next;
        bool end_of_chain;
        if (!fat16_get_next_cluster(current, &next, &end_of_chain)
            || end_of_chain) {
            return false;
        }
        current = next;
    }
    chain_position_store(first_cluster, index, current);
    *cluster = current;
    return true;
}

bool fat16_read_root_file_at(const uint8_t name[11], size_t offset,
                             uint8_t *buffer, size_t length,
                             size_t *bytes_read) {
    if (bytes_read == NULL || (length != 0 && buffer == NULL)) {
        return false;
    }
    *bytes_read = 0;
    struct fat16_directory_entry entry;
    if (!fat16_find_root_file(name, &entry, NULL)) {
        return false;
    }
    if (offset >= entry.size || length == 0) {
        return true;
    }
    size_t remaining = entry.size - offset;
    if (remaining > length) {
        remaining = length;
    }
    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t cluster;
    if (!fat16_cluster_at(entry.first_cluster,
                          (uint32_t)(offset / cluster_bytes), &cluster)) {
        return false;
    }
    size_t within_cluster = offset % cluster_bytes;
    size_t output_offset = 0;
    while (remaining != 0) {
        uint8_t sector_in_cluster = (uint8_t)(within_cluster / SECTOR_SIZE);
        size_t within_sector = within_cluster % SECTOR_SIZE;
        uint32_t lba = volume.first_data_sector
            + (uint32_t)(cluster - 2) * volume.sectors_per_cluster
            + sector_in_cluster;
        if (!block_read_sectors(lba, 1, file_sector_buffer)) {
            return false;
        }
        size_t amount = SECTOR_SIZE - within_sector;
        if (amount > remaining) {
            amount = remaining;
        }
        for (size_t i = 0; i < amount; i++) {
            buffer[output_offset + i] = file_sector_buffer[within_sector + i];
        }
        output_offset += amount;
        remaining -= amount;
        within_cluster += amount;
        if (remaining != 0 && within_cluster == cluster_bytes) {
            uint32_t next;
            bool end_of_chain;
            if (!fat16_get_next_cluster(cluster, &next, &end_of_chain)
                || end_of_chain) {
                return false;
            }
            cluster = next;
            within_cluster = 0;
        }
    }
    *bytes_read = output_offset;
    return true;
}

static bool fat16_ensure_chain_capacity(uint32_t first_cluster,
                                        uint32_t required_clusters,
                                        uint32_t *result_first_cluster) {
    if (result_first_cluster == NULL) {
        return false;
    }
    uint32_t existing_clusters;
    uint32_t existing_tail;
    if (!fat16_chain_length(first_cluster, &existing_clusters, &existing_tail)) {
        return false;
    }
    uint32_t additional = required_clusters > existing_clusters
        ? required_clusters - existing_clusters : 0;
    if (additional == 0) {
        *result_first_cluster = first_cluster;
        return true;
    }
    /* Enough free clusters after the hint, else from the start. */
    uint32_t search_from = free_hint;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t free_cursor = search_from;
        uint32_t found = 0;
        while (found < additional
               && fat16_find_free_cluster(free_cursor, &free_cursor)) {
            found++;
        }
        if (found == additional) {
            break;
        }
        if (search_from <= 1) {
            return false;
        }
        search_from = 1;
    }
    struct chain_position kept[CHAIN_POSITIONS];
    for (size_t i = 0; i < CHAIN_POSITIONS; i++) {
        kept[i] = chain_positions[i];
    }

    uint32_t first_new;
    if (!fat16_find_free_cluster(search_from, &first_new)) {
        return false;
    }
    uint32_t current = first_new;
    for (uint32_t i = 0; i < additional; i++) {
        uint32_t next = FAT_END_OF_CHAIN;
        if (i + 1 < additional
            && !fat16_find_free_cluster(current, &next)) {
            return false;
        }
        if (!fat_batch_add(current, next)) {
            fat_batch.count = 0;
            return false;
        }
        free_hint = current;
        current = next;
    }
    if (!fat_batch_flush()) {
        return false;
    }
    if (existing_clusters != 0
        && !fat16_write_fat_entry(existing_tail, first_new)) {
        return false;
    }
    /* Appending leaves every walked position valid. */
    for (size_t i = 0; i < CHAIN_POSITIONS; i++) {
        chain_positions[i] = kept[i];
    }
    *result_first_cluster = existing_clusters == 0
        ? first_new : first_cluster;
    return true;
}

static bool fat16_write_chain_range(uint32_t first_cluster, size_t offset,
                                    const uint8_t *buffer, size_t length,
                                    bool zero_fill) {
    if (length == 0) {
        return true;
    }
    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t cluster;
    if (!fat16_cluster_at(first_cluster, (uint32_t)(offset / cluster_bytes),
                          &cluster)) {
        return false;
    }
    size_t within_cluster = offset % cluster_bytes;
    size_t input_offset = 0;
    size_t remaining = length;
    while (remaining != 0) {
        uint8_t sector_in_cluster = (uint8_t)(within_cluster / SECTOR_SIZE);
        size_t within_sector = within_cluster % SECTOR_SIZE;
        size_t amount = SECTOR_SIZE - within_sector;
        if (amount > remaining) {
            amount = remaining;
        }
        uint32_t lba = volume.first_data_sector
            + (uint32_t)(cluster - 2) * volume.sectors_per_cluster
            + sector_in_cluster;
        if ((within_sector != 0 || amount != SECTOR_SIZE)
            && !block_read_sectors(lba, 1, file_sector_buffer)) {
            return false;
        }
        for (size_t i = 0; i < amount; i++) {
            file_sector_buffer[within_sector + i] = zero_fill
                ? 0 : buffer[input_offset + i];
        }
        if (!fat_write_sectors(lba, 1, file_sector_buffer)) {
            return false;
        }
        input_offset += amount;
        remaining -= amount;
        within_cluster += amount;
        if (remaining != 0 && within_cluster == cluster_bytes) {
            uint32_t next;
            bool end_of_chain;
            if (!fat16_get_next_cluster(cluster, &next, &end_of_chain)
                || end_of_chain) {
                return false;
            }
            cluster = next;
            within_cluster = 0;
        }
    }
    return true;
}

bool fat16_write_root_file_at(const uint8_t name[11], size_t offset,
                              const uint8_t *buffer, size_t length,
                              size_t *bytes_written) {
    if (bytes_written == NULL || (length != 0 && buffer == NULL)
        || offset > UINT32_MAX || length > UINT32_MAX - offset) {
        return false;
    }
    *bytes_written = 0;
    struct fat16_directory_entry entry;
    size_t entry_index;
    if (!fat16_find_root_file(name, &entry, &entry_index)) {
        return false;
    }
    if (length == 0) {
        return true;
    }

    size_t end = offset + length;
    size_t new_size = end > entry.size ? end : entry.size;
    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t required_clusters = (uint32_t)((new_size + cluster_bytes - 1)
                                             / cluster_bytes);
    uint32_t first_cluster;
    if (!fat16_ensure_chain_capacity(entry.first_cluster, required_clusters,
                                     &first_cluster)) {
        return false;
    }
    if (offset > entry.size
        && !fat16_write_chain_range(first_cluster, entry.size, NULL,
                                    offset - entry.size, true)) {
        return false;
    }
    if (!fat16_write_chain_range(first_cluster, offset, buffer, length, false)) {
        return false;
    }

    uint32_t root_sector = volume.first_root_sector
        + (uint32_t)(entry_index * FAT16_DIRECTORY_ENTRY_SIZE / SECTOR_SIZE);
    if (!cached_root_sector_valid || cached_root_sector != root_sector) {
        if (!block_read_sectors(root_sector, 1, root_sector_buffer)) {
            return false;
        }
        cached_root_sector = root_sector;
        cached_root_sector_valid = true;
    }
    size_t entry_offset = entry_index * FAT16_DIRECTORY_ENTRY_SIZE % SECTOR_SIZE;
    uint8_t *raw_entry = root_sector_buffer + entry_offset;
    if (entry.first_cluster == 0) {
        write_cluster(raw_entry, first_cluster);
    }
    write_le32(raw_entry + 28, (uint32_t)new_size);
    if (!fat_write_sectors(root_sector, 1, root_sector_buffer)) {
        cached_root_sector_valid = false;
        return false;
    }
    *bytes_written = length;
    return true;
}

static void fat16_node_from_raw(struct fat16_node *node, const uint8_t *raw,
                                uint32_t sector, uint16_t offset) {
    for (size_t i = 0; i < 11; i++) {
        node->name[i] = raw[i];
    }
    node->attributes = raw[11];
    node->first_cluster = read_cluster(raw);
    node->size = read_le32(raw + 28);
    node->entry_sector = sector;
    node->entry_offset = offset;
    node->case_flags = raw[12];
    node->long_entries = 0;
    node->directory_cluster = 0;
    node->entry_index = 0;
}

bool fat16_directory_capacity(uint32_t directory_cluster, size_t *capacity) {
    if (!volume_ready || capacity == NULL) {
        return false;
    }
    if (directory_cluster == 0 && !volume.fat32) {
        *capacity = volume.root_entry_count;
        return true;
    }
    uint32_t clusters;
    uint32_t tail;
    if (!fat16_chain_length(directory_start_cluster(directory_cluster),
                            &clusters, &tail)) {
        return false;
    }
    (void)tail;
    *capacity = (size_t)clusters * volume.sectors_per_cluster
        * SECTOR_SIZE / FAT16_DIRECTORY_ENTRY_SIZE;
    return true;
}

/* Sector and byte offset of directory entry index. */
static bool fat16_entry_location(uint32_t directory_cluster, size_t index,
                                 uint32_t *sector_out, uint16_t *offset_out) {
    size_t capacity;
    if (!fat16_directory_capacity(directory_cluster, &capacity)
        || index >= capacity) {
        return false;
    }
    uint32_t sector;
    uint16_t offset;
    if (directory_cluster == 0 && !volume.fat32) {
        sector = volume.first_root_sector
            + (uint32_t)(index * FAT16_DIRECTORY_ENTRY_SIZE / SECTOR_SIZE);
        offset = (uint16_t)(index * FAT16_DIRECTORY_ENTRY_SIZE % SECTOR_SIZE);
    } else {
        size_t entries_per_cluster = (size_t)volume.sectors_per_cluster
            * SECTOR_SIZE / FAT16_DIRECTORY_ENTRY_SIZE;
        uint32_t cluster;
        if (!fat16_cluster_at(directory_start_cluster(directory_cluster),
                              (uint32_t)(index / entries_per_cluster),
                              &cluster)) {
            return false;
        }
        size_t entry_in_cluster = index % entries_per_cluster;
        sector = volume.first_data_sector
            + (uint32_t)(cluster - 2) * volume.sectors_per_cluster
            + (uint32_t)(entry_in_cluster * FAT16_DIRECTORY_ENTRY_SIZE
                         / SECTOR_SIZE);
        offset = (uint16_t)(entry_in_cluster * FAT16_DIRECTORY_ENTRY_SIZE
                            % SECTOR_SIZE);
    }
    *sector_out = sector;
    *offset_out = offset;
    return true;
}

bool fat16_directory_node_at(uint32_t directory_cluster, size_t index,
                             struct fat16_node *node) {
    uint32_t sector;
    uint16_t offset;
    if (node == NULL
        || !fat16_entry_location(directory_cluster, index, &sector, &offset)
        || !directory_read_sector(sector)) {
        return false;
    }
    fat16_node_from_raw(node, directory_sector_buffer + offset, sector, offset);
    node->directory_cluster = directory_cluster;
    node->entry_index = (uint32_t)index;
    return true;
}

bool fat16_lookup_node(uint32_t directory_cluster, const uint8_t name[11],
                       struct fat16_node *node) {
    size_t capacity;
    if (name == NULL || node == NULL
        || !fat16_directory_capacity(directory_cluster, &capacity)) {
        return false;
    }
    for (size_t i = 0; i < capacity; i++) {
        struct fat16_node candidate;
        if (!fat16_directory_node_at(directory_cluster, i, &candidate)) {
            return false;
        }
        if (candidate.name[0] == 0) {
            return false;
        }
        if (candidate.name[0] != 0xe5
            && candidate.attributes != FAT16_ATTRIBUTE_LONG_NAME
            && (candidate.attributes & FAT16_ATTRIBUTE_VOLUME_ID) == 0
            && names_equal(candidate.name, name)) {
            *node = candidate;
            return true;
        }
    }
    return false;
}

static bool fat16_grow_directory(uint32_t directory_cluster,
                                 uint32_t *sector, uint16_t *offset) {
    uint32_t cluster_count;
    uint32_t tail;
    uint32_t new_cluster;
    directory_cluster = directory_start_cluster(directory_cluster);
    if (directory_cluster < 2 || sector == NULL || offset == NULL
        || !fat16_chain_length(directory_cluster, &cluster_count, &tail)
        || !fat16_find_free_cluster(1, &new_cluster)) {
        return false;
    }
    (void)cluster_count;

    for (uint8_t sector_index = 0; sector_index < volume.sectors_per_cluster;
         sector_index++) {
        for (size_t i = 0; i < SECTOR_SIZE; i++) {
            file_sector_buffer[i] = 0;
        }
        uint32_t data_sector = volume.first_data_sector
            + (uint32_t)(new_cluster - 2) * volume.sectors_per_cluster
            + sector_index;
        if (!fat_write_sectors(data_sector, 1, file_sector_buffer)) {
            return false;
        }
    }
    if (!fat16_write_fat_entry(new_cluster, FAT_END_OF_CHAIN)
        || !fat16_write_fat_entry(tail, new_cluster)) {
        return false;
    }
    *sector = volume.first_data_sector
        + (uint32_t)(new_cluster - 2) * volume.sectors_per_cluster;
    *offset = 0;
    return true;
}

static bool fat16_find_directory_slot(uint32_t directory_cluster,
                                      uint32_t *sector, uint16_t *offset) {
    size_t capacity;
    if (sector == NULL || offset == NULL
        || !fat16_directory_capacity(directory_cluster, &capacity)) {
        return false;
    }
    bool deleted_found = false;
    uint32_t deleted_sector = 0;
    uint16_t deleted_offset = 0;
    for (size_t i = 0; i < capacity; i++) {
        struct fat16_node node;
        if (!fat16_directory_node_at(directory_cluster, i, &node)) {
            return false;
        }
        if (node.name[0] == 0) {
            *sector = deleted_found ? deleted_sector : node.entry_sector;
            *offset = deleted_found ? deleted_offset : node.entry_offset;
            return true;
        }
        if (node.name[0] == 0xe5 && !deleted_found) {
            deleted_found = true;
            deleted_sector = node.entry_sector;
            deleted_offset = node.entry_offset;
        }
    }
    if (deleted_found) {
        *sector = deleted_sector;
        *offset = deleted_offset;
        return true;
    }
    return fat16_grow_directory(directory_cluster, sector, offset);
}

static bool fat16_write_directory_slot(uint32_t sector, uint16_t offset,
                                       const uint8_t name[11],
                                       uint8_t attributes,
                                       uint32_t first_cluster,
                                       uint32_t size) {
    if (offset > SECTOR_SIZE - FAT16_DIRECTORY_ENTRY_SIZE
        || !block_read_sectors(sector, 1, file_sector_buffer)) {
        return false;
    }
    uint8_t *raw = file_sector_buffer + offset;
    for (size_t i = 0; i < FAT16_DIRECTORY_ENTRY_SIZE; i++) {
        raw[i] = 0;
    }
    for (size_t i = 0; i < 11; i++) {
        raw[i] = name[i];
    }
    raw[11] = attributes;
    write_cluster(raw, first_cluster);
    write_le32(raw + 28, size);
    if (!fat_write_sectors(sector, 1, file_sector_buffer)) {
        return false;
    }
    cached_root_sector_valid = false;
    return true;
}

bool fat16_create_file_node(uint32_t directory_cluster, const uint8_t name[11],
                            struct fat16_node *node) {
    if (name == NULL || node == NULL) {
        return false;
    }
    struct fat16_node existing;
    if (fat16_lookup_node(directory_cluster, name, &existing)) {
        return false;
    }
    uint32_t sector;
    uint16_t offset;
    if (!fat16_find_directory_slot(directory_cluster, &sector, &offset)
        || !fat16_write_directory_slot(sector, offset, name, 0x20, 0, 0)) {
        return false;
    }
    *node = (struct fat16_node) {
        .attributes = 0x20,
        .first_cluster = 0,
        .size = 0,
        .entry_sector = sector,
        .entry_offset = offset,
        .directory_cluster = directory_cluster,
    };
    for (size_t i = 0; i < 11; i++) {
        node->name[i] = name[i];
    }
    return true;
}

/* Allocates and fills a new directory cluster holding "." and "..". */
static bool fat16_new_directory_cluster(uint32_t parent_cluster,
                                        uint32_t *cluster_out) {
    uint32_t cluster;
    if (!fat16_find_free_cluster(1, &cluster)) {
        return false;
    }

    uint8_t dot_name[11];
    uint8_t dotdot_name[11];
    for (size_t i = 0; i < 11; i++) {
        dot_name[i] = ' ';
        dotdot_name[i] = ' ';
    }
    dot_name[0] = '.';
    dotdot_name[0] = '.';
    dotdot_name[1] = '.';
    for (uint8_t sector_index = 0; sector_index < volume.sectors_per_cluster;
         sector_index++) {
        for (size_t i = 0; i < SECTOR_SIZE; i++) {
            file_sector_buffer[i] = 0;
        }
        if (sector_index == 0) {
            for (size_t i = 0; i < 11; i++) {
                file_sector_buffer[i] = dot_name[i];
                file_sector_buffer[FAT16_DIRECTORY_ENTRY_SIZE + i]
                    = dotdot_name[i];
            }
            file_sector_buffer[11] = FAT16_ATTRIBUTE_DIRECTORY;
            write_cluster(file_sector_buffer, cluster);
            file_sector_buffer[FAT16_DIRECTORY_ENTRY_SIZE + 11]
                = FAT16_ATTRIBUTE_DIRECTORY;
            write_cluster(file_sector_buffer + FAT16_DIRECTORY_ENTRY_SIZE,
                       parent_cluster);
        }
        uint32_t data_sector = volume.first_data_sector
            + (uint32_t)(cluster - 2) * volume.sectors_per_cluster
            + sector_index;
        if (!fat_write_sectors(data_sector, 1, file_sector_buffer)) {
            return false;
        }
    }
    if (!fat16_write_fat_entry(cluster, FAT_END_OF_CHAIN)) {
        return false;
    }
    *cluster_out = cluster;
    return true;
}

bool fat16_create_directory_node(uint32_t parent_cluster,
                                 const uint8_t name[11],
                                 struct fat16_node *node) {
    if (name == NULL || node == NULL) {
        return false;
    }
    struct fat16_node existing;
    if (fat16_lookup_node(parent_cluster, name, &existing)) {
        return false;
    }
    uint32_t entry_sector;
    uint16_t entry_offset;
    uint32_t cluster;
    if (!fat16_find_directory_slot(parent_cluster, &entry_sector, &entry_offset)
        || !fat16_new_directory_cluster(parent_cluster, &cluster)
        || !fat16_write_directory_slot(entry_sector, entry_offset, name,
                                       FAT16_ATTRIBUTE_DIRECTORY, cluster, 0)) {
        return false;
    }
    *node = (struct fat16_node) {
        .attributes = FAT16_ATTRIBUTE_DIRECTORY,
        .first_cluster = cluster,
        .size = 0,
        .entry_sector = entry_sector,
        .entry_offset = entry_offset,
        .directory_cluster = parent_cluster,
    };
    for (size_t i = 0; i < 11; i++) {
        node->name[i] = name[i];
    }
    return true;
}

static bool fat16_dot_entry(const uint8_t name[11]) {
    if (name[0] != '.' || (name[1] != ' ' && name[1] != '.')) {
        return false;
    }
    size_t start = name[1] == '.' ? 2 : 1;
    for (size_t i = start; i < 11; i++) {
        if (name[i] != ' ') {
            return false;
        }
    }
    return true;
}

bool fat16_directory_is_empty(uint32_t directory_cluster, bool *empty) {
    size_t capacity;
    if (directory_cluster < 2 || empty == NULL
        || !fat16_directory_capacity(directory_cluster, &capacity)) {
        return false;
    }
    for (size_t i = 0; i < capacity; i++) {
        struct fat16_node node;
        if (!fat16_directory_node_at(directory_cluster, i, &node)) {
            return false;
        }
        if (node.name[0] == 0) {
            *empty = true;
            return true;
        }
        if (node.name[0] != 0xe5 && !fat16_dot_entry(node.name)) {
            *empty = false;
            return true;
        }
    }
    *empty = true;
    return true;
}

bool fat16_directory_is_descendant(uint32_t directory_cluster,
                                   uint32_t ancestor_cluster,
                                   bool *descendant) {
    if (ancestor_cluster < 2 || descendant == NULL) {
        return false;
    }
    uint32_t current = directory_cluster;
    for (uint32_t visited = 0; visited <= volume.cluster_count; visited++) {
        if (current == ancestor_cluster) {
            *descendant = true;
            return true;
        }
        if (current == 0 || (volume.fat32 && current == volume.root_cluster)) {
            *descendant = false;
            return true;
        }
        if (current < 2 || current >= volume.cluster_count + 2) {
            return false;
        }
        uint32_t sector = volume.first_data_sector
            + (uint32_t)(current - 2) * volume.sectors_per_cluster;
        if (!block_read_sectors(sector, 1, file_sector_buffer)) {
            return false;
        }
        const uint8_t *raw = file_sector_buffer + FAT16_DIRECTORY_ENTRY_SIZE;
        if (raw[0] != '.' || raw[1] != '.'
            || raw[11] != FAT16_ATTRIBUTE_DIRECTORY) {
            return false;
        }
        current = read_cluster(raw);
    }
    return false;
}

static bool fat16_release_chain(uint32_t first_cluster) {
    if (first_cluster == 0) {
        return true;
    }
    uint32_t cluster = first_cluster;
    for (uint32_t visited = 0; visited < volume.cluster_count; visited++) {
        uint32_t next;
        bool end;
        if (!fat16_get_next_cluster(cluster, &next, &end)
            || !fat_batch_add(cluster, 0)) {
            fat_batch.count = 0;
            return false;
        }
        if (end) {
            return fat_batch_flush();
        }
        cluster = next;
    }
    fat_batch.count = 0;
    return false;
}

bool fat16_unlink_node(struct fat16_node *node) {
    if (node == NULL
        || node->entry_offset > SECTOR_SIZE - FAT16_DIRECTORY_ENTRY_SIZE
        || !block_read_sectors(node->entry_sector, 1, file_sector_buffer)) {
        return false;
    }
    uint8_t *raw = file_sector_buffer + node->entry_offset;
    if (!names_equal(raw, node->name)) {
        return false;
    }
    raw[0] = 0xe5;
    if (!fat_write_sectors(node->entry_sector, 1, file_sector_buffer)) {
        return false;
    }
    cached_root_sector_valid = false;
    for (size_t k = 1; k <= node->long_entries && k <= node->entry_index;
         k++) {
        uint32_t sector;
        uint16_t offset;
        if (!fat16_entry_location(node->directory_cluster,
                                  node->entry_index - k, &sector, &offset)
            || !block_read_sectors(sector, 1, file_sector_buffer)) {
            return false;
        }
        uint8_t *entry = file_sector_buffer + offset;
        if (entry[11] != FAT16_ATTRIBUTE_LONG_NAME || entry[0] == 0xe5) {
            break;
        }
        entry[0] = 0xe5;
        if (!fat_write_sectors(sector, 1, file_sector_buffer)) {
            return false;
        }
    }
    node->long_entries = 0;
    return true;
}

bool fat16_delete_node(struct fat16_node *node) {
    return node != NULL && fat16_unlink_node(node)
        && fat16_release_chain(node->first_cluster);
}

bool fat16_release_node(const struct fat16_node *node) {
    return node != NULL && fat16_release_chain(node->first_cluster);
}

static bool fat16_update_directory_parent(uint32_t directory_cluster,
                                          uint32_t parent_cluster) {
    if (directory_cluster < 2) {
        return false;
    }
    uint32_t sector = volume.first_data_sector
        + (uint32_t)(directory_cluster - 2) * volume.sectors_per_cluster;
    if (!block_read_sectors(sector, 1, file_sector_buffer)) {
        return false;
    }
    uint8_t *raw = file_sector_buffer + FAT16_DIRECTORY_ENTRY_SIZE;
    if (raw[0] != '.' || raw[1] != '.'
        || raw[11] != FAT16_ATTRIBUTE_DIRECTORY) {
        return false;
    }
    write_cluster(raw, parent_cluster);
    return fat_write_sectors(sector, 1, file_sector_buffer);
}

bool fat16_move_node(struct fat16_node *source, uint32_t source_parent,
                     uint32_t destination_parent,
                     const uint8_t destination_name[11],
                     struct fat16_node *replacement) {
    if (source == NULL || destination_name == NULL) {
        return false;
    }
    if (replacement != NULL
        && source->entry_sector == replacement->entry_sector
        && source->entry_offset == replacement->entry_offset) {
        return true;
    }

    uint32_t destination_sector;
    uint16_t destination_offset;
    if (replacement != NULL) {
        destination_sector = replacement->entry_sector;
        destination_offset = replacement->entry_offset;
    } else if (source_parent == destination_parent) {
        if (!fat16_write_directory_slot(source->entry_sector,
                                        source->entry_offset,
                                        destination_name,
                                        source->attributes,
                                        source->first_cluster,
                                        source->size)) {
            return false;
        }
        for (size_t i = 0; i < 11; i++) {
            source->name[i] = destination_name[i];
        }
        return true;
    } else if (!fat16_find_directory_slot(destination_parent,
                                           &destination_sector,
                                           &destination_offset)) {
        return false;
    }

    if (!fat16_write_directory_slot(destination_sector, destination_offset,
                                    destination_name, source->attributes,
                                    source->first_cluster, source->size)) {
        return false;
    }
    bool directory = (source->attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0;
    if (directory && source_parent != destination_parent
        && !fat16_update_directory_parent(source->first_cluster,
                                          destination_parent)) {
        return false;
    }
    if (!fat16_unlink_node(source)) {
        return false;
    }
    source->entry_sector = destination_sector;
    source->entry_offset = destination_offset;
    for (size_t i = 0; i < 11; i++) {
        source->name[i] = destination_name[i];
    }
    return true;
}

/* ------------------------------------------------------ long file names */

/*
 * VFAT long file names. A long name is stored as up to 20 extra directory
 * entries (attribute 0x0f, 13 UCS-2 characters each) placed directly before
 * the classic 8.3 entry, last part first, each carrying a checksum of the
 * 8.3 name. Names that already are plain upper-case 8.3 names get no long
 * entries, like other systems do. Only ASCII is created; other characters
 * read from disk are shown as '?'.
 */

#define LFN_CHARACTERS_PER_ENTRY 13
#define LFN_MAX_ENTRIES 20
#define LFN_LAST_ENTRY 0x40
#define SFN_LOWER_BASE 0x08
#define SFN_LOWER_EXTENSION 0x10

static const uint8_t lfn_character_offsets[LFN_CHARACTERS_PER_ENTRY] = {
    1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30,
};

static uint8_t lfn_checksum(const uint8_t short_name[11]) {
    uint8_t sum = 0;
    for (size_t i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + short_name[i]);
    }
    return sum;
}

static char ascii_upper(char character) {
    return character >= 'a' && character <= 'z'
        ? (char)(character - ('a' - 'A')) : character;
}

static bool names_equal_ignore_case(const char *left, size_t left_length,
                                    const char *right, size_t right_length) {
    if (left_length != right_length) {
        return false;
    }
    for (size_t i = 0; i < left_length; i++) {
        if (ascii_upper(left[i]) != ascii_upper(right[i])) {
            return false;
        }
    }
    return true;
}

/* Formats an 8.3 entry as "NAME.EXT", honouring the NT lower-case flags. */
static size_t format_short_name(const struct fat16_node *node, char *name,
                                size_t capacity) {
    size_t length = 0;
    for (size_t i = 0; i < 8 && node->name[i] != ' ' && length + 1 < capacity;
         i++) {
        char c = (char)node->name[i];
        if (i == 0 && node->name[0] == 0x05) {
            c = (char)0xe5;
        }
        if ((node->case_flags & SFN_LOWER_BASE) != 0 && c >= 'A' && c <= 'Z') {
            c = (char)(c + ('a' - 'A'));
        }
        name[length++] = (unsigned char)c < ' ' || (unsigned char)c > '~'
            ? '?' : c;
    }
    if (node->name[8] != ' ' && length + 1 < capacity) {
        name[length++] = '.';
        for (size_t i = 8; i < 11 && node->name[i] != ' '
             && length + 1 < capacity; i++) {
            char c = (char)node->name[i];
            if ((node->case_flags & SFN_LOWER_EXTENSION) != 0
                && c >= 'A' && c <= 'Z') {
                c = (char)(c + ('a' - 'A'));
            }
            name[length++] = (unsigned char)c < ' ' || (unsigned char)c > '~'
                ? '?' : c;
        }
    }
    if (capacity != 0) {
        name[length] = '\0';
    }
    return length;
}

bool fat16_directory_next(uint32_t directory_cluster, size_t *cursor,
                          struct fat16_node *node, char *name,
                          size_t name_capacity, size_t *name_length) {
    size_t capacity;
    if (cursor == NULL || node == NULL || name == NULL || name_capacity == 0
        || name_length == NULL
        || !fat16_directory_capacity(directory_cluster, &capacity)) {
        return false;
    }
    uint16_t long_name[LFN_MAX_ENTRIES * LFN_CHARACTERS_PER_ENTRY];
    size_t long_entries = 0;
    size_t long_total = 0;
    size_t next_sequence = 0;
    uint8_t checksum = 0;
    for (; *cursor < capacity; (*cursor)++) {
        struct fat16_node entry;
        if (!fat16_directory_node_at(directory_cluster, *cursor, &entry)) {
            return false;
        }
        if (entry.name[0] == 0) {
            return false;
        }
        if (entry.name[0] == 0xe5) {
            long_entries = 0;
            next_sequence = 0;
            continue;
        }
        if (entry.attributes == FAT16_ATTRIBUTE_LONG_NAME) {
            /* fat16_directory_node_at left the sector in the cache. */
            const uint8_t *raw = directory_sector_buffer + entry.entry_offset;
            size_t sequence = raw[0] & 0x1f;
            if ((raw[0] & LFN_LAST_ENTRY) != 0) {
                if (sequence == 0 || sequence > LFN_MAX_ENTRIES) {
                    long_entries = 0;
                    next_sequence = 0;
                    continue;
                }
                long_total = sequence;
                long_entries = 0;
                next_sequence = sequence;
                checksum = raw[13];
            } else if (next_sequence == 0 || sequence != next_sequence
                       || raw[13] != checksum) {
                long_entries = 0;
                next_sequence = 0;
                continue;
            }
            size_t base = (sequence - 1) * LFN_CHARACTERS_PER_ENTRY;
            for (size_t i = 0; i < LFN_CHARACTERS_PER_ENTRY; i++) {
                long_name[base + i] = read_le16(raw + lfn_character_offsets[i]);
            }
            long_entries++;
            next_sequence--;
            continue;
        }
        if ((entry.attributes & FAT16_ATTRIBUTE_VOLUME_ID) != 0) {
            long_entries = 0;
            next_sequence = 0;
            continue;
        }
        bool has_long = long_entries != 0 && next_sequence == 0
            && long_entries == long_total
            && lfn_checksum(entry.name) == checksum;
        size_t length = 0;
        if (has_long) {
            size_t limit = long_total * LFN_CHARACTERS_PER_ENTRY;
            for (size_t i = 0; i < limit && long_name[i] != 0
                 && length + 1 < name_capacity; i++) {
                uint16_t c = long_name[i];
                name[length++] = c < ' ' || c > '~' ? '?' : (char)c;
            }
            name[length] = '\0';
        }
        if (!has_long || length == 0) {
            has_long = false;
            length = format_short_name(&entry, name, name_capacity);
        }
        entry.long_entries = has_long ? (uint8_t)long_entries : 0;
        *node = entry;
        *name_length = length;
        (*cursor)++;
        return true;
    }
    return false;
}

bool fat16_lookup_name(uint32_t directory_cluster, const char *name,
                       size_t length, struct fat16_node *node) {
    if (name == NULL || node == NULL || length == 0) {
        return false;
    }
    size_t cursor = 0;
    struct fat16_node candidate;
    char candidate_name[FAT16_LONG_NAME_MAX + 1];
    size_t candidate_length;
    while (fat16_directory_next(directory_cluster, &cursor, &candidate,
                                candidate_name, sizeof(candidate_name),
                                &candidate_length)) {
        if (names_equal_ignore_case(candidate_name, candidate_length,
                                    name, length)) {
            *node = candidate;
            return true;
        }
        if (candidate.long_entries != 0) {
            /* The 8.3 alias (e.g. LONGNA~1.TXT) also finds the file. */
            char alias[13];
            size_t alias_length = format_short_name(&candidate, alias,
                                                    sizeof(alias));
            if (names_equal_ignore_case(alias, alias_length, name, length)) {
                *node = candidate;
                return true;
            }
        }
    }
    return false;
}

bool fat16_long_name_valid(const char *name, size_t length) {
    if (name == NULL || length == 0 || length > FAT16_LONG_NAME_MAX) {
        return false;
    }
    if ((length == 1 && name[0] == '.')
        || (length == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (c < ' ' || c > '~' || c == '<' || c == '>' || c == ':'
            || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?'
            || c == '*') {
            return false;
        }
    }
    /* Windows drops trailing spaces and dots; refuse them instead. */
    return name[length - 1] != ' ' && name[length - 1] != '.';
}

static bool short_name_character(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    const char *extra = "!#$%&'()-@^_`{}~";
    for (size_t i = 0; extra[i] != '\0'; i++) {
        if (c == extra[i]) {
            return true;
        }
    }
    return false;
}

/* True if name already is a plain upper-case 8.3 name (no long entries). */
static bool exact_short_name(const char *name, size_t length,
                             uint8_t short_name[11]) {
    for (size_t i = 0; i < 11; i++) {
        short_name[i] = ' ';
    }
    size_t base = 0;
    size_t extension = 0;
    bool dot = false;
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (c == '.') {
            if (dot || base == 0) {
                return false;
            }
            dot = true;
            continue;
        }
        if (!short_name_character(c)) {
            return false;
        }
        if (dot) {
            if (extension == 3) {
                return false;
            }
            short_name[8 + extension++] = (uint8_t)c;
        } else {
            if (base == 8) {
                return false;
            }
            short_name[base++] = (uint8_t)c;
        }
    }
    return base != 0 && (!dot || extension != 0);
}

/* Builds a unique BASIS~N.EXT alias for a long name. */
static bool generate_short_name(uint32_t directory_cluster, const char *name,
                                size_t length, uint8_t short_name[11]) {
    size_t last_dot = length;
    for (size_t i = length; i > 1; i--) {
        if (name[i - 1] == '.') {
            last_dot = i - 1;
            break;
        }
    }
    /* Lossless: only the letter case changes, so no ~N is needed. */
    bool lossless = last_dot <= 8 && length - last_dot <= 4
        && (last_dot == length || last_dot + 1 < length);
    char base[8];
    size_t base_length = 0;
    for (size_t i = 0; i < last_dot; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') {
            lossless = false;
            continue;
        }
        c = ascii_upper(c);
        if (!short_name_character(c)) {
            lossless = false;
            c = '_';
        }
        if (base_length < sizeof(base)) {
            base[base_length++] = c;
        }
    }
    if (base_length == 0) {
        base[base_length++] = '_';
    }
    uint8_t extension[3] = {' ', ' ', ' '};
    size_t extension_length = 0;
    for (size_t i = last_dot + 1; i < length && extension_length < 3; i++) {
        char c = name[i];
        if (c == ' ' || c == '.') {
            continue;
        }
        c = ascii_upper(c);
        if (!short_name_character(c)) {
            lossless = false;
            c = '_';
        }
        extension[extension_length++] = (uint8_t)c;
    }
    if (lossless) {
        for (size_t i = 0; i < 8; i++) {
            short_name[i] = i < base_length ? (uint8_t)base[i] : ' ';
        }
        for (size_t i = 0; i < 3; i++) {
            short_name[8 + i] = extension[i];
        }
        struct fat16_node existing;
        if (!fat16_lookup_node(directory_cluster, short_name, &existing)) {
            return true;
        }
    }
    for (uint32_t number = 1; number < 1000000; number++) {
        char digits[8];
        size_t digit_count = 0;
        for (uint32_t value = number; value != 0; value /= 10) {
            digits[digit_count++] = (char)('0' + value % 10);
        }
        size_t keep = 8 - 1 - digit_count;
        if (keep > base_length) {
            keep = base_length;
        }
        size_t used = 0;
        for (size_t i = 0; i < keep; i++) {
            short_name[used++] = (uint8_t)base[i];
        }
        short_name[used++] = '~';
        while (digit_count != 0) {
            short_name[used++] = (uint8_t)digits[--digit_count];
        }
        while (used < 8) {
            short_name[used++] = ' ';
        }
        for (size_t i = 0; i < 3; i++) {
            short_name[8 + i] = extension[i];
        }
        struct fat16_node existing;
        if (!fat16_lookup_node(directory_cluster, short_name, &existing)) {
            return true;
        }
    }
    return false;
}

/* Finds count consecutive free entries, growing the directory if needed. */
static bool find_free_entries(uint32_t directory_cluster, size_t count,
                              size_t *start) {
    for (size_t attempt = 0; attempt <= LFN_MAX_ENTRIES + 1; attempt++) {
        size_t capacity;
        if (!fat16_directory_capacity(directory_cluster, &capacity)) {
            return false;
        }
        size_t run = 0;
        size_t run_start = 0;
        for (size_t i = 0; i < capacity; i++) {
            struct fat16_node node;
            if (!fat16_directory_node_at(directory_cluster, i, &node)) {
                return false;
            }
            if (node.name[0] == 0) {
                /* Everything after the end marker is free as well. */
                if (run == 0) {
                    run_start = i;
                }
                if (run_start + count <= capacity) {
                    *start = run_start;
                    return true;
                }
                break;
            }
            if (node.name[0] == 0xe5) {
                if (run == 0) {
                    run_start = i;
                }
                if (++run == count) {
                    *start = run_start;
                    return true;
                }
            } else {
                run = 0;
            }
        }
        uint32_t sector;
        uint16_t offset;
        if ((directory_cluster == 0 && !volume.fat32)
            || !fat16_grow_directory(directory_cluster, &sector, &offset)) {
            return false;
        }
    }
    return false;
}

static bool write_raw_entry(uint32_t directory_cluster, size_t index,
                            const uint8_t raw[FAT16_DIRECTORY_ENTRY_SIZE]) {
    uint32_t sector;
    uint16_t offset;
    if (!fat16_entry_location(directory_cluster, index, &sector, &offset)
        || !block_read_sectors(sector, 1, file_sector_buffer)) {
        return false;
    }
    for (size_t i = 0; i < FAT16_DIRECTORY_ENTRY_SIZE; i++) {
        file_sector_buffer[offset + i] = raw[i];
    }
    if (!fat_write_sectors(sector, 1, file_sector_buffer)) {
        return false;
    }
    cached_root_sector_valid = false;
    return true;
}

/* Writes the long entries (if any) and the 8.3 entry for a name. */
static bool write_named_entries(uint32_t directory_cluster, const char *name,
                                size_t length, const uint8_t short_name[11],
                                bool long_name, uint8_t attributes,
                                uint32_t first_cluster, uint32_t size,
                                struct fat16_node *node) {
    size_t long_count = long_name
        ? (length + LFN_CHARACTERS_PER_ENTRY - 1) / LFN_CHARACTERS_PER_ENTRY
        : 0;
    size_t start;
    if (long_count > LFN_MAX_ENTRIES
        || !find_free_entries(directory_cluster, long_count + 1, &start)) {
        return false;
    }
    uint8_t checksum = lfn_checksum(short_name);
    uint8_t raw[FAT16_DIRECTORY_ENTRY_SIZE];
    for (size_t k = 0; k < long_count; k++) {
        size_t sequence = long_count - k;
        for (size_t i = 0; i < sizeof(raw); i++) {
            raw[i] = 0;
        }
        raw[0] = (uint8_t)(sequence | (k == 0 ? LFN_LAST_ENTRY : 0));
        raw[11] = FAT16_ATTRIBUTE_LONG_NAME;
        raw[13] = checksum;
        for (size_t i = 0; i < LFN_CHARACTERS_PER_ENTRY; i++) {
            size_t at = (sequence - 1) * LFN_CHARACTERS_PER_ENTRY + i;
            uint16_t c = at < length ? (uint8_t)name[at]
                : at == length ? 0 : 0xffff;
            write_le16(raw + lfn_character_offsets[i], c);
        }
        if (!write_raw_entry(directory_cluster, start + k, raw)) {
            return false;
        }
    }
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = 0;
    }
    for (size_t i = 0; i < 11; i++) {
        raw[i] = short_name[i];
    }
    raw[11] = attributes;
    write_cluster(raw, first_cluster);
    write_le32(raw + 28, size);
    size_t index = start + long_count;
    uint32_t sector;
    uint16_t offset;
    if (!write_raw_entry(directory_cluster, index, raw)
        || !fat16_entry_location(directory_cluster, index, &sector, &offset)) {
        return false;
    }
    *node = (struct fat16_node) {
        .attributes = attributes,
        .first_cluster = first_cluster,
        .size = size,
        .entry_sector = sector,
        .entry_offset = offset,
        .directory_cluster = directory_cluster,
        .entry_index = (uint32_t)index,
        .long_entries = (uint8_t)long_count,
    };
    for (size_t i = 0; i < 11; i++) {
        node->name[i] = short_name[i];
    }
    return true;
}

static bool short_name_for(uint32_t directory_cluster, const char *name,
                           size_t length, uint8_t short_name[11],
                           bool *long_name) {
    *long_name = !exact_short_name(name, length, short_name);
    return !*long_name
        || generate_short_name(directory_cluster, name, length, short_name);
}

bool fat16_create_named(uint32_t directory_cluster, const char *name,
                        size_t length, bool directory,
                        struct fat16_node *node) {
    struct fat16_node existing;
    if (node == NULL || !fat16_long_name_valid(name, length)
        || fat16_lookup_name(directory_cluster, name, length, &existing)) {
        return false;
    }
    uint8_t short_name[11];
    bool long_name;
    if (!short_name_for(directory_cluster, name, length, short_name,
                        &long_name)) {
        return false;
    }
    uint32_t cluster = 0;
    if (directory && !fat16_new_directory_cluster(directory_cluster,
                                                  &cluster)) {
        return false;
    }
    if (!write_named_entries(directory_cluster, name, length, short_name,
                             long_name, directory
                             ? FAT16_ATTRIBUTE_DIRECTORY : 0x20,
                             cluster, 0, node)) {
        if (cluster != 0) {
            (void)fat16_release_chain(cluster);
        }
        return false;
    }
    return true;
}

bool fat16_rename_named(struct fat16_node *source, uint32_t source_parent,
                        uint32_t destination_parent, const char *name,
                        size_t length, struct fat16_node *replacement) {
    if (source == NULL || !fat16_long_name_valid(name, length)) {
        return false;
    }
    if (replacement != NULL && !fat16_unlink_node(replacement)) {
        return false;
    }
    uint8_t short_name[11];
    bool long_name;
    struct fat16_node moved;
    if (!short_name_for(destination_parent, name, length, short_name,
                        &long_name)
        || !write_named_entries(destination_parent, name, length, short_name,
                                long_name, source->attributes,
                                source->first_cluster, source->size, &moved)) {
        return false;
    }
    bool directory = (source->attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0;
    if (directory && source_parent != destination_parent
        && !fat16_update_directory_parent(source->first_cluster,
                                          destination_parent)) {
        return false;
    }
    if (!fat16_unlink_node(source)) {
        return false;
    }
    *source = moved;
    return true;
}

static bool fat16_update_node(struct fat16_node *node, uint32_t first_cluster,
                              uint32_t size) {
    if (node == NULL || node->entry_offset > SECTOR_SIZE - FAT16_DIRECTORY_ENTRY_SIZE
        || !block_read_sectors(node->entry_sector, 1, file_sector_buffer)) {
        return false;
    }
    uint8_t *raw = file_sector_buffer + node->entry_offset;
    if (!names_equal(raw, node->name)) {
        return false;
    }
    write_cluster(raw, first_cluster);
    write_le32(raw + 28, size);
    if (!fat_write_sectors(node->entry_sector, 1, file_sector_buffer)) {
        return false;
    }
    cached_root_sector_valid = false;
    node->first_cluster = first_cluster;
    node->size = size;
    return true;
}

bool fat16_truncate_node(struct fat16_node *node) {
    return fat16_resize_node(node, 0);
}

static bool fat16_commit_node(struct fat16_node *node, uint32_t first_cluster,
                              uint32_t size, bool linked) {
    if (linked) {
        return fat16_update_node(node, first_cluster, size);
    }
    node->first_cluster = first_cluster;
    node->size = size;
    return true;
}

static bool fat16_resize_node_internal(struct fat16_node *node,
                                       uint32_t new_size, bool linked) {
    if (node == NULL
        || (node->attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    if (new_size == node->size) {
        return true;
    }
    uint32_t old_first_cluster = node->first_cluster;
    if (new_size == 0) {
        return fat16_commit_node(node, 0, 0, linked)
            && fat16_release_chain(old_first_cluster);
    }

    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t required_clusters = (new_size + cluster_bytes - 1) / cluster_bytes;
    if (new_size > node->size) {
        uint32_t first_cluster;
        return fat16_ensure_chain_capacity(node->first_cluster,
                                           required_clusters, &first_cluster)
            && fat16_write_chain_range(first_cluster, node->size, NULL,
                                       new_size - node->size, true)
            && fat16_commit_node(node, first_cluster, new_size, linked);
    }

    uint32_t retained_tail;
    uint32_t released_head;
    bool end;
    if (!fat16_cluster_at(node->first_cluster, required_clusters - 1,
                          &retained_tail)
        || !fat16_get_next_cluster(retained_tail, &released_head, &end)
        || !fat16_commit_node(node, node->first_cluster, new_size, linked)) {
        return false;
    }
    if (end) {
        return true;
    }
    return fat16_write_fat_entry(retained_tail, FAT_END_OF_CHAIN)
        && fat16_release_chain(released_head);
}

bool fat16_resize_node(struct fat16_node *node, uint32_t new_size) {
    return fat16_resize_node_internal(node, new_size, true);
}

bool fat16_resize_detached_node(struct fat16_node *node, uint32_t new_size) {
    return fat16_resize_node_internal(node, new_size, false);
}

bool fat16_read_node_at(const struct fat16_node *node, size_t offset,
                        uint8_t *buffer, size_t length, size_t *bytes_read) {
    if (node == NULL || bytes_read == NULL
        || (node->attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0
        || (length != 0 && buffer == NULL)) {
        return false;
    }
    *bytes_read = 0;
    if (offset >= node->size || length == 0) {
        return true;
    }
    size_t remaining = node->size - offset;
    if (remaining > length) {
        remaining = length;
    }
    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t cluster;
    if (!fat16_cluster_at(node->first_cluster,
                          (uint32_t)(offset / cluster_bytes), &cluster)) {
        return false;
    }
    size_t within_cluster = offset % cluster_bytes;
    size_t output_offset = 0;
    while (remaining != 0) {
        uint8_t sector_in_cluster = (uint8_t)(within_cluster / SECTOR_SIZE);
        size_t within_sector = within_cluster % SECTOR_SIZE;
        uint32_t sector = volume.first_data_sector
            + (uint32_t)(cluster - 2) * volume.sectors_per_cluster
            + sector_in_cluster;
        if (!block_read_sectors(sector, 1, file_sector_buffer)) {
            return false;
        }
        size_t amount = SECTOR_SIZE - within_sector;
        if (amount > remaining) {
            amount = remaining;
        }
        for (size_t i = 0; i < amount; i++) {
            buffer[output_offset + i] = file_sector_buffer[within_sector + i];
        }
        output_offset += amount;
        remaining -= amount;
        within_cluster += amount;
        if (remaining != 0 && within_cluster == cluster_bytes) {
            uint32_t next;
            bool end;
            if (!fat16_get_next_cluster(cluster, &next, &end) || end) {
                return false;
            }
            cluster = next;
            within_cluster = 0;
        }
    }
    *bytes_read = output_offset;
    return true;
}

static bool fat16_write_node_at_internal(struct fat16_node *node, size_t offset,
                                         const uint8_t *buffer, size_t length,
                                         size_t *bytes_written, bool linked) {
    if (node == NULL || bytes_written == NULL
        || (node->attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0
        || (length != 0 && buffer == NULL)
        || offset > UINT32_MAX || length > UINT32_MAX - offset) {
        return false;
    }
    *bytes_written = 0;
    if (length == 0) {
        return true;
    }
    size_t end = offset + length;
    size_t new_size = end > node->size ? end : node->size;
    size_t cluster_bytes = (size_t)volume.sectors_per_cluster * SECTOR_SIZE;
    uint32_t required_clusters = (uint32_t)((new_size + cluster_bytes - 1)
                                             / cluster_bytes);
    uint32_t first_cluster;
    if (!fat16_ensure_chain_capacity(node->first_cluster, required_clusters,
                                     &first_cluster)
        || (offset > node->size
            && !fat16_write_chain_range(first_cluster, node->size, NULL,
                                        offset - node->size, true))
        || !fat16_write_chain_range(first_cluster, offset, buffer, length, false)
        || !fat16_commit_node(node, first_cluster, (uint32_t)new_size,
                              linked)) {
        return false;
    }
    *bytes_written = length;
    return true;
}

bool fat16_write_node_at(struct fat16_node *node, size_t offset,
                         const uint8_t *buffer, size_t length,
                         size_t *bytes_written) {
    return fat16_write_node_at_internal(node, offset, buffer, length,
                                        bytes_written, true);
}

bool fat16_write_detached_node_at(struct fat16_node *node, size_t offset,
                                  const uint8_t *buffer, size_t length,
                                  size_t *bytes_written) {
    return fat16_write_node_at_internal(node, offset, buffer, length,
                                        bytes_written, false);
}

#define FORMAT_PARTITION_START 2048
#define FORMAT_MINIMUM_PARTITION_SECTORS 16384
#define FORMAT_FAT32_THRESHOLD_SECTORS 1048576
#define FORMAT_FAT16_ROOT_ENTRIES 512
#define FORMAT_FAT16_RESERVED_SECTORS 1
#define FORMAT_FAT32_RESERVED_SECTORS 32
#define FORMAT_FAT_COUNT 2
#define FORMAT_ZERO_CHUNK_SECTORS 8
#define FORMAT_GPT_SECTORS 34
#define FORMAT_MEDIA_DESCRIPTOR 0xf8

struct format_layout {
    bool fat32;
    uint32_t first_sector;
    uint32_t sectors;
    uint32_t sectors_per_fat;
    uint32_t root_sectors;
    uint32_t cluster_count;
    uint16_t reserved_sectors;
    uint16_t root_entries;
    uint8_t sectors_per_cluster;
};

static uint8_t format_zero_buffer[FORMAT_ZERO_CHUNK_SECTORS * SECTOR_SIZE];
/* Smallest reserved area for the next layout (fat16_format_volume). */
static uint16_t format_reserved_minimum;

/* Partitions above 512 MiB get FAT32 and smaller ones FAT16, with the
   cluster sizes Microsoft's format tools choose for those sizes. */
static bool format_compute_layout(uint32_t partition_sectors,
                                  struct format_layout *layout) {
    if (partition_sectors < FORMAT_MINIMUM_PARTITION_SECTORS) {
        return false;
    }
    *layout = (struct format_layout){
        .fat32 = partition_sectors > FORMAT_FAT32_THRESHOLD_SECTORS,
        .first_sector = FORMAT_PARTITION_START,
        .sectors = partition_sectors,
    };
    uint32_t entries_per_fat_sector;
    uint64_t fat_divisor;
    if (layout->fat32) {
        layout->sectors_per_cluster = partition_sectors <= 16777216u ? 8
            : partition_sectors <= 33554432u ? 16
            : partition_sectors <= 67108864u ? 32 : 64;
        layout->reserved_sectors = FORMAT_FAT32_RESERVED_SECTORS;
        layout->root_entries = 0;
        entries_per_fat_sector = SECTOR_SIZE / sizeof(uint32_t);
        fat_divisor = ((uint64_t)256 * layout->sectors_per_cluster
                       + FORMAT_FAT_COUNT) / 2;
    } else {
        layout->sectors_per_cluster = partition_sectors <= 32768u ? 2
            : partition_sectors <= 262144u ? 4
            : partition_sectors <= 524288u ? 8 : 16;
        layout->reserved_sectors = FORMAT_FAT16_RESERVED_SECTORS;
        layout->root_entries = FORMAT_FAT16_ROOT_ENTRIES;
        entries_per_fat_sector = SECTOR_SIZE / sizeof(uint16_t);
        fat_divisor = (uint64_t)256 * layout->sectors_per_cluster
            + FORMAT_FAT_COUNT;
    }
    if (layout->reserved_sectors < format_reserved_minimum) {
        layout->reserved_sectors = format_reserved_minimum;
    }
    layout->root_sectors = (uint32_t)layout->root_entries
        * FAT16_DIRECTORY_ENTRY_SIZE / SECTOR_SIZE;
    uint64_t fat_input = (uint64_t)partition_sectors
        - layout->reserved_sectors - layout->root_sectors;
    layout->sectors_per_fat =
        (uint32_t)((fat_input + fat_divisor - 1) / fat_divisor);
    uint64_t metadata = (uint64_t)layout->reserved_sectors
        + (uint64_t)FORMAT_FAT_COUNT * layout->sectors_per_fat
        + layout->root_sectors;
    if (metadata >= partition_sectors) {
        return false;
    }
    layout->cluster_count = (uint32_t)((partition_sectors - metadata)
                                       / layout->sectors_per_cluster);
    if ((uint64_t)layout->sectors_per_fat * entries_per_fat_sector
        < (uint64_t)layout->cluster_count + 2) {
        return false;
    }
    if (layout->fat32) {
        return layout->cluster_count >= FAT16_CLUSTER_COUNT_MAX
            && layout->cluster_count <= FAT32_CLUSTER_COUNT_MAX;
    }
    return layout->cluster_count >= FAT16_CLUSTER_COUNT_MIN
        && layout->cluster_count < FAT16_CLUSTER_COUNT_MAX;
}

bool fat16_format_plan(uint64_t disk_sectors, enum fat_volume_type *type,
                       uint32_t *partition_sectors) {
    /* An MBR partition must start and end below LBA 2^32. */
    uint32_t usable = disk_sectors > UINT32_MAX
        ? UINT32_MAX : (uint32_t)disk_sectors;
    struct format_layout layout;
    if (type == NULL || partition_sectors == NULL
        || usable <= FORMAT_PARTITION_START
        || !format_compute_layout(usable - FORMAT_PARTITION_START, &layout)) {
        return false;
    }
    *type = layout.fat32 ? FAT_VOLUME_FAT32 : FAT_VOLUME_FAT16;
    *partition_sectors = layout.sectors;
    return true;
}

static bool format_zero_sectors(uint32_t first_sector, uint32_t count) {
    while (count != 0) {
        uint32_t chunk = count < FORMAT_ZERO_CHUNK_SECTORS
            ? count : FORMAT_ZERO_CHUNK_SECTORS;
        if (!fat_write_sectors(first_sector, (uint8_t)chunk,
                                 format_zero_buffer)) {
            return false;
        }
        first_sector += chunk;
        count -= chunk;
    }
    return true;
}

static void format_lba_to_chs(uint32_t lba, uint8_t chs[3]) {
    uint32_t cylinder = lba / (255 * 63);
    if (cylinder > 1023) {
        chs[0] = 0xfe;
        chs[1] = 0xff;
        chs[2] = 0xff;
        return;
    }
    uint32_t head = (lba / 63) % 255;
    uint32_t sector = lba % 63 + 1;
    chs[0] = (uint8_t)head;
    chs[1] = (uint8_t)(sector | ((cylinder >> 2) & 0xc0));
    chs[2] = (uint8_t)cylinder;
}

static void format_copy_text(uint8_t *destination, const char *text,
                             size_t length) {
    for (size_t i = 0; i < length; i++) {
        destination[i] = (uint8_t)text[i];
    }
}

static void format_build_boot_sector(const struct format_layout *layout,
                                     uint32_t volume_id, uint8_t *sector) {
    for (size_t i = 0; i < SECTOR_SIZE; i++) {
        sector[i] = 0;
    }
    size_t extended = layout->fat32 ? 64 : 36;
    sector[0] = 0xeb;
    sector[1] = (uint8_t)(extended + 26 - 2);
    sector[2] = 0x90;
    format_copy_text(sector + 3, "OPOS    ", 8);
    write_le16(sector + 11, SECTOR_SIZE);
    sector[13] = layout->sectors_per_cluster;
    write_le16(sector + 14, layout->reserved_sectors);
    sector[16] = FORMAT_FAT_COUNT;
    write_le16(sector + 17, layout->root_entries);
    if (!layout->fat32 && layout->sectors <= UINT16_MAX) {
        write_le16(sector + 19, (uint16_t)layout->sectors);
    } else {
        write_le32(sector + 32, layout->sectors);
    }
    sector[21] = FORMAT_MEDIA_DESCRIPTOR;
    if (!layout->fat32) {
        write_le16(sector + 22, (uint16_t)layout->sectors_per_fat);
    }
    write_le16(sector + 24, 63);
    write_le16(sector + 26, 255);
    write_le32(sector + 28, layout->first_sector);
    if (layout->fat32) {
        write_le32(sector + 36, layout->sectors_per_fat);
        write_le32(sector + 44, 2);
        write_le16(sector + 48, 1);
        write_le16(sector + 50, 6);
    }
    sector[extended] = 0x80;
    sector[extended + 2] = 0x29;
    write_le32(sector + extended + 3, volume_id);
    format_copy_text(sector + extended + 7, "NO NAME    ", 11);
    format_copy_text(sector + extended + 18,
                     layout->fat32 ? "FAT32   " : "FAT16   ", 8);
    /* Boot code: cli; hlt; jmp back to hlt. */
    sector[extended + 26] = 0xfa;
    sector[extended + 27] = 0xf4;
    sector[extended + 28] = 0xeb;
    sector[extended + 29] = 0xfd;
    sector[510] = 0x55;
    sector[511] = 0xaa;
}

static bool format_write_fat_heads(const struct format_layout *layout,
                                   uint8_t *sector) {
    for (size_t i = 0; i < SECTOR_SIZE; i++) {
        sector[i] = 0;
    }
    if (layout->fat32) {
        write_le32(sector, 0x0ffffff8u);
        write_le32(sector + 4, FAT_END_OF_CHAIN);
        write_le32(sector + 8, FAT_END_OF_CHAIN);
    } else {
        write_le16(sector, 0xfff8);
        write_le16(sector + 2, 0xffff);
    }
    for (uint32_t copy = 0; copy < FORMAT_FAT_COUNT; copy++) {
        if (!fat_write_sectors(layout->first_sector
                                     + layout->reserved_sectors
                                     + copy * layout->sectors_per_fat,
                                 1, sector)) {
            return false;
        }
    }
    return true;
}

static bool format_write_fat32_fsinfo(const struct format_layout *layout,
                                      uint8_t *sector) {
    for (size_t i = 0; i < SECTOR_SIZE; i++) {
        sector[i] = 0;
    }
    write_le32(sector, FAT32_FSINFO_LEAD_SIGNATURE);
    write_le32(sector + 484, FAT32_FSINFO_STRUCT_SIGNATURE);
    write_le32(sector + 488, layout->cluster_count - 1);
    write_le32(sector + 492, 3);
    write_le32(sector + 508, 0xaa550000u);
    return fat_write_sectors(layout->first_sector + 1, 1, sector)
        && fat_write_sectors(layout->first_sector + 7, 1, sector);
}

static bool format_write_mbr(const struct format_layout *layout,
                             uint32_t volume_id, uint8_t *sector) {
    for (size_t i = 0; i < SECTOR_SIZE; i++) {
        sector[i] = 0;
    }
    /* Boot code: cli; int 0x18 (try the next boot device); hlt loop. */
    sector[0] = 0xfa;
    sector[1] = 0xcd;
    sector[2] = 0x18;
    sector[3] = 0xf4;
    sector[4] = 0xeb;
    sector[5] = 0xfd;
    write_le32(sector + 440, volume_id);
    uint8_t *partition = sector + MBR_PARTITION_TABLE_OFFSET;
    uint32_t last_sector = layout->first_sector + layout->sectors - 1;
    format_lba_to_chs(layout->first_sector, partition + 1);
    partition[4] = layout->fat32 ? FAT32_PARTITION_TYPE_LBA
                                 : FAT16_PARTITION_TYPE_LBA;
    format_lba_to_chs(last_sector, partition + 5);
    write_le32(partition + 8, layout->first_sector);
    write_le32(partition + 12, layout->sectors);
    sector[MBR_SIGNATURE_OFFSET] = 0x55;
    sector[MBR_SIGNATURE_OFFSET + 1] = 0xaa;
    return fat_write_sectors(0, 1, sector);
}

/* Drops the mounted volume and every cached sector before a format. */
static void forget_volume(void) {
    volume_ready = false;
    directory_cache_valid = false;
    volume = (struct fat16_volume){0};
    cached_root_sector_valid = false;
    cached_fat_sector_valid = false;
    fat16_forget_positions();
}

static uint32_t format_volume_id(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return low ^ high ^ 0x4f504f53u;
}

/* Writes the reserved area, both FAT heads, the FAT32 FSInfo and backup
   boot sector, and the boot sector last. Also zeroes the partition's last
   sector, where NTFS keeps a backup boot sector. */
static bool format_write_volume(const struct format_layout *layout,
                                uint32_t volume_id) {
    uint32_t metadata_sectors = layout->reserved_sectors
        + FORMAT_FAT_COUNT * layout->sectors_per_fat + layout->root_sectors
        + (layout->fat32 ? layout->sectors_per_cluster : 0);
    uint8_t sector[SECTOR_SIZE];
    bool written = format_zero_sectors(layout->first_sector, metadata_sectors)
        && format_zero_sectors(layout->first_sector + layout->sectors - 1, 1)
        && format_write_fat_heads(layout, sector);
    if (written && layout->fat32) {
        format_build_boot_sector(layout, volume_id, sector);
        written = fat_write_sectors(layout->first_sector + 6, 1, sector)
            && format_write_fat32_fsinfo(layout, sector);
    }
    if (written) {
        format_build_boot_sector(layout, volume_id, sector);
        written = fat_write_sectors(layout->first_sector, 1, sector);
    }
    return written;
}

/* Replaces whatever is on the active disk with an MBR holding one FAT16 or
   FAT32 partition that starts at 1 MiB, then mounts it. The partition table
   is written last, so an interrupted format leaves no half-valid volume. */
bool fat16_format_disk(void) {
    uint64_t disk_sectors = block_sector_count();
    enum fat_volume_type type;
    uint32_t partition_sectors;
    struct format_layout layout;
    if (!fat16_format_plan(disk_sectors, &type, &partition_sectors)
        || !format_compute_layout(partition_sectors, &layout)) {
        return false;
    }

    forget_volume();
    uint32_t volume_id = format_volume_id();
    uint8_t sector[SECTOR_SIZE];
    bool written = format_zero_sectors(0, FORMAT_GPT_SECTORS)
        && (disk_sectors - 1 > UINT32_MAX
            || format_zero_sectors((uint32_t)(disk_sectors
                                              - FORMAT_GPT_SECTORS + 1),
                                   FORMAT_GPT_SECTORS - 1))
        && format_write_volume(&layout, volume_id)
        && format_write_mbr(&layout, volume_id, sector);
    preferred_partition = 1;
    if (!written) {
        (void)fat16_initialize();
        return false;
    }
    return fat16_initialize() && fat16_volume_type() == type;
}

bool fat16_volume_size_ok(uint32_t sectors) {
    struct format_layout layout;
    return format_compute_layout(sectors, &layout);
}

bool fat16_format_volume(uint32_t first_sector, uint32_t sectors,
                         uint16_t reserved_minimum, uint8_t *partition_type) {
    struct format_layout layout;
    uint64_t disk_sectors = block_sector_count();
    format_reserved_minimum = reserved_minimum;
    bool planned = format_compute_layout(sectors, &layout);
    format_reserved_minimum = 0;
    if (first_sector == 0 || (uint64_t)first_sector + sectors > disk_sectors
        || !planned) {
        return false;
    }
    layout.first_sector = first_sector;
    forget_volume();
    disk_state = FAT_DISK_UNRECOGNIZED;
    if (!format_write_volume(&layout, format_volume_id())) {
        return false;
    }
    if (partition_type != NULL) {
        *partition_type = layout.fat32 ? FAT32_PARTITION_TYPE_LBA
                                       : FAT16_PARTITION_TYPE_LBA;
    }
    return true;
}

bool fat16_mount_volume(uint32_t first_sector, uint32_t sectors) {
    forget_volume();
    fsinfo_invalidated = false;
    disk_state = FAT_DISK_UNRECOGNIZED;
    return mount_volume(first_sector, sectors);
}

void fat16_prefer_partition(size_t index) {
    preferred_partition = index;
}

/* ------------------------------------------------------- several volumes */

/*
 * The driver keeps one volume in the globals above. Other mounted volumes
 * wait in slots; fat16_use swaps a slot in. The read caches are dropped on
 * a swap (every write goes straight to disk, so nothing is lost). Slot 0 is
 * the system volume at /disk; it always exists, mounted or not.
 */
struct fat_volume_slot {
    bool used;
    size_t disk;
    struct fat16_volume volume;
    bool volume_ready;
    enum fat_disk_state disk_state;
    bool fsinfo_invalidated;
    char label[12];
    /* New for every extra mount, so open files can tell that their
       volume was unmounted (USB unplug) and the slot reused. */
    uint32_t generation;
    /* block_disk_generation of the disk when mounted. */
    uint32_t disk_generation;
};

static struct fat_volume_slot slots[FAT16_MAX_VOLUMES];
static uint32_t next_generation = 1;
static size_t current_slot;

static void slot_save(void) {
    struct fat_volume_slot *slot = &slots[current_slot];
    slot->used = true;
    slot->disk = block_active_disk();
    slot->volume = volume;
    slot->volume_ready = volume_ready;
    slot->disk_state = disk_state;
    slot->fsinfo_invalidated = fsinfo_invalidated;
}

static void slot_load(size_t index) {
    const struct fat_volume_slot *slot = &slots[index];
    current_slot = index;
    if (slot->disk != BLOCK_NO_DISK) {
        (void)block_select_disk(slot->disk);
    }
    volume = slot->volume;
    volume_ready = slot->volume_ready;
    disk_state = slot->disk_state;
    fsinfo_invalidated = slot->fsinfo_invalidated;
    directory_cache_valid = false;
    cached_root_sector_valid = false;
    cached_fat_sector_valid = false;
    fat16_forget_positions();
}

bool fat16_use(size_t index) {
    if (index >= FAT16_MAX_VOLUMES || (index != 0 && !slots[index].used)
        /* The disk went away (USB unplug) before storage_poll noticed:
           selecting the slot would leave another disk active, or apply
           this volume's layout to a different stick. */
        || (index != 0
            && block_disk_generation(slots[index].disk)
                   != slots[index].disk_generation)) {
        return false;
    }
    if (index != current_slot) {
        slot_save();
        slot_load(index);
    }
    return true;
}

size_t fat16_current_volume(void) {
    return current_slot;
}

/* Volume label, trimmed; "" for NO NAME. Windows and mtools keep the
   label the user set in a root directory entry with the volume ID
   attribute, and often leave the boot sector copy at NO NAME, so the root
   entry wins. */
static void read_label(char label[12]) {
    uint8_t sector[SECTOR_SIZE];
    label[0] = '\0';
    if (!volume_ready) {
        return;
    }
    const uint8_t *text = NULL;
    for (size_t index = 0; index < 64; index++) {
        struct fat16_node node;
        if (!fat16_directory_node_at(0, index, &node) || node.name[0] == 0) {
            break;
        }
        if (node.name[0] != 0xe5
            && node.attributes != FAT16_ATTRIBUTE_LONG_NAME
            && (node.attributes & FAT16_ATTRIBUTE_VOLUME_ID) != 0) {
            for (size_t i = 0; i < 11; i++) {
                sector[i] = node.name[i];
            }
            text = sector;
            break;
        }
    }
    if (text == NULL) {
        if (!block_read_sectors(volume.first_sector, 1, sector)) {
            return;
        }
        text = sector + (volume.fat32 ? 71 : 43);
    }
    size_t length = 0;
    for (size_t i = 0; i < 11; i++) {
        uint8_t character = text[i];
        label[length++] = character >= ' ' && character <= '~'
            ? (char)character : ' ';
    }
    while (length > 0 && label[length - 1] == ' ') {
        length--;
    }
    label[length] = '\0';
    static const char no_name[] = "NO NAME";
    bool unnamed = length == sizeof(no_name) - 1;
    for (size_t i = 0; unnamed && i < length; i++) {
        unnamed = label[i] == no_name[i];
    }
    if (unnamed) {
        label[0] = '\0';
    }
}

/* Called by mount_volume for the slot being mounted. */
static void remember_label(void) {
    read_label(slots[current_slot].label);
}

/* Mounts the FAT volume at first..first+sectors of `disk` into a free
   slot. Leaves slot 0 current. */
static void mount_extra(size_t disk, uint32_t first, uint32_t sectors) {
    size_t free_slot = 0;
    for (size_t i = 1; i < FAT16_MAX_VOLUMES; i++) {
        if (!slots[i].used) {
            free_slot = i;
            break;
        }
    }
    if (free_slot == 0) {
        return;
    }
    (void)fat16_use(0);
    slot_save();
    current_slot = free_slot;
    (void)block_select_disk(disk);
    forget_volume();
    fsinfo_invalidated = false;
    if (mount_volume(first, sectors)) {
        slots[free_slot].generation = next_generation++;
        slots[free_slot].disk_generation = block_disk_generation(disk);
        slot_save();
    } else {
        slots[free_slot].used = false;
    }
    slot_load(0);
}

void fat16_unmount_others(size_t disk) {
    (void)fat16_use(0);
    for (size_t i = 1; i < FAT16_MAX_VOLUMES; i++) {
        if (slots[i].used && (disk == BLOCK_NO_DISK || slots[i].disk == disk)) {
            slots[i] = (struct fat_volume_slot){0};
        }
    }
}

/* Mounts every FAT volume on one disk into free slots, skipping the
   system volume. */
static void mount_disk_volumes(size_t disk, size_t system_disk,
                               uint32_t system_first) {
    uint8_t sector[SECTOR_SIZE];
    struct block_disk_info info;
    if (!block_disk_info(disk, &info)
        || !block_disk_read(disk, 0, 1, sector)) {
        return;
    }
    /* A FAT boot sector at LBA 0 (no partition table) also ends in
       0x55AA; its boot code must not be read as partition entries. */
    bool whole_disk_fat = (sector[0] == 0xeb || sector[0] == 0xe9)
        && read_le16(sector + 11) == SECTOR_SIZE && sector[13] != 0;
    static struct partition_table table;
    if (!whole_disk_fat && partition_read(disk, &table)) {
        for (size_t i = 0; i < table.count; i++) {
            const struct partition_entry *partition = &table.entries[i];
            uint64_t first = partition->first;
            uint64_t count = partition->sectors;
            /* EFI system partitions are FAT too; they are shown but
               never chosen as the system volume. */
            if ((!supported_partition_type(partition->type)
                 && partition->type != EFI_SYSTEM_PARTITION_TYPE)
                || first == 0 || count == 0 || first + count > info.sectors
                || first + count > UINT32_MAX
                || (disk == system_disk && first == system_first)) {
                continue;
            }
            mount_extra(disk, (uint32_t)first, (uint32_t)count);
        }
    }
    if (whole_disk_fat && info.sectors <= UINT32_MAX
        && !(disk == system_disk && system_first == 0)) {
        mount_extra(disk, 0, (uint32_t)info.sectors);
    }
}

void fat16_mount_others(void) {
    fat16_unmount_others(BLOCK_NO_DISK);
    size_t system_disk = block_active_disk();
    uint32_t system_first = volume_ready ? volume.first_sector : UINT32_MAX;
    for (size_t disk = 0; disk < block_disk_count(); disk++) {
        mount_disk_volumes(disk, system_disk, system_first);
    }
}

void fat16_mount_disk(size_t disk) {
    (void)fat16_use(0);
    for (size_t i = 1; i < FAT16_MAX_VOLUMES; i++) {
        if (slots[i].used && slots[i].disk == disk) {
            return; /* already mounted */
        }
    }
    mount_disk_volumes(disk, block_active_disk(),
                       volume_ready ? volume.first_sector : UINT32_MAX);
}

uint32_t fat16_volume_generation(size_t index) {
    return index < FAT16_MAX_VOLUMES && slots[index].used
        ? slots[index].generation : 0;
}

bool fat16_volume_info(size_t index, struct fat16_volume_info *info) {
    if (index >= FAT16_MAX_VOLUMES || info == NULL) {
        return false;
    }
    if (index == current_slot) {
        slot_save();
    }
    const struct fat_volume_slot *slot = &slots[index];
    if (!slot->used || !slot->volume_ready
        || (index != 0
            && block_disk_generation(slot->disk) != slot->disk_generation)) {
        return false;
    }
    info->disk = slot->disk;
    info->first_sector = slot->volume.first_sector;
    info->sectors = slot->volume.total_sectors;
    info->fat32 = slot->volume.fat32;
    for (size_t i = 0; i < sizeof(info->label); i++) {
        info->label[i] = slot->label[i];
    }
    return true;
}

bool fat16_system_mounted(void) {
    return fat16_use(0) && fat16_mounted();
}
