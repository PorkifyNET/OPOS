#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ahci.h"
#include "memory.h"
#include "paging.h"
#include "pci.h"

#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)
#define AHCI_MMIO_BASE 0xffffc00000002000ULL
#define AHCI_MMIO_PAGES 2
#define AHCI_HOST_CAPABILITY 0x00
#define AHCI_HOST_CONTROL 0x04
#define AHCI_HOST_PORTS_IMPLEMENTED 0x0c
#define AHCI_GLOBAL_HOST_CONTROL_AHCI_ENABLE (1u << 31)
#define AHCI_PORT_BASE 0x100
#define AHCI_PORT_STRIDE 0x80
#define AHCI_PORT_SIGNATURE 0x24
#define AHCI_PORT_SATA_STATUS 0x28
#define AHCI_PORT_TASK_FILE_DATA 0x20
#define AHCI_PORT_COMMAND_ISSUE 0x38
#define AHCI_PORT_INTERRUPT_STATUS 0x10
#define AHCI_PORT_SERIAL_ERROR 0x30
#define AHCI_PORT_COMMAND 0x18
#define AHCI_PORT_COMMAND_START (1u << 0)
#define AHCI_PORT_COMMAND_FIS_RECEIVE_ENABLE (1u << 4)
#define AHCI_PORT_COMMAND_FIS_RECEIVE_RUNNING (1u << 14)
#define AHCI_PORT_COMMAND_LIST_RUNNING (1u << 15)
#define AHCI_PORT_TASK_FILE_BUSY (1u << 7)
#define AHCI_PORT_TASK_FILE_DATA_REQUEST (1u << 3)
#define AHCI_PORT_INTERRUPT_ERRORS 0x7d000000u
#define AHCI_SATA_SIGNATURE 0x00000101
#define AHCI_ATAPI_SIGNATURE 0xeb140101
#define AHCI_LINK_DETECTED 3
#define AHCI_INTERFACE_ACTIVE 1
#define AHCI_CAP_PORT_COUNT_MASK 0x1f
#define AHCI_CAP_64BIT_ADDRESSING (1u << 31)
#define AHCI_COMMAND_FIS_LENGTH_DWORDS 5
#define AHCI_FIS_TYPE_REGISTER_H2D 0x27
#define AHCI_FIS_COMMAND_FLAG 0x80
#define AHCI_COMMAND_IDENTIFY 0xec
#define AHCI_COMMAND_READ_DMA_EXT 0x25
#define AHCI_COMMAND_WRITE_DMA_EXT 0x35
#define AHCI_COMMAND_HEADER_WRITE (1u << 6)
#define AHCI_IDENTIFY_LBA_SUPPORTED (1u << 9)
#define AHCI_IDENTIFY_LBA48_SUPPORTED (1u << 10)
#define AHCI_POLL_LIMIT 1000000

struct ahci_command_header {
    uint16_t flags;
    uint16_t prdt_length;
    uint32_t bytes_transferred;
    uint32_t command_table_base;
    uint32_t command_table_base_upper;
    uint32_t reserved[4];
} __attribute__((packed));

struct ahci_prdt_entry {
    uint32_t data_base;
    uint32_t data_base_upper;
    uint32_t reserved;
    uint32_t byte_count_and_interrupt;
} __attribute__((packed));

struct ahci_command_table {
    uint8_t command_fis[64];
    uint8_t atapi_command[16];
    uint8_t reserved[48];
    struct ahci_prdt_entry prdt[1];
} __attribute__((packed));

struct ahci_register_fis {
    uint8_t type;
    uint8_t flags;
    uint8_t command;
    uint8_t feature_low;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t feature_high;
    uint8_t count_low;
    uint8_t count_high;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} __attribute__((packed));

struct ahci_disk {
    bool ready;
    bool lba48;
    uint32_t port_offset;
    uint64_t sector_count;
    uint64_t command_list_physical;
    uint64_t received_fis_physical;
    uint64_t command_table_physical;
    uint64_t data_physical;
    struct ahci_command_header *command_list;
    uint8_t *received_fis;
    struct ahci_command_table *command_table;
    uint8_t *data_buffer;
    uint32_t port;
    char model[AHCI_MODEL_LENGTH + 1];
};

