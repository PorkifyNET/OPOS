#ifndef OPOS_POWER_H
#define OPOS_POWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool power_initialize(const void *rsdp_address);
bool power_shutdown(void);
void power_reboot(void);

#define POWER_INPUT_MAX_HIDS 4
#define POWER_INPUT_HID_LENGTH 16

/* What the ACPI tables (DSDT and SSDTs) declare for keyboard and
   pointer. A raw byte scan: a device switched off by its _STA method is
   still counted. */
struct power_input_report {
    size_t tables_scanned;
    bool boot_flags_valid;
    bool has_8042;          /* FADT IAPC_BOOT_ARCH bit 1 */
    size_t ps2_keyboard;    /* PNP03xx */
    size_t ps2_mouse;       /* PNP0Fxx */
    size_t i2c_hid;         /* PNP0C50 / ACPI0C50: HID over I2C */
    size_t touchpad_hid_count;
    char touchpad_hids[POWER_INPUT_MAX_HIDS][POWER_INPUT_HID_LENGTH];
};

void power_scan_input_devices(struct power_input_report *report);

/* A DSDT or SSDT, header included (data[0..3] is the signature). */
struct power_aml_table {
    const uint8_t *data;
    size_t length;
};
/* Any table the RSDT/XSDT lists (e.g. "APIC" for the MADT), header
   included, or NULL. */
const uint8_t *power_find_table(const char *signature, size_t *length);
/* The DSDT first, then the SSDTs. Returns how many were stored. */
size_t power_aml_tables(struct power_aml_table *tables, size_t max);

#endif
