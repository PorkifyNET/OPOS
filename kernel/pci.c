#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "pci.h"

#define PCI_CONFIG_ADDRESS 0x0cf8
#define PCI_CONFIG_DATA 0x0cfc
#define PCI_CONFIG_COMMAND 0x04
#define PCI_VENDOR_ID_INVALID 0xffff
#define PCI_HEADER_TYPE_MULTI_FUNCTION 0x80
#define PCI_CLASS_MASS_STORAGE 0x01
#define PCI_SUBCLASS_IDE 0x01
#define PCI_SUBCLASS_SATA 0x06
#define PCI_SUBCLASS_NVM 0x08
#define PCI_PROG_IF_AHCI 0x01
#define PCI_PROG_IF_NVME 0x02
#define PCI_BAR_IO_SPACE 0x01
#define PCI_BAR_MEMORY_TYPE_MASK 0x06
#define PCI_BAR_MEMORY_TYPE_32 0x00
#define PCI_BAR_MEMORY_TYPE_64 0x04
#define PCI_BAR_MEMORY_ADDRESS_MASK 0xfffffff0u

static struct pci_storage_controllers storage_controllers;

static uint32_t pci_read_config32(uint8_t bus, uint8_t device,
                                 uint8_t function, uint8_t offset) {
    uint32_t address = 0x80000000u | ((uint32_t)bus << 16)
        | ((uint32_t)device << 11) | ((uint32_t)function << 8)
        | (offset & 0xfc);
    io_out32(PCI_CONFIG_ADDRESS, address);
    return io_in32(PCI_CONFIG_DATA);
}

static uint64_t pci_read_memory_bar(uint8_t bus, uint8_t device,
                                    uint8_t function, uint8_t offset) {
    uint32_t low = pci_read_config32(bus, device, function, offset);
    if ((low & PCI_BAR_IO_SPACE) != 0) {
        return 0;
    }

    uint64_t address = low & PCI_BAR_MEMORY_ADDRESS_MASK;
    uint32_t memory_type = low & PCI_BAR_MEMORY_TYPE_MASK;
    if (memory_type == PCI_BAR_MEMORY_TYPE_64 && offset < 0x24) {
        address |= (uint64_t)pci_read_config32(bus, device, function,
                                               offset + 4) << 32;
    } else if (memory_type != PCI_BAR_MEMORY_TYPE_32) {
        return 0;
    }
    return address;
}

static bool pci_function_exists(uint8_t bus, uint8_t device,
                                uint8_t function) {
    return (pci_read_config32(bus, device, function, 0) & 0xffff)
        != PCI_VENDOR_ID_INVALID;
}

void pci_initialize(void) {
    storage_controllers = (struct pci_storage_controllers){0};
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            if (!pci_function_exists((uint8_t)bus, device, 0)) {
                continue;
            }

            uint8_t header_type = (uint8_t)(pci_read_config32(
                (uint8_t)bus, device, 0, 0x0c) >> 16);
            uint8_t function_count =
                (header_type & PCI_HEADER_TYPE_MULTI_FUNCTION) != 0 ? 8 : 1;
            for (uint8_t function = 0; function < function_count; function++) {
                if (!pci_function_exists((uint8_t)bus, device, function)) {
                    continue;
                }
                uint32_t class_info =
                    pci_read_config32((uint8_t)bus, device, function, 0x08);
                uint8_t class_code = (uint8_t)(class_info >> 24);
                uint8_t subclass = (uint8_t)(class_info >> 16);
                uint8_t programming_interface = (uint8_t)(class_info >> 8);
                if (class_code != PCI_CLASS_MASS_STORAGE) {
                    continue;
                }

                if (subclass == PCI_SUBCLASS_SATA
                    && programming_interface == PCI_PROG_IF_AHCI) {
                    if (storage_controllers.ahci_count == 0) {
                        storage_controllers.ahci_bus = (uint8_t)bus;
                        storage_controllers.ahci_device = device;
                        storage_controllers.ahci_function = function;
                        storage_controllers.ahci_bar5 = pci_read_memory_bar(
                            (uint8_t)bus, device, function, 0x24);
                    }
                    storage_controllers.ahci_count++;
                } else if (subclass == PCI_SUBCLASS_NVM
                           && programming_interface == PCI_PROG_IF_NVME) {
                    if (storage_controllers.nvme_count == 0) {
                        storage_controllers.nvme_bus = (uint8_t)bus;
                        storage_controllers.nvme_device = device;
                        storage_controllers.nvme_function = function;
                        storage_controllers.nvme_bar0 = pci_read_memory_bar(
                            (uint8_t)bus, device, function, 0x10);
                    }
                    storage_controllers.nvme_count++;
                } else if (subclass != PCI_SUBCLASS_IDE) {
                    if (storage_controllers.unsupported_count == 0) {
                        uint32_t identifier = pci_read_config32(
                            (uint8_t)bus, device, function, 0);
                        storage_controllers.unsupported_subclass = subclass;
                        storage_controllers.unsupported_vendor_id =
                            (uint16_t)identifier;
                        storage_controllers.unsupported_device_id =
                            (uint16_t)(identifier >> 16);
                    }
                    storage_controllers.unsupported_count++;
                }
            }
        }
    }

}

