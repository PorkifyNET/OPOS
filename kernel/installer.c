#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "fat16.h"
#include "installer.h"
#include "partition.h"
#include "vfs.h"

#define SECTOR_SIZE 512
#define USTAR_BLOCK_SIZE 512
#define BIOS_BOOT_SECTORS 63
/* Bytes 440-511 of sector 0: disk signature, partition table, 0x55AA. */
#define MBR_DISK_AREA_OFFSET 440
#define WRITE_CHUNK (64 * 1024)
#define VERIFY_CHUNK 4096

struct payload_file {
    const uint8_t *data;
    size_t size;
};

static const uint8_t *archive;
static size_t archive_length;
static struct payload_file kernel_image;
static struct payload_file initramfs_image;
static uint8_t verify_buffer[VERIFY_CHUNK];
static uint8_t sector_buffer[SECTOR_SIZE];

void installer_set_payload(const void *install_archive, size_t archive_size,
                           const void *kernel, size_t kernel_size,
                           const void *initramfs, size_t initramfs_size) {
    archive = install_archive;
    archive_length = install_archive != NULL ? archive_size : 0;
    kernel_image = (struct payload_file){kernel, kernel_size};
    initramfs_image = (struct payload_file){initramfs, initramfs_size};
}

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static bool octal_size(const uint8_t *field, size_t field_size, size_t *value) {
    size_t result = 0;
    bool started = false;
    for (size_t i = 0; i < field_size; i++) {
        uint8_t character = field[i];
        if (character == '\0' || character == ' ') {
            if (started) {
                break;
            }
            continue;
        }
        if (character < '0' || character > '7' || result > SIZE_MAX / 8) {
            return false;
        }
        started = true;
        result = result * 8 + (size_t)(character - '0');
    }
    *value = result;
    return started;
}

/* Finds a regular file in the install archive by its exact ustar name. */
static bool archive_find(const char *name, struct payload_file *file) {
    size_t name_length = text_length(name);
    size_t offset = 0;
    while (archive != NULL && offset + USTAR_BLOCK_SIZE <= archive_length) {
        const uint8_t *header = archive + offset;
        if (header[0] == '\0') {
            return false;
        }
        size_t size;
        if (!octal_size(header + 124, 12, &size)
            || size > archive_length - offset - USTAR_BLOCK_SIZE) {
            return false;
        }
        bool matches = header[156] == '0' || header[156] == '\0';
        for (size_t i = 0; matches && i < 100; i++) {
            char expected = i < name_length ? name[i] : '\0';
            if ((char)header[i] != expected) {
                matches = false;
            }
            if (expected == '\0') {
                break;
            }
        }
        if (matches) {
            file->data = header + USTAR_BLOCK_SIZE;
            file->size = size;
            return true;
        }
        offset += USTAR_BLOCK_SIZE
            + (size + USTAR_BLOCK_SIZE - 1) / USTAR_BLOCK_SIZE * USTAR_BLOCK_SIZE;
    }
    return false;
}

static const char *const archive_files[] = {
    "bios-boot.bin", "limine-bios.sys", "BOOTX64.EFI", "limine.conf",
};

bool installer_available(void) {
    struct payload_file file;
    if (kernel_image.data == NULL || kernel_image.size == 0
        || initramfs_image.data == NULL || initramfs_image.size == 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(archive_files) / sizeof(archive_files[0]);
         i++) {
        if (!archive_find(archive_files[i], &file) || file.size == 0) {
            return false;
        }
    }
    return archive_find("bios-boot.bin", &file)
        && file.size == BIOS_BOOT_SECTORS * SECTOR_SIZE;
}

/* Writes Limine's BIOS boot sector and stage 2 into sectors 0-62 while
   keeping the disk signature and partition table the format wrote. */
static bool write_bios_boot(const struct payload_file *boot) {
    if (!block_read_sectors(0, 1, sector_buffer)) {
        return false;
    }
    for (size_t i = 0; i < MBR_DISK_AREA_OFFSET; i++) {
        sector_buffer[i] = boot->data[i];
    }
    if (!block_write_sectors(0, 1, sector_buffer)) {
        return false;
    }
    for (uint32_t sector = 1; sector < BIOS_BOOT_SECTORS; sector++) {
        for (size_t i = 0; i < SECTOR_SIZE; i++) {
            sector_buffer[i] = boot->data[sector * SECTOR_SIZE + i];
        }
        if (!block_write_sectors(sector, 1, sector_buffer)) {
            return false;
        }
    }
    return true;
}

static bool make_directory(const char *path) {
    struct vfs_metadata metadata;
    size_t length = text_length(path);
    return vfs_create_directory(path, length)
        || (vfs_stat(path, length, &metadata)
            && metadata.type == VFS_NODE_DIRECTORY);
}

/* Writes a file in chunks and reads it back to compare. */
static bool copy_file(const char *path, const struct payload_file *file) {
    size_t length = text_length(path);
    if (!vfs_create_file(path, length, true)) {
        return false;
    }
    for (size_t offset = 0; offset < file->size; offset += WRITE_CHUNK) {
        size_t chunk = file->size - offset < WRITE_CHUNK
            ? file->size - offset : WRITE_CHUNK;
        size_t written = 0;
        if (!vfs_write_at(path, length, offset, file->data + offset, chunk,
                          &written)
            || written != chunk) {
            return false;
        }
    }
    size_t size;
    if (!vfs_file_size(path, length, &size) || size != file->size) {
        return false;
    }
    for (size_t offset = 0; offset < file->size; offset += VERIFY_CHUNK) {
        size_t chunk = file->size - offset < VERIFY_CHUNK
            ? file->size - offset : VERIFY_CHUNK;
        size_t read = 0;
        if (!vfs_read_at(path, length, offset, verify_buffer, chunk, &read)
            || read != chunk) {
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (verify_buffer[i] != file->data[offset + i]) {
                return false;
            }
        }
    }
    return true;
}

struct install_copy {
    const char *path;
    const char *archive_name; /* NULL: kernel or initramfs image */
    const struct payload_file *image;
    const char *step;
};

/* The archive's limine.conf plus entries for other systems; made by
   build_boot_menu before the files are copied. */
static struct payload_file boot_menu;

static const struct install_copy install_copies[] = {
    {"/disk/boot/kernel.elf", NULL, &kernel_image, "COPYING KERNEL"},
    {"/disk/boot/initramfs.tar", NULL, &initramfs_image,
     "COPYING SYSTEM PROGRAMS"},
    {"/disk/boot/limine/limine-bios.sys", "limine-bios.sys", NULL,
     "COPYING BIOS BOOTLOADER"},
    {"/disk/boot/limine/limine.conf", NULL, &boot_menu,
     "WRITING BOOT MENU"},
    {"/disk/EFI/BOOT/BOOTX64.EFI", "BOOTX64.EFI", NULL,
     "COPYING UEFI BOOTLOADER"},
};
#define INSTALL_COPY_COUNT (sizeof(install_copies) / sizeof(install_copies[0]))

static const char *const install_directories[] = {
    "/disk/boot", "/disk/boot/limine", "/disk/EFI", "/disk/EFI/BOOT",
};

static void report(installer_progress progress, const char *step,
                   unsigned percent) {
    if (progress != NULL) {
        progress(step, percent);
    }
}

static bool fail(const char **error, const char *reason) {
    if (error != NULL) {
        *error = reason;
    }
    return false;
}

static bool sector_zero(const uint8_t *sector) {
    for (size_t i = 0; i < SECTOR_SIZE; i++) {
        if (sector[i] != 0) {
            return false;
        }
    }
    return true;
}

static uint32_t le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/* Same rule as fat16.c: whole-disk filesystems and LVM keep their
   signatures in the first 128 KiB, partitions usually start at 1 MiB. */
#define SUMMARY_BLANK_SECTORS 256
#define SUMMARY_PARTITION_SECTOR 2048
#define GPT_ENTRY_SECTORS 32

