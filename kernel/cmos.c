#include <stdbool.h>
#include <stdint.h>

#include "accounts.h"
#include "cmos.h"
#include "fat16.h"
#include "installer.h"
#include "io.h"

#define CMOS_INDEX_PORT 0x70
#define CMOS_DATA_PORT 0x71
#define CMOS_NMI_DISABLE 0x80
/* Bytes 0x48-0x49 are unused by the PC CMOS map and by SeaBIOS/QEMU. */
#define CMOS_SETTINGS_MAGIC_REGISTER 0x48
#define CMOS_SETTINGS_REGISTER 0x49
#define CMOS_SETTINGS_MAGIC 0xa5
#define SETTINGS_DEFAULT (SETTING_AUTOSTART_DESKTOP | SETTING_TERMINAL_COLORS)

static uint8_t cmos_read(uint8_t index) {
    io_out8(CMOS_INDEX_PORT, CMOS_NMI_DISABLE | index);
    return io_in8(CMOS_DATA_PORT);
}

static void cmos_write(uint8_t index, uint8_t value) {
    io_out8(CMOS_INDEX_PORT, CMOS_NMI_DISABLE | index);
    io_out8(CMOS_DATA_PORT, value);
}

static bool rtc_updating(void) {
    return (cmos_read(0x0a) & 0x80) != 0;
}

static void rtc_read_raw(uint8_t values[6]) {
    static const uint8_t registers[6] = {0x00, 0x02, 0x04, 0x07, 0x08, 0x09};
    for (int i = 0; i < 6; i++) {
        values[i] = cmos_read(registers[i]);
    }
}

static uint8_t bcd_to_binary(uint8_t value) {
    return (uint8_t)((value & 0x0f) + (value >> 4) * 10);
}

bool rtc_read(struct rtc_time *time) {
    if (time == 0) {
        return false;
    }
    /* Read twice until two reads agree, so an update cannot tear the value. */
    uint8_t first[6];
    uint8_t second[6];
    int attempts = 0;
    do {
        while (rtc_updating() && ++attempts < 100000) {
        }
        rtc_read_raw(first);
        while (rtc_updating() && ++attempts < 100000) {
        }
        rtc_read_raw(second);
        bool same = true;
        for (int i = 0; i < 6; i++) {
            same = same && first[i] == second[i];
        }
        if (same) {
            break;
        }
    } while (++attempts < 100000);

    uint8_t status_b = cmos_read(0x0b);
    bool binary = (status_b & 0x04) != 0;
    bool hour_24 = (status_b & 0x02) != 0;
    bool pm = (second[2] & 0x80) != 0;
    second[2] &= 0x7f;
    if (!binary) {
        for (int i = 0; i < 6; i++) {
            second[i] = bcd_to_binary(second[i]);
        }
    }
    if (!hour_24) {
        second[2] = (uint8_t)(second[2] % 12 + (pm ? 12 : 0));
    }
    time->second = second[0];
    time->minute = second[1];
    time->hour = second[2];
    time->day = second[3];
    time->month = second[4];
    time->year = (uint16_t)(2000 + second[5]);
    return time->month >= 1 && time->month <= 12 && time->day >= 1;
}

