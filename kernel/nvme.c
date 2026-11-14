#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "memory.h"
#include "nvme.h"
#include "paging.h"
#include "pci.h"

#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)
#define SECTOR_SIZE 512
#define SECTORS_PER_PAGE (PAGE_SIZE / SECTOR_SIZE)
#define NVME_MMIO_BASE 0xffffc00000004000ULL
#define NVME_MMIO_PAGES 2
#define NVME_REGISTER_CAPABILITY 0x00
#define NVME_REGISTER_INTERRUPT_MASK_SET 0x0c
#define NVME_REGISTER_CONFIGURATION 0x14
#define NVME_REGISTER_STATUS 0x1c
#define NVME_REGISTER_ADMIN_QUEUE_ATTRIBUTES 0x24
#define NVME_REGISTER_ADMIN_SUBMISSION_QUEUE 0x28
#define NVME_REGISTER_ADMIN_COMPLETION_QUEUE 0x30
#define NVME_DOORBELL_BASE 0x1000
#define NVME_CAP_NVM_COMMAND_SET (1ULL << 37)
#define NVME_CONFIGURATION_ENABLE (1u << 0)
#define NVME_CONFIGURATION_IO_SQ_ENTRY_SIZE (6u << 16)
#define NVME_CONFIGURATION_IO_CQ_ENTRY_SIZE (4u << 20)
#define NVME_STATUS_READY (1u << 0)
#define NVME_STATUS_FATAL (1u << 1)
#define NVME_QUEUE_ENTRIES 16
#define NVME_ADMIN_QUEUE 0
#define NVME_IO_QUEUE 1
#define NVME_ADMIN_CREATE_IO_SQ 0x01
#define NVME_ADMIN_CREATE_IO_CQ 0x05
#define NVME_ADMIN_IDENTIFY 0x06
#define NVME_IO_FLUSH 0x00
#define NVME_IO_WRITE 0x01
#define NVME_IO_READ 0x02
#define NVME_IDENTIFY_NAMESPACE 0x00
#define NVME_IDENTIFY_CONTROLLER 0x01
#define NVME_IDENTIFY_ACTIVE_NAMESPACES 0x02
#define NVME_IDENTIFY_VOLATILE_WRITE_CACHE 525
#define NVME_IDENTIFY_MDTS 77
#define NVME_POLL_LIMIT 20000000u

struct nvme_submission_entry {
    uint32_t command;
    uint32_t namespace_id;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t dword10;
    uint32_t dword11;
    uint32_t dword12;
    uint32_t dword13;
    uint32_t dword14;
    uint32_t dword15;
};

_Static_assert(sizeof(struct nvme_submission_entry) == 64,
               "NVMe submission entries are 64 bytes");

struct nvme_completion_entry {
    uint32_t result;
    uint32_t reserved;
    uint16_t submission_head;
    uint16_t submission_queue;
    uint16_t command_id;
    uint16_t status;
} __attribute__((packed));

struct nvme_queue {
    uint16_t id;
    uint16_t submission_tail;
    uint16_t completion_head;
    bool phase;
    uint64_t submission_physical;
    uint64_t completion_physical;
    volatile struct nvme_submission_entry *submission;
    volatile struct nvme_completion_entry *completion;
};

static volatile uint8_t *nvme_registers;
static uint32_t doorbell_stride;
static struct nvme_queue admin_queue;
static struct nvme_queue io_queue;
/* Bounce buffer: data_pages contiguous pages (one if a contiguous run
   could not be had), so a read or write moves up to 64 KiB per command
   instead of one page. prp_list names its pages after the first. */
#define NVME_DATA_PAGES 16
static uint64_t data_physical;
static uint8_t *data_page;
static uint32_t data_pages;
static uint32_t transfer_pages; /* data_pages, or less under MDTS */
static uint64_t prp_list_physical;
static uint64_t *prp_list;
static uint16_t next_command_id;
static bool volatile_write_cache;
static struct nvme_status controller_status;

struct nvme_namespace {
    uint32_t id;
    uint64_t blocks;
};
static struct nvme_namespace namespaces[NVME_MAX_NAMESPACES];
static size_t namespace_count;
static char controller_model[NVME_MODEL_LENGTH + 1];

static uint32_t mmio_read32(size_t offset) {
    return *(volatile uint32_t *)(nvme_registers + offset);
}

