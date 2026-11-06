#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "io.h"
#include "paging.h"
#include "pci.h"
#include "sdhci.h"

/*
 * SD Host Controller Specification registers (one 256-byte block per
 * slot). Everything is polled: the signal-enable registers stay 0, so
 * the controller never raises an interrupt.
 */
#define REG_BLOCK_SIZE 0x04
#define REG_BLOCK_COUNT 0x06
#define REG_ARGUMENT 0x08
#define REG_TRANSFER_MODE 0x0c /* 16 bits; the command register follows */
#define REG_RESPONSE 0x10
#define REG_BUFFER 0x20
#define REG_PRESENT_STATE 0x24
#define REG_HOST_CONTROL 0x28
#define REG_POWER_CONTROL 0x29
#define REG_CLOCK_CONTROL 0x2c
#define REG_TIMEOUT_CONTROL 0x2e
#define REG_SOFTWARE_RESET 0x2f
#define REG_INT_STATUS 0x30
#define REG_ERROR_STATUS 0x32
#define REG_INT_ENABLE 0x34
#define REG_ERROR_ENABLE 0x36
#define REG_INT_SIGNAL 0x38
#define REG_ERROR_SIGNAL 0x3a
#define REG_HOST_CONTROL_2 0x3e
#define REG_CAPABILITIES 0x40
#define REG_VERSION 0xfe

#define PRESENT_CMD_INHIBIT (1u << 0)
#define PRESENT_DAT_INHIBIT (1u << 1)
#define PRESENT_CARD_INSERTED (1u << 16)

#define HOST_CONTROL_4BIT 0x02

#define CLOCK_INTERNAL_ENABLE 0x0001
#define CLOCK_INTERNAL_STABLE 0x0002
#define CLOCK_CARD_ENABLE 0x0004

#define RESET_ALL 0x01
#define RESET_CMD 0x02
#define RESET_DAT 0x04

#define INT_COMMAND_COMPLETE 0x0001
#define INT_TRANSFER_COMPLETE 0x0002
#define INT_BUFFER_WRITE_READY 0x0010
#define INT_BUFFER_READ_READY 0x0020
#define INT_ERROR 0x8000
#define ERROR_COMMAND_TIMEOUT 0x0001

#define CAP_VOLTAGE_33 (1u << 24)
#define CAP_VOLTAGE_30 (1u << 25)
#define CAP_VOLTAGE_18 (1u << 26)

#define POWER_ON 0x01
#define POWER_33 0x0e
#define POWER_30 0x0c
#define POWER_18 0x0a

/* Transfer mode bits. */
#define MODE_BLOCK_COUNT 0x0002
#define MODE_AUTO_CMD12 0x0004
#define MODE_READ 0x0010
#define MODE_MULTI_BLOCK 0x0020

/* Command register: response type and checks. */
#define RESP_NONE 0x00
#define RESP_136 0x09        /* R2: 136 bits, CRC */
#define RESP_48_NO_CRC 0x02  /* R3 (OCR) */
#define RESP_48 0x1a         /* R1, R6, R7: CRC and index checked */
#define RESP_48_BUSY 0x1b    /* R1b */
#define COMMAND_DATA 0x20

/* Card status (R1) error bits 31..19. */
#define R1_ERRORS 0xfff80000u
#define R1_READY_FOR_DATA (1u << 8)
#define R1_STATE(status) (((status) >> 9) & 0x0f)
#define R1_STATE_TRANSFER 4

#define SECTOR_SIZE 512
#define SDHCI_MMIO_BASE 0xffffc00000120000ULL
#define SDHCI_MAX_SLOTS 6
#define SDHCI_CONTROLLERS 4

/* Timeouts in polling rounds; each round waits about a microsecond. */
#define TIMEOUT_SHORT 100000     /* ~0.1 s */
#define TIMEOUT_TRANSFER 2000000 /* ~2 s */
#define INIT_ATTEMPTS 1000       /* ACMD41 / CMD1, 1 ms apart */

struct sdhci_disk {
    volatile uint8_t *registers;
    uint16_t rca;
    bool mmc;
    /* SDHC/SDXC or sector-mode eMMC: addresses are sectors, not bytes. */
    bool block_addressing;
    uint64_t sectors;
    uint8_t slot;
    char model[8];
};

static struct sdhci_disk disks[SDHCI_MAX_DISKS];
static size_t disk_count;
static size_t mapped_slots;