static bool disk_blank(size_t disk, uint64_t sectors) {
    for (uint32_t lba = 0; lba < SUMMARY_BLANK_SECTORS && lba < sectors;
         lba++) {
        if (!block_disk_read(disk, lba, 1, sector_buffer)
            || !sector_zero(sector_buffer)) {
            return false;
        }
    }
    return sectors <= SUMMARY_PARTITION_SECTOR
        || (block_disk_read(disk, SUMMARY_PARTITION_SECTOR, 1, sector_buffer)
            && sector_zero(sector_buffer));
}

static bool fat_boot_sector(const uint8_t *sector) {
    uint16_t bytes_per_sector = (uint16_t)(sector[11] | (sector[12] << 8));
    return sector[510] == 0x55 && sector[511] == 0xaa
        && (sector[0] == 0xeb || sector[0] == 0xe9)
        && bytes_per_sector == SECTOR_SIZE && sector[13] != 0
        && sector[16] != 0;
}

/* Counts used GPT entries (non-zero type GUID) in the usual array at
   LBA 2, with the entry size and count from the header in sector 1. */
static size_t gpt_partition_count(size_t disk) {
    if (!block_disk_read(disk, 1, 1, sector_buffer)) {
        return 0;
    }
    uint32_t entry_count = le32(sector_buffer + 80);
    uint32_t entry_size = le32(sector_buffer + 84);
    uint32_t first_lba = le32(sector_buffer + 72);
    if (entry_size < 128 || entry_size > SECTOR_SIZE
        || SECTOR_SIZE % entry_size != 0 || first_lba == 0) {
        return 0;
    }
    size_t used = 0;
    size_t seen = 0;
    for (uint32_t sector = 0; sector < GPT_ENTRY_SECTORS && seen < entry_count;
         sector++) {
        if (!block_disk_read(disk, first_lba + sector, 1, sector_buffer)) {
            break;
        }
        for (uint32_t offset = 0;
             offset < SECTOR_SIZE && seen < entry_count;
             offset += entry_size, seen++) {
            for (size_t i = 0; i < 16; i++) {
                if (sector_buffer[offset + i] != 0) {
                    used++;
                    break;
                }
            }
        }
    }
    return used;
}

bool installer_disk_summary(size_t disk,
                            struct installer_disk_summary *summary) {
    *summary = (struct installer_disk_summary){0};
    struct block_disk_info info;
    if (!block_disk_info(disk, &info)
        || !block_disk_read(disk, 0, 1, sector_buffer)) {
        return false;
    }
    summary->readable = true;
    if (sector_buffer[510] == 0x55 && sector_buffer[511] == 0xaa) {
        if (fat_boot_sector(sector_buffer)) {
            summary->whole_disk_fat = true;
            return true;
        }
        uint8_t types[4];
        uint32_t sizes[4];
        for (size_t i = 0; i < 4; i++) {
            const uint8_t *entry = sector_buffer + 446 + i * 16;
            types[i] = entry[4];
            sizes[i] = le32(entry + 12);
            if (types[i] == 0xee) {
                summary->gpt = true;
            }
        }
        if (summary->gpt) {
            summary->partition_count = gpt_partition_count(disk);
            return true;
        }
        for (size_t i = 0; i < 4; i++) {
            if (types[i] != 0 && sizes[i] != 0) {
                summary->mbr_types[summary->partition_count] = types[i];
                summary->mbr_sectors[summary->partition_count] = sizes[i];
                summary->partition_count++;
            }
        }
        if (summary->partition_count != 0) {
            return true;
        }
    }
    summary->blank = disk_blank(disk, info.sectors);
    return true;
}

const char *installer_partition_type_name(uint8_t type) {
    switch (type) {
    case 0x01:
        return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0e:
        return "FAT16";
    case 0x0b:
    case 0x0c:
        return "FAT32";
    case 0x07:
        return "NTFS/EXFAT";
    case 0x05:
    case 0x0f:
    case 0x85:
        return "EXTENDED";
    case 0x27:
        return "WINDOWS RECOVERY";
    case 0x82:
        return "LINUX SWAP";
    case 0x83:
        return "LINUX";
    case 0x8e:
        return "LINUX LVM";
    case 0xa5:
    case 0xa6:
    case 0xa9:
        return "BSD";
    case 0xaf:
        return "MACOS";
    case 0xef:
        return "EFI SYSTEM";
    case 0xfd:
        return "LINUX RAID";
    default:
        return "OTHER";
    }
}

const char *installer_disk_unusable(size_t disk) {
    struct block_disk_info info;
    enum fat_volume_type type;
    uint32_t partition_sectors;
    if (!block_disk_info(disk, &info)) {
        return "DISK NOT FOUND";
    }
    if (info.boot_disk) {
        return "OPOS IS RUNNING FROM THIS DISK";
    }
    if (!fat16_format_plan(info.sectors, &type, &partition_sectors)
        || info.sectors < 2048u + INSTALLER_MIN_SECTORS) {
        return "TOO SMALL FOR OPOS";
    }
    return NULL;
}

static bool install_steps(installer_progress progress, const char **error);
static struct installer_boot_scan install_scan;
static bool build_boot_menu(const struct installer_boot_scan *scan);

/* Runs the install as one write batch: the per-write cache flush made it
   take ~30 s, so the disk cache is flushed once at the end instead. */
bool installer_install(size_t disk, installer_progress progress,
                       const char **error) {
    if (!installer_available()) {
        return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
    }
    const char *unusable = installer_disk_unusable(disk);
    if (unusable != NULL) {
        return fail(error, unusable);
    }
    if (vfs_disk_objects_open()) {
        return fail(error, "FILES ON THE DISK ARE OPEN");
    }
    /* Look for other systems while their volumes are still mounted. */
    installer_boot_scan(disk, NULL, &install_scan);
    if (!build_boot_menu(&install_scan)) {
        return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
    }
    /* Other volumes may live on the disk being written: drop them (they
       are mounted again afterwards) and make the system volume current. */
    fat16_unmount_others(BLOCK_NO_DISK);
    /* From here /disk shows the chosen disk, also when the install fails,
       so the user sees the state the failed install left behind. */
    if (disk != block_active_disk()) {
        if (!block_select_disk(disk)) {
            return fail(error, "DISK NOT FOUND");
        }
        (void)fat16_initialize();
    }
    block_begin_batch();
    bool installed = install_steps(progress, error);
    if (!block_end_batch() && installed) {
        installed = fail(error, "FLUSHING THE DISK CACHE FAILED");
    }
    fat16_mount_others();
    if (installed) {
        report(progress, "INSTALLATION COMPLETE", 100);
    }
    return installed;
}

/* Creates the boot folders and copies the kernel, initramfs and Limine
   files onto the mounted volume; progress runs from 25 to 95 percent. */
static bool copy_system_files(installer_progress progress,
                              const char **error) {
    report(progress, "CREATING FOLDERS", 25);
    for (size_t i = 0;
         i < sizeof(install_directories) / sizeof(install_directories[0]);
         i++) {
        if (!make_directory(install_directories[i])) {
            return fail(error, "CREATING BOOT FOLDERS FAILED");
        }
    }
    for (size_t i = 0; i < INSTALL_COPY_COUNT; i++) {
        const struct install_copy *copy = &install_copies[i];
        struct payload_file file;
        if (copy->image != NULL) {
            file = *copy->image;
        } else if (!archive_find(copy->archive_name, &file)) {
            return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
        }
        report(progress, copy->step,
               (unsigned)(30 + i * 65 / INSTALL_COPY_COUNT));
        if (!copy_file(copy->path, &file)) {
            return fail(error, "COPYING FILES FAILED");
        }
    }
    return true;
}

static bool install_steps(installer_progress progress, const char **error) {
    struct payload_file boot;
    if (!installer_available() || !archive_find("bios-boot.bin", &boot)) {
        return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
    }
    if (block_sector_count() == 0) {
        return fail(error, "NO DISK FOUND");
    }
    if (vfs_disk_objects_open()) {
        return fail(error, "FILES ON THE DISK ARE OPEN");
    }
    /* Other volumes may live on the disk being written: drop them (they
       are mounted again afterwards) and make the system volume current. */
    fat16_unmount_others(BLOCK_NO_DISK);

    report(progress, "PARTITIONING AND FORMATTING DISK", 5);
    if (!fat16_format_disk()) {
        return fail(error, "FORMATTING THE DISK FAILED");
    }
    report(progress, "INSTALLING BIOS BOOT CODE", 20);
    if (!write_bios_boot(&boot)) {
        return fail(error, "WRITING BIOS BOOT CODE FAILED");
    }
    return copy_system_files(progress, error);
}

