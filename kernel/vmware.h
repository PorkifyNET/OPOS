#ifndef OPOS_VMWARE_H
#define OPOS_VMWARE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * VMware "backdoor" services (I/O port 0x5658), also offered by QEMU's
 * vmport for the mouse part:
 *  - vmmouse: absolute pointer, so the host cursor moves in and out of the
 *    VM window without grabbing.
 *  - GuestRPC: tells VMware the guest can change resolution, then applies
 *    the "Resolution_Set" requests VMware sends (View > Autosize > Autofit
 *    Guest) through the SVGA driver.
 * Both are skipped when no hypervisor/backdoor is present.
 */

/* Call after mouse_initialize() and svga_initialize(). */
void vmware_initialize(void);
bool vmware_backdoor_present(void);
bool vmware_mouse_active(void);

/* Drains queued absolute pointer events (from the PS/2 IRQ). Coordinates
   are 0..65535 across the screen. Wheel notches (positive = up) are added
   to *wheel. Returns true if any event was read. */
bool vmware_mouse_read(uint32_t *x, uint32_t *y, uint8_t *buttons,
                       int32_t *wheel);

/* Polls the GuestRPC channel; call from the main loops. Rate-limited
   internally. Not for interrupt context. */
void vmware_tools_poll(void);

/* Settings value meaning "follow the VM window" (see settings_display_mode). */
#define VMWARE_DISPLAY_AUTOFIT 255
/* True when VMware can send window sizes (GuestRPC + SVGA present). */
bool vmware_autofit_available(void);
bool vmware_autofit_enabled(void);
/* Turns autofit on/off; on also applies the last size the host sent. */
void vmware_set_autofit(bool enabled);

#endif