/* ----------------------------------------------------------- registers */

static uint8_t read8(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint8_t *)(base + offset);
}

static uint16_t read16(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint16_t *)(base + offset);
}

static uint32_t read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void write8(volatile uint8_t *base, uint32_t offset, uint8_t value) {
    *(volatile uint8_t *)(base + offset) = value;
}

static void write16(volatile uint8_t *base, uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(base + offset) = value;
}

static void write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

/* Port 0x80 reads take about a microsecond on PCs (longer under KVM). */
static void delay_us(uint32_t microseconds) {
    for (uint32_t i = 0; i < microseconds; i++) {
        io_in8(0x80);
    }
}

static bool reset_lines(volatile uint8_t *regs, uint8_t mask) {
    write8(regs, REG_SOFTWARE_RESET, mask);
    for (uint32_t i = 0; i < TIMEOUT_SHORT; i++) {
        if ((read8(regs, REG_SOFTWARE_RESET) & mask) == 0) {
            return true;
        }
        delay_us(1);
    }
    return false;
}

/* --------------------------------------------------------------- clock */

static uint32_t base_clock_khz(volatile uint8_t *regs) {
    uint32_t caps = read32(regs, REG_CAPABILITIES);
    bool v3 = (read16(regs, REG_VERSION) & 0xff) >= 2;
    uint32_t mhz = v3 ? (caps >> 8) & 0xff : (caps >> 8) & 0x3f;
    return mhz != 0 ? mhz * 1000 : 50000; /* unknown: assume 50 MHz */
}

/* Runs the card clock at no more than target_khz. */
static bool set_clock(volatile uint8_t *regs, uint32_t target_khz) {
    write16(regs, REG_CLOCK_CONTROL, 0);
    uint32_t base = base_clock_khz(regs);
    bool v3 = (read16(regs, REG_VERSION) & 0xff) >= 2;
    uint32_t divider = 0; /* card clock = base / (2 * divider), 0 = base */
    if (base > target_khz) {
        if (v3) {
            divider = (base + 2 * target_khz - 1) / (2 * target_khz);
            if (divider > 0x3ff) {
                divider = 0x3ff;
            }
        } else {
            divider = 1;
            while (divider < 0x80 && base / (2 * divider) > target_khz) {
                divider <<= 1;
            }
        }
    }
    uint16_t value = (uint16_t)(((divider & 0xff) << 8)
                                | (((divider >> 8) & 0x03) << 6)
                                | CLOCK_INTERNAL_ENABLE);
    write16(regs, REG_CLOCK_CONTROL, value);
    for (uint32_t i = 0; i < TIMEOUT_SHORT; i++) {
        if ((read16(regs, REG_CLOCK_CONTROL) & CLOCK_INTERNAL_STABLE) != 0) {
            write16(regs, REG_CLOCK_CONTROL, value | CLOCK_CARD_ENABLE);
            delay_us(1000);
            return true;
        }
        delay_us(1);
    }
    return false;
}

/* ------------------------------------------------------------ commands */

enum command_result {
    COMMAND_OK,
    COMMAND_TIMEOUT, /* no response: the card does not know the command */
    COMMAND_ERROR,
};

struct command {
    uint8_t index;
    uint32_t argument;
    uint8_t flags;  /* RESP_* | COMMAND_DATA */
    uint16_t mode;  /* MODE_* for data commands */
    uint16_t blocks;
    uint32_t response[4];
};

static bool wait_status(volatile uint8_t *regs, uint16_t mask,
                        uint32_t timeout, uint16_t *status) {
    for (uint32_t i = 0; i < timeout; i++) {
        uint16_t value = read16(regs, REG_INT_STATUS);
        if ((value & (mask | INT_ERROR)) != 0) {
            *status = value;
            return true;
        }
        delay_us(1);
    }
    return false;
}

static enum command_result fail_command(volatile uint8_t *regs) {
    uint16_t errors = read16(regs, REG_ERROR_STATUS);
    write16(regs, REG_ERROR_STATUS, 0xffff);
    write16(regs, REG_INT_STATUS, 0xffff);
    reset_lines(regs, RESET_CMD | RESET_DAT);
    return (errors & ~ERROR_COMMAND_TIMEOUT) == 0
        && (errors & ERROR_COMMAND_TIMEOUT) != 0
        ? COMMAND_TIMEOUT : COMMAND_ERROR;
}