/* ------------------------------------------------------ other systems */

/* Partition boot sector code (vbrstub.S); it goes at VBR_CODE_OFFSET and
   loads Limine's boot sector from partition sector VBR_BOOT_AREA. */
extern const uint8_t opos_vbr_stub[];
extern const uint8_t opos_vbr_stub_end[];
#define VBR_CODE_OFFSET 0x5a
#define VBR_BOOT_AREA 8
/* Limine's boot sector reads 32256 bytes from its stage 2 offset (4096
   bytes into the boot area), so the area needs 8 + 71 sectors. */
#define VBR_RESERVED_SECTORS 96
#define LIMINE_STAGE2_POINTER 0x1a4
#define LIMINE_STAGE2_OFFSET 4096
#define BOOT_BACKUP_PATH "/disk/OPOS/BOOTBAK.BIN"
#define BOOT_MENU_CAPACITY 8192
#define EFI_PATH_MAX 160
#define MBR_TABLE_START 446

static char boot_menu_text[BOOT_MENU_CAPACITY];
static uint8_t boot_backup[BIOS_BOOT_SECTORS * SECTOR_SIZE];

static size_t append(char *buffer, size_t capacity, size_t length,
                     const char *text) {
    while (*text != '\0' && length + 1 < capacity) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
    return length;
}

static size_t append_hex(char *buffer, size_t capacity, size_t length,
                         uint32_t value, unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    while (digits-- > 0 && length + 1 < capacity) {
        buffer[length++] = hex[(value >> (digits * 4)) & 0xf];
    }
    buffer[length] = '\0';
    return length;
}

static size_t append_decimal(char *buffer, size_t capacity, size_t length,
                             uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0 && length + 1 < capacity) {
        buffer[length++] = digits[--count];
    }
    buffer[length] = '\0';
    return length;
}

static char upper(char character) {
    return character >= 'a' && character <= 'z'
        ? (char)(character - 'a' + 'A') : character;
}

