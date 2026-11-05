#ifndef OPOS_ATA_H
#define OPOS_ATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Legacy IDE: unit 0 primary master, 1 primary slave, 2 secondary master,
   3 secondary slave. ATAPI (CD/DVD) units are not disks and are skipped. */
#define ATA_UNIT_COUNT 4
#define ATA_MODEL_LENGTH 40

/* Probes all four units; returns true when at least one disk answered. */
bool ata_initialize(void);
uint64_t ata_sector_count(size_t unit);
/* Model string from IDENTIFY, trimmed; "" for a missing unit. */
const char *ata_model(size_t unit);
bool ata_read_sectors(size_t unit, uint32_t first_sector,
                      uint8_t sector_count, void *buffer);
/* Batch writes: skip the cache flush after each write until ata_flush. */
void ata_set_flush_deferred(bool deferred);
bool ata_flush(size_t unit);
bool ata_write_sectors(size_t unit, uint32_t first_sector,
                       uint8_t sector_count, const void *buffer);

#endif