static uint64_t mmio_read64(size_t offset) {
    uint64_t low = mmio_read32(offset);
    return low | ((uint64_t)mmio_read32(offset + 4) << 32);
}

static void mmio_write32(size_t offset, uint32_t value) {
    *(volatile uint32_t *)(nvme_registers + offset) = value;
}

static void mmio_write64(size_t offset, uint64_t value) {
    mmio_write32(offset, (uint32_t)value);
    mmio_write32(offset + 4, (uint32_t)(value >> 32));
}

static void zero_page(void *page) {
    volatile uint8_t *bytes = page;
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        bytes[i] = 0;
    }
}

static bool allocate_page(uint64_t *physical, void **virtual_address) {
    *physical = physical_page_alloc();
    if (*physical == 0) {
        return false;
    }
    *virtual_address = physical_page_to_hhdm(*physical);
    if (*virtual_address == NULL) {
        (void)physical_page_free(*physical);
        *physical = 0;
        return false;
    }
    zero_page(*virtual_address);
    return true;
}

static void release_page(uint64_t *physical) {
    if (*physical != 0) {
        (void)physical_page_free(*physical);
        *physical = 0;
    }
}

static void release_queue(struct nvme_queue *queue) {
    release_page(&queue->submission_physical);
    release_page(&queue->completion_physical);
    *queue = (struct nvme_queue){0};
}

static bool allocate_queue(struct nvme_queue *queue, uint16_t id) {
    *queue = (struct nvme_queue){.id = id, .phase = true};
    void *submission;
    void *completion;
    if (!allocate_page(&queue->submission_physical, &submission)
        || !allocate_page(&queue->completion_physical, &completion)) {
        release_queue(queue);
        return false;
    }
    queue->submission = submission;
    queue->completion = completion;
    return true;
}

static size_t submission_doorbell(uint16_t queue_id) {
    return NVME_DOORBELL_BASE + (size_t)(2u * queue_id) * doorbell_stride;
}

static size_t completion_doorbell(uint16_t queue_id) {
    return NVME_DOORBELL_BASE + (size_t)(2u * queue_id + 1u) * doorbell_stride;
}

static bool wait_ready(bool ready) {
    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        uint32_t status = mmio_read32(NVME_REGISTER_STATUS);
        if (status == 0xffffffffu
            || (ready && (status & NVME_STATUS_FATAL) != 0)) {
            return false;
        }
        if (((status & NVME_STATUS_READY) != 0) == ready) {
            return true;
        }
        io_wait();
    }
    return false;
}

/* Submits one command and polls for its completion. The driver keeps at most
   one command outstanding per queue, so the queues can never fill up. */
static bool submit_and_wait(struct nvme_queue *queue,
                            const struct nvme_submission_entry *entry) {
    uint16_t command_id = next_command_id++;
    struct nvme_submission_entry command = *entry;
    command.command = (command.command & 0xffffu)
        | ((uint32_t)command_id << 16);
    const uint32_t *source = (const uint32_t *)&command;
    volatile uint32_t *destination =
        (volatile uint32_t *)&queue->submission[queue->submission_tail];
    for (size_t i = 0; i < sizeof(command) / sizeof(uint32_t); i++) {
        destination[i] = source[i];
    }
    queue->submission_tail =
        (uint16_t)((queue->submission_tail + 1) % NVME_QUEUE_ENTRIES);
    __asm__ volatile ("mfence" : : : "memory");
    mmio_write32(submission_doorbell(queue->id), queue->submission_tail);

    volatile struct nvme_completion_entry *completion =
        &queue->completion[queue->completion_head];
    for (uint32_t i = 0; i < NVME_POLL_LIMIT; i++) {
        uint16_t status = completion->status;
        if (((status & 1u) != 0) != queue->phase) {
            io_wait();
            continue;
        }
        __asm__ volatile ("" : : : "memory");
        bool matches = completion->command_id == command_id;
        queue->completion_head =
            (uint16_t)((queue->completion_head + 1) % NVME_QUEUE_ENTRIES);
        if (queue->completion_head == 0) {
            queue->phase = !queue->phase;
        }
        mmio_write32(completion_doorbell(queue->id), queue->completion_head);
        return matches && (status >> 1) == 0;
    }
    return false;
}

static bool admin_identify(uint32_t namespace_id, uint32_t cns) {
    zero_page(data_page);
    struct nvme_submission_entry entry = {
        .command = NVME_ADMIN_IDENTIFY,
        .namespace_id = namespace_id,
        .prp1 = data_physical,
        .dword10 = cns,
    };
    return submit_and_wait(&admin_queue, &entry);
}

