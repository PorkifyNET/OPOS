#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "partition.h"

#define SECTOR_SIZE 512
#define MBR_TABLE_OFFSET 446
#define GPT_PROTECTIVE_TYPE 0xee
/* The entry array OPOS can rewrite: 128 entries of 128 bytes is usual. */
#define GPT_ARRAY_MAX_SECTORS 64
#define GPT_ENTRY_NAME_OFFSET 56
#define GPT_ENTRY_NAME_CHARS 36

const uint8_t partition_guid_basic_data[16] = {
    0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
    0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7,
};
const uint8_t partition_guid_efi_system[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b,
};

struct known_type {
    uint8_t guid[16];
    uint8_t type;
};

static const struct known_type known_types[] = {
    /* Linux filesystem data */
    {{0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
      0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4}, 0x83},
    /* Linux x86-64 root */
    {{0xe3, 0xbc, 0x68, 0x4f, 0xcd, 0xe8, 0xb1, 0x4d,
      0x96, 0xe7, 0xfb, 0xca, 0xf9, 0x84, 0xb7, 0x09}, 0x83},
    /* Linux swap */
    {{0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
      0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f}, 0x82},
    /* Linux LVM */
    {{0x79, 0xd3, 0xd6, 0xe6, 0x07, 0xf5, 0xc2, 0x44,
      0xa2, 0x3c, 0x23, 0x8f, 0x2a, 0x3d, 0xf9, 0x28}, 0x8e},
    /* Microsoft reserved */
    {{0x16, 0xe3, 0xc9, 0xe3, 0x5c, 0x0b, 0xb8, 0x4d,
      0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae},
     PARTITION_TYPE_MS_RESERVED},
    /* Windows recovery environment */
    {{0xa4, 0xbb, 0x94, 0xde, 0xd1, 0x06, 0x40, 0x4d,
      0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac}, 0x27},
    /* BIOS boot (GRUB core image) */
    {{0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
      0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49},
     PARTITION_TYPE_BIOS_BOOT},
};

static uint8_t sector[SECTOR_SIZE];
static uint8_t array[GPT_ARRAY_MAX_SECTORS * SECTOR_SIZE];

static uint32_t le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t le64(const uint8_t *bytes) {
    return (uint64_t)le32(bytes) | ((uint64_t)le32(bytes + 4) << 32);
}

static void put32(uint8_t *bytes, uint32_t value) {
    for (size_t i = 0; i < 4; i++) {
        bytes[i] = (uint8_t)(value >> (8 * i));
    }
}

static void put64(uint8_t *bytes, uint64_t value) {
    for (size_t i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(value >> (8 * i));
    }
}

