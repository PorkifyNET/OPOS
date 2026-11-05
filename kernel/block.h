#ifndef OPOS_BLOCK_H
#define OPOS_BLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum block_device {
    BLOCK_DEVICE_NONE,
    BLOCK_DEVICE_ATA,
    BLOCK_DEVICE_AHCI,
    BLOCK_DEVICE_NVME,
    BLOCK_DEVICE_SCSI,
};

#define BLOCK_MAX_DISKS 16
#define BLOCK_NO_DISK ((size_t)-1)

struct block_disk_info {
    enum block_device device;
    uint64_t sectors;
    /* Drive model from IDENTIFY; may be "". */
    const char *model;
    /* Where it is attached, e.g. "IDE PRIMARY MASTER", "SATA PORT 2". */
    char location[32];
    /* OPOS was booted from this disk (the boot loader's disk ID matches). */
    bool boot_disk;
};

/* The boot loader's identity of the disk it loaded OPOS from: the MBR disk
   signature and/or the GPT disk GUID (zero when unknown). Call before
   block_initialize. */
void block_set_boot_identity(uint32_t mbr_disk_id, const uint8_t gpt_guid[16]);

/* Lists every IDE, SATA (AHCI), NVMe and SCSI (PVSCSI, LSI, BusLogic) disk
   with 512-byte sectors, then makes the boot disk active, or the first disk
   when the boot disk is not among them (for example a live CD). Initialize
   AHCI, NVMe and the SCSI host drivers first. */
enum block_device block_initialize(void);
size_t block_disk_count(void);
bool block_disk_info(size_t index, struct block_disk_info *info);
/* Reads from any listed disk without changing the active one. */
bool block_disk_read(size_t index, uint32_t first_sector,
                     uint8_t sector_count, void *buffer);
/* Makes another disk active. The caller remounts the filesystem. */
bool block_select_disk(size_t index);
size_t block_active_disk(void);

/* The block_* calls below act on the active disk. */
enum block_device block_active_device(void);
const char *block_device_name(enum block_device device);
uint64_t block_sector_count(void);
bool block_read_sectors(uint32_t first_sector, uint8_t sector_count,
                        void *buffer);
bool block_write_sectors(uint32_t first_sector, uint8_t sector_count,
                         const void *buffer);

/* Batched writes: until block_end_batch, writes skip the per-write cache
   flush; block_end_batch flushes once. Use for bulk writes (installer). */
void block_begin_batch(void);
bool block_end_batch(void);

#endif