static bool same_name(const char *a, const char *b) {
    while (*a != '\0' && upper(*a) == upper(*b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static bool bytes_contain(const uint8_t *bytes, size_t size,
                          const char *text) {
    size_t length = text_length(text);
    for (size_t at = 0; at + length <= size; at++) {
        size_t i = 0;
        while (i < length && bytes[at + i] == (uint8_t)text[i]) {
            i++;
        }
        if (i == length) {
            return true;
        }
    }
    return false;
}

static enum installer_boot_code classify_mbr_code(const uint8_t *sector) {
    bool zero = true;
    for (size_t i = 0; i < MBR_DISK_AREA_OFFSET; i++) {
        zero = zero && sector[i] == 0;
    }
    if (zero) {
        return INSTALLER_BOOT_CODE_NONE;
    }
    if (bytes_contain(sector + 3, 8, "LIMINE  ")) {
        return INSTALLER_BOOT_CODE_LIMINE;
    }
    if (bytes_contain(sector, MBR_DISK_AREA_OFFSET, "GRUB")) {
        return INSTALLER_BOOT_CODE_GRUB;
    }
    if (bytes_contain(sector, MBR_DISK_AREA_OFFSET, "Missing operating system")
        || bytes_contain(sector, MBR_DISK_AREA_OFFSET,
                         "Invalid partition table")
        || bytes_contain(sector, MBR_DISK_AREA_OFFSET,
                         "Error loading operating system")) {
        return INSTALLER_BOOT_CODE_STANDARD;
    }
    return INSTALLER_BOOT_CODE_UNKNOWN;
}

const char *installer_boot_code_name(enum installer_boot_code code) {
    switch (code) {
    case INSTALLER_BOOT_CODE_NONE:
        return "NO BOOT CODE";
    case INSTALLER_BOOT_CODE_LIMINE:
        return "LIMINE";
    case INSTALLER_BOOT_CODE_GRUB:
        return "GNU GRUB";
    case INSTALLER_BOOT_CODE_STANDARD:
        return "STANDARD (WINDOWS/DOS) BOOT CODE";
    default:
        return "UNKNOWN BOOT CODE";
    }
}

/* What a partition's boot sector starts on BIOS, or NULL. */
static const char *bios_system_name(const uint8_t *sector) {
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        return NULL;
    }
    if (bytes_contain(sector + 3, 8, "-FVE-FS-")) {
        return "WINDOWS (BITLOCKER)";
    }
    if (bytes_contain(sector, SECTOR_SIZE, "BOOTMGR")) {
        return "WINDOWS";
    }
    if (bytes_contain(sector, SECTOR_SIZE, "NTLDR")) {
        return "WINDOWS XP";
    }
    size_t stub = (size_t)(opos_vbr_stub_end - opos_vbr_stub);
    bool opos = sector[0] == 0xeb && sector[1] == VBR_CODE_OFFSET - 2;
    for (size_t i = 0; opos && i < stub; i++) {
        opos = sector[VBR_CODE_OFFSET + i] == opos_vbr_stub[i];
    }
    return opos ? "OPOS" : NULL;
}

static bool scan_add(struct installer_boot_scan *scan,
                     const struct installer_boot_entry *entry) {
    if (scan->entry_count >= INSTALLER_BOOT_ENTRIES_MAX) {
        return false;
    }
    scan->entries[scan->entry_count++] = *entry;
    return true;
}

static void entry_title(struct installer_boot_entry *entry, const char *name,
                        const char *where, unsigned number) {
    size_t length = append(entry->title, sizeof(entry->title), 0, name);
    length = append(entry->title, sizeof(entry->title), length, where);
    if (number != 0) {
        append_decimal(entry->title, sizeof(entry->title), length, number);
    }
}

/* The mounted FAT volume at this location, as a /disk path prefix. */
static bool volume_prefix(size_t disk, uint32_t first, char *prefix) {
    for (size_t volume = 0; volume < FAT16_MAX_VOLUMES; volume++) {
        struct fat16_volume_info info;
        if (fat16_volume_info(volume, &info) && info.disk == disk
            && info.first_sector == first) {
            size_t length = append(prefix, 8, 0, "/disk");
            if (volume != 0) {
                char digit[2] = {(char)('1' + volume), '\0'};
                append(prefix, 8, length, digit);
            }
            return true;
        }
    }
    return false;
}

/* Finds `name` in folder `path` (any case) and appends "/" plus its
   stored name to path, since Limine matches long names case sensitively. */
static bool find_child(char *path, size_t capacity, const char *name,
                       enum vfs_node_type type) {
    size_t length = text_length(path);
    size_t count;
    if (!vfs_directory_count(path, length, &count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        struct vfs_directory_entry entry;
        if (vfs_directory_entry_at(path, length, i, &entry)
            && entry.type == type && same_name(entry.name, name)) {
            if (length + 1 + entry.name_length >= capacity) {
                return false;
            }
            length = append(path, capacity, length, "/");
            append(path, capacity, length, entry.name);
            return true;
        }
    }
    return false;
}

static void add_efi_entry(struct installer_boot_scan *scan, uint32_t number,
                          const char *name, const char *path) {
    struct installer_boot_entry entry = {.uefi = true,
                                         .partition = (uint8_t)number};
    entry_title(&entry, name, " (UEFI) - PARTITION ", number);
    append(entry.path, sizeof(entry.path), 0, path);
    (void)scan_add(scan, &entry);
}

/* Limine tells FAT16 from FAT32 by the cluster count, as the FAT
   specification does, so it cannot read a FAT32 volume with fewer than
   65525 clusters (mkfs.fat makes those, e.g. for 100 MB). */
static bool limine_reads_fat(size_t disk, uint32_t first) {
    if (!block_disk_read(disk, first, 1, sector_buffer)
        || !fat_boot_sector(sector_buffer)) {
        return false;
    }
    uint32_t reserved = (uint32_t)(sector_buffer[14] | (sector_buffer[15] << 8));
    uint32_t root_entries =
        (uint32_t)(sector_buffer[17] | (sector_buffer[18] << 8));
    uint32_t total = (uint32_t)(sector_buffer[19] | (sector_buffer[20] << 8));
    uint32_t per_fat = (uint32_t)(sector_buffer[22] | (sector_buffer[23] << 8));
    if (total == 0) {
        total = le32(sector_buffer + 32);
    }
    bool fat32 = per_fat == 0;
    if (fat32) {
        per_fat = le32(sector_buffer + 36);
    }
    uint64_t metadata = (uint64_t)reserved
        + (uint64_t)sector_buffer[16] * per_fat
        + (root_entries * 32 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    if (metadata >= total) {
        return false;
    }
    uint64_t clusters = (total - metadata) / sector_buffer[13];
    return fat32 ? clusters >= 65525 : clusters < 65525;
}

/* UEFI boot loaders on the FAT partition in `slot`: Windows Boot Manager,
   Linux distributions' shim or GRUB, or else the fallback loader. */
static void scan_efi_loaders(struct installer_boot_scan *scan, size_t disk,
                             uint32_t number, uint32_t first) {
    char prefix[8];
    char path[EFI_PATH_MAX];
    if (!limine_reads_fat(disk, first) || !volume_prefix(disk, first, prefix)) {
        return;
    }
    size_t prefix_length = text_length(prefix);
    size_t length = append(path, sizeof(path), 0, prefix);
    struct vfs_metadata metadata;
    /* An OPOS installation boots through its own boot sector. */
    append(path, sizeof(path), length, "/boot/kernel.elf");
    if (vfs_stat(path, text_length(path), &metadata)) {
        return;
    }
    path[length] = '\0';
    if (!find_child(path, sizeof(path), "EFI", VFS_NODE_DIRECTORY)) {
        return;
    }
    size_t efi_length = text_length(path);
    size_t count;
    if (!vfs_directory_count(path, efi_length, &count)) {
        return;
    }
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        struct vfs_directory_entry folder;
        path[efi_length] = '\0';
        if (!vfs_directory_entry_at(path, efi_length, i, &folder)
            || folder.type != VFS_NODE_DIRECTORY || folder.name[0] == '.'
            || same_name(folder.name, "BOOT")
            || efi_length + 1 + folder.name_length >= sizeof(path)) {
            continue;
        }
        size_t at = append(path, sizeof(path), efi_length, "/");
        at = append(path, sizeof(path), at, folder.name);
        if (same_name(folder.name, "Microsoft")) {
            if (find_child(path, sizeof(path), "Boot", VFS_NODE_DIRECTORY)
                && find_child(path, sizeof(path), "bootmgfw.efi",
                              VFS_NODE_FILE)) {
                add_efi_entry(scan, number, "WINDOWS", path + prefix_length);
                found = true;
            }
            continue;
        }
        if (find_child(path, sizeof(path), "shimx64.efi", VFS_NODE_FILE)
            || find_child(path, sizeof(path), "grubx64.efi", VFS_NODE_FILE)) {
            char name[24];
            size_t name_length = 0;
            for (size_t c = 0; folder.name[c] != '\0' && name_length < 16;
                 c++) {
                name[name_length++] = upper(folder.name[c]);
            }
            name[name_length] = '\0';
            add_efi_entry(scan, number, name, path + prefix_length);
            found = true;
        }
    }
    path[efi_length] = '\0';
    if (!found && find_child(path, sizeof(path), "BOOT", VFS_NODE_DIRECTORY)
        && find_child(path, sizeof(path), "BOOTX64.EFI", VFS_NODE_FILE)) {
        add_efi_entry(scan, number, "UEFI SYSTEM", path + prefix_length);
    }
}

/* Formats a GPT GUID (mixed-endian on disk) as Limine expects. */
static void gpt_guid_text(const uint8_t *guid, char *text, size_t capacity) {
    size_t length = 0;
    length = append_hex(text, capacity, length, le32(guid), 8);
    length = append(text, capacity, length, "-");
    length = append_hex(text, capacity, length,
                        (uint32_t)(guid[4] | (guid[5] << 8)), 4);
    length = append(text, capacity, length, "-");
    length = append_hex(text, capacity, length,
                        (uint32_t)(guid[6] | (guid[7] << 8)), 4);
    length = append(text, capacity, length, "-");
    for (size_t i = 8; i < 16; i++) {
        if (i == 10) {
            length = append(text, capacity, length, "-");
        }
        length = append_hex(text, capacity, length, guid[i], 2);
    }
}

/* A disk OPOS does not go onto: if its MBR boot code starts something,
   the menu chainloads that MBR. */
static void scan_other_disk(struct installer_boot_scan *scan, size_t disk,
                            const struct block_disk_info *info) {
    if (!block_disk_read(disk, 0, 1, sector_buffer)
        || sector_buffer[510] != 0x55 || sector_buffer[511] != 0xaa
        || fat_boot_sector(sector_buffer)) {
        return;
    }
    enum installer_boot_code code = classify_mbr_code(sector_buffer);
    if (code == INSTALLER_BOOT_CODE_NONE) {
        return;
    }
    struct installer_boot_entry entry = {.mbr_id =
                                             le32(sector_buffer + 440)};
    bool gpt = false;
    uint32_t firsts[4];
    for (size_t i = 0; i < 4; i++) {
        const uint8_t *slot = sector_buffer + MBR_TABLE_START + i * 16;
        gpt = gpt || slot[4] == 0xee;
        firsts[i] = slot[4] != 0 ? le32(slot + 8) : 0;
    }
    const char *name = code == INSTALLER_BOOT_CODE_GRUB ? "LINUX (GRUB)"
        : code == INSTALLER_BOOT_CODE_LIMINE ? "LIMINE" : "SYSTEM";
    if (gpt) {
        if (!block_disk_read(disk, 1, 1, sector_buffer)
            || !bytes_contain(sector_buffer, 8, "EFI PART")) {
            return;
        }
        gpt_guid_text(sector_buffer + 56, entry.gpt_guid,
                      sizeof(entry.gpt_guid));
        entry.mbr_id = 0;
    } else {
        if (entry.mbr_id == 0) {
            return;
        }
        for (size_t i = 0; i < 4 && code == INSTALLER_BOOT_CODE_STANDARD;
             i++) {
            const char *system;
            if (firsts[i] != 0
                && block_disk_read(disk, firsts[i], 1, sector_buffer)
                && (system = bios_system_name(sector_buffer)) != NULL) {
                name = system;
                break;
            }
        }
    }
    size_t length = append(entry.title, sizeof(entry.title), 0, name);
    length = append(entry.title, sizeof(entry.title), length, " - ");
    append(entry.title, sizeof(entry.title), length, info->location);
    (void)scan_add(scan, &entry);
}

void installer_boot_scan(size_t disk, const struct installer_layout *layout,
                         struct installer_boot_scan *scan) {
    *scan = (struct installer_boot_scan){0};
    if (layout != NULL && block_disk_read(disk, 0, 1, sector_buffer)
        && sector_buffer[510] == 0x55 && sector_buffer[511] == 0xaa) {
        scan->code = classify_mbr_code(sector_buffer);
    }
    if (layout != NULL) {
        /* Sectors 1-62 below the first partition. */
        uint32_t end = BIOS_BOOT_SECTORS;
        for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
            if (layout->slots[i].original_type != 0
                && layout->slots[i].original_first < end) {
                end = layout->slots[i].original_first;
            }
        }
        for (uint32_t lba = 1; lba < end && !scan->gap_used
                               && scan->code != INSTALLER_BOOT_CODE_LIMINE;
             lba++) {
            scan->gap_used = block_disk_read(disk, lba, 1, sector_buffer)
                && !sector_zero(sector_buffer);
        }
        for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
            const struct installer_slot *slot = &layout->slots[i];
            /* Only partitions the install keeps unchanged. */
            if (slot->type == 0 || slot->created || i == layout->target
                || slot->first != slot->original_first) {
                continue;
            }
            const char *system;
            if (block_disk_read(disk, slot->first, 1, sector_buffer)
                && (system = bios_system_name(sector_buffer)) != NULL) {
                struct installer_boot_entry entry = {
                    .partition = (uint8_t)slot->number};
                entry_title(&entry, system, " - PARTITION ", slot->number);
                (void)scan_add(scan, &entry);
            }
            scan_efi_loaders(scan, disk, slot->number, slot->first);
            if (slot->type == 0xef && scan->esp_number == 0
                && limine_reads_fat(disk, slot->first)) {
                scan->esp_number = slot->number;
                scan->esp_first = slot->first;
            }
        }
    }
    for (size_t other = 0; other < block_disk_count(); other++) {
        struct block_disk_info info;
        /* The installation media is no system to start later. */
        if (other != disk && block_disk_info(other, &info)
            && !info.boot_disk) {
            scan_other_disk(scan, other, &info);
        }
    }
}

const char *installer_boot_mode_problem(const struct installer_layout *layout,
                                        const struct installer_boot_scan *scan,
                                        enum installer_boot_mode mode) {
    switch (mode) {
    case INSTALLER_BOOT_LIMINE_MBR:
        if (layout->gpt) {
            return "GPT DISK: THE MBR ONLY PROTECTS THE PARTITION TABLE";
        }
        return installer_layout_bios_bootable(layout) ? NULL
            : "A PARTITION STARTS BEFORE SECTOR 63, LIMINE DOES NOT FIT";
    case INSTALLER_BOOT_ACTIVE:
        if (layout->gpt) {
            return "GPT DISKS HAVE NO ACTIVE PARTITION";
        }
        return scan->code == INSTALLER_BOOT_CODE_STANDARD ? NULL
            : "ONLY FOR STANDARD (WINDOWS/DOS) BOOT CODE";
    case INSTALLER_BOOT_KEEP:
        return NULL;
    case INSTALLER_BOOT_UEFI_DEFAULT:
        return scan->esp_number != 0 ? NULL
            : "NO EFI SYSTEM PARTITION ON THIS DISK THAT LIMINE CAN READ";
    }
    return "UNKNOWN CHOICE";
}

/* Limine's menu when nothing else would be lost, otherwise the least
   change that still starts OPOS. GRUB and unknown code are kept. */
enum installer_boot_mode installer_boot_default_mode(
    const struct installer_layout *layout,
    const struct installer_boot_scan *scan) {
    if (layout->gpt) {
        return scan->esp_number != 0 ? INSTALLER_BOOT_UEFI_DEFAULT
                                     : INSTALLER_BOOT_KEEP;
    }
    bool limine_fits = installer_boot_mode_problem(
        layout, scan, INSTALLER_BOOT_LIMINE_MBR) == NULL;
    switch (scan->code) {
    case INSTALLER_BOOT_CODE_NONE:
    case INSTALLER_BOOT_CODE_LIMINE:
        return limine_fits && !scan->gap_used ? INSTALLER_BOOT_LIMINE_MBR
                                              : INSTALLER_BOOT_KEEP;
    case INSTALLER_BOOT_CODE_STANDARD:
        return limine_fits && !scan->gap_used ? INSTALLER_BOOT_LIMINE_MBR
                                              : INSTALLER_BOOT_ACTIVE;
    default:
        return INSTALLER_BOOT_KEEP;
    }
}

/* True when `path` ends in /EFI/BOOT/BOOTX64.EFI (any case). */
static bool fallback_loader_path(const char *path) {
    static const char tail[] = "/EFI/BOOT/BOOTX64.EFI";
    size_t length = text_length(path);
    size_t tail_length = sizeof(tail) - 1;
    return length >= tail_length
        && same_name(path + length - tail_length, tail);
}

/*
 * limine.conf from the archive, then one entry per other system. BIOS
 * entries are hidden on UEFI and the other way round. The menu for the
 * OPOS partition loads OPOS from boot(); the copy on the EFI system
 * partition (kernel_partition != 0) names the OPOS partition, and there
 * the replaced fallback loader is started from its backup.
 */
static bool build_menu(const struct installer_boot_scan *scan,
                       uint32_t kernel_partition, char *text,
                       struct payload_file *menu) {
    struct payload_file base;
    if (!archive_find("limine.conf", &base)
        || base.size + 16 >= BOOT_MENU_CAPACITY) {
        return false;
    }
    size_t length = 0;
    for (size_t i = 0; i < base.size; i++) {
        bool boot_resource = kernel_partition != 0 && i + 7 <= base.size
            && base.data[i] == 'b' && base.data[i + 1] == 'o'
            && base.data[i + 2] == 'o' && base.data[i + 3] == 't'
            && base.data[i + 4] == '(' && base.data[i + 5] == ')'
            && base.data[i + 6] == ':';
        if (boot_resource) {
            length = append(text, BOOT_MENU_CAPACITY, length, "boot(");
            length = append_decimal(text, BOOT_MENU_CAPACITY, length,
                                    kernel_partition);
            length = append(text, BOOT_MENU_CAPACITY, length, "):");
            i += 6;
        } else if (length + 1 < BOOT_MENU_CAPACITY) {
            text[length++] = (char)base.data[i];
        }
    }
    text[length] = '\0';
    char line[EFI_PATH_MAX + 32];
    for (size_t i = 0; scan != NULL && i < scan->entry_count; i++) {
        const struct installer_boot_entry *entry = &scan->entries[i];
        size_t at = append(line, sizeof(line), 0, "\n/");
        at = append(line, sizeof(line), at, entry->title);
        at = append(line, sizeof(line), at, entry->uefi
                        ? "\n    protocol: efi\n    path: boot("
                        : "\n    protocol: bios\n");
        if (entry->uefi) {
            at = append_decimal(line, sizeof(line), at, entry->partition);
            at = append(line, sizeof(line), at, "):");
            size_t path_start = at;
            at = append(line, sizeof(line), at, entry->path);
            if (kernel_partition != 0
                && entry->partition == scan->esp_number
                && fallback_loader_path(line + path_start) && at >= 3) {
                /* BOOTX64.EFI becomes BOOTX64.OLD */
                line[at - 3] = 'O';
                line[at - 2] = 'L';
                line[at - 1] = 'D';
            }
            at = append(line, sizeof(line), at, "\n");
        } else if (entry->partition != 0) {
            at = append(line, sizeof(line), at, "    partition: ");
            at = append_decimal(line, sizeof(line), at, entry->partition);
            at = append(line, sizeof(line), at, "\n");
        } else if (entry->gpt_guid[0] != '\0') {
            at = append(line, sizeof(line), at, "    gpt_uuid: ");
            at = append(line, sizeof(line), at, entry->gpt_guid);
            at = append(line, sizeof(line), at, "\n");
        } else {
            at = append(line, sizeof(line), at, "    mbr_id: ");
            at = append_hex(line, sizeof(line), at, entry->mbr_id, 8);
            at = append(line, sizeof(line), at, "\n");
        }
        if (length + at + 1 >= BOOT_MENU_CAPACITY) {
            break;
        }
        length = append(text, BOOT_MENU_CAPACITY, length, line);
    }
    *menu = (struct payload_file){(const uint8_t *)text, length};
    return true;
}

static bool build_boot_menu(const struct installer_boot_scan *scan) {
    return build_menu(scan, 0, boot_menu_text, &boot_menu);
}

/* Gives the freshly formatted OPOS partition at `first` (active disk) a
   boot sector that starts Limine: the stub after the BPB, and Limine's
   boot sector plus stage 2 in the reserved sectors from VBR_BOOT_AREA,
   with the stage 2 offset pointing there. FAT32's backup boot sector
   gets the same code. */
static bool write_partition_boot(uint32_t first,
                                 const struct payload_file *boot) {
    static uint8_t boot_sector[SECTOR_SIZE];
    size_t stub = (size_t)(opos_vbr_stub_end - opos_vbr_stub);
    if (stub > 510 - VBR_CODE_OFFSET
        || !block_read_sectors(first, 1, boot_sector)
        || !fat_boot_sector(boot_sector)
        || (uint16_t)(boot_sector[14] | (boot_sector[15] << 8))
            < VBR_RESERVED_SECTORS) {
        return false;
    }
    uint64_t stage2 = ((uint64_t)first + VBR_BOOT_AREA) * SECTOR_SIZE
        + LIMINE_STAGE2_OFFSET;
    for (uint32_t sector = 0; sector < BIOS_BOOT_SECTORS; sector++) {
        for (size_t i = 0; i < SECTOR_SIZE; i++) {
            sector_buffer[i] = boot->data[sector * SECTOR_SIZE + i];
        }
        if (sector == 0) {
            for (size_t i = 0; i < 8; i++) {
                sector_buffer[LIMINE_STAGE2_POINTER + i] =
                    (uint8_t)(stage2 >> (8 * i));
            }
        }
        if (!block_write_sectors(first + VBR_BOOT_AREA + sector, 1,
                                 sector_buffer)) {
            return false;
        }
    }
    bool fat32 = boot_sector[22] == 0 && boot_sector[23] == 0;
    boot_sector[0] = 0xeb;
    boot_sector[1] = VBR_CODE_OFFSET - 2;
    boot_sector[2] = 0x90;
    /* FAT16's BPB ends at 0x3e; the old boot code there goes too. */
    for (size_t i = fat32 ? VBR_CODE_OFFSET : 0x3e; i < 510; i++) {
        boot_sector[i] = i >= VBR_CODE_OFFSET && i - VBR_CODE_OFFSET < stub
            ? opos_vbr_stub[i - VBR_CODE_OFFSET] : 0;
    }
    uint16_t backup = (uint16_t)(boot_sector[50] | (boot_sector[51] << 8));
    return block_write_sectors(first, 1, boot_sector)
        && (!fat32 || backup == 0 || backup >= VBR_BOOT_AREA
            || block_write_sectors(first + backup, 1, boot_sector));
}

/* ------------------------------------------------- manual partitioning */

#define ALIGNMENT 2048u
#define MBR_TABLE_OFFSET 446
/* Limine's BIOS stage 2 occupies sectors 1-62 after the MBR. */
#define BIOS_GAP_END 63u

static bool extended_type(uint8_t type) {
    return type == 0x05 || type == 0x0f || type == 0x85;
}

static void write_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static void lba_to_chs(uint32_t lba, uint8_t chs[3]) {
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

bool installer_layout_load(size_t disk, struct installer_layout *layout,
                           const char **error) {
    struct block_disk_info info;
    *layout = (struct installer_layout){.disk = disk,
                                        .target = INSTALLER_NO_SLOT};
    if (!block_disk_info(disk, &info)
        || !block_disk_read(disk, 0, 1, sector_buffer)) {
        return fail(error, "THE DISK CANNOT BE READ");
    }
    /* MBR entries end below LBA 2^32. */
    layout->disk_sectors = info.sectors > UINT32_MAX ? UINT32_MAX
                                                     : info.sectors;
    layout->usable_first = ALIGNMENT;
    layout->usable_end = layout->disk_sectors;
    if (sector_buffer[510] != 0x55 || sector_buffer[511] != 0xaa) {
        if (!disk_blank(disk, info.sectors)) {
            return fail(error,
                        "UNKNOWN DATA: ONLY ERASING THE WHOLE DISK WORKS");
        }
        return true;
    }
    if (fat_boot_sector(sector_buffer)) {
        return fail(error,
                    "NO PARTITION TABLE: ONLY ERASING THE WHOLE DISK WORKS");
    }
    static struct partition_table table;
    if (!partition_read(disk, &table)) {
        return fail(error, "THE DISK CANNOT BE READ");
    }
    if (table.scheme == PARTITION_SCHEME_GPT) {
        if (info.sectors > UINT32_MAX) {
            return fail(error,
                        "GPT DISK OVER 2 TB: ONLY ERASING THE WHOLE DISK WORKS");
        }
        if (table.truncated) {
            return fail(error, table.count == 0
                                   ? "THE GPT LOOKS DAMAGED"
                                   : "TOO MANY PARTITIONS ON THIS GPT DISK");
        }
        layout->gpt = true;
        layout->usable_first = table.first_usable;
        layout->usable_end = table.last_usable + 1;
    }
    for (size_t i = 0; i < table.count; i++) {
        const struct partition_entry *entry = &table.entries[i];
        size_t index = layout->gpt ? i : entry->number - 1;
        if (index >= INSTALLER_SLOTS
            || entry->first == 0 || entry->first + entry->sectors > info.sectors
            || entry->first + entry->sectors > UINT32_MAX) {
            return fail(error, "THE PARTITION TABLE LOOKS DAMAGED");
        }
        struct installer_slot *slot = &layout->slots[index];
        slot->status = entry->status;
        slot->type = entry->type;
        slot->first = (uint32_t)entry->first;
        slot->sectors = (uint32_t)entry->sectors;
        slot->number = entry->number;
        slot->original_type = slot->type;
        slot->original_first = slot->first;
        slot->original_sectors = slot->sectors;
        slot->original_number = slot->number;
        for (size_t g = 0; g < 16; g++) {
            slot->original_guid[g] = entry->type_guid[g];
        }
    }
    for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
        for (size_t j = i + 1; j < INSTALLER_SLOTS; j++) {
            const struct installer_slot *a = &layout->slots[i];
            const struct installer_slot *b = &layout->slots[j];
            if (a->type != 0 && b->type != 0
                && a->first < (uint64_t)b->first + b->sectors
                && b->first < (uint64_t)a->first + a->sectors) {
                return fail(error, "THE PARTITION TABLE LOOKS DAMAGED");
            }
        }
    }
    return true;
}

size_t installer_layout_free(const struct installer_layout *layout,
                             struct installer_region *regions,
                             size_t capacity) {
    /* Used slots in disk order (at most four, so an insertion sort). */
    size_t order[INSTALLER_SLOTS];
    size_t used = 0;
    for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
        if (layout->slots[i].type == 0) {
            continue;
        }
        size_t at = used++;
        while (at > 0 && layout->slots[order[at - 1]].first
                             > layout->slots[i].first) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    size_t count = 0;
    uint64_t cursor =
        (layout->usable_first + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
    for (size_t k = 0; k <= used; k++) {
        uint64_t limit = k < used ? layout->slots[order[k]].first
                                  : layout->usable_end;
        if (limit > layout->usable_end) {
            limit = layout->usable_end;
        }
        if (limit > cursor && limit - cursor >= ALIGNMENT
            && count < capacity) {
            regions[count].first = (uint32_t)cursor;
            regions[count].sectors = (uint32_t)(limit - cursor);
            count++;
        }
        if (k < used) {
            uint64_t end = (uint64_t)layout->slots[order[k]].first
                + layout->slots[order[k]].sectors;
            end = (end + ALIGNMENT - 1) / ALIGNMENT * ALIGNMENT;
            if (end > cursor) {
                cursor = end;
            }
        }
    }
    return count;
}

bool installer_layout_create(struct installer_layout *layout,
                             const struct installer_region *region,
                             uint32_t sectors, const char **error) {
    size_t slot = INSTALLER_NO_SLOT;
    size_t slot_limit = layout->gpt ? INSTALLER_SLOTS : INSTALLER_MBR_SLOTS;
    for (size_t i = 0; i < slot_limit; i++) {
        if (layout->slots[i].type == 0) {
            slot = i;
            break;
        }
    }
    if (slot == INSTALLER_NO_SLOT) {
        return fail(error, layout->gpt ? "TOO MANY PARTITIONS ON THIS DISK"
                                       : "ALL FOUR PARTITION SLOTS ARE IN USE");
    }
    /* MBR: the slot is the table entry. GPT: the first entry number no
       kept partition uses (deleted entries are freed before new ones are
       added). */
    uint32_t number = (uint32_t)slot + 1;
    if (layout->gpt) {
        for (number = 1;; number++) {
            bool taken = false;
            for (size_t i = 0; i < INSTALLER_SLOTS && !taken; i++) {
                taken = layout->slots[i].type != 0
                    && layout->slots[i].number == number;
            }
            if (!taken) {
                break;
            }
        }
    }
    if (sectors > region->sectors) {
        sectors = region->sectors;
    }
    if (sectors != region->sectors) {
        sectors -= sectors % ALIGNMENT;
    }
    if (sectors < INSTALLER_MIN_SECTORS || !fat16_volume_size_ok(sectors)) {
        return fail(error, "TOO SMALL: OPOS NEEDS AT LEAST 64 MB");
    }
    /* The original_* fields keep what the slot held on disk. */
    struct installer_slot *created = &layout->slots[slot];
    created->type = 0x0c;
    created->status = 0;
    created->first = region->first;
    created->sectors = sectors;
    created->created = true;
    created->number = number;
    layout->target = slot;
    return true;
}

bool installer_layout_delete(struct installer_layout *layout, size_t slot) {
    if (slot >= INSTALLER_SLOTS || layout->slots[slot].type == 0) {
        return false;
    }
    struct installer_slot *entry = &layout->slots[slot];
    entry->type = 0;
    entry->status = 0;
    entry->first = 0;
    entry->sectors = 0;
    entry->created = false;
    entry->number = 0;
    if (layout->target == slot) {
        layout->target = INSTALLER_NO_SLOT;
    }
    return true;
}

const char *installer_layout_slot_problem(
    const struct installer_layout *layout, size_t slot) {
    if (slot >= INSTALLER_SLOTS || layout->slots[slot].type == 0) {
        return "NO PARTITION";
    }
    const struct installer_slot *entry = &layout->slots[slot];
    if (extended_type(entry->type)) {
        return "EXTENDED PARTITION (HOLDS OTHER PARTITIONS)";
    }
    if (entry->type == 0xef && !entry->created) {
        return "EFI SYSTEM PARTITION: OTHER SYSTEMS START FROM IT";
    }
    if (entry->type == PARTITION_TYPE_MS_RESERVED) {
        return "RESERVED BY WINDOWS";
    }
    if (entry->type == PARTITION_TYPE_BIOS_BOOT) {
        return "BIOS BOOT PARTITION: GRUB KEEPS ITS CODE HERE";
    }
    if (entry->sectors < INSTALLER_MIN_SECTORS
        || !fat16_volume_size_ok(entry->sectors)) {
        return "TOO SMALL: OPOS NEEDS AT LEAST 64 MB";
    }
    return NULL;
}

bool installer_layout_bios_bootable(const struct installer_layout *layout) {
    if (layout->gpt) {
        return false;
    }
    for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
        if (layout->slots[i].type != 0
            && layout->slots[i].first < BIOS_GAP_END) {
            return false;
        }
    }
    return true;
}

/* GPT: deleted and replaced partitions lose their entries, the OPOS
   partition becomes basic data, new partitions get entries. The entry
   numbers the new partitions got are stored back into the plan. */
static bool write_gpt_table(struct installer_layout *layout) {
    struct partition_gpt_edit edits[2 * INSTALLER_SLOTS];
    size_t slot_of[2 * INSTALLER_SLOTS];
    size_t count = 0;
    for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
        const struct installer_slot *slot = &layout->slots[i];
        if (slot->original_type != 0 && (slot->type == 0 || slot->created)) {
            edits[count] = (struct partition_gpt_edit){
                .number = slot->original_number, .remove = true};
            slot_of[count++] = INSTALLER_NO_SLOT;
        }
    }
    for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
        const struct installer_slot *slot = &layout->slots[i];
        bool retype = i == layout->target && !slot->created;
        if (!retype && !(slot->created && slot->type != 0)) {
            continue;
        }
        struct partition_gpt_edit *edit = &edits[count];
        *edit = (struct partition_gpt_edit){
            .number = retype ? slot->original_number : 0,
            .first = slot->first, .sectors = slot->sectors};
        for (size_t g = 0; g < 16; g++) {
            edit->type_guid[g] = partition_guid_basic_data[g];
        }
        slot_of[count++] = i;
    }
    if (!partition_gpt_apply(edits, count)) {
        return false;
    }
    for (size_t k = 0; k < count; k++) {
        if (slot_of[k] != INSTALLER_NO_SLOT && edits[k].number == 0) {
            layout->slots[slot_of[k]].number = edits[k].assigned;
        }
    }
    return true;
}

