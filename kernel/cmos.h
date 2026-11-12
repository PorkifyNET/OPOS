#ifndef OPOS_CMOS_H
#define OPOS_CMOS_H

#include <stdbool.h>
#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

/* Reads the battery-backed real-time clock (host time in VMs). */
bool rtc_read(struct rtc_time *time);
/*
 * Sets the real-time clock. Years 2000-2099 only. Returns false and leaves
 * the clock alone when a field is out of range.
 */
bool rtc_write(const struct rtc_time *time);

/*
 * Persistent settings, kept in OPOS.CFG on the mounted FAT disk and copied
 * into spare CMOS bytes. Without a disk only CMOS is used, which some
 * firmware (VMware) resets on boot.
 */
#define SETTING_AUTOSTART_DESKTOP 0x01
#define SETTING_TERMINAL_COLORS 0x02
#define SETTING_CLOCK_12H 0x04
/* Bits 0x08-0x10 hold the date format (enum date_format), not a flag. */
#define SETTING_DATE_FORMAT_MASK 0x18
#define SETTING_DATE_FORMAT_SHIFT 3
/* Desktop windows float instead of tiling (clear = tiled, the default). */
#define SETTING_FLOATING_LAYOUT 0x20
/* The Welcome window was already shown on a first login. */
#define SETTING_WELCOME_SEEN 0x40
#define SETTING_BOOT_SHELL_ONCE 0x80
/* Per-user settings: kept in the logged-in account (accounts.c); the
   other bits are machine-wide. */
#define SETTINGS_PERSONAL_MASK (SETTING_TERMINAL_COLORS | SETTING_CLOCK_12H \
                                | SETTING_DATE_FORMAT_MASK                  \
                                | SETTING_FLOATING_LAYOUT                   \
                                | SETTING_WELCOME_SEEN)

enum date_format {
    DATE_FORMAT_ISO,       /* YYYY-MM-DD */
    DATE_FORMAT_DMY_DOT,   /* DD.MM.YYYY */
    DATE_FORMAT_MDY_SLASH, /* MM/DD/YYYY */
    DATE_FORMAT_DMY_SLASH, /* DD/MM/YYYY */
    DATE_FORMAT_COUNT
};

bool settings_get(uint8_t setting);
void settings_set(uint8_t setting, bool enabled);
/* Display mode index for svga_set_mode (0 = boot default). */
uint8_t settings_display_mode(void);
void settings_set_display_mode(uint8_t mode);
enum date_format settings_date_format(void);
void settings_set_date_format(enum date_format format);
/* Master sound volume, 0-100 (machine-wide). */
#define SETTINGS_SOUND_VOLUME_DEFAULT 70
uint8_t settings_sound_volume(void);
void settings_set_sound_volume(uint8_t volume);
/* Returns true once if a one-time boot into the shell was requested. */
bool settings_take_boot_shell_once(void);

#endif
