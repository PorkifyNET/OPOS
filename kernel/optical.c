#include "optical.h"

#include "ahci.h"
#include "ata.h"
#include "scsi.h"

#define CDB_TEST_UNIT_READY 0x00
#define CDB_REQUEST_SENSE 0x03
#define CDB_START_STOP_UNIT 0x1b
#define CDB_PREVENT_ALLOW_REMOVAL 0x1e
#define CDB_READ_CAPACITY_10 0x25
#define CDB_READ_10 0x28

/* A drive that just got a disc reports UNIT ATTENTION and then "becoming
   ready" for a while; a few tries get past both. */
#define READY_ATTEMPTS 6
#define READ_ATTEMPTS 3

struct optical_drive {
    enum optical_bus bus;
    size_t unit;
    uint32_t transfer_limit; /* bytes per command */
    uint32_t sectors;
    bool media;
};

static struct optical_drive drives[OPTICAL_MAX_DRIVES];
static size_t drive_count;
/* Bounce buffer for small replies, so callers need not care about size. */
static uint8_t reply[OPTICAL_SECTOR_SIZE];

static void add_drive(enum optical_bus bus, size_t unit, uint32_t limit) {
    if (drive_count < OPTICAL_MAX_DRIVES && limit >= OPTICAL_SECTOR_SIZE) {
        drives[drive_count++] = (struct optical_drive){
            .bus = bus,
            .unit = unit,
            .transfer_limit = limit,
        };
    }
}

void optical_initialize(void) {
    drive_count = 0;
    for (size_t unit = 0; unit < ATA_UNIT_COUNT; unit++) {
        if (ata_is_atapi(unit)) {
            add_drive(OPTICAL_BUS_IDE, unit, ATA_PACKET_LIMIT);
        }
    }
    for (size_t i = 0; i < ahci_atapi_count(); i++) {
        add_drive(OPTICAL_BUS_SATA, i, ahci_packet_limit());
    }
    for (size_t i = 0; i < scsi_cdrom_count(); i++) {
        add_drive(OPTICAL_BUS_SCSI, i, scsi_cdrom_limit(i));
    }
}

size_t optical_drive_count(void) {
    return drive_count;
}

static enum scsi_result packet(const struct optical_drive *drive,
                               const uint8_t cdb[12], void *buffer,
                               uint32_t length) {
    switch (drive->bus) {
    case OPTICAL_BUS_IDE:
        return ata_packet(drive->unit, cdb, buffer, length);
    case OPTICAL_BUS_SATA:
        return ahci_packet(drive->unit, cdb, buffer, length);
    case OPTICAL_BUS_SCSI:
        return scsi_cdrom_packet(drive->unit, cdb, buffer, length);
    default:
        return SCSI_FAILED;
    }
}

/* Reads (and so clears) the sense data after CHECK CONDITION; returns the
   sense key, or 0xff when it cannot be read. */
static uint8_t request_sense(const struct optical_drive *drive) {
    uint8_t cdb[12] = {CDB_REQUEST_SENSE, 0, 0, 0, 18};
    if (packet(drive, cdb, reply, 18) != SCSI_OK) {
        return 0xff;
    }
    return reply[2] & 0x0f;
}

bool optical_drive_info(size_t index, struct optical_drive_info *info) {
    if (index >= drive_count || info == NULL) {
        return false;
    }
    const struct optical_drive *drive = &drives[index];
    info->bus = drive->bus;
    info->sectors = drive->sectors;
    info->media = drive->media;
    info->location[0] = '\0';
    static const char *const ide_names[ATA_UNIT_COUNT] = {
        "IDE PRIMARY MASTER", "IDE PRIMARY SLAVE",
        "IDE SECONDARY MASTER", "IDE SECONDARY SLAVE",
    };
    const char *location = NULL;
    switch (drive->bus) {
    case OPTICAL_BUS_IDE:
        info->model = ata_atapi_model(drive->unit);
        location = ide_names[drive->unit];
        break;
    case OPTICAL_BUS_SATA: {
        info->model = ahci_atapi_model(drive->unit);
        static const char prefix[] = "SATA PORT ";
        size_t used = 0;
        for (; prefix[used] != '\0'; used++) {
            info->location[used] = prefix[used];
        }
        uint32_t port = ahci_atapi_port(drive->unit);
        if (port >= 10) {
            info->location[used++] = (char)('0' + port / 10);
        }
        info->location[used++] = (char)('0' + port % 10);
        info->location[used] = '\0';
        break;
    }
    case OPTICAL_BUS_SCSI:
        info->model = scsi_cdrom_model(drive->unit);
        scsi_cdrom_location(drive->unit, info->location,
                            sizeof(info->location));
        break;
    }
    if (location != NULL) {
        size_t i = 0;
        for (; location[i] != '\0' && i + 1 < sizeof(info->location); i++) {
            info->location[i] = location[i];
        }
        info->location[i] = '\0';
    }
    return true;
}