static volatile uint8_t *ahci_registers;
static struct ahci_status controller_status;
#define AHCI_MAX_DISKS 8
static struct ahci_disk disks[AHCI_MAX_DISKS];
static size_t disk_count;

static uint32_t mmio_read32(size_t offset) {
    return *(volatile uint32_t *)(ahci_registers + offset);
}

static void mmio_write32(size_t offset, uint32_t value) {
    *(volatile uint32_t *)(ahci_registers + offset) = value;
}

static bool wait_port_clear(uint32_t port_offset, uint32_t mask) {
    for (unsigned i = 0; i < AHCI_POLL_LIMIT; i++) {
        if ((mmio_read32(port_offset + AHCI_PORT_COMMAND) & mask) == 0) {
            return true;
        }
    }
    return false;
}

static bool wait_command_complete(uint32_t port_offset) {
    for (unsigned i = 0; i < AHCI_POLL_LIMIT; i++) {
        if ((mmio_read32(port_offset + AHCI_PORT_INTERRUPT_STATUS)
             & AHCI_PORT_INTERRUPT_ERRORS) != 0
            || (mmio_read32(port_offset + AHCI_PORT_TASK_FILE_DATA) & 1) != 0) {
            return false;
        }
        if ((mmio_read32(port_offset + AHCI_PORT_COMMAND_ISSUE) & 1) == 0) {
            return true;
        }
    }
    return false;
}

static bool wait_task_file_ready(uint32_t port_offset) {
    for (unsigned i = 0; i < AHCI_POLL_LIMIT; i++) {
        if ((mmio_read32(port_offset + AHCI_PORT_TASK_FILE_DATA)
             & (AHCI_PORT_TASK_FILE_BUSY | AHCI_PORT_TASK_FILE_DATA_REQUEST))
            == 0) {
            return true;
        }
    }
    return false;
}

static void zero_dma_page(uint8_t *page) {
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        page[i] = 0;
    }
}

static void release_disk_pages(struct ahci_disk *disk) {
    uint64_t physical_pages[] = {
        disk->command_list_physical,
        disk->received_fis_physical,
        disk->command_table_physical,
        disk->data_physical
    };
    for (size_t i = 0; i < sizeof(physical_pages) / sizeof(physical_pages[0]);
         i++) {
        if (physical_pages[i] != 0) {
            (void)physical_page_free(physical_pages[i]);
        }
    }
    *disk = (struct ahci_disk){0};
}

static bool allocate_disk_pages(struct ahci_disk *disk, uint32_t capability) {
    uint64_t *physical_pages[] = {
        &disk->command_list_physical,
        &disk->received_fis_physical,
        &disk->command_table_physical,
        &disk->data_physical
    };
    void **virtual_pages[] = {
        (void **)&disk->command_list,
        (void **)&disk->received_fis,
        (void **)&disk->command_table,
        (void **)&disk->data_buffer
    };

    for (size_t i = 0; i < 4; i++) {
        *physical_pages[i] = physical_page_alloc();
        if (*physical_pages[i] == 0
            || (((capability & AHCI_CAP_64BIT_ADDRESSING) == 0)
                && *physical_pages[i] > UINT32_MAX)) {
            release_disk_pages(disk);
            return false;
        }
        *virtual_pages[i] = physical_page_to_hhdm(*physical_pages[i]);
        if (*virtual_pages[i] == NULL) {
            release_disk_pages(disk);
            return false;
        }
        zero_dma_page(*virtual_pages[i]);
    }
    return true;
}