/* Sends a command and waits for its response (not for its data). */
static enum command_result send_command(volatile uint8_t *regs,
                                        struct command *command) {
    uint32_t inhibit = PRESENT_CMD_INHIBIT;
    if ((command->flags & COMMAND_DATA) != 0
        || (command->flags & 0x03) == (RESP_48_BUSY & 0x03)) {
        inhibit |= PRESENT_DAT_INHIBIT;
    }
    uint32_t waited = 0;
    while ((read32(regs, REG_PRESENT_STATE) & inhibit) != 0) {
        if (++waited >= TIMEOUT_TRANSFER) {
            reset_lines(regs, RESET_CMD | RESET_DAT);
            return COMMAND_ERROR;
        }
        delay_us(1);
    }
    write16(regs, REG_INT_STATUS, 0xffff);
    write16(regs, REG_ERROR_STATUS, 0xffff);
    uint16_t mode = 0;
    if ((command->flags & COMMAND_DATA) != 0) {
        write16(regs, REG_BLOCK_SIZE, SECTOR_SIZE);
        write16(regs, REG_BLOCK_COUNT, command->blocks);
        mode = command->mode;
    }
    write8(regs, REG_TIMEOUT_CONTROL, 0x0e);
    write32(regs, REG_ARGUMENT, command->argument);
    /* Transfer mode and command in one write: the command write starts it. */
    write32(regs, REG_TRANSFER_MODE,
            mode | (uint32_t)((command->index & 0x3f) << 8
                              | command->flags) << 16);
    uint16_t status;
    if (!wait_status(regs, INT_COMMAND_COMPLETE, TIMEOUT_SHORT, &status)) {
        reset_lines(regs, RESET_CMD | RESET_DAT);
        return COMMAND_TIMEOUT;
    }
    if ((status & INT_ERROR) != 0) {
        return fail_command(regs);
    }
    write16(regs, REG_INT_STATUS, INT_COMMAND_COMPLETE);
    for (size_t i = 0; i < 4; i++) {
        command->response[i] = read32(regs, REG_RESPONSE + (uint32_t)i * 4);
    }
    if ((command->flags & 0x03) == (RESP_48_BUSY & 0x03)
        && (command->flags & COMMAND_DATA) == 0) {
        /* R1b: the end of busy is reported as transfer complete. */
        if (!wait_status(regs, INT_TRANSFER_COMPLETE, TIMEOUT_TRANSFER,
                         &status)
            || (status & INT_ERROR) != 0) {
            return fail_command(regs);
        }
        write16(regs, REG_INT_STATUS, INT_TRANSFER_COMPLETE);
    }
    return COMMAND_OK;
}

static enum command_result simple_command(volatile uint8_t *regs,
                                          uint8_t index, uint32_t argument,
                                          uint8_t flags, uint32_t *response) {
    struct command command = {index, argument, flags, 0, 0, {0, 0, 0, 0}};
    enum command_result result = send_command(regs, &command);
    if (response != NULL) {
        *response = command.response[0];
    }
    return result;
}

/* SD application command: CMD55 then ACMDn. */
static enum command_result app_command(volatile uint8_t *regs, uint16_t rca,
                                       uint8_t index, uint32_t argument,
                                       uint8_t flags, uint32_t *response) {
    enum command_result result = simple_command(
        regs, 55, (uint32_t)rca << 16, RESP_48, NULL);
    if (result != COMMAND_OK) {
        return result;
    }
    return simple_command(regs, index, argument, flags, response);
}

/*
 * Bit `bit` of a 128-bit register (CSD or CID) from an R2 response. The
 * controller drops the CRC byte, so register bit N sits at response bit
 * N - 8.
 */
static uint32_t r2_bits(const uint32_t response[4], uint32_t high,
                        uint32_t low) {
    uint32_t value = 0;
    for (uint32_t bit = high + 1; bit-- > low;) {
        uint32_t at = bit - 8;
        value = (value << 1) | ((response[at / 32] >> (at % 32)) & 1);
    }
    return value;
}

/* ---------------------------------------------------------------- data */

