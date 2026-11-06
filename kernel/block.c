#include "ahci.h"
#include "ata.h"
#include "block.h"
#include "nvme.h"
#include "scsi.h"
#include "sdhci.h"

struct block_disk {
    enum block_device device;
    size_t unit;
    bool boot_disk;
};

static struct block_disk disks[BLOCK_MAX_DISKS];
static size_t disk_count;
static size_t active_index = BLOCK_NO_DISK;

static uint32_t boot_mbr_id;
static uint8_t boot_gpt_guid[16];
static bool boot_gpt_known;

void block_set_boot_identity(uint32_t mbr_disk_id,
                             const uint8_t gpt_guid[16]) {
    boot_mbr_id = mbr_disk_id;
    boot_gpt_known = false;
    for (size_t i = 0; i < 16; i++) {
        boot_gpt_guid[i] = gpt_guid != NULL ? gpt_guid[i] : 0;
        boot_gpt_known = boot_gpt_known || boot_gpt_guid[i] != 0;
    }
}

static uint64_t disk_sectors(const struct block_disk *disk) {
    switch (disk->device) {
    case BLOCK_DEVICE_ATA:
        return ata_sector_count(disk->unit);
    case BLOCK_DEVICE_AHCI:
        return ahci_sector_count(disk->unit);
    case BLOCK_DEVICE_NVME:
        return nvme_sector_count(disk->unit);
    case BLOCK_DEVICE_SCSI:
        return scsi_sector_count(disk->unit);
    case BLOCK_DEVICE_SD:
        return sdhci_sector_count(disk->unit);
    default:
        return 0;
    }
}

static bool disk_read(const struct block_disk *disk, uint32_t first_sector,
                      uint8_t sector_count, void *buffer) {
    switch (disk->device) {
    case BLOCK_DEVICE_ATA:
        return ata_read_sectors(disk->unit, first_sector, sector_count,
                                buffer);
    case BLOCK_DEVICE_AHCI:
        return ahci_read_sectors(disk->unit, first_sector, sector_count,
                                 buffer);
    case BLOCK_DEVICE_NVME:
        return nvme_read_sectors(disk->unit, first_sector, sector_count,
                                 buffer);
    case BLOCK_DEVICE_SCSI:
        return scsi_read_sectors(disk->unit, first_sector, sector_count,
                                 buffer);
    case BLOCK_DEVICE_SD:
        return sdhci_read_sectors(disk->unit, first_sector, sector_count,
                                  buffer);
    default:
        return false;
    }
}

static bool disk_write(const struct block_disk *disk, uint32_t first_sector,
                       uint8_t sector_count, const void *buffer) {
    switch (disk->device) {
    case BLOCK_DEVICE_ATA:
        return ata_write_sectors(disk->unit, first_sector, sector_count,
                                 buffer);
    case BLOCK_DEVICE_AHCI:
        return ahci_write_sectors(disk->unit, first_sector, sector_count,
                                  buffer);
    case BLOCK_DEVICE_NVME:
        return nvme_write_sectors(disk->unit, first_sector, sector_count,
                                  buffer);
    case BLOCK_DEVICE_SCSI:
        return scsi_write_sectors(disk->unit, first_sector, sector_count,
                                  buffer);
    case BLOCK_DEVICE_SD:
        return sdhci_write_sectors(disk->unit, first_sector, sector_count,
                                   buffer);
    default:
        return false;
    }
}

static void add_disk(enum block_device device, size_t unit) {
    if (disk_count < BLOCK_MAX_DISKS) {
        disks[disk_count++] = (struct block_disk){device, unit, false};
    }
}

/* Matches the boot loader's MBR signature (bytes 440-443 of sector 0) or
   GPT disk GUID (offset 56 of the GPT header in sector 1). */