/* Writes the planned table into sector 0, keeping the boot code and the
   disk signature (a new one is made if the disk has none). */
static bool write_partition_table(struct installer_layout *layout) {
    if (layout->gpt) {
        return write_gpt_table(layout);
    }
    if (!block_read_sectors(0, 1, sector_buffer)) {
        return false;
    }
    if (sector_buffer[510] != 0x55 || sector_buffer[511] != 0xaa) {
        for (size_t i = 0; i < SECTOR_SIZE; i++) {
            sector_buffer[i] = 0;
        }
    }
    if (le32(sector_buffer + MBR_DISK_AREA_OFFSET) == 0) {
        uint32_t low;
        uint32_t high;
        __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
        write_le32(sector_buffer + MBR_DISK_AREA_OFFSET,
                   (low ^ high ^ 0x4f504f53u) | 1u);
    }
    for (size_t i = 0; i < INSTALLER_MBR_SLOTS; i++) {
        const struct installer_slot *slot = &layout->slots[i];
        uint8_t *entry = sector_buffer + MBR_TABLE_OFFSET + i * 16;
        for (size_t byte = 0; byte < 16; byte++) {
            entry[byte] = 0;
        }
        if (slot->type == 0) {
            continue;
        }
        entry[0] = slot->status;
        lba_to_chs(slot->first, entry + 1);
        entry[4] = slot->type;
        lba_to_chs(slot->first + slot->sectors - 1, entry + 5);
        write_le32(entry + 8, slot->first);
        write_le32(entry + 12, slot->sectors);
    }
    sector_buffer[510] = 0x55;
    sector_buffer[511] = 0xaa;
    return block_write_sectors(0, 1, sector_buffer);
}

