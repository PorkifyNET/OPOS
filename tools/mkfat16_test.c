#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SECTOR_SIZE 512
#define TOTAL_SECTORS 32768
#define FAT_SECTORS 128
#define ROOT_FIRST_SECTOR 257
#define DATA_FIRST_SECTOR 289

static void put16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static int write_sector(FILE *file, uint32_t lba, const uint8_t *sector) {
    return fseek(file, (long)lba * SECTOR_SIZE, SEEK_SET) == 0
        && fwrite(sector, 1, SECTOR_SIZE, file) == SECTOR_SIZE;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s IMAGE\n", argv[0]);
        return 2;
    }
    FILE *file = fopen(argv[1], "wb+");
    if (file == NULL
        || fseek(file, (long)TOTAL_SECTORS * SECTOR_SIZE - 1, SEEK_SET) != 0
        || fputc(0, file) == EOF) {
        fprintf(stderr, "cannot create image\n");
        return 1;
    }

    uint8_t sector[SECTOR_SIZE] = {0};
    sector[0] = 0xeb;
    sector[1] = 0x3c;
    sector[2] = 0x90;
    memcpy(sector + 3, "OPOSFAT ", 8);
    put16(sector + 11, SECTOR_SIZE);
    sector[13] = 1;
    put16(sector + 14, 1);
    sector[16] = 2;
    put16(sector + 17, 512);
    put16(sector + 19, TOTAL_SECTORS);
    sector[21] = 0xf8;
    put16(sector + 22, FAT_SECTORS);
    put16(sector + 24, 63);
    put16(sector + 26, 255);
    sector[36] = 0x80;
    sector[38] = 0x29;
    put32(sector + 39, 0x4f504f53);
    memcpy(sector + 43, "OPOS TEST  ", 11);
    memcpy(sector + 54, "FAT16   ", 8);
    sector[510] = 0x55;
    sector[511] = 0xaa;
    if (!write_sector(file, 0, sector)) {
        return 1;
    }

    memset(sector, 0, sizeof(sector));
    put16(sector, 0xfff8);
    put16(sector + 2, 0xffff);
    put16(sector + 4, 0xffff);
    if (!write_sector(file, 1, sector)
        || !write_sector(file, 1 + FAT_SECTORS, sector)) {
        return 1;
    }

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "PERSIST TXT", 11);
    sector[11] = 0x20;
    put16(sector + 26, 2);
    put32(sector + 28, 7);
    if (!write_sector(file, ROOT_FIRST_SECTOR, sector)) {
        return 1;
    }

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "initial", 7);
    if (!write_sector(file, DATA_FIRST_SECTOR, sector)
        || fclose(file) != 0) {
        return 1;
    }
    return 0;
}
