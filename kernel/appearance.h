#ifndef OPOS_APPEARANCE_H
#define OPOS_APPEARANCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "image.h"
#include "touchpad_gesture.h"

/*
 * Desktop appearance: colour theme, background picture and account
 * pictures.
 *
 * Each account keeps its choices in /disk/Users/<name>/AppData/DESKTOP.CFG
 * and its picture (96x96, already cropped and scaled) in
 * AppData/PICTURE.RAW. A disk without accounts uses /disk/OPOS/DESKTOP.CFG.
 * The live session keeps everything in memory only.
 */

enum theme {
    THEME_OCEAN,    /* the original blue */
    THEME_EMERALD,
    THEME_VIOLET,
    THEME_ROSE,
    THEME_GRAPHITE,
    THEME_LIGHT,
    THEME_COUNT
};

enum wallpaper_style {
    WALLPAPER_FILL,    /* cover the screen, cropping the edges */
    WALLPAPER_FIT,     /* whole picture, bars on two sides */
    WALLPAPER_STRETCH,
    WALLPAPER_CENTER,  /* actual size */
    WALLPAPER_TILE,
    WALLPAPER_STYLE_COUNT
};

/* Where the taskbar sits; the windows get the rest of the screen. */
enum taskbar_position {
    TASKBAR_BOTTOM,
    TASKBAR_TOP,
    TASKBAR_LEFT,
    TASKBAR_RIGHT,
    TASKBAR_POSITION_COUNT
};

/* Bundled pictures live here (initramfs). */
#define WALLPAPER_DIRECTORY "/share/wallpapers"
#define ACCOUNT_PICTURE_SIZE 96

/* Reads the settings of the logged-in account, or of the machine. Call
   after login, logout and disk changes. */
void appearance_load(void);

enum theme appearance_theme(void);
const char *appearance_theme_name(enum theme theme);
void appearance_set_theme(enum theme theme);

/* Maps a colour of the built-in (Ocean) palette to the current theme.
   Status colours (red, yellow, green) keep their meaning. */
void appearance_map_color(uint8_t *red, uint8_t *green, uint8_t *blue);

/* "" when the plain gradient is used. */
const char *appearance_wallpaper_path(void);
/* Short name for the Settings row: PLAIN, a bundled picture's name, or
   the file name of a custom picture. */
void appearance_wallpaper_label(char *text, size_t capacity);
/* Decodes the picture first; on failure nothing changes and the reason
   is returned. An empty path selects the plain gradient. */
enum image_result appearance_set_wallpaper(const char *path, size_t length);
/* Steps through PLAIN, the bundled pictures and the current custom one. */
void appearance_cycle_wallpaper(int delta);
enum wallpaper_style appearance_wallpaper_style(void);
const char *appearance_wallpaper_style_name(enum wallpaper_style style);
void appearance_set_wallpaper_style(enum wallpaper_style style);

enum taskbar_position appearance_taskbar(void);
const char *appearance_taskbar_name(enum taskbar_position position);
void appearance_set_taskbar(enum taskbar_position position);
/* Auto-hide: the taskbar shows only while it is needed (see desktop.c
   taskbar_shown) and windows get the whole screen. */
bool appearance_taskbar_autohide(void);
void appearance_set_taskbar_autohide(bool hide);

/* SYSTEM SOUNDS: startup, shutdown, notification and other chimes
   (system_sounds.c). On by default. */
bool appearance_system_sounds(void);
void appearance_set_system_sounds(bool on);

/* Touchpad gestures (tap-to-click, scroll direction, pointer speed),
   saved with the other choices; applied to the touchpad on load. */
void appearance_touchpad(struct tp_options *options);
void appearance_set_touchpad(const struct tp_options *options);

/* The background at screen size (0x00RRGGBB), or NULL for the gradient
   (also when the picture cannot be loaded any more). */
const uint32_t *appearance_wallpaper_pixels(uint32_t width, uint32_t height);

/* Account pictures: ACCOUNT_PICTURE_SIZE squared, 0xAARRGGBB, or NULL. */
const uint32_t *appearance_account_picture(size_t account);
/* Crops the image to a centred square and saves it for the logged-in
   account. */
enum image_result appearance_set_account_picture(const char *path,
                                                 size_t length);
bool appearance_remove_account_picture(void);
/* Forget cached pictures (accounts renamed, created or deleted). */
void appearance_pictures_changed(void);

/*
 * Loads and decodes an image file. The pixels stay valid until the next
 * call with the same slot. Errors: image_result, or IMAGE_CORRUPT when the
 * file cannot be read.
 */
enum image_slot {
    IMAGE_SLOT_WORK,   /* wallpaper and picture changes */
    IMAGE_SLOT_VIEWER, /* the image viewer window */
    IMAGE_SLOT_COUNT
};
enum image_result appearance_image_load(enum image_slot slot,
                                        const char *path, size_t length,
                                        struct image *image,
                                        enum image_format *format);
/* Scratch buffer for scaled copies (screen-sized at most). */
uint32_t *appearance_scale_buffer(enum image_slot slot, size_t pixels);
/* True if the name ends in .png, .jpg, .jpeg or .bmp (any case). */
bool appearance_is_image_name(const char *name, size_t length);

#endif
