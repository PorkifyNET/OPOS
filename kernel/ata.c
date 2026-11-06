#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ata.h"
#include "io.h"

/* Register offsets from a channel's command block base. */
#define ATA_DATA 0
#define ATA_SECTOR_COUNT 2
#define ATA_LBA_LOW 3
#define ATA_LBA_MID 4
#define ATA_LBA_HIGH 5
#define ATA_DRIVE_HEAD 6
#define ATA_STATUS 7
#define ATA_COMMAND 7
#define ATA_STATUS_ERROR 0x01
#define ATA_STATUS_DEVICE_FAULT 0x20
#define ATA_STATUS_DRQ 0x08
#define ATA_STATUS_BUSY 0x80
#define ATA_COMMAND_IDENTIFY 0xec
#define ATA_COMMAND_READ_SECTORS 0x20
#define ATA_COMMAND_READ_SECTORS_EXT 0x24
#define ATA_COMMAND_WRITE_SECTORS 0x30
#define ATA_COMMAND_WRITE_SECTORS_EXT 0x34
#define ATA_COMMAND_CACHE_FLUSH 0xe7
#define ATA_COMMAND_CACHE_FLUSH_EXT 0xea
#define ATA_LBA28_MAX 0x0fffffff
#define ATA_IDENTIFY_LBA48_SUPPORTED (1u << 10)
#define ATA_POLL_LIMIT 1000000
/* A channel with nothing attached floats its status register high; the
   IDENTIFY wait gives up quickly instead of spinning the full limit. */
#define ATA_PROBE_POLL_LIMIT 100000

#define ATA_FEATURES 1
#define ATA_COMMAND_PACKET 0xa0
#define ATA_COMMAND_IDENTIFY_PACKET 0xa1
/* Packet commands wait for the drive itself (spin-up, seeks), so they get
   a much longer poll limit than register commands. */
#define ATA_PACKET_POLL_LIMIT 50000000

struct ata_unit {
    bool ready;
    bool lba48;
    /* ATAPI CD/DVD drive (ready stays false: it is not a disk). */
    bool atapi;
    uint64_t sector_count;
    char model[ATA_MODEL_LENGTH + 1];
};

static const uint16_t channel_base[2] = {0x1f0, 0x170};
static const uint16_t channel_control[2] = {0x3f6, 0x376};

static struct ata_unit units[ATA_UNIT_COUNT];
/* While set, writes skip the per-write cache flush; ata_flush sends one. */
static bool flush_deferred;

static uint16_t base_of(size_t unit) {
    return channel_base[unit / 2];
}

/* Drive/head bits for the unit: bit 4 selects the slave. */
static uint8_t drive_bits(size_t unit) {
    return (unit & 1) != 0 ? 0x10 : 0x00;
}

static void ata_delay_400ns(size_t unit) {
    for (unsigned i = 0; i < 4; i++) {
        (void)io_in8(channel_control[unit / 2]);
    }
}

static bool ata_wait_status_limit(size_t unit, bool require_data,
                                  unsigned limit) {
    uint16_t base = base_of(unit);
    for (unsigned i = 0; i < limit; i++) {
        uint8_t status = io_in8(base + ATA_STATUS);
        if (status == 0 || status == 0xff) {
            return false;
        }
        if ((status & ATA_STATUS_BUSY) != 0) {
            continue;
        }
        if ((status & (ATA_STATUS_ERROR | ATA_STATUS_DEVICE_FAULT)) != 0) {
            return false;
        }
        if (!require_data || (status & ATA_STATUS_DRQ) != 0) {
            return true;
        }
    }
    return false;
}

static bool ata_wait_status(size_t unit, bool require_data) {
    return ata_wait_status_limit(unit, require_data, ATA_POLL_LIMIT);
}

/* String I/O moves a whole sector per instruction; hypervisors (KVM,
   VMware) handle it in bulk instead of trapping on every 16-bit word,
   which made disk writes about 50 KB/s. */
static void ata_read_data_sector(size_t unit, uint8_t *buffer) {
    void *destination = buffer;
    size_t words = 512 / sizeof(uint16_t);
    __asm__ volatile ("cld; rep insw"
                      : "+D"(destination), "+c"(words)
                      : "d"((uint16_t)(base_of(unit) + ATA_DATA))
                      : "memory");
}

static void ata_write_data_sector(size_t unit, const uint8_t *buffer) {
    const void *source = buffer;
    size_t words = 512 / sizeof(uint16_t);
    __asm__ volatile ("cld; rep outsw"
                      : "+S"(source), "+c"(words)
                      : "d"((uint16_t)(base_of(unit) + ATA_DATA))
                      : "memory");
}

