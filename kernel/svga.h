#ifndef OPOS_SVGA_H
#define OPOS_SVGA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * VMware SVGA II display driver (PCI 15ad:0405). Used by VMware
 * Workstation, VirtualBox with the VMSVGA adapter, and QEMU -vga vmware.
 * Without the device OPOS keeps the framebuffer Limine set up at boot.
 *
 * Mode index 0 is "boot default" (the Limine resolution); indexes 1.. are
 * the fixed list below.
 */
bool svga_initialize(void);
bool svga_available(void);
size_t svga_mode_count(void);
/* Width/height of a mode index (index 0 reports the boot resolution). */
bool svga_mode_size(size_t index, uint32_t *width, uint32_t *height);
bool svga_mode_supported(size_t index);
bool svga_set_mode(size_t index);
/* Mode index reported while a size requested by the host is active. */
#define SVGA_MODE_CUSTOM ((size_t)-1)
/* Any size (clamped to 640x480 and the device maximum); used for host
   resize requests. Not saved in settings. */
bool svga_set_size(uint32_t width, uint32_t height);
size_t svga_current_mode(void);
/* Called from the timer interrupt: tells the host to repaint if needed. */
void svga_timer_tick(void);

#endif