static bool map_nvme_registers(uint64_t physical_address) {
    if (physical_address == 0) {
        return false;
    }
    uint64_t page_base = physical_address & ~(uint64_t)PAGE_MASK;
    size_t page_offset = (size_t)(physical_address & PAGE_MASK);
    for (size_t page = 0; page < NVME_MMIO_PAGES; page++) {
        if (!paging_map_page(NVME_MMIO_BASE + page * PAGE_SIZE,
                             page_base + page * PAGE_SIZE,
                             PAGING_WRITABLE | PAGING_WRITE_THROUGH
                                 | PAGING_CACHE_DISABLE | PAGING_NO_EXECUTE)) {
            for (size_t mapped = 0; mapped < page; mapped++) {
                (void)paging_unmap_page(NVME_MMIO_BASE + mapped * PAGE_SIZE,
                                        NULL);
            }
            return false;
        }
    }
    nvme_registers = (volatile uint8_t *)(NVME_MMIO_BASE + page_offset);
    return true;
}

static void release_controller(void) {
    release_queue(&admin_queue);
    release_queue(&io_queue);
    for (uint32_t i = 1; i < data_pages; i++) {
        (void)physical_page_free(data_physical + (uint64_t)i * PAGE_SIZE);
    }
    release_page(&data_physical);
    release_page(&prp_list_physical);
    data_page = NULL;
    prp_list = NULL;
    data_pages = 0;
    transfer_pages = 0;
}

/* The bounce buffer: NVME_DATA_PAGES contiguous pages, else one. */
static bool allocate_data_pages(void) {
    void *list;
    if (!allocate_page(&prp_list_physical, &list)) {
        return false;
    }
    prp_list = list;
    data_physical = physical_pages_alloc_contiguous(NVME_DATA_PAGES);
    if (data_physical != 0) {
        data_pages = NVME_DATA_PAGES;
        data_page = physical_page_to_hhdm(data_physical);
        /* The list starts at the second page; the first is PRP1. */
        for (uint32_t i = 1; i < data_pages; i++) {
            prp_list[i - 1] = data_physical + (uint64_t)i * PAGE_SIZE;
        }
        return true;
    }
    void *page;
    if (!allocate_page(&data_physical, &page)) {
        return false;
    }
    data_pages = 1;
    data_page = page;
    return true;
}

static bool fail_controller(void) {
    uint32_t configuration = mmio_read32(NVME_REGISTER_CONFIGURATION);
    mmio_write32(NVME_REGISTER_CONFIGURATION,
                 configuration & ~NVME_CONFIGURATION_ENABLE);
    (void)wait_ready(false);
    release_controller();
    controller_status = (struct nvme_status){0};
    return false;
}