static uint32_t be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
        | ((uint32_t)bytes[2] << 8) | bytes[3];
}

bool optical_check(size_t index) {
    if (index >= drive_count) {
        return false;
    }
    struct optical_drive *drive = &drives[index];
    drive->media = false;
    drive->sectors = 0;
    bool ready = false;
    unsigned no_medium = 0;
    for (unsigned attempt = 0; attempt < READY_ATTEMPTS && !ready;
         attempt++) {
        uint8_t cdb[12] = {CDB_TEST_UNIT_READY};
        enum scsi_result result = packet(drive, cdb, NULL, 0);
        if (result == SCSI_OK) {
            ready = true;
        } else if (result == SCSI_CHECK_CONDITION) {
            /* 6 UNIT ATTENTION (disc changed) and 2 NOT READY "becoming
               ready" clear on a retry. MEDIUM NOT PRESENT is final when
               it repeats: right after a disc change, drives (and QEMU)
               report it once, as if the tray had just closed. */
            uint8_t key = request_sense(drive);
            if (key == 2 && reply[12] == 0x3a) {
                if (++no_medium == 2) {
                    return false;
                }
            } else {
                no_medium = 0;
            }
        } else {
            return false;
        }
    }
    if (!ready) {
        return false;
    }
    uint8_t cdb[12] = {CDB_READ_CAPACITY_10};
    for (unsigned attempt = 0; attempt < READ_ATTEMPTS; attempt++) {
        enum scsi_result result = packet(drive, cdb, reply, 8);
        if (result == SCSI_OK) {
            uint32_t last = be32(reply);
            uint32_t block = be32(reply + 4);
            /* Some drives report 2352 or 0 for data discs; reads are still
               2048 bytes per sector. */
            if (block != 0 && block != OPTICAL_SECTOR_SIZE && block != 2352) {
                return false;
            }
            drive->sectors = last + 1;
            drive->media = true;
            return true;
        }
        if (result != SCSI_CHECK_CONDITION) {
            return false;
        }
        (void)request_sense(drive);
    }
    return false;
}

bool optical_read(size_t index, uint32_t first_sector, uint32_t count,
                  void *buffer) {
    if (index >= drive_count || buffer == NULL) {
        return false;
    }
    const struct optical_drive *drive = &drives[index];
    uint32_t chunk_limit = drive->transfer_limit / OPTICAL_SECTOR_SIZE;
    uint8_t *destination = buffer;
    while (count != 0) {
        uint32_t chunk = count < chunk_limit ? count : chunk_limit;
        uint8_t cdb[12] = {
            CDB_READ_10, 0,
            (uint8_t)(first_sector >> 24), (uint8_t)(first_sector >> 16),
            (uint8_t)(first_sector >> 8), (uint8_t)first_sector,
            0, (uint8_t)(chunk >> 8), (uint8_t)chunk,
        };
        bool done = false;
        for (unsigned attempt = 0; attempt < READ_ATTEMPTS && !done;
             attempt++) {
            enum scsi_result result = packet(drive, cdb, destination,
                                             chunk * OPTICAL_SECTOR_SIZE);
            if (result == SCSI_OK) {
                done = true;
            } else if (result == SCSI_CHECK_CONDITION) {
                uint8_t key = request_sense(drive);
                /* NOT READY (no disc) and MEDIUM ERROR do not get
                   better by retrying. */
                if (key == 2 || key == 3) {
                    return false;
                }
            } else {
                return false;
            }
        }
        if (!done) {
            return false;
        }
        destination += (size_t)chunk * OPTICAL_SECTOR_SIZE;
        first_sector += chunk;
        count -= chunk;
    }
    return true;
}

bool optical_eject(size_t index) {
    if (index >= drive_count) {
        return false;
    }
    const struct optical_drive *drive = &drives[index];
    uint8_t allow[12] = {CDB_PREVENT_ALLOW_REMOVAL};
    (void)packet(drive, allow, NULL, 0);
    /* LoEj=1, Start=0: open the tray. */
    uint8_t cdb[12] = {CDB_START_STOP_UNIT, 0, 0, 0, 0x02};
    enum scsi_result result = packet(drive, cdb, NULL, 0);
    if (result == SCSI_CHECK_CONDITION) {
        (void)request_sense(drive);
    }
    drives[index].media = false;
    drives[index].sectors = 0;
    return result == SCSI_OK;
}