static bool same_guid(const uint8_t *a, const uint8_t *b) {
    for (size_t i = 0; i < 16; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static bool zero_guid(const uint8_t *guid) {
    for (size_t i = 0; i < 16; i++) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}

static uint32_t crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* A FAT boot sector at LBA 0 also ends in 0x55AA; its boot code must not
   be read as partition entries. */
static bool fat_boot_sector(const uint8_t *bytes) {
    return (bytes[0] == 0xeb || bytes[0] == 0xe9)
        && (bytes[11] | (bytes[12] << 8)) == SECTOR_SIZE && bytes[13] != 0
        && bytes[16] != 0;
}

static bool gpt_header_valid(const uint8_t *header, uint64_t lba) {
    static const char signature[8] = "EFI PART";
    for (size_t i = 0; i < 8; i++) {
        if (header[i] != (uint8_t)signature[i]) {
            return false;
        }
    }
    uint32_t size = le32(header + 12);
    if (size < 92 || size > SECTOR_SIZE || le64(header + 24) != lba) {
        return false;
    }
    static uint8_t copy[SECTOR_SIZE];
    for (size_t i = 0; i < size; i++) {
        copy[i] = header[i];
    }
    put32(copy + 16, 0);
    return crc32(copy, size) == le32(header + 16);
}

static uint8_t gpt_type(size_t disk, const uint8_t *guid, uint64_t first) {
    if (same_guid(guid, partition_guid_efi_system)) {
        return 0xef;
    }
    if (same_guid(guid, partition_guid_basic_data)) {
        /* Basic data holds FAT, NTFS or exFAT; only FAT is ours. */
        return first <= UINT32_MAX
                && block_disk_read(disk, (uint32_t)first, 1, sector)
                && sector[510] == 0x55 && sector[511] == 0xaa
                && fat_boot_sector(sector)
            ? 0x0c : 0x07;
    }
    for (size_t i = 0; i < sizeof(known_types) / sizeof(known_types[0]);
         i++) {
        if (same_guid(guid, known_types[i].guid)) {
            return known_types[i].type;
        }
    }
    return PARTITION_TYPE_GPT_OTHER;
}

static bool read_gpt(size_t disk, uint64_t disk_sectors,
                     struct partition_table *table) {
    if (!block_disk_read(disk, 1, 1, sector) || !gpt_header_valid(sector, 1)) {
        return false;
    }
    uint64_t entries_lba = le64(sector + 72);
    uint32_t entry_count = le32(sector + 80);
    uint32_t entry_size = le32(sector + 84);
    table->first_usable = le64(sector + 40);
    table->last_usable = le64(sector + 48);
    for (size_t i = 0; i < 16; i++) {
        table->disk_guid[i] = sector[56 + i];
    }
    if (entry_size < 128 || entry_size > SECTOR_SIZE
        || SECTOR_SIZE % entry_size != 0 || entries_lba < 2
        || entries_lba > UINT32_MAX - 1024) {
        return false;
    }
    uint32_t per_sector = SECTOR_SIZE / entry_size;
    uint32_t number = 0;
    for (uint32_t s = 0; number < entry_count; s++) {
        if (!block_disk_read(disk, (uint32_t)(entries_lba + s), 1, sector)) {
            return false;
        }
        for (uint32_t e = 0; e < per_sector && number < entry_count; e++) {
            const uint8_t *entry = sector + e * entry_size;
            number++;
            if (zero_guid(entry)) {
                continue;
            }
            uint64_t first = le64(entry + 32);
            uint64_t last = le64(entry + 40);
            if (last < first || last >= disk_sectors) {
                continue;
            }
            if (table->count >= PARTITION_MAX) {
                table->truncated = true;
                continue;
            }
            struct partition_entry *out = &table->entries[table->count++];
            *out = (struct partition_entry){.number = number,
                                            .first = first,
                                            .sectors = last - first + 1};
            for (size_t i = 0; i < 16; i++) {
                out->type_guid[i] = entry[i];
                out->unique_guid[i] = entry[16 + i];
            }
        }
    }
    /* Classifying reads other sectors through the shared buffer. */
    for (size_t i = 0; i < table->count; i++) {
        struct partition_entry *entry = &table->entries[i];
        entry->type = gpt_type(disk, entry->type_guid, entry->first);
    }
    return true;
}

bool partition_read(size_t disk, struct partition_table *table) {
    *table = (struct partition_table){0};
    struct block_disk_info info;
    if (!block_disk_info(disk, &info) || !block_disk_read(disk, 0, 1, sector)) {
        return false;
    }
    if (sector[510] != 0x55 || sector[511] != 0xaa || fat_boot_sector(sector)) {
        return true;
    }
    uint8_t mbr[4 * 16];
    bool protective = false;
    for (size_t i = 0; i < sizeof(mbr); i++) {
        mbr[i] = sector[MBR_TABLE_OFFSET + i];
    }
    for (size_t i = 0; i < 4; i++) {
        protective = protective || mbr[i * 16 + 4] == GPT_PROTECTIVE_TYPE;
    }
    if (protective) {
        table->scheme = PARTITION_SCHEME_GPT;
        if (!read_gpt(disk, info.sectors, table)) {
            /* Unreadable GPT: report the scheme with no partitions. */
            table->count = 0;
            table->truncated = true;
        }
        return true;
    }
    table->scheme = PARTITION_SCHEME_MBR;
    for (size_t i = 0; i < 4; i++) {
        const uint8_t *entry = mbr + i * 16;
        uint32_t first = le32(entry + 8);
        uint32_t count = le32(entry + 12);
        if (entry[4] == 0 || count == 0) {
            continue;
        }
        table->entries[table->count++] = (struct partition_entry){
            .number = (uint32_t)i + 1, .first = first, .sectors = count,
            .type = entry[4], .status = entry[0]};
    }
    return true;
}

/* ----------------------------------------------------------- GPT writes */

static void new_guid(uint8_t guid[16]) {
    static uint64_t counter;
    for (size_t i = 0; i < 16; i += 8) {
        uint32_t low;
        uint32_t high;
        __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
        uint64_t value = (((uint64_t)high << 32) | low) ^ (++counter * 0x9e3779b97f4a7c15ULL);
        value ^= value >> 31;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27;
        put64(guid + i, value);
    }
    guid[7] = (uint8_t)((guid[7] & 0x0f) | 0x40);   /* version 4 */
    guid[8] = (uint8_t)((guid[8] & 0x3f) | 0x80);   /* RFC 4122 variant */
}

static bool write_array(uint64_t lba, uint32_t sectors) {
    for (uint32_t s = 0; s < sectors; s++) {
        if (!block_write_sectors((uint32_t)(lba + s), 1,
                                 array + s * SECTOR_SIZE)) {
            return false;
        }
    }
    return true;
}

bool partition_gpt_apply(struct partition_gpt_edit *edits, size_t count) {
    static uint8_t header[SECTOR_SIZE];
    uint64_t disk_sectors = block_sector_count();
    if (disk_sectors == 0 || disk_sectors > UINT32_MAX
        || !block_read_sectors(1, 1, header) || !gpt_header_valid(header, 1)) {
        return false;
    }
    uint64_t backup_lba = le64(header + 32);
    uint64_t entries_lba = le64(header + 72);
    uint32_t entry_count = le32(header + 80);
    uint32_t entry_size = le32(header + 84);
    uint64_t first_usable = le64(header + 40);
    uint64_t last_usable = le64(header + 48);
    uint64_t array_bytes = (uint64_t)entry_count * entry_size;
    uint32_t array_sectors =
        (uint32_t)((array_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE);
    if (entry_size < 128 || entry_size > SECTOR_SIZE
        || SECTOR_SIZE % entry_size != 0 || array_sectors == 0
        || array_sectors > GPT_ARRAY_MAX_SECTORS
        || backup_lba >= disk_sectors || backup_lba <= array_sectors) {
        return false;
    }
    for (uint32_t s = 0; s < array_sectors; s++) {
        if (!block_read_sectors((uint32_t)(entries_lba + s), 1,
                                array + s * SECTOR_SIZE)) {
            return false;
        }
    }
    if (crc32(array, (size_t)array_bytes) != le32(header + 88)) {
        return false;
    }

    for (size_t k = 0; k < count; k++) {
        struct partition_gpt_edit *edit = &edits[k];
        uint8_t *entry;
        if (edit->number == 0) {
            uint64_t last = edit->first + edit->sectors - 1;
            if (edit->sectors == 0 || edit->first < first_usable
                || last > last_usable) {
                return false;
            }
            uint32_t free_number = 0;
            for (uint32_t n = 1; n <= entry_count && free_number == 0; n++) {
                if (zero_guid(array + (n - 1) * entry_size)) {
                    free_number = n;
                }
            }
            if (free_number == 0) {
                return false;
            }
            entry = array + (free_number - 1) * entry_size;
            for (uint32_t i = 0; i < entry_size; i++) {
                entry[i] = 0;
            }
            new_guid(entry + 16);
            put64(entry + 32, edit->first);
            put64(entry + 40, last);
            static const char name[] = "OPOS";
            for (size_t i = 0; name[i] != '\0' && i < GPT_ENTRY_NAME_CHARS;
                 i++) {
                entry[GPT_ENTRY_NAME_OFFSET + i * 2] = (uint8_t)name[i];
            }
            edit->assigned = free_number;
        } else {
            if (edit->number > entry_count) {
                return false;
            }
            entry = array + (edit->number - 1) * entry_size;
            if (edit->remove) {
                for (uint32_t i = 0; i < entry_size; i++) {
                    entry[i] = 0;
                }
                continue;
            }
        }
        for (size_t i = 0; i < 16; i++) {
            entry[i] = edit->type_guid[i];
        }
    }

    uint32_t array_crc = crc32(array, (size_t)array_bytes);
    uint32_t header_size = le32(header + 12);
    /* Backup array sits right before the backup header. */
    uint64_t backup_entries = backup_lba - array_sectors;
    static uint8_t backup[SECTOR_SIZE];
    for (size_t i = 0; i < SECTOR_SIZE; i++) {
        backup[i] = header[i];
    }

    put32(header + 88, array_crc);
    put32(header + 16, 0);
    put32(header + 16, crc32(header, header_size));

    put64(backup + 24, backup_lba);
    put64(backup + 32, 1);
    put64(backup + 72, backup_entries);
    put32(backup + 88, array_crc);
    put32(backup + 16, 0);
    put32(backup + 16, crc32(backup, header_size));

    /* Backup first: if writing stops halfway, the primary is still the
       old, consistent table. */
    return write_array(backup_entries, array_sectors)
        && block_write_sectors((uint32_t)backup_lba, 1, backup)
        && write_array(entries_lba, array_sectors)
        && block_write_sectors(1, 1, header);
}
