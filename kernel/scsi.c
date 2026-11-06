#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scsi.h"

#define SECTOR_SIZE 512
#define COMMAND_RETRIES 4

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_INQUIRY 0x12
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2a
#define SCSI_SYNCHRONIZE_CACHE_10 0x35
#define SCSI_SERVICE_ACTION_IN_16 0x9e
#define SCSI_READ_CAPACITY_16 0x10

struct scsi_disk {
    const struct scsi_host *host;
    uint8_t target;
    uint64_t sectors;
    char model[26];
    /* False once the device is gone (USB unplug); the slot then reads
       as empty and can be reused by the next device. */
    bool present;
    /* INQUIRY RMB: removable medium (USB sticks, card readers). */
    bool removable;
    /* New for every device put in this slot. */
    uint32_t generation;
};

static struct scsi_disk disks[SCSI_MAX_DISKS];
static size_t disk_count;
static bool flush_deferred;
/* CD/DVD targets; `sectors` is unused (optical.c asks the drive). */
static struct scsi_disk cdroms[SCSI_MAX_CDROMS];
static size_t cdrom_count;

/* Retries after CHECK CONDITION, e.g. the unit attention every target
   reports once after a reset. */
static bool command(const struct scsi_host *host, uint8_t target,
                    const uint8_t *cdb, uint8_t cdb_length,
                    enum scsi_direction direction, uint32_t length) {
    if (length > host->buffer_size) {
        return false;
    }
    for (unsigned attempt = 0; attempt < COMMAND_RETRIES; attempt++) {
        enum scsi_result result =
            host->execute(target, cdb, cdb_length, direction, length);
        if (result == SCSI_OK) {
            return true;
        }
        if (result != SCSI_CHECK_CONDITION) {
            return false;
        }
    }
    return false;
}

static uint32_t be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
        | ((uint32_t)bytes[2] << 8) | bytes[3];
}

/* Copies INQUIRY's vendor and product, collapsing the space padding. */
static void set_model(struct scsi_disk *disk, const uint8_t *inquiry) {
    size_t length = 0;
    for (size_t i = 8; i < 32; i++) {
        char character = (char)inquiry[i];
        if (i == 16 && length != 0 && disk->model[length - 1] != ' ') {
            disk->model[length++] = ' ';
        }
        if (character < 0x20 || character > 0x7e) {
            character = ' ';
        }
        if (character == ' '
            && (length == 0 || disk->model[length - 1] == ' ')) {
            continue;
        }
        disk->model[length++] = character;
    }
    while (length != 0 && disk->model[length - 1] == ' ') {
        length--;
    }
    disk->model[length] = '\0';
}

static uint32_t next_generation = 1;

static bool probe_target(const struct scsi_host *host, uint8_t target) {
    uint8_t *data = host->buffer;
    uint8_t inquiry[6] = {SCSI_INQUIRY, 0, 0, 0, 36, 0};
    if (!command(host, target, inquiry, 6, SCSI_DATA_IN, 36)) {
        return false;
    }
    /* Peripheral qualifier 0 (connected), device type 5 (CD/DVD). */
    if (data[0] == 0x05) {
        if (cdrom_count < SCSI_MAX_CDROMS) {
            struct scsi_disk *cdrom = &cdroms[cdrom_count++];
            *cdrom = (struct scsi_disk){.host = host, .target = target};
            set_model(cdrom, data);
        }
        return false;
    }
    /* Peripheral qualifier 0 (connected), device type 0 (disk). */
    if (data[0] != 0x00) {
        return false;
    }
    struct scsi_disk disk = {
        .host = host,
        .target = target,
        .present = true,
        .removable = (data[1] & 0x80) != 0 || host->hotplug,
    };
    set_model(&disk, data);
    uint8_t ready[6] = {SCSI_TEST_UNIT_READY};
    (void)command(host, target, ready, 6, SCSI_DATA_NONE, 0);

    uint8_t capacity10[10] = {SCSI_READ_CAPACITY_10};
    if (!command(host, target, capacity10, 10, SCSI_DATA_IN, 8)) {
        return false;
    }
    uint64_t last = be32(data);
    uint32_t block_size = be32(data + 4);
    if (last == 0xffffffffu) {
        uint8_t capacity16[16] = {SCSI_SERVICE_ACTION_IN_16,
                                  SCSI_READ_CAPACITY_16};
        capacity16[13] = 32;
        if (!command(host, target, capacity16, 16, SCSI_DATA_IN, 32)) {
            return false;
        }
        last = ((uint64_t)be32(data) << 32) | be32(data + 4);
        block_size = be32(data + 8);
    }
    /* Reuse the slot of a device that was unplugged, else append. */
    size_t slot = disk_count;
    for (size_t i = 0; i < disk_count; i++) {
        if (!disks[i].present) {
            slot = i;
            break;
        }
    }
    if (block_size != SECTOR_SIZE || slot >= SCSI_MAX_DISKS) {
        return false;
    }
    disk.sectors = last + 1;
    disk.generation = next_generation++;
    disks[slot] = disk;
    if (slot == disk_count) {
        disk_count++;
    }
    return true;
}