static bool issue_ahci_command(struct ahci_disk *disk, uint8_t command, uint64_t lba,
                               void *buffer, bool write) {
    if (!disk->ready || buffer == NULL) {
        return false;
    }

    uint32_t port_offset = disk->port_offset;
    if (!wait_task_file_ready(port_offset)) {
        return false;
    }

    struct ahci_command_header *header = &disk->command_list[0];
    for (size_t i = 0; i < sizeof(*header); i++) {
        ((uint8_t *)header)[i] = 0;
    }
    for (size_t i = 0; i < sizeof(*disk->command_table); i++) {
        ((uint8_t *)disk->command_table)[i] = 0;
    }
    zero_dma_page(disk->data_buffer);
    if (write) {
        const uint8_t *source = buffer;
        for (size_t i = 0; i < 512; i++) {
            disk->data_buffer[i] = source[i];
        }
    }

    header->flags = AHCI_COMMAND_FIS_LENGTH_DWORDS
        | (write ? AHCI_COMMAND_HEADER_WRITE : 0);
    header->prdt_length = 1;
    header->command_table_base = (uint32_t)disk->command_table_physical;
    header->command_table_base_upper =
        (uint32_t)(disk->command_table_physical >> 32);

    struct ahci_register_fis *fis =
        (struct ahci_register_fis *)disk->command_table->command_fis;
    fis->type = AHCI_FIS_TYPE_REGISTER_H2D;
    fis->flags = AHCI_FIS_COMMAND_FLAG;
    fis->command = command;
    if (command == AHCI_COMMAND_READ_DMA_EXT
        || command == AHCI_COMMAND_WRITE_DMA_EXT) {
        fis->lba0 = (uint8_t)lba;
        fis->lba1 = (uint8_t)(lba >> 8);
        fis->lba2 = (uint8_t)(lba >> 16);
        fis->device = 1u << 6;
        fis->lba3 = (uint8_t)(lba >> 24);
        fis->lba4 = (uint8_t)(lba >> 32);
        fis->lba5 = (uint8_t)(lba >> 40);
        fis->count_low = 1;
    }
    disk->command_table->prdt[0].data_base =
        (uint32_t)disk->data_physical;
    disk->command_table->prdt[0].data_base_upper =
        (uint32_t)(disk->data_physical >> 32);
    disk->command_table->prdt[0].byte_count_and_interrupt = 511;

    mmio_write32(port_offset + AHCI_PORT_INTERRUPT_STATUS, 0xffffffff);
    mmio_write32(port_offset + AHCI_PORT_SERIAL_ERROR, 0xffffffff);
    __asm__ volatile ("" : : : "memory");
    mmio_write32(port_offset + AHCI_PORT_COMMAND_ISSUE, 1);
    if (!wait_command_complete(port_offset)
        || disk->command_list[0].bytes_transferred < 512) {
        return false;
    }

    if (!write) {
        uint8_t *destination = buffer;
        for (size_t i = 0; i < 512; i++) {
            destination[i] = disk->data_buffer[i];
        }
    }
    return true;
}

