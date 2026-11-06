#ifndef OPOS_SCSI_H
#define OPOS_SCSI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * SCSI disks behind any host adapter (PVSCSI, LSI MPT). A host driver
 * describes itself with a struct scsi_host and calls scsi_scan_host; this
 * layer finds the disks (INQUIRY, READ CAPACITY) and turns sector reads and
 * writes into SCSI commands. Disks need 512-byte blocks; only LUN 0 is used.
 */
#define SCSI_MAX_DISKS 16
#define SCSI_MAX_TARGETS 16

enum scsi_direction {
    SCSI_DATA_NONE,
    SCSI_DATA_IN,   /* device to host */
    SCSI_DATA_OUT,  /* host to device */
};

enum scsi_result {
    SCSI_OK,
    /* The target answered CHECK CONDITION; the command may be retried. */
    SCSI_CHECK_CONDITION,
    /* No device, adapter error, timeout or other SCSI status. */
    SCSI_FAILED,
};

struct scsi_host {
    /* Shown in disk locations, e.g. "PVSCSI" gives "PVSCSI TARGET 3". */
    const char *name;
    /* The adapter's data buffer; transfers never exceed buffer_size. */
    uint8_t *buffer;
    uint32_t buffer_size;
    /* Runs one command on `target`, LUN 0, moving `length` bytes between
       the device and `buffer`. */
    enum scsi_result (*execute)(uint8_t target, const uint8_t *cdb,
                                uint8_t cdb_length,
                                enum scsi_direction direction,
                                uint32_t length);
    /* Optional: describes where a target is (e.g. "USB PORT 3"); without
       it the location is "<name> TARGET n". */
    void (*location)(uint8_t target, char *buffer, size_t capacity);
    /* Every disk on this host can be unplugged (USB). */
    bool hotplug;
};

/* Probes targets 0..target_count-1 (at most SCSI_MAX_TARGETS) and adds
   every disk found. `host` must stay valid. Returns the disks added. */
size_t scsi_scan_host(const struct scsi_host *host, unsigned target_count);

/* Hot-plug (USB): adds the disk at one target, reusing the slot of a
   removed disk when there is one; true when a disk was added. */
bool scsi_probe_target(const struct scsi_host *host, uint8_t target);
/* The device at this target is gone: its disk reads as absent (0 sectors,
   every transfer fails) until its slot is reused. */
void scsi_remove_target(const struct scsi_host *host, uint8_t target);

/* Disk slots ever used, present or not; indexes stay stable. */
size_t scsi_disk_count(void);
bool scsi_disk_present(size_t disk);
bool scsi_removable(size_t disk);
/* Changes whenever a new device takes the slot; 0 when absent. */
uint32_t scsi_disk_generation(size_t disk);
uint64_t scsi_sector_count(size_t disk);
/* "VENDOR PRODUCT" from INQUIRY. */
const char *scsi_model(size_t disk);
/* "<HOST> TARGET n", or what the host's location callback says. */
void scsi_location(size_t disk, char *buffer, size_t capacity);
bool scsi_read_sectors(size_t disk, uint32_t first_sector,
                       uint8_t sector_count, void *buffer);
bool scsi_write_sectors(size_t disk, uint32_t first_sector,
                        uint8_t sector_count, const void *buffer);
/* Batch writes: skip the cache flush after each write until scsi_flush. */
void scsi_set_flush_deferred(bool deferred);
bool scsi_flush(size_t disk);

/* CD/DVD targets (peripheral device type 5), kept apart from the disks
   and driven by optical.c. */
#define SCSI_MAX_CDROMS 4
size_t scsi_cdrom_count(void);
const char *scsi_cdrom_model(size_t drive);
void scsi_cdrom_location(size_t drive, char *buffer, size_t capacity);
/* Most bytes one scsi_cdrom_packet call moves (the host's buffer). */
uint32_t scsi_cdrom_limit(size_t drive);
/* Runs one command (no retries), reading `length` bytes into buffer. */
enum scsi_result scsi_cdrom_packet(size_t drive, const uint8_t cdb[12],
                                   void *buffer, uint32_t length);

#endif
