#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "memory.h"
#include "paging.h"
#include "pci.h"
#include "pvscsi.h"
#include "scsi.h"

/*
 * Register layout and ring formats follow VMware's PVSCSI interface as
 * published in Linux's drivers/scsi/vmw_pvscsi.h. One request ring page
 * (32 entries) and one completion ring page (128 entries); the driver keeps
 * one command outstanding and polls the completion ring. Disk discovery and
 * sector I/O live in scsi.c.
 */

#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)

#define PVSCSI_VENDOR_ID 0x15ad
#define PVSCSI_DEVICE_ID 0x07c0
#define PVSCSI_MMIO_BASE 0xffffc00000010000ULL
#define PVSCSI_MMIO_PAGES 8
#define PVSCSI_TARGETS 16

#define REG_COMMAND 0x0
#define REG_COMMAND_DATA 0x4
#define REG_COMMAND_STATUS 0x8
#define REG_INTR_STATUS 0x100c
#define REG_INTR_MASK 0x2010
#define REG_KICK_NON_RW_IO 0x3014
#define REG_KICK_RW_IO 0x4018

#define CMD_ADAPTER_RESET 1
#define CMD_SETUP_RINGS 3

#define FLAG_DIR_NONE (1u << 2)
#define FLAG_DIR_TOHOST (1u << 3)
#define FLAG_DIR_TODEVICE (1u << 4)

#define BTSTAT_SUCCESS 0x00
#define BTSTAT_LINKED_COMMAND_COMPLETED 0x0a
#define BTSTAT_LINKED_COMMAND_COMPLETED_WITH_FLAG 0x0b
#define BTSTAT_DATA_UNDERRUN 0x0c
#define SDSTAT_GOOD 0x00
#define SDSTAT_CHECK 0x02

#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2a

#define REQ_RING_LOG2 5   /* 4096 / 128-byte entries */
#define CMP_RING_LOG2 7   /* 4096 / 32-byte entries */
/* Data buffer: 64 KiB of contiguous pages, 128 sectors per command. */
#define DATA_PAGES 16
#define SENSE_SIZE 64
#define POLL_LIMIT 50000000u

struct rings_state {
    uint32_t req_prod;
    uint32_t req_cons;
    uint32_t req_entries_log2;
    uint32_t cmp_prod;
    uint32_t cmp_cons;
    uint32_t cmp_entries_log2;
    uint32_t req_call_threshold;
    uint8_t pad[100];
    uint32_t msg_prod;
    uint32_t msg_cons;
    uint32_t msg_entries_log2;
} __attribute__((packed));

struct request_desc {
    uint64_t context;
    uint64_t data_addr;
    uint64_t data_len;
    uint64_t sense_addr;
    uint32_t sense_len;
    uint32_t flags;
    uint8_t cdb[16];
    uint8_t cdb_len;
    uint8_t lun[8];
    uint8_t tag;
    uint8_t bus;
    uint8_t target;
    uint16_t vcpu_hint;
    uint8_t unused[58];
} __attribute__((packed));

struct completion_desc {
    uint64_t context;
    uint64_t data_len;
    uint32_t sense_len;
    uint16_t host_status;
    uint16_t scsi_status;
    uint32_t pad[2];
} __attribute__((packed));

struct setup_rings {
    uint32_t req_pages;
    uint32_t cmp_pages;
    uint64_t state_ppn;
    uint64_t req_ppns[32];
    uint64_t cmp_ppns[32];
} __attribute__((packed));

_Static_assert(sizeof(struct request_desc) == 128, "PVSCSI request is 128 bytes");
_Static_assert(sizeof(struct completion_desc) == 32,
               "PVSCSI completion is 32 bytes");

static volatile uint8_t *registers;
static volatile struct rings_state *state;
static volatile struct request_desc *request_ring;
static volatile struct completion_desc *completion_ring;
static uint64_t state_physical;
static uint64_t request_physical;
static uint64_t completion_physical;
static uint64_t data_physical;
static uint64_t sense_physical;
static uint64_t next_context = 1;
static size_t disk_count;

static uint32_t read32(size_t offset) {
    return *(volatile uint32_t *)(registers + offset);
}

static void write32(size_t offset, uint32_t value) {
    *(volatile uint32_t *)(registers + offset) = value;
}

static void barrier(void) {
    __asm__ volatile ("mfence" ::: "memory");
}

static void *zeroed_page(uint64_t *physical) {
    *physical = physical_page_alloc();
    if (*physical == 0) {
        return NULL;
    }
    uint8_t *page = physical_page_to_hhdm(*physical);
    for (size_t i = 0; page != NULL && i < PAGE_SIZE; i++) {
        page[i] = 0;
    }
    return page;
}

/* Writes a command and its descriptor dwords to the command registers. */
static uint32_t device_command(uint32_t command, const void *descriptor,
                               size_t length) {
    write32(REG_COMMAND, command);
    const uint32_t *words = descriptor;
    for (size_t i = 0; i < length / 4; i++) {
        write32(REG_COMMAND_DATA, words[i]);
    }
    return read32(REG_COMMAND_STATUS);
}