/* Moves `blocks` sectors through the buffer data port. */
static bool pio_transfer(volatile uint8_t *regs, uint8_t *buffer,
                         uint32_t blocks, bool read) {
    uint16_t ready = read ? INT_BUFFER_READ_READY : INT_BUFFER_WRITE_READY;
    for (uint32_t block = 0; block < blocks; block++) {
        uint16_t status;
        if (!wait_status(regs, ready, TIMEOUT_TRANSFER, &status)
            || (status & INT_ERROR) != 0) {
            return false;
        }
        write16(regs, REG_INT_STATUS, ready);
        uint8_t *bytes = buffer + (size_t)block * SECTOR_SIZE;
        for (size_t i = 0; i < SECTOR_SIZE; i += 4) {
            if (read) {
                uint32_t word = read32(regs, REG_BUFFER);
                bytes[i] = (uint8_t)word;
                bytes[i + 1] = (uint8_t)(word >> 8);
                bytes[i + 2] = (uint8_t)(word >> 16);
                bytes[i + 3] = (uint8_t)(word >> 24);
            } else {
                write32(regs, REG_BUFFER,
                        (uint32_t)bytes[i] | (uint32_t)bytes[i + 1] << 8
                        | (uint32_t)bytes[i + 2] << 16
                        | (uint32_t)bytes[i + 3] << 24);
            }
        }
    }
    uint16_t status;
    if (!wait_status(regs, INT_TRANSFER_COMPLETE, TIMEOUT_TRANSFER, &status)
        || (status & INT_ERROR) != 0) {
        return false;
    }
    write16(regs, REG_INT_STATUS, INT_TRANSFER_COMPLETE);
    return true;
}

/* Waits until the card is back in the transfer state (after writes). */
static bool wait_card_ready(const struct sdhci_disk *disk) {
    for (uint32_t i = 0; i < 2000; i++) {
        uint32_t status;
        if (simple_command(disk->registers, 13, (uint32_t)disk->rca << 16,
                           RESP_48, &status) != COMMAND_OK) {
            return false;
        }
        if ((status & R1_ERRORS) != 0) {
            return false;
        }
        if ((status & R1_READY_FOR_DATA) != 0
            && R1_STATE(status) == R1_STATE_TRANSFER) {
            return true;
        }
        delay_us(1000);
    }
    return false;
}

static bool transfer(const struct sdhci_disk *disk, uint32_t first_sector,
                     uint8_t sector_count, uint8_t *buffer, bool read) {
    volatile uint8_t *regs = disk->registers;
    bool multi = sector_count > 1;
    struct command command = {
        .index = read ? (multi ? 18 : 17) : (multi ? 25 : 24),
        .argument = disk->block_addressing ? first_sector
                                           : first_sector * SECTOR_SIZE,
        .flags = RESP_48 | COMMAND_DATA,
        .mode = (uint16_t)((read ? MODE_READ : 0)
                           | (multi ? MODE_MULTI_BLOCK | MODE_BLOCK_COUNT
                                      | MODE_AUTO_CMD12 : 0)),
        .blocks = sector_count,
    };
    if (send_command(regs, &command) != COMMAND_OK
        || (command.response[0] & R1_ERRORS) != 0) {
        reset_lines(regs, RESET_CMD | RESET_DAT);
        return false;
    }
    if (!pio_transfer(regs, buffer, sector_count, read)) {
        write16(regs, REG_ERROR_STATUS, 0xffff);
        write16(regs, REG_INT_STATUS, 0xffff);
        reset_lines(regs, RESET_CMD | RESET_DAT);
        if (multi) {
            (void)simple_command(regs, 12, 0, RESP_48_BUSY, NULL);
        }
        return false;
    }
    return read || wait_card_ready(disk);
}

static bool check_range(size_t index, uint32_t first_sector,
                        uint8_t sector_count) {
    return index < disk_count && sector_count != 0
        && (uint64_t)first_sector + sector_count <= disks[index].sectors
        && (disks[index].block_addressing
            || (uint64_t)first_sector + sector_count <= UINT32_MAX / SECTOR_SIZE);
}

bool sdhci_read_sectors(size_t disk, uint32_t first_sector,
                        uint8_t sector_count, void *buffer) {
    return check_range(disk, first_sector, sector_count)
        && transfer(&disks[disk], first_sector, sector_count, buffer, true);
}

bool sdhci_write_sectors(size_t disk, uint32_t first_sector,
                         uint8_t sector_count, const void *buffer) {
    return check_range(disk, first_sector, sector_count)
        && transfer(&disks[disk], first_sector, sector_count,
                    (uint8_t *)(uintptr_t)buffer, false);
}