void pci_get_storage_controllers(struct pci_storage_controllers *controllers) {
    if (controllers == NULL) {
        return;
    }
    *controllers = storage_controllers;
}

static void pci_enable_memory_and_bus_master(uint8_t bus, uint8_t device,
                                             uint8_t function) {
    uint32_t command_status =
        pci_read_config32(bus, device, function, PCI_CONFIG_COMMAND);
    uint16_t command = (uint16_t)command_status;
    command |= (1u << 1) | (1u << 2);
    io_out32(PCI_CONFIG_ADDRESS,
             0x80000000u | ((uint32_t)bus << 16)
                 | ((uint32_t)device << 11) | ((uint32_t)function << 8)
                 | PCI_CONFIG_COMMAND);
    io_out16(PCI_CONFIG_DATA, command);
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id,
                     struct pci_device *found) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            if (!pci_function_exists((uint8_t)bus, device, 0)) {
                continue;
            }
            uint8_t header_type = (uint8_t)(pci_read_config32(
                (uint8_t)bus, device, 0, 0x0c) >> 16);
            uint8_t function_count =
                (header_type & PCI_HEADER_TYPE_MULTI_FUNCTION) != 0 ? 8 : 1;
            for (uint8_t function = 0; function < function_count; function++) {
                uint32_t identifier =
                    pci_read_config32((uint8_t)bus, device, function, 0);
                if ((uint16_t)identifier != vendor_id
                    || (uint16_t)(identifier >> 16) != device_id) {
                    continue;
                }
                if (found != NULL) {
                    *found = (struct pci_device){
                        (uint8_t)bus, device, function, vendor_id, device_id,
                    };
                }
                return true;
            }
        }
    }
    return false;
}

uint64_t pci_bar_address(const struct pci_device *device, uint8_t bar,
                         bool *io_space) {
    if (device == NULL || bar > 5) {
        return 0;
    }
    uint8_t offset = (uint8_t)(0x10 + bar * 4);
    uint32_t low = pci_read_config32(device->bus, device->device,
                                     device->function, offset);
    bool io = (low & PCI_BAR_IO_SPACE) != 0;
    if (io_space != NULL) {
        *io_space = io;
    }
    if (io) {
        return low & 0xfffffffcu;
    }
    return pci_read_memory_bar(device->bus, device->device, device->function,
                               offset);
}

void pci_enable_device(const struct pci_device *device) {
    if (device == NULL) {
        return;
    }
    uint32_t command_status = pci_read_config32(
        device->bus, device->device, device->function, PCI_CONFIG_COMMAND);
    uint16_t command = (uint16_t)command_status;
    command |= (1u << 0) | (1u << 1) | (1u << 2);
    io_out32(PCI_CONFIG_ADDRESS,
             0x80000000u | ((uint32_t)device->bus << 16)
                 | ((uint32_t)device->device << 11)
                 | ((uint32_t)device->function << 8) | PCI_CONFIG_COMMAND);
    io_out16(PCI_CONFIG_DATA, command);
}

bool pci_enable_ahci_memory_and_bus_master(void) {
    if (storage_controllers.ahci_count == 0) {
        return false;
    }
    pci_enable_memory_and_bus_master(storage_controllers.ahci_bus,
                                     storage_controllers.ahci_device,
                                     storage_controllers.ahci_function);
    return true;
}

bool pci_enable_nvme_memory_and_bus_master(void) {
    if (storage_controllers.nvme_count == 0) {
        return false;
    }
    pci_enable_memory_and_bus_master(storage_controllers.nvme_bus,
                                     storage_controllers.nvme_device,
                                     storage_controllers.nvme_function);
    return true;
}

bool pci_find_class(uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                    size_t index, struct pci_device *found) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            if (!pci_function_exists((uint8_t)bus, device, 0)) {
                continue;
            }
            uint8_t header_type = (uint8_t)(pci_read_config32(
                (uint8_t)bus, device, 0, 0x0c) >> 16);
            uint8_t function_count =
                (header_type & PCI_HEADER_TYPE_MULTI_FUNCTION) != 0 ? 8 : 1;
            for (uint8_t function = 0; function < function_count; function++) {
                uint32_t identifier =
                    pci_read_config32((uint8_t)bus, device, function, 0);
                if ((uint16_t)identifier == PCI_VENDOR_ID_INVALID) {
                    continue;
                }
                uint32_t class_register =
                    pci_read_config32((uint8_t)bus, device, function, 0x08);
                if ((uint8_t)(class_register >> 24) != class_code
                    || (uint8_t)(class_register >> 16) != subclass
                    || (uint8_t)(class_register >> 8) != prog_if
                    || index-- != 0) {
                    continue;
                }
                if (found != NULL) {
                    *found = (struct pci_device){
                        (uint8_t)bus, device, function,
                        (uint16_t)identifier, (uint16_t)(identifier >> 16),
                    };
                }
                return true;
            }
        }
    }
    return false;
}