static bool configure_sata_port(struct ahci_disk *disk, uint32_t port_offset,
                                uint32_t capability) {
    uint32_t port_command = mmio_read32(port_offset + AHCI_PORT_COMMAND);
    port_command &= ~(AHCI_PORT_COMMAND_START
                      | AHCI_PORT_COMMAND_FIS_RECEIVE_ENABLE);
    mmio_write32(port_offset + AHCI_PORT_COMMAND, port_command);
    if (!wait_port_clear(port_offset,
                         AHCI_PORT_COMMAND_LIST_RUNNING
                             | AHCI_PORT_COMMAND_FIS_RECEIVE_RUNNING)
        || !allocate_disk_pages(disk, capability)) {
        return false;
    }

    mmio_write32(port_offset + 0x00,
                 (uint32_t)disk->command_list_physical);
    mmio_write32(port_offset + 0x04,
                 (uint32_t)(disk->command_list_physical >> 32));
    mmio_write32(port_offset + 0x08,
                 (uint32_t)disk->received_fis_physical);
    mmio_write32(port_offset + 0x0c,
                 (uint32_t)(disk->received_fis_physical >> 32));
    mmio_write32(port_offset + AHCI_PORT_INTERRUPT_STATUS, 0xffffffff);
    mmio_write32(port_offset + AHCI_PORT_SERIAL_ERROR, 0xffffffff);

    port_command = mmio_read32(port_offset + AHCI_PORT_COMMAND);
    mmio_write32(port_offset + AHCI_PORT_COMMAND,
                 port_command | AHCI_PORT_COMMAND_FIS_RECEIVE_ENABLE);
    port_command = mmio_read32(port_offset + AHCI_PORT_COMMAND);
    mmio_write32(port_offset + AHCI_PORT_COMMAND,
                 port_command | AHCI_PORT_COMMAND_START);

    disk->port_offset = port_offset;
    disk->ready = true;
    uint8_t identify_data[512];
    if (!issue_ahci_command(disk, AHCI_COMMAND_IDENTIFY, 0, identify_data, false)) {
        disk->ready = false;
        release_disk_pages(disk);
        return false;
    }

    const uint16_t *identify = (const uint16_t *)identify_data;
    if ((identify[49] & AHCI_IDENTIFY_LBA_SUPPORTED) == 0) {
        disk->ready = false;
        release_disk_pages(disk);
        return false;
    }
    disk->lba48 =
        (identify[83] & AHCI_IDENTIFY_LBA48_SUPPORTED) != 0;
    if (disk->lba48) {
        disk->sector_count = (uint64_t)identify[100]
            | ((uint64_t)identify[101] << 16)
            | ((uint64_t)identify[102] << 32)
            | ((uint64_t)identify[103] << 48);
    } else {
        disk->sector_count = (uint32_t)identify[60]
            | ((uint32_t)identify[61] << 16);
    }
    if (disk->sector_count == 0) {
        disk->ready = false;
        release_disk_pages(disk);
        return false;
    }
    /* Model: words 27-46, big-endian character pairs padded with spaces. */
    size_t length = 0;
    for (size_t word = 27; word <= 46; word++) {
        disk->model[length++] = (char)(identify[word] >> 8);
        disk->model[length++] = (char)identify[word];
    }
    disk->model[length] = '\0';
    while (length > 0
           && (disk->model[length - 1] == ' '
               || disk->model[length - 1] == '\0')) {
        disk->model[--length] = '\0';
    }
    for (size_t i = 0; i < length; i++) {
        if (disk->model[i] < ' ' || disk->model[i] > '~') {
            disk->model[i] = '?';
        }
    }
    return true;
}

static bool map_ahci_registers(uint64_t physical_address) {
    if (physical_address == 0) {
        return false;
    }

    uint64_t page_base = physical_address & ~(uint64_t)PAGE_MASK;
    size_t page_offset = (size_t)(physical_address & PAGE_MASK);
    if (page_offset + AHCI_PORT_BASE + 32 * AHCI_PORT_STRIDE
        > AHCI_MMIO_PAGES * PAGE_SIZE) {
        return false;
    }

    for (size_t page = 0; page < AHCI_MMIO_PAGES; page++) {
        if (!paging_map_page(AHCI_MMIO_BASE + page * PAGE_SIZE,
                             page_base + page * PAGE_SIZE,
                             PAGING_WRITABLE | PAGING_WRITE_THROUGH
                                 | PAGING_CACHE_DISABLE | PAGING_NO_EXECUTE)) {
            for (size_t mapped = 0; mapped < page; mapped++) {
                (void)paging_unmap_page(AHCI_MMIO_BASE + mapped * PAGE_SIZE,
                                        NULL);
            }
            return false;
        }
    }
    ahci_registers = (volatile uint8_t *)(AHCI_MMIO_BASE + page_offset);
    return true;
}