static bool partition_steps(struct installer_layout *plan,
                            enum installer_boot_mode mode,
                            installer_progress progress, const char **error) {
    struct payload_file boot;
    if (!archive_find("bios-boot.bin", &boot)
        || boot.size != BIOS_BOOT_SECTORS * SECTOR_SIZE) {
        return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
    }
    struct installer_slot *target = &plan->slots[plan->target];
    /* The old table stays on disk until the files are in place, so an
       interrupted install never leaves a table entry for half a volume. */
    report(progress, "FORMATTING THE OPOS PARTITION", 5);
    uint8_t type;
    if (!fat16_format_volume(target->first, target->sectors,
                             VBR_RESERVED_SECTORS, &type)) {
        return fail(error, "FORMATTING THE PARTITION FAILED");
    }
    target->type = type;
    if (!fat16_mount_volume(target->first, target->sectors)) {
        return fail(error, "THE NEW PARTITION CANNOT BE MOUNTED");
    }
    if (!copy_system_files(progress, error)) {
        return false;
    }
    if (mode == INSTALLER_BOOT_LIMINE_MBR) {
        /* Sectors 0-62 as they were, so the old boot code can be put
           back (e.g. with dd) if Limine is not wanted after all. */
        struct payload_file backup = {boot_backup, sizeof(boot_backup)};
        if (!make_directory("/disk/OPOS")
            || !copy_file(BOOT_BACKUP_PATH, &backup)) {
            return fail(error, "SAVING THE OLD BOOT CODE FAILED");
        }
    }
    report(progress, "INSTALLING PARTITION BOOT CODE", 95);
    if (!write_partition_boot(target->first, &boot)) {
        return fail(error, "WRITING PARTITION BOOT CODE FAILED");
    }
    if (mode == INSTALLER_BOOT_ACTIVE) {
        for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
            plan->slots[i].status = i == plan->target ? 0x80 : 0x00;
        }
    }
    report(progress, "WRITING THE PARTITION TABLE", 96);
    if (!write_partition_table(plan)) {
        return fail(error, "WRITING THE PARTITION TABLE FAILED");
    }
    if (mode == INSTALLER_BOOT_LIMINE_MBR) {
        report(progress, "INSTALLING BIOS BOOT CODE", 98);
        if (!write_bios_boot(&boot)) {
            return fail(error, "WRITING BIOS BOOT CODE FAILED");
        }
    }
    return true;
}

