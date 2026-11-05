#ifndef OPOS_AHCI_H
#define OPOS_AHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#endif