bool ahci_initialize(void) {
    controller_status = (struct ahci_status){0};
    for (size_t i = 0; i < AHCI_MAX_DISKS; i++) {
        release_disk_pages(&disks[i]);
    }
    disk_count = 0;
    ahci_registers = NULL;

    struct pci_storage_controllers controllers;
    pci_get_storage_controllers(&controllers);
    if (controllers.ahci_count == 0
        || !pci_enable_ahci_memory_and_bus_master()
        || !map_ahci_registers(controllers.ahci_bar5)) {
        return false;
    }

    uint32_t host_control = mmio_read32(AHCI_HOST_CONTROL);
    mmio_write32(AHCI_HOST_CONTROL,
                 host_control | AHCI_GLOBAL_HOST_CONTROL_AHCI_ENABLE);
    controller_status.implemented_ports =
        mmio_read32(AHCI_HOST_PORTS_IMPLEMENTED);

    uint32_t capability = mmio_read32(AHCI_HOST_CAPABILITY);
    uint32_t port_count = (capability & AHCI_CAP_PORT_COUNT_MASK) + 1;
    if (port_count > 32) {
        port_count = 32;
    }

    for (uint32_t port = 0; port < port_count; port++) {
        if ((controller_status.implemented_ports & (1u << port)) == 0) {
            continue;
        }
        size_t port_offset = AHCI_PORT_BASE + port * AHCI_PORT_STRIDE;
        uint32_t sata_status = mmio_read32(port_offset + AHCI_PORT_SATA_STATUS);
        uint8_t link_status = (uint8_t)(sata_status & 0x0f);
        uint8_t interface_power = (uint8_t)((sata_status >> 8) & 0x0f);
        if (link_status != AHCI_LINK_DETECTED
            || interface_power != AHCI_INTERFACE_ACTIVE) {
            continue;
        }

        uint32_t signature = mmio_read32(port_offset + AHCI_PORT_SIGNATURE);
        if (signature == AHCI_SATA_SIGNATURE) {
            controller_status.sata_devices++;
            if (disk_count < AHCI_MAX_DISKS
                && configure_sata_port(&disks[disk_count],
                                       (uint32_t)port_offset, capability)) {
                disks[disk_count].port = port;
                if (disk_count == 0) {
                    controller_status.first_sata_sector_count =
                        disks[0].sector_count;
                }
                disk_count++;
            }
        } else if (signature == AHCI_ATAPI_SIGNATURE) {
            controller_status.atapi_devices++;
        }
    }
    return true;
}

void ahci_get_status(struct ahci_status *status) {
    if (status != NULL) {
        *status = controller_status;
    }
}

size_t ahci_disk_count(void) {
    return disk_count;
}

static struct ahci_disk *disk_at(size_t index) {
    return index < disk_count && disks[index].ready ? &disks[index] : NULL;
}

uint64_t ahci_sector_count(size_t index) {
    struct ahci_disk *disk = disk_at(index);
    return disk != NULL ? disk->sector_count : 0;
}

const char *ahci_model(size_t index) {
    struct ahci_disk *disk = disk_at(index);
    return disk != NULL ? disk->model : "";
}

uint32_t ahci_port(size_t index) {
    struct ahci_disk *disk = disk_at(index);
    return disk != NULL ? disk->port : 0;
}

static bool transfer_valid(const struct ahci_disk *disk,
                           uint32_t first_sector, uint8_t sector_count,
                           const void *buffer) {
    return disk != NULL && buffer != NULL && sector_count != 0
        && (uint64_t)first_sector < disk->sector_count
        && (uint64_t)sector_count <= disk->sector_count - first_sector
        && (disk->lba48
            || (uint64_t)first_sector + sector_count <= 0x10000000ULL);
}

bool ahci_read_sectors(size_t index, uint32_t first_sector,
                       uint8_t sector_count, void *buffer) {
    struct ahci_disk *disk = disk_at(index);
    if (!transfer_valid(disk, first_sector, sector_count, buffer)) {
        return false;
    }

    uint8_t *destination = buffer;
    for (uint16_t sector = 0; sector < sector_count; sector++) {
        if (!issue_ahci_command(disk, AHCI_COMMAND_READ_DMA_EXT,
                                (uint64_t)first_sector + sector,
                                destination + (size_t)sector * 512, false)) {
            return false;
        }
    }
    return true;
}

bool ahci_write_sectors(size_t index, uint32_t first_sector,
                        uint8_t sector_count, const void *buffer) {
    struct ahci_disk *disk = disk_at(index);
    if (!transfer_valid(disk, first_sector, sector_count, buffer)) {
        return false;
    }

    const uint8_t *source = buffer;
    for (uint16_t sector = 0; sector < sector_count; sector++) {
        if (!issue_ahci_command(disk, AHCI_COMMAND_WRITE_DMA_EXT,
                                (uint64_t)first_sector + sector,
                                (void *)(source + (size_t)sector * 512), true)) {
            return false;
        }
    }
    return true;
}
