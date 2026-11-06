#ifndef OPOS_BOOT_DISPLAY_H
#define OPOS_BOOT_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Screen size chosen by the bootloader. Without a display driver that can
 * switch modes (VMware SVGA), OPOS keeps the mode Limine set at boot. The
 * firmware's modes (VBE on BIOS, GOP on UEFI) come from Limine's
 * framebuffer response; choosing one rewrites the `resolution:` option of
 * the OPOS entries in the installed limine.conf files, and the new size
 * applies after a restart.
 */

enum boot_display_result {
    BOOT_DISPLAY_OK,
    /* Live media, or no installed limine.conf with an OPOS entry. */
    BOOT_DISPLAY_NOT_INSTALLED,
    BOOT_DISPLAY_NOT_ADMIN,
    BOOT_DISPLAY_WRITE_FAILED,
};

/* Called by main.c while reading Limine's framebuffer response. */
void boot_display_add_mode(uint32_t width, uint32_t height);
void boot_display_set_running(uint32_t width, uint32_t height);

/* Firmware modes, smallest first (the running size is always listed). */
size_t boot_display_mode_count(void);
bool boot_display_mode_size(size_t index, uint32_t *width, uint32_t *height);
void boot_display_running(uint32_t *width, uint32_t *height);

/*
 * Size in the installed limine.conf; 0x0 means none is set (Limine picks
 * the display's preferred size). False when OPOS is not installed on the
 * system disk. Read once and cached; boot_display_reload re-reads it.
 */
bool boot_display_configured(uint32_t *width, uint32_t *height);
void boot_display_reload(void);

/* Writes the size (0x0 = remove the option) for the next boot. */
enum boot_display_result boot_display_configure(uint32_t width,
                                                uint32_t height);

/*
 * Used by updates (update.c): adds an "OPOS (previous version)" entry to
 * the installed limine.conf files, a copy of the OPOS entry that boots
 * .../boot/kernel.elf with that path changed to .../boot/old/kernel.elf
 * (its initramfs module is found next to the kernel). True when every
 * file with such an entry has the copy afterwards. No permission check.
 */
bool boot_display_add_previous_entry(void);

#endif