/* Runs one SCSI command against `target`, LUN 0, using the data buffer. */
static enum scsi_result execute(uint8_t target, const uint8_t *cdb,
                                uint8_t cdb_length,
                                enum scsi_direction direction,
                                uint32_t length) {
    uint32_t request_mask = (1u << REQ_RING_LOG2) - 1;
    uint32_t completion_mask = (1u << CMP_RING_LOG2) - 1;
    uint32_t producer = state->req_prod;
    volatile struct request_desc *request =
        &request_ring[producer & request_mask];
    volatile uint8_t *bytes = (volatile uint8_t *)request;
    for (size_t i = 0; i < sizeof(*request); i++) {
        bytes[i] = 0;
    }
    uint64_t context = next_context++;
    request->context = context;
    request->data_addr = direction == SCSI_DATA_NONE ? 0 : data_physical;
    request->data_len = direction == SCSI_DATA_NONE ? 0 : length;
    request->sense_addr = sense_physical;
    request->sense_len = SENSE_SIZE;
    request->flags = direction == SCSI_DATA_IN ? FLAG_DIR_TOHOST
        : direction == SCSI_DATA_OUT ? FLAG_DIR_TODEVICE : FLAG_DIR_NONE;
    for (size_t i = 0; i < cdb_length && i < 16; i++) {
        request->cdb[i] = cdb[i];
    }
    request->cdb_len = cdb_length;
    request->target = target;
    barrier();
    state->req_prod = producer + 1;
    barrier();
    bool read_write = cdb[0] == SCSI_READ_10 || cdb[0] == SCSI_WRITE_10;
    write32(read_write ? REG_KICK_RW_IO : REG_KICK_NON_RW_IO, 0);

    for (uint32_t poll = 0; poll < POLL_LIMIT; poll++) {
        barrier();
        uint32_t consumer = state->cmp_cons;
        if (consumer == state->cmp_prod) {
            io_wait();
            continue;
        }
        volatile struct completion_desc *completion =
            &completion_ring[consumer & completion_mask];
        uint64_t done = completion->context;
        uint16_t host = completion->host_status;
        uint16_t scsi = completion->scsi_status;
        state->cmp_cons = consumer + 1;
        barrier();
        /* Clear any interrupt status the device latched. */
        write32(REG_INTR_STATUS, read32(REG_INTR_STATUS));
        if (done != context) {
            continue;
        }
        bool host_ok = host == BTSTAT_SUCCESS
            || host == BTSTAT_LINKED_COMMAND_COMPLETED
            || host == BTSTAT_LINKED_COMMAND_COMPLETED_WITH_FLAG
            || host == BTSTAT_DATA_UNDERRUN;
        if (host_ok && scsi == SDSTAT_GOOD) {
            return SCSI_OK;
        }
        return host_ok && scsi == SDSTAT_CHECK ? SCSI_CHECK_CONDITION
                                               : SCSI_FAILED;
    }
    return SCSI_FAILED;
}

static struct scsi_host host = {
    .name = "PVSCSI",
    .buffer_size = DATA_PAGES * PAGE_SIZE,
    .execute = execute,
};

/* The register window is the first memory BAR; VMware puts an I/O BAR
   first, QEMU only has the memory BAR. */
static bool map_registers(const struct pci_device *device) {
    uint64_t physical = 0;
    for (uint8_t bar = 0; bar < 6 && physical == 0; bar++) {
        bool io_space = false;
        uint64_t address = pci_bar_address(device, bar, &io_space);
        if (address != 0 && !io_space) {
            physical = address;
        }
    }
    if (physical == 0 || (physical & PAGE_MASK) != 0) {
        return false;
    }
    for (size_t page = 0; page < PVSCSI_MMIO_PAGES; page++) {
        if (!paging_map_page(PVSCSI_MMIO_BASE + page * PAGE_SIZE,
                             physical + page * PAGE_SIZE,
                             PAGING_WRITABLE | PAGING_WRITE_THROUGH
                                 | PAGING_CACHE_DISABLE
                                 | PAGING_NO_EXECUTE)) {
            for (size_t mapped = 0; mapped < page; mapped++) {
                (void)paging_unmap_page(PVSCSI_MMIO_BASE + mapped * PAGE_SIZE,
                                        NULL);
            }
            return false;
        }
    }
    registers = (volatile uint8_t *)PVSCSI_MMIO_BASE;
    return true;
}

bool pvscsi_initialize(void) {
    struct pci_device device;
    if (registers != NULL
        || !pci_find_device(PVSCSI_VENDOR_ID, PVSCSI_DEVICE_ID, &device)) {
        return false;
    }
    pci_enable_device(&device);
    if (!map_registers(&device)) {
        return false;
    }
    void *sense;
    state = zeroed_page(&state_physical);
    request_ring = zeroed_page(&request_physical);
    completion_ring = zeroed_page(&completion_physical);
    sense = zeroed_page(&sense_physical);
    data_physical = physical_pages_alloc_contiguous(DATA_PAGES);
    host.buffer = data_physical != 0 ? physical_page_to_hhdm(data_physical)
                                     : NULL;
    if (state == NULL || request_ring == NULL || completion_ring == NULL
        || sense == NULL || host.buffer == NULL) {
        return false;
    }

    (void)device_command(CMD_ADAPTER_RESET, NULL, 0);
    write32(REG_INTR_MASK, 0);
    write32(REG_INTR_STATUS, read32(REG_INTR_STATUS));

    static struct setup_rings setup;
    setup = (struct setup_rings){0};
    setup.req_pages = 1;
    setup.cmp_pages = 1;
    setup.state_ppn = state_physical / PAGE_SIZE;
    setup.req_ppns[0] = request_physical / PAGE_SIZE;
    setup.cmp_ppns[0] = completion_physical / PAGE_SIZE;
    (void)device_command(CMD_SETUP_RINGS, &setup, sizeof(setup));
    if (state->req_entries_log2 == 0) {
        state->req_entries_log2 = REQ_RING_LOG2;
        state->cmp_entries_log2 = CMP_RING_LOG2;
    }

    disk_count = scsi_scan_host(&host, PVSCSI_TARGETS);
    return disk_count != 0;
}

size_t pvscsi_disk_count(void) {
    return disk_count;
}