static bool unit_ready(size_t unit) {
    return unit < ATA_UNIT_COUNT && units[unit].ready;
}

/* Programs the task file for one transfer. Requests that end inside the
   LBA28 range keep using the 28-bit commands; anything beyond uses the
   48-bit EXT commands when the drive supports them. */
static bool ata_start_command(size_t unit, uint32_t first_sector,
                              uint8_t sector_count, bool write) {
    if (!unit_ready(unit) || sector_count == 0
        || first_sector >= units[unit].sector_count
        || (uint64_t)sector_count
            > units[unit].sector_count - first_sector) {
        return false;
    }
    bool lba48 = (uint64_t)first_sector + sector_count - 1 > ATA_LBA28_MAX;
    if (lba48 && !units[unit].lba48) {
        return false;
    }
    uint16_t base = base_of(unit);

    if (lba48) {
        io_out8(base + ATA_DRIVE_HEAD, (uint8_t)(0x40 | drive_bits(unit)));
        ata_delay_400ns(unit);
        if (!ata_wait_status(unit, false)) {
            return false;
        }
        io_out8(base + ATA_SECTOR_COUNT, 0);
        io_out8(base + ATA_LBA_LOW, (uint8_t)(first_sector >> 24));
        io_out8(base + ATA_LBA_MID, 0);
        io_out8(base + ATA_LBA_HIGH, 0);
        io_out8(base + ATA_SECTOR_COUNT, sector_count);
        io_out8(base + ATA_LBA_LOW, (uint8_t)first_sector);
        io_out8(base + ATA_LBA_MID, (uint8_t)(first_sector >> 8));
        io_out8(base + ATA_LBA_HIGH, (uint8_t)(first_sector >> 16));
        io_out8(base + ATA_COMMAND, write ? ATA_COMMAND_WRITE_SECTORS_EXT
                                          : ATA_COMMAND_READ_SECTORS_EXT);
        return true;
    }

    io_out8(base + ATA_DRIVE_HEAD,
            (uint8_t)(0xe0 | drive_bits(unit)
                      | ((first_sector >> 24) & 0x0f)));
    ata_delay_400ns(unit);
    if (!ata_wait_status(unit, false)) {
        return false;
    }
    io_out8(base + ATA_SECTOR_COUNT, sector_count);
    io_out8(base + ATA_LBA_LOW, (uint8_t)first_sector);
    io_out8(base + ATA_LBA_MID, (uint8_t)(first_sector >> 8));
    io_out8(base + ATA_LBA_HIGH, (uint8_t)(first_sector >> 16));
    io_out8(base + ATA_COMMAND, write ? ATA_COMMAND_WRITE_SECTORS
                                      : ATA_COMMAND_READ_SECTORS);
    return true;
}

bool ata_read_sectors(size_t unit, uint32_t first_sector,
                      uint8_t sector_count, void *buffer) {
    if (buffer == NULL
        || !ata_start_command(unit, first_sector, sector_count, false)) {
        return false;
    }

    uint8_t *destination = buffer;
    for (uint16_t i = 0; i < sector_count; i++) {
        if (!ata_wait_status(unit, true)) {
            return false;
        }
        ata_read_data_sector(unit, destination + (size_t)i * 512);
    }
    return ata_wait_status(unit, false);
}

static bool send_flush(size_t unit) {
    io_out8(base_of(unit) + ATA_COMMAND,
            units[unit].lba48 ? ATA_COMMAND_CACHE_FLUSH_EXT
                              : ATA_COMMAND_CACHE_FLUSH);
    return ata_wait_status(unit, false);
}

bool ata_write_sectors(size_t unit, uint32_t first_sector,
                       uint8_t sector_count, const void *buffer) {
    if (buffer == NULL
        || !ata_start_command(unit, first_sector, sector_count, true)) {
        return false;
    }

    const uint8_t *source = buffer;
    for (uint16_t i = 0; i < sector_count; i++) {
        if (!ata_wait_status(unit, true)) {
            return false;
        }
        ata_write_data_sector(unit, source + (size_t)i * 512);
    }
    if (!ata_wait_status(unit, false)) {
        return false;
    }
    return flush_deferred || send_flush(unit);
}

void ata_set_flush_deferred(bool deferred) {
    flush_deferred = deferred;
}

bool ata_flush(size_t unit) {
    if (!unit_ready(unit)) {
        return false;
    }
    io_out8(base_of(unit) + ATA_DRIVE_HEAD, (uint8_t)(0xe0 | drive_bits(unit)));
    ata_delay_400ns(unit);
    if (!ata_wait_status(unit, false)) {
        return false;
    }
    return send_flush(unit);
}

