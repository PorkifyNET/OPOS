#ifndef OPOS_ATA_H
#define OPOS_ATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scsi.h"

/* Legacy IDE: unit 0 primary master, 1 primary slave, 2 secondary master,
   3 secondary slave. ATAPI (CD/DVD) units are not disks; they are listed
   separately and driven with packet commands (optical.c). */
#define ATA_UNIT_COUNT 4
#define ATA_MODEL_LENGTH 40
/* Most bytes one ata_packet call moves. */
#define ATA_PACKET_LIMIT 65536

/* ATAPI CD/DVD drive at this unit. */
bool ata_is_atapi(size_t unit);
const char *ata_atapi_model(size_t unit);
/* Runs a 12-byte SCSI command on an ATAPI unit (PIO), reading `length`
   bytes into buffer (NULL and 0 for commands without data). */
enum scsi_result ata_packet(size_t unit, const uint8_t cdb[12], void *buffer,
                            uint32_t length);

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
