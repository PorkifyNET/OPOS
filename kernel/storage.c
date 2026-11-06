#include "storage.h"

#include "block.h"
#include "fat16.h"
#include "iso9660.h"
#include "optical.h"
#include "partition.h"
#include "xhci.h"

/*
 * Volume discovery for the user interface. It only reads: a few sectors
 * at the start of each volume are enough to name its filesystem.
 */

#define SECTOR_SIZE 512
/* Sectors 0-7 of a volume (FAT/NTFS/exFAT boot sector, ext superblock at
   byte 1024, the swap signature at the end of the first 4 KiB page). */
#define HEAD_SECTORS 8
#define ISO9660_SECTOR 64   /* byte 32768: the first volume descriptor */
#define BTRFS_SECTOR 128    /* byte 65536: the btrfs superblock */

static size_t append_decimal(char *text, size_t capacity, size_t used,
                             uint64_t value);
static size_t append_string(char *text, size_t capacity, size_t used,
                            const char *string);

static uint16_t le16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static uint32_t le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool bytes_are(const uint8_t *bytes, const char *text) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (bytes[i] != (uint8_t)text[i]) {
            return false;
        }
    }
    return true;
}

static bool all_zero(const uint8_t *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Copies a space- or NUL-padded label, keeping printable ASCII only. */
static void copy_label(char *label, const uint8_t *text, size_t length) {
    size_t used = 0;
    for (size_t i = 0; i < length && text[i] != 0
         && used + 1 < STORAGE_LABEL_CAPACITY; i++) {
        uint8_t character = text[i];
        label[used++] = character >= ' ' && character <= '~'
            ? (char)character : '?';
    }
    while (used > 0 && label[used - 1] == ' ') {
        used--;
    }
    label[used] = '\0';
    static const char no_name[] = "NO NAME";
    bool unnamed = used == sizeof(no_name) - 1;
    for (size_t i = 0; unnamed && i < used; i++) {
        unnamed = label[i] == no_name[i];
    }
    if (unnamed) {
        label[0] = '\0';
    }
}

/* FAT12/16/32 from the BIOS parameter block; false if it is not FAT. */
static bool detect_fat(const uint8_t *boot, struct storage_volume *volume) {
    if (!(boot[0] == 0xeb || boot[0] == 0xe9) || le16(boot + 11) != 512) {
        return false;
    }
    uint8_t per_cluster = boot[13];
    uint16_t reserved = le16(boot + 14);
    uint8_t fats = boot[16];
    if (per_cluster == 0 || (per_cluster & (per_cluster - 1)) != 0
        || reserved == 0 || fats == 0 || fats > 4) {
        return false;
    }
    uint32_t root_sectors = ((uint32_t)le16(boot + 17) * 32 + 511) / 512;
    uint32_t total = le16(boot + 19) != 0 ? le16(boot + 19) : le32(boot + 32);
    uint32_t fat_size = le16(boot + 22);
    bool fat32 = fat_size == 0;
    if (fat32) {
        fat_size = le32(boot + 36);
    }
    uint64_t overhead = reserved + (uint64_t)fats * fat_size + root_sectors;
    if (total == 0 || fat_size == 0 || overhead >= total) {
        return false;
    }
    uint32_t clusters = (uint32_t)((total - overhead) / per_cluster);
    if (fat32) {
        volume->filesystem = STORAGE_FS_FAT32;
    } else {
        volume->filesystem = clusters < 4085 ? STORAGE_FS_FAT12
                                             : STORAGE_FS_FAT16;
    }
    /* Extended boot signature 0x29 means the label field is present. */
    size_t signature = fat32 ? 66 : 38;
    if (boot[signature] == 0x29) {
        copy_label(volume->label, boot + signature + 5, 11);
    }
    return true;
}

static void detect(struct storage_volume *volume) {
    static uint8_t head[HEAD_SECTORS * SECTOR_SIZE];
    static uint8_t extra[SECTOR_SIZE];
    volume->filesystem = STORAGE_FS_UNKNOWN;
    volume->label[0] = '\0';
    if (volume->first + BTRFS_SECTOR + 1 > UINT32_MAX) {
        return; /* block_disk_read takes 32-bit sector numbers */
    }
    uint32_t first = (uint32_t)volume->first;
    if (!block_disk_read(volume->disk, first, HEAD_SECTORS, head)) {
        volume->filesystem = STORAGE_FS_READ_ERROR;
        return;
    }
    if (bytes_are(head + 3, "NTFS    ")) {
        volume->filesystem = STORAGE_FS_NTFS;
        return;
    }
    if (bytes_are(head + 3, "EXFAT   ")) {
        volume->filesystem = STORAGE_FS_EXFAT;
        return;
    }
    if (bytes_are(head + 3, "-FVE-FS-")) {
        volume->filesystem = STORAGE_FS_BITLOCKER;
        return;
    }
    if (detect_fat(head, volume)) {
        return;
    }
    if (le16(head + 1024 + 56) == 0xef53) {
        volume->filesystem = STORAGE_FS_EXT;
        copy_label(volume->label, head + 1024 + 120, 16);
        return;
    }
    if (bytes_are(head, "XFSB")) {
        volume->filesystem = STORAGE_FS_XFS;
        copy_label(volume->label, head + 108, 12);
        return;
    }
    if (bytes_are(head + 4086, "SWAPSPACE2")
        || bytes_are(head + 4086, "SWAP-SPACE")) {
        volume->filesystem = STORAGE_FS_SWAP;
        copy_label(volume->label, head + 1024 + 28, 16);
        return;
    }
    if (bytes_are(head + 1024, "H+") || bytes_are(head + 1024, "HX")) {
        volume->filesystem = STORAGE_FS_HFS_PLUS;
        return;
    }
    if (bytes_are(head + 32, "NXSB")) {
        volume->filesystem = STORAGE_FS_APFS;
        return;
    }
    bool head_zero = all_zero(head, sizeof(head));
    if (volume->sectors > ISO9660_SECTOR
        && block_disk_read(volume->disk, first + ISO9660_SECTOR, 1, extra)
        && bytes_are(extra + 1, "CD001")) {
        volume->filesystem = STORAGE_FS_ISO9660;
        /* The primary volume descriptor's volume identifier. */
        if (extra[0] == 1) {
            copy_label(volume->label, extra + 40, 32);
        }
        return;
    }
    if (volume->sectors > BTRFS_SECTOR
        && block_disk_read(volume->disk, first + BTRFS_SECTOR, 1, extra)
        && bytes_are(extra + 0x40, "_BHRfS_M")) {
        volume->filesystem = STORAGE_FS_BTRFS;
        copy_label(volume->label, extra + 0x12b, STORAGE_LABEL_CAPACITY);
        return;
    }
    if (head_zero) {
        volume->filesystem = STORAGE_FS_EMPTY;
    }
}

/* Matches the volume against the mounted FAT volumes. */
static void find_mount(struct storage_volume *volume) {
    volume->mount = STORAGE_NOT_MOUNTED;
    volume->path[0] = '\0';
    struct fat16_volume_info info;
    for (size_t i = 0; i < FAT16_MAX_VOLUMES; i++) {
        if (fat16_volume_info(i, &info) && info.disk == volume->disk
            && info.first_sector == volume->first) {
            volume->mount = i;
            (void)storage_mount_path(i, volume->path, sizeof(volume->path));
            /* The FAT driver also reads the root directory's label. */
            if (info.label[0] != '\0') {
                size_t j = 0;
                for (; info.label[j] != '\0' && j + 1 < STORAGE_LABEL_CAPACITY;
                     j++) {
                    volume->label[j] = info.label[j];
                }
                volume->label[j] = '\0';
            }
            return;
        }
    }
}

/* Partitions that only hold boot loader or reserved data are not shown. */
static bool hidden_partition(uint8_t type) {
    return type == PARTITION_TYPE_MS_RESERVED
        || type == PARTITION_TYPE_BIOS_BOOT
        || type == 0x05 || type == 0x0f || type == 0x85; /* extended */
}

size_t storage_list(struct storage_volume *volumes, size_t capacity) {
    size_t count = 0;
    static struct partition_table table;
    static uint8_t sector[SECTOR_SIZE];
    for (size_t disk = 0; disk < block_disk_count() && count < capacity;
         disk++) {
        struct block_disk_info info;
        if (!block_disk_info(disk, &info)) {
            continue;
        }
        /* A FAT boot sector at LBA 0 also ends in 0x55AA; its boot code
           must not be read as partition entries (as fat16_mount_others). */
        bool readable = block_disk_read(disk, 0, 1, sector);
        bool whole_disk_fat = readable
            && (sector[0] == 0xeb || sector[0] == 0xe9)
            && le16(sector + 11) == SECTOR_SIZE && sector[13] != 0;
        bool table_read = readable && !whole_disk_fat
            && partition_read(disk, &table);
        if (table_read && table.scheme != PARTITION_SCHEME_NONE) {
            size_t shown = 0;
            for (size_t i = 0; i < table.count && count < capacity; i++) {
                const struct partition_entry *entry = &table.entries[i];
                if (hidden_partition(entry->type) || entry->sectors == 0) {
                    continue;
                }
                struct storage_volume *volume = &volumes[count++];
                *volume = (struct storage_volume){
                    .disk = disk,
                    .partition = entry->number,
                    .first = entry->first,
                    .sectors = entry->sectors,
                    .partition_type = entry->type,
                    .removable = info.removable,
                    .sd_card = info.device == BLOCK_DEVICE_SD,
                };
                detect(volume);
                find_mount(volume);
                shown++;
            }
            if (shown != 0 || count >= capacity) {
                continue;
            }
        }
        struct storage_volume *volume = &volumes[count++];
        *volume = (struct storage_volume){
            .disk = disk,
            .sectors = info.sectors,
            .removable = info.removable,
            .sd_card = info.device == BLOCK_DEVICE_SD,
        };
        if (!readable) {
            volume->filesystem = STORAGE_FS_READ_ERROR;
            volume->mount = STORAGE_NOT_MOUNTED;
            continue;
        }
        detect(volume);
        find_mount(volume);
        if (table_read && table.scheme != PARTITION_SCHEME_NONE) {
            volume->filesystem = STORAGE_FS_NO_PARTITIONS;
        }
    }
    /* CD/DVD drives: look for a new or changed disc first. */
    iso_refresh();
    for (size_t drive = 0; drive < optical_drive_count() && count < capacity;
         drive++) {
        struct optical_drive_info info;
        if (!optical_drive_info(drive, &info)) {
            continue;
        }
        struct storage_volume *volume = &volumes[count++];
        *volume = (struct storage_volume){
            .optical = true,
            .disk = drive,
            .sectors = (uint64_t)info.sectors * (OPTICAL_SECTOR_SIZE / 512),
            .mount = STORAGE_NOT_MOUNTED,
        };
        switch (iso_state(drive)) {
        case ISO_DISC_MOUNTED: {
            volume->filesystem = STORAGE_FS_ISO9660;
            const char *label = iso_label(drive);
            size_t i = 0;
            for (; label[i] != '\0' && i + 1 < STORAGE_LABEL_CAPACITY; i++) {
                volume->label[i] = label[i];
            }
            volume->label[i] = '\0';
            size_t used = append_string(volume->path, sizeof(volume->path),
                                        0, "/cdrom");
            if (drive != 0) {
                append_decimal(volume->path, sizeof(volume->path), used,
                               drive + 1);
            }
            break;
        }
        case ISO_DISC_UDF:
            volume->filesystem = STORAGE_FS_UDF;
            break;
        case ISO_DISC_UNKNOWN:
            volume->filesystem = STORAGE_FS_UNKNOWN;
            break;
        case ISO_DISC_NONE:
        default:
            volume->filesystem = STORAGE_FS_NO_DISC;
            volume->sectors = 0;
            break;
        }
    }
    return count;
}

const char *storage_filesystem_name(enum storage_filesystem filesystem) {
    switch (filesystem) {
    case STORAGE_FS_EMPTY: return "EMPTY";
    case STORAGE_FS_FAT12: return "FAT12";
    case STORAGE_FS_FAT16: return "FAT16";
    case STORAGE_FS_FAT32: return "FAT32";
    case STORAGE_FS_EXFAT: return "EXFAT";
    case STORAGE_FS_NTFS: return "NTFS";
    case STORAGE_FS_EXT: return "EXT4";
    case STORAGE_FS_BTRFS: return "BTRFS";
    case STORAGE_FS_XFS: return "XFS";
    case STORAGE_FS_BITLOCKER: return "BITLOCKER";
    case STORAGE_FS_ISO9660: return "ISO 9660";
    case STORAGE_FS_SWAP: return "LINUX SWAP";
    case STORAGE_FS_HFS_PLUS: return "HFS+";
    case STORAGE_FS_APFS: return "APFS";
    case STORAGE_FS_NO_PARTITIONS: return "NO PARTITIONS";
    case STORAGE_FS_READ_ERROR: return "READ ERROR";
    case STORAGE_FS_NO_DISC: return "NO DISC";
    case STORAGE_FS_UDF: return "UDF";
    case STORAGE_FS_UNKNOWN:
    default: return "UNKNOWN";
    }
}

static size_t append_decimal(char *text, size_t capacity, size_t used,
                             uint64_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0 && used + 1 < capacity) {
        text[used++] = digits[--count];
    }
    text[used] = '\0';
    return used;
}

