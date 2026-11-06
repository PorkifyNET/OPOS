#ifndef OPOS_I2C_HID_H
#define OPOS_I2C_HID_H

#include <stdbool.h>

/* Finds the ACPI HID-over-I2C touchpad and its Intel LPSS (DesignWare)
   I2C controller and starts it in mouse mode. Needs the timer. */
bool i2c_hid_initialize(void);
/* Reads pending reports, at most once per timer tick. Call from the
   desktop and shell loops (not from interrupt handlers). */
void i2c_hid_poll(void);
bool i2c_hid_active(void);
/* True when the touchpad runs in Precision Touchpad (multi-touch) mode:
   tap-to-click, two-finger scrolling (options: touchpad_gesture.h). */
bool i2c_hid_multitouch(void);
/* Switches between multi-touch and plain mouse mode (shell fallback).
   False when the touchpad lacks that mode or the switch failed. */
bool i2c_hid_set_multitouch(bool multitouch);
/* True when ACPI declares a HID-over-I2C device. */
bool i2c_hid_present(void);
/* Where initialisation got to, for the boot log. */
const char *i2c_hid_stage(void);
/* Detailed state for the shell "touchpad" command. */
void i2c_hid_write_status(void);

#endif