bool nvme_initialize(void) {
    controller_status = (struct nvme_status){0};
    namespace_count = 0;
    if (nvme_registers != NULL) {
        return false;
    }

    struct pci_storage_controllers controllers;
    pci_get_storage_controllers(&controllers);
    if (controllers.nvme_count == 0
        || !pci_enable_nvme_memory_and_bus_master()
        || !map_nvme_registers(controllers.nvme_bar0)) {
        return false;
    }

    uint64_t capability = mmio_read64(NVME_REGISTER_CAPABILITY);
    doorbell_stride = 4u << ((capability >> 32) & 0x0f);
    uint32_t max_entries = (uint32_t)(capability & 0xffff) + 1;
    uint32_t minimum_page_shift = 12 + (uint32_t)((capability >> 48) & 0x0f);
    size_t register_offset = (size_t)((uintptr_t)nvme_registers & PAGE_MASK);
    if (capability == UINT64_MAX
        || (capability & NVME_CAP_NVM_COMMAND_SET) == 0
        || minimum_page_shift != 12 || max_entries < NVME_QUEUE_ENTRIES
        || register_offset + completion_doorbell(NVME_IO_QUEUE) + 4
            > NVME_MMIO_PAGES * PAGE_SIZE) {
        return false;
    }

    uint32_t configuration = mmio_read32(NVME_REGISTER_CONFIGURATION);
    if ((configuration & NVME_CONFIGURATION_ENABLE) != 0) {
        mmio_write32(NVME_REGISTER_CONFIGURATION,
                     configuration & ~NVME_CONFIGURATION_ENABLE);
    }
    if (!wait_ready(false)
        || !allocate_queue(&admin_queue, NVME_ADMIN_QUEUE)
        || !allocate_queue(&io_queue, NVME_IO_QUEUE)
        || !allocate_data_pages()) {
        return fail_controller();
    }

    mmio_write32(NVME_REGISTER_INTERRUPT_MASK_SET, 0xffffffffu);
    mmio_write32(NVME_REGISTER_ADMIN_QUEUE_ATTRIBUTES,
                 ((uint32_t)(NVME_QUEUE_ENTRIES - 1) << 16)
                     | (NVME_QUEUE_ENTRIES - 1));
    mmio_write64(NVME_REGISTER_ADMIN_SUBMISSION_QUEUE,
                 admin_queue.submission_physical);
    mmio_write64(NVME_REGISTER_ADMIN_COMPLETION_QUEUE,
                 admin_queue.completion_physical);
    mmio_write32(NVME_REGISTER_CONFIGURATION,
                 NVME_CONFIGURATION_IO_CQ_ENTRY_SIZE
                     | NVME_CONFIGURATION_IO_SQ_ENTRY_SIZE
                     | NVME_CONFIGURATION_ENABLE);
    if (!wait_ready(true)
        || !admin_identify(0, NVME_IDENTIFY_CONTROLLER)) {
        return fail_controller();
    }
    volatile_write_cache =
        (data_page[NVME_IDENTIFY_VOLATILE_WRITE_CACHE] & 1u) != 0;
    /* Maximum data transfer size: 2^MDTS minimum-size (4 KiB) pages,
       0 for no limit. */
    uint8_t mdts = data_page[NVME_IDENTIFY_MDTS];
    transfer_pages = data_pages;
    if (mdts != 0 && mdts < 31 && (1u << mdts) < transfer_pages) {
        transfer_pages = 1u << mdts;
    }

    /* Model number: identify-controller bytes 24-63, space padded. */
    size_t model_length = 0;
    for (size_t i = 24; i < 64; i++) {
        char character = (char)data_page[i];
        controller_model[model_length++] =
            character >= ' ' && character <= '~' ? character : ' ';
    }
    while (model_length > 0 && controller_model[model_length - 1] == ' ') {
        model_length--;
    }
    controller_model[model_length] = '\0';

    uint32_t active[NVME_MAX_NAMESPACES] = {1};
    size_t active_count = 1;
    if (admin_identify(0, NVME_IDENTIFY_ACTIVE_NAMESPACES)) {
        active_count = 0;
        for (size_t i = 0; i < NVME_MAX_NAMESPACES; i++) {
            uint32_t id = (uint32_t)data_page[4 * i]
                | ((uint32_t)data_page[4 * i + 1] << 8)
                | ((uint32_t)data_page[4 * i + 2] << 16)
                | ((uint32_t)data_page[4 * i + 3] << 24);
            if (id == 0) {
                break;
            }
            active[active_count++] = id;
        }
        if (active_count == 0) {
            active[0] = 1;
            active_count = 1;
        }
    }
    for (size_t i = 0; i < active_count; i++) {
        if (!admin_identify(active[i], NVME_IDENTIFY_NAMESPACE)) {
            continue;
        }
        uint64_t blocks = 0;
        for (size_t byte = 0; byte < 8; byte++) {
            blocks |= (uint64_t)data_page[byte] << (8 * byte);
        }
        uint8_t format_index = data_page[26] & 0x0f;
        uint8_t block_shift = data_page[128 + 4 * format_index + 2];
        uint32_t block_size = block_shift < 32 ? 1u << block_shift : 0;
        if (controller_status.namespace_id == 0) {
            controller_status.namespace_id = active[i];
            controller_status.logical_block_size = block_size;
            controller_status.namespace_blocks = blocks;
        }
        /* The block layer only handles 512-byte sectors. */
        if (block_size == SECTOR_SIZE && blocks != 0) {
            namespaces[namespace_count].id = active[i];
            namespaces[namespace_count].blocks = blocks;
            namespace_count++;
        }
    }
    if (controller_status.namespace_id == 0) {
        return fail_controller();
    }

    struct nvme_submission_entry create_completion = {
        .command = NVME_ADMIN_CREATE_IO_CQ,
        .prp1 = io_queue.completion_physical,
        .dword10 = ((uint32_t)(NVME_QUEUE_ENTRIES - 1) << 16) | NVME_IO_QUEUE,
        .dword11 = 1,
    };
    struct nvme_submission_entry create_submission = {
        .command = NVME_ADMIN_CREATE_IO_SQ,
        .prp1 = io_queue.submission_physical,
        .dword10 = ((uint32_t)(NVME_QUEUE_ENTRIES - 1) << 16) | NVME_IO_QUEUE,
        .dword11 = ((uint32_t)NVME_IO_QUEUE << 16) | 1,
    };
    if (!submit_and_wait(&admin_queue, &create_completion)
        || !submit_and_wait(&admin_queue, &create_submission)) {
        return fail_controller();
    }

    controller_status.controller_ready = true;
    return true;
}