/* ---------------------------------------------------------- card setup */

static void copy_name(char *out, const uint32_t cid[4], uint32_t high,
                      size_t count) {
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t c = r2_bits(cid, high - (uint32_t)i * 8,
                             high - (uint32_t)i * 8 - 7);
        if (c >= ' ' && c <= '~') {
            out[used++] = (char)c;
        }
    }
    while (used > 0 && out[used - 1] == ' ') {
        used--;
    }
    out[used] = '\0';
}

/* Sectors from an SD or MMC CSD (byte-addressed cards and SD v2). */
static uint64_t csd_sectors(const uint32_t csd[4], bool mmc) {
    if (!mmc && r2_bits(csd, 127, 126) == 1) {
        return ((uint64_t)r2_bits(csd, 69, 48) + 1) * 1024;
    }
    uint64_t c_size = r2_bits(csd, 73, 62);
    uint32_t multiplier = r2_bits(csd, 49, 47);
    uint32_t block_length = r2_bits(csd, 83, 80);
    uint64_t bytes = (c_size + 1) << (multiplier + 2 + block_length);
    return bytes / SECTOR_SIZE;
}

/* MMC SEND_EXT_CSD: SEC_COUNT (bytes 212-215) of sector-mode eMMC. */
static uint64_t mmc_ext_csd_sectors(volatile uint8_t *regs) {
    static uint8_t ext_csd[SECTOR_SIZE];
    struct command command = {
        .index = 8, .argument = 0, .flags = RESP_48 | COMMAND_DATA,
        .mode = MODE_READ, .blocks = 1,
    };
    if (send_command(regs, &command) != COMMAND_OK
        || !pio_transfer(regs, ext_csd, 1, true)) {
        reset_lines(regs, RESET_CMD | RESET_DAT);
        return 0;
    }
    return (uint64_t)ext_csd[212] | (uint64_t)ext_csd[213] << 8
        | (uint64_t)ext_csd[214] << 16 | (uint64_t)ext_csd[215] << 24;
}

static bool power_up(volatile uint8_t *regs) {
    uint32_t caps = read32(regs, REG_CAPABILITIES);
    uint8_t voltage = (caps & CAP_VOLTAGE_33) != 0 ? POWER_33
                    : (caps & CAP_VOLTAGE_30) != 0 ? POWER_30
                    : (caps & CAP_VOLTAGE_18) != 0 ? POWER_18
                    : POWER_33;
    write8(regs, REG_POWER_CONTROL, 0);
    delay_us(1000);
    write8(regs, REG_POWER_CONTROL, voltage);
    write8(regs, REG_POWER_CONTROL, voltage | POWER_ON);
    delay_us(10000);
    return (read8(regs, REG_POWER_CONTROL) & POWER_ON) != 0;
}

/* Step that failed in the last card_initialize, for the boot log. */
static const char *failed_step;

static bool fail(const char *step) {
    failed_step = step;
    return false;
}