/* IDENTIFY strings are big-endian word pairs padded with spaces. */
static void copy_model(const uint16_t *identify, char *model) {
    size_t length = 0;
    for (size_t word = 27; word <= 46; word++) {
        model[length++] = (char)(identify[word] >> 8);
        model[length++] = (char)identify[word];
    }
    model[length] = '\0';
    while (length > 0 && (model[length - 1] == ' ' || model[length - 1] == '\0')) {
        model[--length] = '\0';
    }
    for (size_t i = 0; i < length; i++) {
        if (model[i] < ' ' || model[i] > '~') {
            model[i] = '?';
        }
    }
}

/* ------------------------------------------------------ ATAPI (CD/DVD) */

/* After IDENTIFY was aborted with the ATAPI signature. */
static bool probe_atapi(size_t unit) {
    struct ata_unit *state = &units[unit];
    uint16_t base = base_of(unit);
    io_out8(base + ATA_COMMAND, ATA_COMMAND_IDENTIFY_PACKET);
    ata_delay_400ns(unit);
    if (!ata_wait_status_limit(unit, true, ATA_PROBE_POLL_LIMIT)) {
        return false;
    }
    uint16_t identify[256];
    for (size_t i = 0; i < 256; i++) {
        identify[i] = io_in16(base + ATA_DATA);
    }
    /* Word 0: bits 15-14 = 10b (ATAPI), bits 12-8 = 5 (CD/DVD). */
    if ((identify[0] >> 14) != 2 || ((identify[0] >> 8) & 0x1f) != 5) {
        return false;
    }
    copy_model(identify, state->model);
    state->atapi = true;
    return true;
}

bool ata_is_atapi(size_t unit) {
    return unit < ATA_UNIT_COUNT && units[unit].atapi;
}

const char *ata_atapi_model(size_t unit) {
    return ata_is_atapi(unit) ? units[unit].model : "";
}

/* Waits until BSY clears; *status gets the final status. */
static bool packet_wait(size_t unit, uint8_t *status) {
    uint16_t base = base_of(unit);
    for (unsigned i = 0; i < ATA_PACKET_POLL_LIMIT; i++) {
        uint8_t value = io_in8(base + ATA_STATUS);
        if (value == 0xff) {
            return false;
        }
        if ((value & ATA_STATUS_BUSY) == 0) {
            *status = value;
            return true;
        }
    }
    return false;
}

enum scsi_result ata_packet(size_t unit, const uint8_t cdb[12], void *buffer,
                            uint32_t length) {
    if (!ata_is_atapi(unit) || cdb == NULL || length > ATA_PACKET_LIMIT
        || (length != 0 && buffer == NULL)) {
        return SCSI_FAILED;
    }
    uint16_t base = base_of(unit);
    uint8_t status;
    io_out8(base + ATA_DRIVE_HEAD, (uint8_t)(0xa0 | drive_bits(unit)));
    ata_delay_400ns(unit);
    if (!packet_wait(unit, &status)) {
        return SCSI_FAILED;
    }
    /* Byte count limit: how much the drive may send per DRQ block. */
    uint32_t limit = length > 0xf800 ? 0xf800 : length;
    io_out8(base + ATA_FEATURES, 0); /* PIO, no overlap */
    io_out8(base + ATA_SECTOR_COUNT, 0);
    io_out8(base + ATA_LBA_LOW, 0);
    io_out8(base + ATA_LBA_MID, (uint8_t)limit);
    io_out8(base + ATA_LBA_HIGH, (uint8_t)(limit >> 8));
    io_out8(base + ATA_COMMAND, ATA_COMMAND_PACKET);
    ata_delay_400ns(unit);
    if (!packet_wait(unit, &status)) {
        return SCSI_FAILED;
    }
    if ((status & ATA_STATUS_ERROR) != 0) {
        return SCSI_CHECK_CONDITION;
    }
    if ((status & ATA_STATUS_DRQ) == 0) {
        return SCSI_FAILED;
    }
    for (size_t i = 0; i < 12; i += 2) {
        io_out16(base + ATA_DATA, (uint16_t)(cdb[i] | (cdb[i + 1] << 8)));
    }

    uint8_t *destination = buffer;
    uint32_t done = 0;
    for (;;) {
        ata_delay_400ns(unit);
        if (!packet_wait(unit, &status)) {
            return SCSI_FAILED;
        }
        if ((status & ATA_STATUS_ERROR) != 0) {
            return SCSI_CHECK_CONDITION;
        }
        if ((status & ATA_STATUS_DEVICE_FAULT) != 0) {
            return SCSI_FAILED;
        }
        if ((status & ATA_STATUS_DRQ) == 0) {
            break;
        }
        uint32_t bytes = (uint32_t)io_in8(base + ATA_LBA_MID)
            | ((uint32_t)io_in8(base + ATA_LBA_HIGH) << 8);
        if (bytes == 0) {
            return SCSI_FAILED;
        }
        uint32_t words = (bytes + 1) / 2;
        if ((bytes & 1) == 0 && done + bytes <= length) {
            /* The usual case: a whole block fits, move it in one go. */
            void *target = destination + done;
            size_t count = words;
            __asm__ volatile ("cld; rep insw"
                              : "+D"(target), "+c"(count)
                              : "d"((uint16_t)(base + ATA_DATA))
                              : "memory");
        } else {
            for (uint32_t i = 0; i < words; i++) {
                uint16_t word = io_in16(base + ATA_DATA);
                uint32_t at = done + i * 2;
                if (at < length) {
                    destination[at] = (uint8_t)word;
                }
                if (at + 1 < length) {
                    destination[at + 1] = (uint8_t)(word >> 8);
                }
            }
        }
        done += bytes;
    }
    return done >= length ? SCSI_OK : SCSI_FAILED;
}