static bool is_boot_disk(const struct block_disk *disk) {
    uint8_t sector[512];
    if (boot_mbr_id != 0 && disk_read(disk, 0, 1, sector)
        && sector[510] == 0x55 && sector[511] == 0xaa) {
        uint32_t id = (uint32_t)sector[440] | ((uint32_t)sector[441] << 8)
            | ((uint32_t)sector[442] << 16) | ((uint32_t)sector[443] << 24);
        if (id == boot_mbr_id) {
            return true;
        }
    }
    if (boot_gpt_known && disk_read(disk, 1, 1, sector)) {
        static const char signature[8] = "EFI PART";
        for (size_t i = 0; i < 8; i++) {
            if (sector[i] != (uint8_t)signature[i]) {
                return false;
            }
        }
        for (size_t i = 0; i < 16; i++) {
            if (sector[56 + i] != boot_gpt_guid[i]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

enum block_device block_initialize(void) {
    disk_count = 0;
    active_index = BLOCK_NO_DISK;
    (void)ata_initialize();
    for (size_t unit = 0; unit < ATA_UNIT_COUNT; unit++) {
        if (ata_sector_count(unit) != 0) {
            add_disk(BLOCK_DEVICE_ATA, unit);
        }
    }
    for (size_t unit = 0; unit < ahci_disk_count(); unit++) {
        add_disk(BLOCK_DEVICE_AHCI, unit);
    }
    for (size_t unit = 0; unit < nvme_disk_count(); unit++) {
        add_disk(BLOCK_DEVICE_NVME, unit);
    }
    for (size_t unit = 0; unit < scsi_disk_count(); unit++) {
        add_disk(BLOCK_DEVICE_SCSI, unit);
    }
    for (size_t unit = 0; unit < sdhci_disk_count(); unit++) {
        add_disk(BLOCK_DEVICE_SD, unit);
    }
    for (size_t i = 0; i < disk_count; i++) {
        disks[i].boot_disk = is_boot_disk(&disks[i]);
        if (disks[i].boot_disk && active_index == BLOCK_NO_DISK) {
            active_index = i;
        }
    }
    if (active_index == BLOCK_NO_DISK && disk_count != 0) {
        active_index = 0;
    }
    return block_active_device();
}

bool block_disk_present(size_t index) {
    if (index >= disk_count) {
        return false;
    }
    /* Only SCSI disks (USB sticks among them) can go away. */
    return disks[index].device != BLOCK_DEVICE_SCSI
        || scsi_disk_present(disks[index].unit);
}

uint32_t block_disk_generation(size_t index) {
    if (!block_disk_present(index)) {
        return 0;
    }
    return disks[index].device == BLOCK_DEVICE_SCSI
        ? scsi_disk_generation(disks[index].unit) : 1;
}

bool block_rescan(void) {
    bool added = false;
    for (size_t unit = 0; unit < scsi_disk_count(); unit++) {
        bool known = false;
        for (size_t i = 0; i < disk_count && !known; i++) {
            known = disks[i].device == BLOCK_DEVICE_SCSI
                && disks[i].unit == unit;
        }
        if (!known && scsi_disk_present(unit)
            && disk_count < BLOCK_MAX_DISKS) {
            add_disk(BLOCK_DEVICE_SCSI, unit);
            added = true;
        }
    }
    return added;
}

size_t block_disk_count(void) {
    return disk_count;
}

static size_t append(char *buffer, size_t capacity, size_t offset,
                     const char *text) {
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    buffer[offset] = '\0';
    return offset;
}

static size_t append_number(char *buffer, size_t capacity, size_t offset,
                            uint32_t value) {
    char digits[12];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0 && offset + 1 < capacity) {
        buffer[offset++] = digits[--count];
    }
    buffer[offset] = '\0';
    return offset;
}

bool block_disk_info(size_t index, struct block_disk_info *info) {
    if (index >= disk_count || info == NULL || !block_disk_present(index)) {
        return false;
    }
    const struct block_disk *disk = &disks[index];
    info->device = disk->device;
    info->sectors = disk_sectors(disk);
    info->boot_disk = disk->boot_disk;
    info->removable = (disk->device == BLOCK_DEVICE_SCSI
                       && scsi_removable(disk->unit))
        || (disk->device == BLOCK_DEVICE_SD && sdhci_removable(disk->unit));
    char *location = info->location;
    size_t capacity = sizeof(info->location);
    location[0] = '\0';
    switch (disk->device) {
    case BLOCK_DEVICE_ATA: {
        static const char *const names[ATA_UNIT_COUNT] = {
            "IDE PRIMARY MASTER", "IDE PRIMARY SLAVE",
            "IDE SECONDARY MASTER", "IDE SECONDARY SLAVE",
        };
        info->model = ata_model(disk->unit);
        append(location, capacity, 0, names[disk->unit]);
        break;
    }
    case BLOCK_DEVICE_AHCI:
        info->model = ahci_model(disk->unit);
        append_number(location, capacity,
                      append(location, capacity, 0, "SATA PORT "),
                      ahci_port(disk->unit));
        break;
    case BLOCK_DEVICE_NVME:
        info->model = nvme_model(disk->unit);
        append_number(location, capacity,
                      append(location, capacity, 0, "NVME NAMESPACE "),
                      nvme_namespace_id(disk->unit));
        break;
    case BLOCK_DEVICE_SCSI:
        info->model = scsi_model(disk->unit);
        scsi_location(disk->unit, location, capacity);
        break;
    case BLOCK_DEVICE_SD:
        info->model = sdhci_model(disk->unit);
        sdhci_location(disk->unit, location, capacity);
        break;
    default:
        info->model = "";
        break;
    }
    return true;
}

bool block_disk_read(size_t index, uint32_t first_sector,
                     uint8_t sector_count, void *buffer) {
    return index < disk_count
        && disk_read(&disks[index], first_sector, sector_count, buffer);
}

bool block_select_disk(size_t index) {
    if (index >= disk_count || !block_disk_present(index)) {
        return false;
    }
    active_index = index;
    return true;
}

size_t block_active_disk(void) {
    return active_index;
}

static const struct block_disk *active(void) {
    return active_index < disk_count ? &disks[active_index] : NULL;
}

enum block_device block_active_device(void) {
    const struct block_disk *disk = active();
    return disk != NULL ? disk->device : BLOCK_DEVICE_NONE;
}

const char *block_device_name(enum block_device device) {
    switch (device) {
    case BLOCK_DEVICE_ATA:
        return "IDE disk";
    case BLOCK_DEVICE_AHCI:
        return "SATA disk";
    case BLOCK_DEVICE_NVME:
        return "NVMe disk";
    case BLOCK_DEVICE_SCSI:
        return "SCSI disk";
    case BLOCK_DEVICE_SD:
        return "SD/eMMC disk";
    default:
        return "none";
    }
}

void block_begin_batch(void) {
    ata_set_flush_deferred(true);
    nvme_set_flush_deferred(true);
    scsi_set_flush_deferred(true);
}

bool block_end_batch(void) {
    ata_set_flush_deferred(false);
    nvme_set_flush_deferred(false);
    scsi_set_flush_deferred(false);
    const struct block_disk *disk = active();
    if (disk == NULL) {
        return true;
    }
    switch (disk->device) {
    case BLOCK_DEVICE_ATA:
        return ata_flush(disk->unit);
    case BLOCK_DEVICE_NVME:
        return nvme_flush(disk->unit);
    case BLOCK_DEVICE_SCSI:
        return scsi_flush(disk->unit);
    default:
        return true;
    }
}

uint64_t block_sector_count(void) {
    const struct block_disk *disk = active();
    return disk != NULL ? disk_sectors(disk) : 0;
}

bool block_read_sectors(uint32_t first_sector, uint8_t sector_count,
                        void *buffer) {
    const struct block_disk *disk = active();
    return disk != NULL && disk_read(disk, first_sector, sector_count, buffer);
}

bool block_write_sectors(uint32_t first_sector, uint8_t sector_count,
                         const void *buffer) {
    const struct block_disk *disk = active();
    return disk != NULL
        && disk_write(disk, first_sector, sector_count, buffer);
}