static size_t append_string(char *text, size_t capacity, size_t used,
                            const char *string) {
    for (size_t i = 0; string[i] != '\0' && used + 1 < capacity; i++) {
        text[used++] = string[i];
    }
    text[used] = '\0';
    return used;
}

size_t storage_mount_path(size_t mount, char *path, size_t capacity) {
    if (capacity < 7 || mount >= FAT16_MAX_VOLUMES) {
        return 0;
    }
    size_t used = append_string(path, capacity, 0, "/disk");
    if (mount != 0) {
        used = append_decimal(path, capacity, used, mount + 1);
    }
    return used;
}

size_t storage_format_size(uint64_t sectors, char *text, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    uint64_t megabytes = sectors / 2048;
    const char *unit = " MB";
    uint64_t whole = megabytes;
    uint64_t tenths = 0;
    bool decimal = false;
    if (megabytes >= 1024 * 1024) {
        whole = megabytes / (1024 * 1024);
        tenths = megabytes % (1024 * 1024) * 10 / (1024 * 1024);
        decimal = whole < 10;
        unit = " TB";
    } else if (megabytes >= 1024) {
        whole = megabytes / 1024;
        tenths = megabytes % 1024 * 10 / 1024;
        decimal = whole < 10;
        unit = " GB";
    } else if (sectors < 2048) {
        whole = sectors / 2;
        unit = " KB";
    }
    text[0] = '\0';
    size_t used = append_decimal(text, capacity, 0, whole);
    if (decimal && tenths != 0) {
        used = append_string(text, capacity, used, ".");
        used = append_decimal(text, capacity, used, tenths);
    }
    return append_string(text, capacity, used, unit);
}

/* block_disk_generation of each disk as last handled by storage_poll. */
static uint32_t handled[BLOCK_MAX_DISKS];

bool storage_poll(void) {
    bool changed = xhci_poll();
    if (block_rescan()) {
        changed = true;
    }
    for (size_t disk = 0; disk < block_disk_count() && disk < BLOCK_MAX_DISKS;
         disk++) {
        uint32_t generation = block_disk_generation(disk);
        if (generation == handled[disk]) {
            continue;
        }
        bool known = handled[disk] != 0;
        if (known) {
            /* Pulled out, or another stick took its slot. */
            fat16_unmount_others(disk);
            changed = true;
        }
        if (generation != 0) {
            fat16_mount_disk(disk);
        }
        handled[disk] = generation;
    }
    return changed;
}

void storage_initialize(void) {
    (void)xhci_initialize();
    (void)storage_poll();
}
