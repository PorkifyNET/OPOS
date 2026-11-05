/*
 * Builds the first 63 sectors of an OPOS install disk: Limine's BIOS boot
 * sector plus its stage 2 in the post-MBR gap. It does the same thing as
 * `limine bios-install` on an MBR disk, which places stage 2 at byte 4096
 * and stores that offset at 0x1a4 in the boot sector. The kernel installer
 * writes this image to sectors 0-62 and keeps its own disk signature and
 * partition table (bytes 440-511 of sector 0).
 *
 * Usage: mkbiosboot LIMINE-BIOS-HDD.H OUTPUT  (the header is compiled in,
 * so the first argument only documents the dependency).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "limine-bios-hdd.h"

#define SECTOR_SIZE 512
#define IMAGE_SECTORS 63
#define STAGE2_OFFSET 4096
#define STAGE2_POINTER_OFFSET 0x1a4

int main(int argc, char **argv) {
    static uint8_t image[IMAGE_SECTORS * SECTOR_SIZE];
    size_t size = sizeof(binary_limine_hdd_bin_data);
    if (argc != 3) {
        fprintf(stderr, "usage: %s LIMINE-BIOS-HDD.H OUTPUT\n", argv[0]);
        return 1;
    }
    if (size <= SECTOR_SIZE
        || size - SECTOR_SIZE > sizeof(image) - STAGE2_OFFSET) {
        fprintf(stderr, "mkbiosboot: stage 2 (%zu bytes) does not fit\n",
                size - SECTOR_SIZE);
        return 1;
    }
    memcpy(image, binary_limine_hdd_bin_data, SECTOR_SIZE);
    memcpy(image + STAGE2_OFFSET, binary_limine_hdd_bin_data + SECTOR_SIZE,
           size - SECTOR_SIZE);
    uint64_t stage2 = STAGE2_OFFSET;
    for (size_t i = 0; i < sizeof(stage2); i++) {
        image[STAGE2_POINTER_OFFSET + i] = (uint8_t)(stage2 >> (8 * i));
    }
    /* Timestamp, disk signature and partition table belong to the disk. */
    memset(image + 218, 0, 6);
    memset(image + 440, 0, SECTOR_SIZE - 440);

    FILE *output = fopen(argv[2], "wb");
    if (output == NULL || fwrite(image, 1, sizeof(image), output) != sizeof(image)
        || fclose(output) != 0) {
        perror(argv[2]);
        return 1;
    }
    return 0;
}
