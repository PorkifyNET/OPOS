#ifndef OPOS_AHCI_H
#define OPOS_AHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scsi.h"

#define AHCI_MODEL_LENGTH 40

struct ahci_status {
    size_t sata_devices;
    size_t atapi_devices;
    uint32_t implemented_ports;
    uint64_t first_sata_sector_count;
};

/* Sets up every SATA disk on the first AHCI controller (at most 8). Disks
   are numbered 0..ahci_disk_count()-1 in port order. */
bool ahci_initialize(void);
void ahci_get_status(struct ahci_status *status);
size_t ahci_disk_count(void);
uint64_t ahci_sector_count(size_t disk);
const char *ahci_model(size_t disk);
uint32_t ahci_port(size_t disk);
bool ahci_read_sectors(size_t disk, uint32_t first_sector,
                       uint8_t sector_count, void *buffer);
bool ahci_write_sectors(size_t disk, uint32_t first_sector,
                        uint8_t sector_count, const void *buffer);

/* ATAPI CD/DVD drives on the same controller (at most 4), numbered in
   port order. Driven with packet commands by optical.c. */
size_t ahci_atapi_count(void);
const char *ahci_atapi_model(size_t drive);
uint32_t ahci_atapi_port(size_t drive);
/* Most bytes one ahci_packet call moves. */
uint32_t ahci_packet_limit(void);
/* Runs a 12-byte SCSI command, reading `length` bytes into buffer (NULL
   and 0 for commands without data). */
enum scsi_result ahci_packet(size_t drive, const uint8_t cdb[12],
                             void *buffer, uint32_t length);

#endif