size_t scsi_scan_host(const struct scsi_host *host, unsigned target_count) {
    size_t added = 0;
    if (host == NULL || host->buffer == NULL || host->buffer_size < 4096) {
        return 0;
    }
    for (unsigned target = 0;
         target < target_count && target < SCSI_MAX_TARGETS; target++) {
        if (probe_target(host, (uint8_t)target)) {
            added++;
        }
    }
    return added;
}

bool scsi_probe_target(const struct scsi_host *host, uint8_t target) {
    return host != NULL && host->buffer != NULL && host->buffer_size >= 4096
        && probe_target(host, target);
}

void scsi_remove_target(const struct scsi_host *host, uint8_t target) {
    for (size_t i = 0; i < disk_count; i++) {
        if (disks[i].present && disks[i].host == host
            && disks[i].target == target) {
            disks[i].present = false;
            disks[i].sectors = 0;
        }
    }
}

size_t scsi_disk_count(void) {
    return disk_count;
}

bool scsi_disk_present(size_t disk) {
    return disk < disk_count && disks[disk].present;
}

uint32_t scsi_disk_generation(size_t disk) {
    return scsi_disk_present(disk) ? disks[disk].generation : 0;
}

bool scsi_removable(size_t disk) {
    return scsi_disk_present(disk) && disks[disk].removable;
}

uint64_t scsi_sector_count(size_t disk) {
    return scsi_disk_present(disk) ? disks[disk].sectors : 0;
}

const char *scsi_model(size_t disk) {
    return scsi_disk_present(disk) ? disks[disk].model : "";
}

void scsi_location(size_t disk, char *buffer, size_t capacity) {
    size_t length = 0;
    if (capacity == 0) {
        return;
    }
    if (disk < disk_count && disks[disk].host->location != NULL) {
        disks[disk].host->location(disks[disk].target, buffer, capacity);
        return;
    }
    if (disk < disk_count) {
        const char *parts[2] = {disks[disk].host->name, " TARGET "};
        for (size_t p = 0; p < 2; p++) {
            for (const char *c = parts[p]; *c != '\0' && length + 1 < capacity;
                 c++) {
                buffer[length++] = *c;
            }
        }
        unsigned target = disks[disk].target;
        if (target >= 10 && length + 1 < capacity) {
            buffer[length++] = (char)('0' + target / 10);
        }
        if (length + 1 < capacity) {
            buffer[length++] = (char)('0' + target % 10);
        }
    }
    buffer[length] = '\0';
}

static void read_write_cdb(uint8_t opcode, uint32_t lba, uint16_t count,
                           uint8_t cdb[10]) {
    for (size_t i = 0; i < 10; i++) {
        cdb[i] = 0;
    }
    cdb[0] = opcode;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
}

static bool in_range(size_t disk, uint32_t first_sector,
                     uint8_t sector_count) {
    return disk < disk_count && sector_count != 0
        && (uint64_t)first_sector + sector_count <= disks[disk].sectors;
}

bool scsi_read_sectors(size_t disk, uint32_t first_sector,
                       uint8_t sector_count, void *buffer) {
    if (!in_range(disk, first_sector, sector_count) || buffer == NULL) {
        return false;
    }
    const struct scsi_host *host = disks[disk].host;
    uint32_t chunk_limit = host->buffer_size / SECTOR_SIZE;
    uint8_t *destination = buffer;
    uint32_t done = 0;
    while (done < sector_count) {
        uint32_t chunk = sector_count - done;
        if (chunk > chunk_limit) {
            chunk = chunk_limit;
        }
        uint8_t cdb[10];
        read_write_cdb(SCSI_READ_10, first_sector + done, (uint16_t)chunk,
                       cdb);
        if (!command(host, disks[disk].target, cdb, 10, SCSI_DATA_IN,
                     chunk * SECTOR_SIZE)) {
            return false;
        }
        for (size_t i = 0; i < chunk * SECTOR_SIZE; i++) {
            destination[done * SECTOR_SIZE + i] = host->buffer[i];
        }
        done += chunk;
    }
    return true;
}

