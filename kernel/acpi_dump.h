#ifndef OPOS_ACPI_DUMP_H
#define OPOS_ACPI_DUMP_H

#include <stdbool.h>
#include <stdint.h>

/* Prints the HID-over-I2C devices and I2C controllers the ACPI tables
   declare, with their I2C address, interrupt and register resources. */
void acpi_dump_touchpad(void);
/* Prints every PCI function: bus:dev.fn vendor:device class. */
void acpi_dump_pci(void);

/* The first HID-over-I2C device (PNP0C50) in the ACPI tables. */
struct acpi_i2c_hid {
    char hid[16];
    char path[64];
    char controller[64];        /* I2cSerialBus resource source */
    bool address_valid;
    uint16_t address;
    uint32_t speed;
    uint16_t descriptor_register; /* constant from _DSM; 0 = unknown */
    bool gsi_valid;
    uint32_t gsi;
    bool level;
    bool active_low;
    bool controller_adr_valid;
    uint32_t controller_adr;    /* PCI device << 16 | function */
};
bool acpi_find_i2c_hid(struct acpi_i2c_hid *out);

#endif