static bool probe_unit(size_t unit) {
    struct ata_unit *state = &units[unit];
    *state = (struct ata_unit){0};
    uint16_t base = base_of(unit);

    io_out8(base + ATA_DRIVE_HEAD, (uint8_t)(0xa0 | drive_bits(unit)));
    ata_delay_400ns(unit);
    uint8_t status = io_in8(base + ATA_STATUS);
    if (status == 0 || status == 0xff) {
        return false;
    }
    io_out8(base + ATA_SECTOR_COUNT, 0);
    io_out8(base + ATA_LBA_LOW, 0);
    io_out8(base + ATA_LBA_MID, 0);
    io_out8(base + ATA_LBA_HIGH, 0);
    io_out8(base + ATA_COMMAND, ATA_COMMAND_IDENTIFY);

    /* ATAPI devices abort IDENTIFY and set the 0x14/0xEB signature. */
    bool answered = ata_wait_status_limit(unit, false, ATA_PROBE_POLL_LIMIT);
    uint8_t signature_mid = io_in8(base + ATA_LBA_MID);
    uint8_t signature_high = io_in8(base + ATA_LBA_HIGH);
    if (signature_mid == 0x14 && signature_high == 0xeb) {
        (void)probe_atapi(unit);
        return false;
    }
    if (!answered || signature_mid != 0 || signature_high != 0
        || !ata_wait_status_limit(unit, true, ATA_PROBE_POLL_LIMIT)) {
        return false;
    }

    uint16_t identify[256];
    for (size_t i = 0; i < 256; i++) {
        identify[i] = io_in16(base + ATA_DATA);
    }

    if ((identify[49] & (1u << 9)) == 0) {
        return false;
    }
    state->sector_count = (uint32_t)identify[60]
        | ((uint32_t)identify[61] << 16);
    if ((identify[83] & ATA_IDENTIFY_LBA48_SUPPORTED) != 0) {
        uint64_t lba48_sectors = (uint64_t)identify[100]
            | ((uint64_t)identify[101] << 16)
            | ((uint64_t)identify[102] << 32)
            | ((uint64_t)identify[103] << 48);
        if (lba48_sectors > state->sector_count) {
            state->lba48 = true;
            state->sector_count = lba48_sectors;
        }
    }
    /* The block layer addresses sectors with 32-bit LBAs (2 TiB). */
    if (state->sector_count > (uint64_t)UINT32_MAX + 1) {
        state->sector_count = (uint64_t)UINT32_MAX + 1;
    }
    if (state->sector_count == 0
        || (!state->lba48
            && state->sector_count > (uint64_t)ATA_LBA28_MAX + 1)) {
        *state = (struct ata_unit){0};
        return false;
    }
    copy_model(identify, state->model);

    state->ready = true;
    uint8_t first_sector[512];
    if (!ata_read_sectors(unit, 0, 1, first_sector)) {
        *state = (struct ata_unit){0};
        return false;
    }
    return true;
}

bool ata_initialize(void) {
    bool any = false;
    for (size_t unit = 0; unit < ATA_UNIT_COUNT; unit++) {
        if (probe_unit(unit)) {
            any = true;
        }
    }
    return any;
}

uint64_t ata_sector_count(size_t unit) {
    return unit_ready(unit) ? units[unit].sector_count : 0;
}

const char *ata_model(size_t unit) {
    return unit_ready(unit) ? units[unit].model : "";
}