/* Brings up the card in one slot; true when it is ready for transfers. */
static bool card_initialize(volatile uint8_t *regs, struct sdhci_disk *disk) {
    failed_step = "NO CARD SIZE";
    if (!reset_lines(regs, RESET_ALL)) {
        return fail("RESET");
    }
    write16(regs, REG_INT_ENABLE, INT_COMMAND_COMPLETE | INT_TRANSFER_COMPLETE
                                  | INT_BUFFER_WRITE_READY
                                  | INT_BUFFER_READ_READY);
    write16(regs, REG_ERROR_ENABLE, 0x03ff);
    write16(regs, REG_INT_SIGNAL, 0);
    write16(regs, REG_ERROR_SIGNAL, 0);
    write8(regs, REG_HOST_CONTROL, 0);
    if (!power_up(regs) || !set_clock(regs, 400)) {
        return fail("POWER/CLOCK");
    }
    delay_us(2000); /* at least 74 clocks before the first command */

    *disk = (struct sdhci_disk){.registers = regs};
    (void)simple_command(regs, 0, 0, RESP_NONE, NULL);
    delay_us(1000);

    /* SD v2 cards answer CMD8 (voltage check) with the pattern echoed. */
    uint32_t response = 0;
    bool sd_v2 = simple_command(regs, 8, 0x1aa, RESP_48, &response)
        == COMMAND_OK && (response & 0xfff) == 0x1aa;

    uint32_t ocr = 0;
    bool ready = false;
    enum command_result result = app_command(regs, 0, 41, 0, RESP_48_NO_CRC,
                                             &ocr);
    if (result == COMMAND_OK) {
        /* SD: 2.7-3.6 V, HCS (high capacity) when the card is v2. */
        uint32_t argument = 0x00ff8000u | (sd_v2 ? 0x40000000u : 0);
        for (uint32_t i = 0; i < INIT_ATTEMPTS && !ready; i++) {
            if (app_command(regs, 0, 41, argument, RESP_48_NO_CRC, &ocr)
                != COMMAND_OK) {
                return fail("ACMD41");
            }
            ready = (ocr & 0x80000000u) != 0;
            if (!ready) {
                delay_us(1000);
            }
        }
        disk->block_addressing = sd_v2 && (ocr & 0x40000000u) != 0;
    } else {
        /* No CMD55: MMC/eMMC. CMD1 asks for sector mode, both voltages. */
        disk->mmc = true;
        (void)simple_command(regs, 0, 0, RESP_NONE, NULL);
        delay_us(1000);
        for (uint32_t i = 0; i < INIT_ATTEMPTS && !ready; i++) {
            if (simple_command(regs, 1, 0x40ff8080u, RESP_48_NO_CRC, &ocr)
                != COMMAND_OK) {
                return fail("CMD1");
            }
            ready = (ocr & 0x80000000u) != 0;
            if (!ready) {
                delay_us(1000);
            }
        }
        disk->block_addressing = (ocr & 0x60000000u) == 0x40000000u;
    }
    if (!ready) {
        return fail("NOT READY (OCR BUSY)");
    }

    struct command cid = {.index = 2, .flags = RESP_136};
    if (send_command(regs, &cid) != COMMAND_OK) {
        return fail("CMD2 (CID)");
    }
    if (disk->mmc) {
        copy_name(disk->model, cid.response, 103, 6);
        disk->rca = 1;
        if (simple_command(regs, 3, 1u << 16, RESP_48, NULL) != COMMAND_OK) {
            return fail("CMD3");
        }
    } else {
        copy_name(disk->model, cid.response, 103, 5);
        if (simple_command(regs, 3, 0, RESP_48, &response) != COMMAND_OK) {
            return fail("CMD3");
        }
        disk->rca = (uint16_t)(response >> 16);
    }
    struct command csd = {.index = 9, .argument = (uint32_t)disk->rca << 16,
                          .flags = RESP_136};
    if (send_command(regs, &csd) != COMMAND_OK) {
        return fail("CMD9 (CSD)");
    }
    disk->sectors = csd_sectors(csd.response, disk->mmc);
    if (simple_command(regs, 7, (uint32_t)disk->rca << 16, RESP_48_BUSY,
                       NULL) != COMMAND_OK) {
        return fail("CMD7 (SELECT)");
    }
    if (disk->mmc && disk->block_addressing) {
        disk->sectors = mmc_ext_csd_sectors(regs);
    }
    if (!disk->block_addressing
        && simple_command(regs, 16, SECTOR_SIZE, RESP_48, NULL)
           != COMMAND_OK) {
        return fail("CMD16");
    }

    /* 4-bit bus; on failure the card stays on 1 bit. */
    bool wide;
    if (disk->mmc) {
        /* SWITCH: write byte 183 (BUS_WIDTH) = 1. */
        wide = simple_command(regs, 6, 0x03b70100u, RESP_48_BUSY, NULL)
            == COMMAND_OK;
        uint32_t status = 0;
        wide = wide && simple_command(regs, 13, (uint32_t)disk->rca << 16,
                                      RESP_48, &status) == COMMAND_OK
            && (status & (1u << 7)) == 0; /* SWITCH_ERROR */
    } else {
        wide = app_command(regs, disk->rca, 6, 2, RESP_48, NULL)
            == COMMAND_OK;
    }
    if (wide) {
        write8(regs, REG_HOST_CONTROL,
               read8(regs, REG_HOST_CONTROL) | HOST_CONTROL_4BIT);
    }
    if (!set_clock(regs, disk->mmc ? 20000 : 25000)) {
        return fail("CLOCK");
    }
    return disk->sectors != 0;
}