/* ------------------------------------------- UEFI fallback loader on ESP */

static char esp_menu_text[BOOT_MENU_CAPACITY];
static uint8_t copy_buffer[VERIFY_CHUNK];

/* Copies a file between mounted volumes in chunks. */
static bool copy_existing(const char *from, const char *to) {
    size_t from_length = text_length(from);
    size_t to_length = text_length(to);
    size_t size;
    if (!vfs_file_size(from, from_length, &size)
        || !vfs_create_file(to, to_length, true)) {
        return false;
    }
    for (size_t offset = 0; offset < size; offset += VERIFY_CHUNK) {
        size_t chunk = size - offset < VERIFY_CHUNK ? size - offset
                                                    : VERIFY_CHUNK;
        size_t done = 0;
        if (!vfs_read_at(from, from_length, offset, copy_buffer, chunk, &done)
            || done != chunk
            || !vfs_write_at(to, to_length, offset, copy_buffer, chunk, &done)
            || done != chunk) {
            return false;
        }
    }
    size_t copied;
    return vfs_file_size(to, to_length, &copied) && copied == size;
}

static bool file_matches(const char *path, const struct payload_file *file) {
    size_t length = text_length(path);
    size_t size;
    if (!vfs_file_size(path, length, &size) || size != file->size) {
        return false;
    }
    for (size_t offset = 0; offset < size; offset += VERIFY_CHUNK) {
        size_t chunk = size - offset < VERIFY_CHUNK ? size - offset
                                                    : VERIFY_CHUNK;
        size_t done = 0;
        if (!vfs_read_at(path, length, offset, copy_buffer, chunk, &done)
            || done != chunk) {
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (copy_buffer[i] != file->data[offset + i]) {
                return false;
            }
        }
    }
    return true;
}

