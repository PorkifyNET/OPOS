#ifndef OPOS_SDHCI_H
#define OPOS_SDHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * SD Host Controller (PCI class 08/05) driver for SD cards and eMMC, the
 * built-in storage of many small laptops and tablets. Each slot with a
 * working card becomes a disk. Transfers use PIO through the buffer data
 * port and are polled, at default speed (25 MHz) with a 4-bit bus when
 * the card accepts it.
 */

#define SDHCI_MAX_DISKS 4

bool sdhci_initialize(void);
size_t sdhci_disk_count(void);
uint64_t sdhci_sector_count(size_t disk);
/* Product name from the card's CID, e.g. "SD16G" or "DF4032". */
const char *sdhci_model(size_t disk);
/* "SD CARD SLOT n" or "EMMC". */
void sdhci_location(size_t disk, char *buffer, size_t capacity);
/* SD cards sit in a slot and can be taken out; eMMC is soldered on. */
bool sdhci_removable(size_t disk);
bool sdhci_read_sectors(size_t disk, uint32_t first_sector,
                        uint8_t sector_count, void *buffer);
bool sdhci_write_sectors(size_t disk, uint32_t first_sector,
                         uint8_t sector_count, const void *buffer);

#endif