bool scsi_flush(size_t disk) {
    if (!scsi_disk_present(disk)) {
        return false;
    }
    uint8_t cdb[10] = {SCSI_SYNCHRONIZE_CACHE_10};
    return command(disks[disk].host, disks[disk].target, cdb, 10,
                   SCSI_DATA_NONE, 0);
}

void scsi_set_flush_deferred(bool deferred) {
    flush_deferred = deferred;
}

bool scsi_write_sectors(size_t disk, uint32_t first_sector,
                        uint8_t sector_count, const void *buffer) {
    if (!in_range(disk, first_sector, sector_count) || buffer == NULL) {
        return false;
    }
    const struct scsi_host *host = disks[disk].host;
    uint32_t chunk_limit = host->buffer_size / SECTOR_SIZE;
    const uint8_t *source = buffer;
    uint32_t done = 0;
    while (done < sector_count) {
        uint32_t chunk = sector_count - done;
        if (chunk > chunk_limit) {
            chunk = chunk_limit;
        }
        for (size_t i = 0; i < chunk * SECTOR_SIZE; i++) {
            host->buffer[i] = source[done * SECTOR_SIZE + i];
        }
        uint8_t cdb[10];
        read_write_cdb(SCSI_WRITE_10, first_sector + done, (uint16_t)chunk,
                       cdb);
        if (!command(host, disks[disk].target, cdb, 10, SCSI_DATA_OUT,
                     chunk * SECTOR_SIZE)) {
            return false;
        }
        done += chunk;
    }
    return flush_deferred || scsi_flush(disk);
}

/* ------------------------------------------------------------ CD/DVD */

size_t scsi_cdrom_count(void) {
    return cdrom_count;
}

const char *scsi_cdrom_model(size_t drive) {
    return drive < cdrom_count ? cdroms[drive].model : "";
}

void scsi_cdrom_location(size_t drive, char *buffer, size_t capacity) {
    size_t length = 0;
    if (capacity == 0) {
        return;
    }
    if (drive < cdrom_count) {
        const char *parts[2] = {cdroms[drive].host->name, " TARGET "};
        for (size_t p = 0; p < 2; p++) {
            for (const char *c = parts[p]; *c != '\0' && length + 1 < capacity;
                 c++) {
                buffer[length++] = *c;
            }
        }
        unsigned target = cdroms[drive].target;
        if (target >= 10 && length + 1 < capacity) {
            buffer[length++] = (char)('0' + target / 10);
        }
        if (length + 1 < capacity) {
            buffer[length++] = (char)('0' + target % 10);
        }
    }
    buffer[length] = '\0';
}

uint32_t scsi_cdrom_limit(size_t drive) {
    return drive < cdrom_count ? cdroms[drive].host->buffer_size : 0;
}

enum scsi_result scsi_cdrom_packet(size_t drive, const uint8_t cdb[12],
                                   void *buffer, uint32_t length) {
    if (drive >= cdrom_count || cdb == NULL
        || (length != 0 && buffer == NULL)) {
        return SCSI_FAILED;
    }
    const struct scsi_host *host = cdroms[drive].host;
    if (length > host->buffer_size) {
        return SCSI_FAILED;
    }
    /* The CDB length follows from the opcode's group. */
    uint8_t group = cdb[0] >> 5;
    uint8_t cdb_length = group == 0 ? 6 : group == 5 ? 12 : 10;
    enum scsi_result result =
        host->execute(cdroms[drive].target, cdb, cdb_length,
                      length != 0 ? SCSI_DATA_IN : SCSI_DATA_NONE, length);
    if (result == SCSI_OK && length != 0) {
        uint8_t *destination = buffer;
        for (uint32_t i = 0; i < length; i++) {
            destination[i] = host->buffer[i];
        }
    }
    return result;
}