/* Maps one slot's register block and tries its card. */
static void probe_slot(const struct pci_device *device, uint8_t bar,
                       uint8_t slot) {
    if (disk_count >= SDHCI_MAX_DISKS || mapped_slots >= SDHCI_MAX_SLOTS) {
        return;
    }
    bool io_space = false;
    uint64_t address = pci_bar_address(device, bar, &io_space);
    if (address == 0 || io_space) {
        return;
    }
    uintptr_t window = SDHCI_MMIO_BASE + mapped_slots * 0x1000;
    if (!paging_map_page(window, address & ~(uint64_t)0xfff,
                         PAGING_WRITABLE | PAGING_CACHE_DISABLE
                         | PAGING_NO_EXECUTE)) {
        return;
    }
    mapped_slots++;
    volatile uint8_t *regs = (volatile uint8_t *)(window + (address & 0xfff));
    if ((address & 0xfff) > 0x1000 - 0x100) {
        return; /* register block would cross the page */
    }
    struct sdhci_disk *disk = &disks[disk_count];
    console_write_string("OPOS: SD host ");
    console_write_hex(device->vendor_id);
    console_write_string(":");
    console_write_hex(device->device_id);
    console_write_string(" slot ");
    console_write_hex(slot);
    console_write_string(" version ");
    console_write_hex(read16(regs, REG_VERSION) & 0xff);
    console_write_string(" caps ");
    console_write_hex(read32(regs, REG_CAPABILITIES));
    bool ok = card_initialize(regs, disk);
    if (ok) {
        console_write_string(disk->mmc ? ": eMMC ready\n" : ": SD card ready\n");
    } else {
        console_write_string(": no card (");
        console_write_string(failed_step);
        console_write_string(")\n");
    }
    if (ok) {
        disk->slot = slot;
        if (disk->model[0] == '\0') {
            disk->model[0] = '?';
            disk->model[1] = '\0';
        }
        disk_count++;
    } else {
        /* Leave an empty or unusable slot powered down. */
        write16(regs, REG_CLOCK_CONTROL, 0);
        write8(regs, REG_POWER_CONTROL, 0);
    }
}

bool sdhci_initialize(void) {
    disk_count = 0;
    mapped_slots = 0;
    static const uint8_t interfaces[] = {0x00, 0x01, 0x02};
    size_t controllers = 0;
    for (size_t i = 0; i < sizeof(interfaces); i++) {
        for (size_t index = 0; controllers < SDHCI_CONTROLLERS; index++) {
            struct pci_device device;
            if (!pci_find_class(0x08, 0x05, interfaces[i], index, &device)) {
                break;
            }
            controllers++;
            pci_enable_device(&device);
            /* Slot information: number of slots - 1 and the first BAR. */
            uint32_t info = pci_config_read32(&device, 0x40);
            uint8_t first_bar = (uint8_t)(info & 0x07);
            uint8_t slots = (uint8_t)(((info >> 4) & 0x07) + 1);
            if (first_bar > 5) {
                first_bar = 0;
            }
            for (uint8_t slot = 0; slot < slots && first_bar + slot <= 5;
                 slot++) {
                probe_slot(&device, (uint8_t)(first_bar + slot), slot);
            }
        }
    }
    if (controllers == 0) {
        /* Some Intel tablets/laptops hide the eMMC host from PCI (ACPI
           mode); then it is not found here. */
        console_write_string("OPOS: no SD host controller on PCI\n");
    }
    return disk_count != 0;
}

size_t sdhci_disk_count(void) {
    return disk_count;
}

uint64_t sdhci_sector_count(size_t disk) {
    return disk < disk_count ? disks[disk].sectors : 0;
}

const char *sdhci_model(size_t disk) {
    return disk < disk_count ? disks[disk].model : "";
}

bool sdhci_removable(size_t disk) {
    return disk < disk_count && !disks[disk].mmc;
}

void sdhci_location(size_t disk, char *buffer, size_t capacity) {
    if (capacity == 0) {
        return;
    }
    const char *text = disk < disk_count && disks[disk].mmc
        ? "EMMC" : "SD CARD SLOT ";
    size_t used = 0;
    while (*text != '\0' && used + 1 < capacity) {
        buffer[used++] = *text++;
    }
    if (disk < disk_count && !disks[disk].mmc && used + 1 < capacity) {
        buffer[used++] = (char)('1' + disks[disk].slot);
    }
    buffer[used] = '\0';
}