/* Finds folder `name` under `path` (any case) or creates it. */
static bool enter_directory(char *path, size_t capacity, const char *name) {
    if (find_child(path, capacity, name, VFS_NODE_DIRECTORY)) {
        return true;
    }
    size_t length = text_length(path);
    if (length + 1 + text_length(name) >= capacity) {
        return false;
    }
    length = append(path, capacity, length, "/");
    append(path, capacity, length, name);
    return make_directory(path);
}

/*
 * Makes Limine the fallback loader of the EFI system partition: the old
 * \EFI\BOOT\BOOTX64.EFI is kept as BOOTX64.OLD (unless a backup exists
 * already), and the boot menu goes to \EFI\limine\limine.conf, where
 * Limine looks for it, with OPOS loaded from partition `opos_partition`.
 */
static bool install_esp(size_t disk, const struct installer_boot_scan *scan,
                        uint32_t opos_partition, const char **error) {
    struct payload_file limine;
    struct payload_file menu;
    char path[EFI_PATH_MAX];
    if (!archive_find("BOOTX64.EFI", &limine)
        || !build_menu(scan, opos_partition, esp_menu_text, &menu)) {
        return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
    }
    if (!volume_prefix(disk, scan->esp_first, path)) {
        return fail(error, "THE EFI SYSTEM PARTITION CANNOT BE MOUNTED");
    }
    if (!enter_directory(path, sizeof(path), "EFI")) {
        return fail(error, "WRITING TO THE EFI SYSTEM PARTITION FAILED");
    }
    size_t efi_length = text_length(path);
    if (!enter_directory(path, sizeof(path), "limine")) {
        return fail(error, "WRITING TO THE EFI SYSTEM PARTITION FAILED");
    }
    append(path, sizeof(path), text_length(path), "/limine.conf");
    if (!copy_file(path, &menu)) {
        return fail(error, "WRITING TO THE EFI SYSTEM PARTITION FAILED");
    }
    path[efi_length] = '\0';
    if (!enter_directory(path, sizeof(path), "BOOT")) {
        return fail(error, "WRITING TO THE EFI SYSTEM PARTITION FAILED");
    }
    size_t boot_length = text_length(path);
    if (find_child(path, sizeof(path), "BOOTX64.EFI", VFS_NODE_FILE)
        && !file_matches(path, &limine)) {
        char backup[EFI_PATH_MAX];
        size_t length = append(backup, sizeof(backup), 0, path);
        backup[boot_length] = '\0';
        length = boot_length;
        struct vfs_metadata metadata;
        length = append(backup, sizeof(backup), length, "/BOOTX64.OLD");
        if (!vfs_stat(backup, length, &metadata)
            && !copy_existing(path, backup)) {
            return fail(error, "SAVING THE OLD UEFI LOADER FAILED");
        }
    }
    path[boot_length] = '\0';
    if (!find_child(path, sizeof(path), "BOOTX64.EFI", VFS_NODE_FILE)) {
        append(path, sizeof(path), boot_length, "/BOOTX64.EFI");
    }
    if (!copy_file(path, &limine)) {
        return fail(error, "WRITING TO THE EFI SYSTEM PARTITION FAILED");
    }
    return true;
}

bool installer_install_partition(const struct installer_layout *layout,
                                 enum installer_boot_mode mode,
                                 installer_progress progress,
                                 const char **error) {
    if (!installer_available()) {
        return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
    }
    const char *unusable = installer_disk_unusable(layout->disk);
    if (unusable != NULL) {
        return fail(error, unusable);
    }
    if (layout->target == INSTALLER_NO_SLOT) {
        return fail(error, "NO PARTITION CHOSEN FOR OPOS");
    }
    const char *problem =
        installer_layout_slot_problem(layout, layout->target);
    if (problem != NULL) {
        return fail(error, problem);
    }
    /* The disk must still hold the table the plan was made from. */
    struct installer_layout current;
    if (!installer_layout_load(layout->disk, &current, error)) {
        return false;
    }
    for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
        if (current.slots[i].original_type != layout->slots[i].original_type
            || current.slots[i].original_first
                != layout->slots[i].original_first
            || current.slots[i].original_sectors
                != layout->slots[i].original_sectors) {
            return fail(error, "THE DISK CHANGED; CHOOSE THE PARTITIONS AGAIN");
        }
    }
    if (vfs_disk_objects_open()) {
        return fail(error, "FILES ON THE DISK ARE OPEN");
    }
    /* Look for other systems while their volumes are still mounted. */
    installer_boot_scan(layout->disk, layout, &install_scan);
    if (mode >= INSTALLER_BOOT_MODES) {
        return fail(error, "UNKNOWN BOOT CHOICE");
    }
    problem = installer_boot_mode_problem(layout, &install_scan, mode);
    if (problem != NULL) {
        return fail(error, problem);
    }
    if (!build_boot_menu(&install_scan)) {
        return fail(error, "INSTALL FILES MISSING FROM BOOT MEDIA");
    }
    for (uint32_t sector = 0;
         mode == INSTALLER_BOOT_LIMINE_MBR && sector < BIOS_BOOT_SECTORS;
         sector++) {
        if (!block_disk_read(layout->disk, sector, 1,
                             boot_backup + sector * SECTOR_SIZE)) {
            return fail(error, "THE DISK CANNOT BE READ");
        }
    }
    /* Other volumes may live on the disk being written: drop them (they
       are mounted again afterwards) and make the system volume current. */
    fat16_unmount_others(BLOCK_NO_DISK);
    if (!block_select_disk(layout->disk)) {
        return fail(error, "DISK NOT FOUND");
    }
    struct installer_layout plan = *layout;
    block_begin_batch();
    bool installed = partition_steps(&plan, mode, progress, error);
    if (!block_end_batch() && installed) {
        installed = fail(error, "FLUSHING THE DISK CACHE FAILED");
    }
    /* Mount through the table now, as the next boot will. */
    fat16_prefer_partition(plan.slots[plan.target].number);
    (void)fat16_initialize();
    fat16_mount_others();
    if (installed && !fat16_system_mounted()) {
        installed = fail(error, "THE NEW PARTITION CANNOT BE MOUNTED");
    }
    if (installed && mode == INSTALLER_BOOT_UEFI_DEFAULT) {
        report(progress, "INSTALLING THE UEFI BOOT MENU", 99);
        installed = install_esp(layout->disk, &install_scan,
                                plan.slots[plan.target].number, error);
    }
    if (installed) {
        report(progress, "INSTALLATION COMPLETE", 100);
    }
    return installed;
}
