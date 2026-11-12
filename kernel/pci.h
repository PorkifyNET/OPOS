#ifndef OPOS_PCI_H
#define OPOS_PCI_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct pci_storage_controllers {
    size_t ahci_count;
    size_t nvme_count;
    uint8_t ahci_bus;
    uint8_t ahci_device;
    uint8_t ahci_function;
    uint64_t ahci_bar5;
    uint8_t nvme_bus;
    uint8_t nvme_device;
    uint8_t nvme_function;
    uint64_t nvme_bar0;
    size_t unsupported_count;
    uint8_t unsupported_subclass;
    uint16_t unsupported_vendor_id;
    uint16_t unsupported_device_id;
};

struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
};

struct pci_function_info {
    struct pci_device device;
    uint32_t class_code; /* class << 16 | subclass << 8 | prog-if */
};
/* Stores up to max functions; returns how many exist (may be more). */
size_t pci_list(struct pci_function_info *functions, size_t max);

/* Finds the first function with this vendor/device ID. */
bool pci_find_device(uint16_t vendor_id, uint16_t device_id,
                     struct pci_device *found);
/* Finds the index-th function (0 = first) with this class, subclass and
   programming interface, e.g. 0x0c/0x03/0x30 for an xHCI controller. */
bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    size_t index, struct pci_device *found);
/* BAR index 0-5. I/O BARs return the port base, memory BARs the address
 * (64-bit BARs combined). Returns 0 when the BAR is unset. */
uint64_t pci_bar_address(const struct pci_device *device, uint8_t bar,
                         bool *io_space);
/* Sets I/O space, memory space and bus master in the command register. */
void pci_enable_device(const struct pci_device *device);
/* Puts the function in power state D0 through its PCI power management
   capability. Returns the state it was in (0-3), or -1 without one. */
int pci_set_power_d0(const struct pci_device *device);
/* Reads a 32-bit configuration register (offset is rounded down to 4). */
uint32_t pci_config_read32(const struct pci_device *device, uint8_t offset);
/* Writes a 32-bit configuration register (offset is rounded down to 4). */
void pci_config_write32(const struct pci_device *device, uint8_t offset,
                        uint32_t value);

void pci_initialize(void);
void pci_get_storage_controllers(struct pci_storage_controllers *controllers);
bool pci_enable_ahci_memory_and_bus_master(void);
bool pci_enable_nvme_memory_and_bus_master(void);

#endif