void nvme_get_status(struct nvme_status *status) {
    if (status != NULL) {
        *status = controller_status;
    }
}

size_t nvme_disk_count(void) {
    return controller_status.controller_ready ? namespace_count : 0;
}

uint64_t nvme_sector_count(size_t disk) {
    return disk < nvme_disk_count() ? namespaces[disk].blocks : 0;
}

const char *nvme_model(size_t disk) {
    return disk < nvme_disk_count() ? controller_model : "";
}

uint32_t nvme_namespace_id(size_t disk) {
    return disk < nvme_disk_count() ? namespaces[disk].id : 0;
}

/* While set, writes skip the flush; nvme_flush sends one. */
static bool flush_deferred;

bool nvme_flush(size_t disk) {
    if (nvme_sector_count(disk) == 0) {
        return false;
    }
    if (!volatile_write_cache) {
        return true;
    }
    struct nvme_submission_entry flush = {
        .command = NVME_IO_FLUSH,
        .namespace_id = namespaces[disk].id,
    };
    return submit_and_wait(&io_queue, &flush);
}

static bool transfer(size_t disk, uint32_t first_sector, uint8_t sector_count,
                     uint8_t *buffer, bool write) {
    uint64_t disk_sectors = nvme_sector_count(disk);
    if (disk_sectors == 0 || buffer == NULL || sector_count == 0
        || (uint64_t)first_sector >= disk_sectors
        || (uint64_t)sector_count > disk_sectors - first_sector) {
        return false;
    }

    uint32_t done = 0;
    while (done < sector_count) {
        uint32_t chunk = sector_count - done;
        uint32_t most = (transfer_pages != 0 ? transfer_pages : 1)
            * SECTORS_PER_PAGE;
        if (chunk > most) {
            chunk = most;
        }
        uint32_t pages = (chunk + SECTORS_PER_PAGE - 1) / SECTORS_PER_PAGE;
        size_t bytes = (size_t)chunk * SECTOR_SIZE;
        uint8_t *part = buffer + (size_t)done * SECTOR_SIZE;
        if (write) {
            for (size_t i = 0; i < bytes; i++) {
                data_page[i] = part[i];
            }
        }
        uint64_t lba = (uint64_t)first_sector + done;
        struct nvme_submission_entry entry = {
            .command = write ? NVME_IO_WRITE : NVME_IO_READ,
            .namespace_id = namespaces[disk].id,
            .prp1 = data_physical,
            /* A second page directly; more through the PRP list. */
            .prp2 = pages == 2 ? data_physical + PAGE_SIZE
                  : pages > 2 ? prp_list_physical : 0,
            .dword10 = (uint32_t)lba,
            .dword11 = (uint32_t)(lba >> 32),
            .dword12 = chunk - 1,
        };
        if (!submit_and_wait(&io_queue, &entry)) {
            return false;
        }
        if (!write) {
            for (size_t i = 0; i < bytes; i++) {
                part[i] = data_page[i];
            }
        }
        done += chunk;
    }

    if (write && volatile_write_cache && !flush_deferred) {
        return nvme_flush(disk);
    }
    return true;
}

void nvme_set_flush_deferred(bool deferred) {
    flush_deferred = deferred;
}

bool nvme_read_sectors(size_t disk, uint32_t first_sector,
                       uint8_t sector_count, void *buffer) {
    return transfer(disk, first_sector, sector_count, buffer, false);
}

bool nvme_write_sectors(size_t disk, uint32_t first_sector,
                        uint8_t sector_count, const void *buffer) {
    return transfer(disk, first_sector, sector_count, (uint8_t *)buffer,
                    true);
}
