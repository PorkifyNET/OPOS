#ifndef OPOS_OPTICAL_H
#define OPOS_OPTICAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * CD/DVD drives: ATAPI on IDE (ata.c) and SATA (ahci.c), and SCSI CD-ROM
 * targets (scsi.c). Each driver runs packet commands (SCSI CDBs); this
 * layer turns them into "is a disc in", "how big" and "read 2048-byte
 * sectors". Drives are read-only and kept apart from the block disks,
 * which are 512-byte and writable.
 */
#define OPTICAL_MAX_DRIVES 8
#define OPTICAL_SECTOR_SIZE 2048

enum optical_bus {
    OPTICAL_BUS_IDE,
    OPTICAL_BUS_SATA,
    OPTICAL_BUS_SCSI,
};

struct optical_drive_info {
    enum optical_bus bus;
    const char *model;
    /* e.g. "IDE SECONDARY MASTER", "SATA PORT 1", "PVSCSI TARGET 2". */
    char location[32];
    /* Disc size in 2048-byte sectors, as of the last optical_check. */
    uint32_t sectors;
    bool media;
};

/* Lists the drives the controller drivers found. Call after the block
   drivers are initialized. */
void optical_initialize(void);
size_t optical_drive_count(void);
bool optical_drive_info(size_t drive, struct optical_drive_info *info);
/* Asks the drive whether a disc is loaded and reads its size. */
bool optical_check(size_t drive);
/* Reads 2048-byte sectors. */
bool optical_read(size_t drive, uint32_t first_sector, uint32_t count,
                  void *buffer);
/* Opens the tray (or ejects the disc). */
bool optical_eject(size_t drive);

#endif