static uint8_t binary_to_bcd(uint8_t value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

/* Day of week for the RTC weekday register: 1 = Sunday ... 7 = Saturday. */
static uint8_t day_of_week(uint16_t year, uint8_t month, uint8_t day) {
    static const uint8_t offsets[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year--;
    }
    unsigned weekday = (year + year / 4 - year / 100 + year / 400
                        + offsets[month - 1] + day) % 7;
    return (uint8_t)(weekday + 1);
}

bool rtc_write(const struct rtc_time *time) {
    if (time == 0 || time->year < 2000 || time->year > 2099
        || time->month < 1 || time->month > 12 || time->day < 1
        || time->day > 31 || time->hour > 23 || time->minute > 59
        || time->second > 59) {
        return false;
    }
    uint8_t status_b = cmos_read(0x0b);
    bool binary = (status_b & 0x04) != 0;
    bool hour_24 = (status_b & 0x02) != 0;

    uint8_t hour = time->hour;
    bool pm = false;
    if (!hour_24) {
        pm = hour >= 12;
        hour = (uint8_t)(hour % 12 == 0 ? 12 : hour % 12);
    }
    static const uint8_t registers[7] = {0x00, 0x02, 0x04, 0x06,
                                         0x07, 0x08, 0x09};
    uint8_t values[7] = {
        time->second, time->minute, hour,
        day_of_week(time->year, time->month, time->day),
        time->day, time->month, (uint8_t)(time->year - 2000),
    };
    if (!binary) {
        for (int i = 0; i < 7; i++) {
            values[i] = binary_to_bcd(values[i]);
        }
    }
    if (pm) {
        values[2] |= 0x80;
    }

    /* Status B bit 7 (SET) stops updates while the registers change. */
    cmos_write(0x0b, (uint8_t)(status_b | 0x80));
    for (int i = 0; i < 7; i++) {
        cmos_write(registers[i], values[i]);
    }
    cmos_write(0x0b, (uint8_t)(status_b & ~0x80));
    return true;
}

/*
 * Settings live in OPOS.CFG in the root of the mounted FAT disk, with a copy
 * in CMOS. Some firmware (VMware) rewrites the spare CMOS bytes on every
 * boot, so the file wins whenever a disk is mounted.
 *
 * File layout: magic "OPCF", version, flags byte, then (version 2+) the
 * display mode index and (version 3+) the sound volume plus one (0 = not
 * set, the default volume). Older files lack the later bytes.
 */
static const uint8_t settings_file_name[11] = {'O', 'P', 'O', 'S', ' ', ' ',
                                               ' ', ' ', 'C', 'F', 'G'};
static const uint8_t settings_file_magic[4] = {'O', 'P', 'C', 'F'};
#define SETTINGS_FILE_VERSION 3
#define SETTINGS_FILE_V1_SIZE 6
#define SETTINGS_FILE_V2_SIZE 7
#define SETTINGS_FILE_SIZE 8
#define CMOS_DISPLAY_MODE_REGISTER 0x4a
#define CMOS_SOUND_VOLUME_REGISTER 0x4b

struct settings_values {
    uint8_t flags;
    uint8_t display_mode;
    uint8_t volume; /* volume + 1; 0 = default */
};

/* Cached file values, so the taskbar does not read the disk every frame. */
static bool settings_file_cached;
static struct settings_values settings_file_values;

static bool settings_file_read(struct settings_values *values) {
    struct fat16_node node;
    if (!fat16_lookup_node(0, settings_file_name, &node)
        || (node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0
        || (node.size != SETTINGS_FILE_V1_SIZE
            && node.size != SETTINGS_FILE_V2_SIZE
            && node.size != SETTINGS_FILE_SIZE)) {
        return false;
    }
    uint8_t data[SETTINGS_FILE_SIZE] = {0};
    size_t bytes_read = 0;
    if (!fat16_read_node_at(&node, 0, data, node.size, &bytes_read)
        || bytes_read != node.size) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (data[i] != settings_file_magic[i]) {
            return false;
        }
    }
    if (data[4] == 1 && node.size == SETTINGS_FILE_V1_SIZE) {
        values->flags = data[5];
        values->display_mode = 0;
        values->volume = 0;
        return true;
    }
    if (data[4] == 2 && node.size == SETTINGS_FILE_V2_SIZE) {
        values->flags = data[5];
        values->display_mode = data[6];
        values->volume = 0;
        return true;
    }
    if (data[4] != SETTINGS_FILE_VERSION || node.size != SETTINGS_FILE_SIZE) {
        return false;
    }
    values->flags = data[5];
    values->display_mode = data[6];
    values->volume = data[7];
    return true;
}

static bool settings_file_write(const struct settings_values *values) {
    struct fat16_node node;
    if (!fat16_lookup_node(0, settings_file_name, &node)) {
        if (!fat16_create_file_node(0, settings_file_name, &node)) {
            return false;
        }
    } else if ((node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    uint8_t data[SETTINGS_FILE_SIZE] = {
        settings_file_magic[0], settings_file_magic[1],
        settings_file_magic[2], settings_file_magic[3],
        SETTINGS_FILE_VERSION, values->flags, values->display_mode,
        values->volume,
    };
    size_t written = 0;
    return fat16_write_node_at(&node, 0, data, sizeof(data), &written)
        && written == sizeof(data);
}

static struct settings_values settings_cmos_load(void) {
    if (cmos_read(CMOS_SETTINGS_MAGIC_REGISTER) != CMOS_SETTINGS_MAGIC) {
        return (struct settings_values){SETTINGS_DEFAULT, 0, 0};
    }
    return (struct settings_values){cmos_read(CMOS_SETTINGS_REGISTER),
                                    cmos_read(CMOS_DISPLAY_MODE_REGISTER),
                                    cmos_read(CMOS_SOUND_VOLUME_REGISTER)};
}

/* A live session (installation media) mounts whatever FAT volume it finds
   first, which may belong to another system: it must not write OPOS.CFG
   there, so it keeps its settings in CMOS only. */
static bool settings_use_file(void) {
    return fat16_system_mounted() && !installer_available();
}

static struct settings_values settings_load(void) {
    if (!settings_use_file()) {
        settings_file_cached = false;
        return settings_cmos_load();
    }
    if (!settings_file_cached) {
        struct settings_values values;
        if (!settings_file_read(&values)) {
            /* First boot with this disk: carry the CMOS values over. */
            values = settings_cmos_load();
            (void)settings_file_write(&values);
        }
        settings_file_values = values;
        settings_file_cached = true;
    }
    return settings_file_values;
}

static void settings_store(struct settings_values values) {
    cmos_write(CMOS_SETTINGS_REGISTER, values.flags);
    cmos_write(CMOS_DISPLAY_MODE_REGISTER, values.display_mode);
    cmos_write(CMOS_SOUND_VOLUME_REGISTER, values.volume);
    cmos_write(CMOS_SETTINGS_MAGIC_REGISTER, CMOS_SETTINGS_MAGIC);
    if (settings_use_file()) {
        (void)settings_file_write(&values);
        settings_file_values = values;
        settings_file_cached = true;
    }
}

/* Machine settings with the logged-in account's personal bits applied. */
static struct settings_values settings_effective(void) {
    struct settings_values values = settings_load();
    uint8_t personal;
    if (accounts_personal_settings(&personal)) {
        values.flags = (uint8_t)((values.flags & ~SETTINGS_PERSONAL_MASK)
                                 | (personal & SETTINGS_PERSONAL_MASK));
    } else if (accounts_current() != ACCOUNT_NONE) {
        /* An account that never changed a setting uses the machine's
           personal defaults, but has not seen Welcome yet. */
        values.flags = (uint8_t)(values.flags & ~SETTING_WELCOME_SEEN);
    }
    return values;
}

/* Stores personal bits in the account when someone is logged in, so the
   machine-wide file keeps its own personal defaults. */
static void settings_update(struct settings_values values) {
    if (accounts_current() != ACCOUNT_NONE) {
        accounts_set_personal_settings(
            (uint8_t)(values.flags & SETTINGS_PERSONAL_MASK));
        struct settings_values machine = settings_load();
        values.flags = (uint8_t)((values.flags & ~SETTINGS_PERSONAL_MASK)
                                 | (machine.flags & SETTINGS_PERSONAL_MASK));
    }
    settings_store(values);
}

bool settings_get(uint8_t setting) {
    return (settings_effective().flags & setting) != 0;
}

void settings_set(uint8_t setting, bool enabled) {
    struct settings_values values = settings_effective();
    values.flags = enabled ? (uint8_t)(values.flags | setting)
                           : (uint8_t)(values.flags & ~setting);
    settings_update(values);
}

uint8_t settings_display_mode(void) {
    return settings_load().display_mode;
}

uint8_t settings_sound_volume(void) {
    uint8_t stored = settings_load().volume;
    return stored == 0 || stored > 101 ? SETTINGS_SOUND_VOLUME_DEFAULT
                                       : (uint8_t)(stored - 1);
}

void settings_set_sound_volume(uint8_t volume) {
    struct settings_values values = settings_load();
    values.volume = (uint8_t)((volume > 100 ? 100 : volume) + 1);
    settings_store(values);
}

void settings_set_display_mode(uint8_t mode) {
    struct settings_values values = settings_effective();
    values.display_mode = mode;
    settings_update(values);
}

enum date_format settings_date_format(void) {
    uint8_t format = (uint8_t)((settings_effective().flags & SETTING_DATE_FORMAT_MASK)
                               >> SETTING_DATE_FORMAT_SHIFT);
    return format < DATE_FORMAT_COUNT ? (enum date_format)format
                                      : DATE_FORMAT_ISO;
}

void settings_set_date_format(enum date_format format) {
    struct settings_values values = settings_effective();
    values.flags = (uint8_t)(values.flags & ~SETTING_DATE_FORMAT_MASK);
    values.flags |= (uint8_t)(((unsigned)format << SETTING_DATE_FORMAT_SHIFT)
                              & SETTING_DATE_FORMAT_MASK);
    settings_update(values);
}


bool settings_take_boot_shell_once(void) {
    if (!settings_get(SETTING_BOOT_SHELL_ONCE)) {
        return false;
    }
    settings_set(SETTING_BOOT_SHELL_ONCE, false);
    return true;
}
