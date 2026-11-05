#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "memory.h"
#include "mpt.h"
#include "pci.h"
#include "scsi.h"

/*
 * LSI Fusion-MPT host driver: the message interface shared by the LSI
 * 53C1030 (VMware "LSI Logic Parallel") and SAS1068 (VMware "LSI Logic
 * SAS", QEMU mptsas1068). Message layouts follow the MPI 1.5 headers
 * (QEMU hw/scsi/mpi.h). Setup messages go through the doorbell handshake;
 * SCSI I/O frames go through the request FIFO and the driver polls the
 * reply FIFO with interrupts masked. Registers are reached through the
 * I/O BAR.
 */

#define PAGE_SIZE 4096

#define LSI_VENDOR_ID 0x1000

#define REG_DOORBELL 0x00
#define REG_INTR_STATUS 0x30
#define REG_INTR_MASK 0x34
#define REG_REQUEST_FIFO 0x40
#define REG_REPLY_FIFO 0x44

#define DOORBELL_STATE_MASK 0xf0000000u
#define DOORBELL_STATE_RESET 0x00000000u
#define DOORBELL_STATE_READY 0x10000000u
#define DOORBELL_STATE_OPERATIONAL 0x20000000u
#define DOORBELL_STATE_FAULT 0x40000000u
#define DOORBELL_ACTIVE 0x08000000u
#define DOORBELL_FUNCTION_SHIFT 24
#define DOORBELL_DWORDS_SHIFT 16

#define INTR_DOORBELL 0x00000001u
#define INTR_REPLY 0x00000008u
#define INTR_IOP_DOORBELL 0x80000000u

#define FUNCTION_SCSI_IO 0x00
#define FUNCTION_IOC_INIT 0x02
#define FUNCTION_IOC_FACTS 0x03
#define FUNCTION_PORT_ENABLE 0x06
#define FUNCTION_HANDSHAKE 0x42
#define FUNCTION_MESSAGE_UNIT_RESET 0x40

#define WHOINIT_HOST_DRIVER 0x04

#define IOCSTATUS_MASK 0x7fff
#define IOCSTATUS_SUCCESS 0x0000
#define IOCSTATUS_SCSI_RECOVERED_ERROR 0x0040
#define IOCSTATUS_SCSI_DATA_UNDERRUN 0x0045

#define SCSI_STATUS_GOOD 0x00
#define SCSI_STATUS_CHECK 0x02

#define CONTROL_WRITE 0x01000000u
#define CONTROL_READ 0x02000000u

/* Last element, end of buffer, simple, end of list, 64-bit address. */
#define SGE_FLAGS_SIMPLE_LAST 0xd3000000u
#define SGE_FLAGS_HOST_TO_IOC 0x04000000u

#define ADDRESS_REPLY 0x80000000u
#define REPLY_FIFO_EMPTY 0xffffffffu

/* Frames page: request frame, sense buffer, then reply frames. */
#define REQUEST_OFFSET 0
#define SENSE_OFFSET 128
#define SENSE_SIZE 64
#define REPLY_OFFSET 256
#define REPLY_FRAME_SIZE 128
#define REPLY_FRAMES 16

/* Data buffer: 64 KiB of contiguous pages, 128 sectors per command. */
#define DATA_PAGES 16
#define MAX_TARGETS 16
#define POLL_LIMIT 50000000u
#define HANDSHAKE_POLL_LIMIT 5000000u

struct ioc_facts_reply {
    uint16_t msg_version;
    uint8_t msg_length;
    uint8_t function;
    uint16_t header_version;
    uint8_t ioc_number;
    uint8_t msg_flags;
    uint32_t msg_context;
    uint16_t ioc_exceptions;
    uint16_t ioc_status;
    uint32_t ioc_log_info;
    uint8_t max_chain_depth;
    uint8_t who_init;
    uint8_t block_size;
    uint8_t flags;
    uint16_t reply_queue_depth;
    uint16_t request_frame_size;
    uint16_t obsolete_fw_version;
    uint16_t product_id;
    uint32_t current_host_mfa_high;
    uint16_t global_credits;
    uint8_t number_of_ports;
    uint8_t event_state;
    uint32_t current_sense_high;
    uint16_t current_reply_frame_size;
    uint8_t max_devices;
    uint8_t max_buses;
    uint32_t fw_image_size;
    uint32_t ioc_capabilities;
    uint32_t fw_version;
    uint16_t high_priority_queue_depth;
    uint16_t reserved;
    uint32_t host_page_buffer_sge[3];
    uint32_t reply_fifo_host_signaling;
} __attribute__((packed));

struct ioc_init_request {
    uint8_t who_init;
    uint8_t reserved;
    uint8_t chain_offset;
    uint8_t function;
    uint8_t flags;
    uint8_t max_devices;
    uint8_t max_buses;
    uint8_t msg_flags;
    uint32_t msg_context;
    uint16_t reply_frame_size;
    uint16_t reserved1;
    uint32_t host_mfa_high;
    uint32_t sense_high;
    uint32_t reply_fifo_host_signaling;
    uint32_t host_page_buffer_sge[3];
    uint16_t msg_version;
    uint16_t header_version;
} __attribute__((packed));

/* Common header of every reply: IOCStatus at 0x0e. */
struct default_reply {
    uint8_t reserved[2];
    uint8_t msg_length;
    uint8_t function;
    uint8_t reserved1[4];
    uint32_t msg_context;
    uint16_t reserved2;
    uint16_t ioc_status;
    uint32_t ioc_log_info;
} __attribute__((packed));

struct port_enable_request {
    uint8_t reserved[2];
    uint8_t chain_offset;
    uint8_t function;
    uint8_t reserved1[2];
    uint8_t port_number;
    uint8_t msg_flags;
    uint32_t msg_context;
} __attribute__((packed));

struct ioc_facts_request {
    uint8_t reserved[2];
    uint8_t chain_offset;
    uint8_t function;
    uint8_t reserved1[3];
    uint8_t msg_flags;
    uint32_t msg_context;
} __attribute__((packed));

struct scsi_io_request {
    uint8_t target_id;
    uint8_t bus;
    uint8_t chain_offset;
    uint8_t function;
    uint8_t cdb_length;
    uint8_t sense_buffer_length;
    uint8_t reserved;
    uint8_t msg_flags;
    uint32_t msg_context;
    uint8_t lun[8];
    uint32_t control;
    uint8_t cdb[16];
    uint32_t data_length;
    uint32_t sense_buffer_low;
    /* One 64-bit simple scatter/gather element. */
    uint32_t sge_flags_length;
    uint32_t sge_address_low;
    uint32_t sge_address_high;
} __attribute__((packed));

struct scsi_io_reply {
    uint8_t target_id;
    uint8_t bus;
    uint8_t msg_length;
    uint8_t function;
    uint8_t cdb_length;
    uint8_t sense_buffer_length;
    uint8_t reserved;
    uint8_t msg_flags;
    uint32_t msg_context;
    uint8_t scsi_status;
    uint8_t scsi_state;
    uint16_t ioc_status;
    uint32_t ioc_log_info;
    uint32_t transfer_count;
    uint32_t sense_count;
    uint32_t response_info;
    uint16_t task_tag;
    uint16_t reserved1;
} __attribute__((packed));

_Static_assert(sizeof(struct ioc_facts_reply) == 0x50, "IOC Facts reply");
_Static_assert(sizeof(struct ioc_init_request) == 0x2c, "IOC Init request");
_Static_assert(sizeof(struct scsi_io_request) == 0x3c, "SCSI IO request");
_Static_assert(sizeof(struct scsi_io_reply) == 0x24, "SCSI IO reply");

static uint16_t io_base;
static uint8_t *frames;
static uint64_t frames_physical;
static uint64_t data_physical;
static uint32_t next_context = 1;
static size_t disk_count;

static uint32_t read32(uint16_t offset) {
    return io_in32((uint16_t)(io_base + offset));
}

static void write32(uint16_t offset, uint32_t value) {
    io_out32((uint16_t)(io_base + offset), value);
}

static uint32_t ioc_state(void) {
    return read32(REG_DOORBELL) & DOORBELL_STATE_MASK;
}

static bool wait_state(uint32_t wanted) {
    for (uint32_t poll = 0; poll < POLL_LIMIT; poll++) {
        if (ioc_state() == wanted) {
            return true;
        }
        io_wait();
    }
    return false;
}

static bool wait_doorbell_interrupt(void) {
    for (uint32_t poll = 0; poll < HANDSHAKE_POLL_LIMIT; poll++) {
        if ((read32(REG_INTR_STATUS) & INTR_DOORBELL) != 0) {
            return true;
        }
        io_wait();
    }
    return false;
}

/* The IOC sets IOP_DOORBELL while it has not taken the last dword yet. */
static bool wait_doorbell_ack(void) {
    for (uint32_t poll = 0; poll < HANDSHAKE_POLL_LIMIT; poll++) {
        if ((read32(REG_INTR_STATUS) & INTR_IOP_DOORBELL) == 0) {
            return true;
        }
        io_wait();
    }
    return false;
}

/*
 * Sends `request` through the doorbell and reads the reply 16 bits at a
 * time into `reply` (at most `reply_size` bytes; extra words are dropped).
 * Returns false on a timeout or a failing IOCStatus.
 */
static bool handshake(const void *request, size_t request_size, void *reply,
                      size_t reply_size) {
    if ((read32(REG_DOORBELL) & DOORBELL_ACTIVE) != 0) {
        return false;
    }
    write32(REG_INTR_STATUS, 0);
    write32(REG_DOORBELL,
            ((uint32_t)FUNCTION_HANDSHAKE << DOORBELL_FUNCTION_SHIFT)
                | ((uint32_t)(request_size / 4) << DOORBELL_DWORDS_SHIFT));
    if (!wait_doorbell_interrupt()) {
        return false;
    }
    write32(REG_INTR_STATUS, 0);
    if (!wait_doorbell_ack()) {
        return false;
    }
    const uint32_t *words = request;
    for (size_t i = 0; i < request_size / 4; i++) {
        write32(REG_DOORBELL, words[i]);
        if (!wait_doorbell_ack()) {
            return false;
        }
    }

    uint16_t *halves = reply;
    size_t reply_halves = reply_size / 2;
    for (size_t i = 0; i < reply_halves; i++) {
        halves[i] = 0;
    }
    /* Word 1 holds MsgLength (in dwords) in its low byte. */
    size_t total = 2;
    for (size_t i = 0; i < total; i++) {
        if (!wait_doorbell_interrupt()) {
            return false;
        }
        uint16_t half = (uint16_t)read32(REG_DOORBELL);
        write32(REG_INTR_STATUS, 0);
        if (i < reply_halves) {
            halves[i] = half;
        }
        if (i == 1) {
            total = (size_t)(half & 0xff) * 2;
            if (total < 2) {
                total = 2;
            }
        }
    }
    /* The IOC raises one last doorbell interrupt once the reply is done. */
    (void)wait_doorbell_interrupt();
    write32(REG_INTR_STATUS, 0);

    if (reply_size >= sizeof(struct default_reply)) {
        const struct default_reply *header = reply;
        if ((header->ioc_status & IOCSTATUS_MASK) != IOCSTATUS_SUCCESS) {
            return false;
        }
    }
    return true;
}

static bool reset_ioc(void) {
    uint32_t current = ioc_state();
    if (current == DOORBELL_STATE_RESET) {
        if (wait_state(DOORBELL_STATE_READY)) {
            return true;
        }
        current = ioc_state();
    }
    if (current != DOORBELL_STATE_READY
        || (read32(REG_DOORBELL) & DOORBELL_ACTIVE) != 0) {
        write32(REG_DOORBELL, (uint32_t)FUNCTION_MESSAGE_UNIT_RESET
                                  << DOORBELL_FUNCTION_SHIFT);
        (void)wait_doorbell_ack();
        if (!wait_state(DOORBELL_STATE_READY)) {
            return false;
        }
    }
    return true;
}

static enum scsi_result execute(uint8_t target, const uint8_t *cdb,
                                uint8_t cdb_length,
                                enum scsi_direction direction,
                                uint32_t length) {
    volatile struct scsi_io_request *request =
        (volatile struct scsi_io_request *)(frames + REQUEST_OFFSET);
    volatile uint8_t *bytes = (volatile uint8_t *)request;
    for (size_t i = 0; i < sizeof(*request); i++) {
        bytes[i] = 0;
    }
    uint32_t context = next_context++ & 0x7fffffffu;
    if (context == 0) {
        context = next_context++;
    }
    if (direction == SCSI_DATA_NONE) {
        length = 0;
    }
    request->target_id = target;
    request->function = FUNCTION_SCSI_IO;
    request->cdb_length = cdb_length;
    request->sense_buffer_length = SENSE_SIZE;
    request->msg_context = context;
    request->control = direction == SCSI_DATA_IN ? CONTROL_READ
        : direction == SCSI_DATA_OUT ? CONTROL_WRITE : 0;
    for (size_t i = 0; i < cdb_length && i < 16; i++) {
        request->cdb[i] = cdb[i];
    }
    request->data_length = length;
    request->sense_buffer_low = (uint32_t)(frames_physical + SENSE_OFFSET);
    request->sge_flags_length = SGE_FLAGS_SIMPLE_LAST | length
        | (direction == SCSI_DATA_OUT ? SGE_FLAGS_HOST_TO_IOC : 0);
    request->sge_address_low = length != 0 ? (uint32_t)data_physical : 0;
    request->sge_address_high =
        length != 0 ? (uint32_t)(data_physical >> 32) : 0;
    __asm__ volatile ("mfence" ::: "memory");
    write32(REG_REQUEST_FIFO, (uint32_t)(frames_physical + REQUEST_OFFSET));

    for (uint32_t poll = 0; poll < POLL_LIMIT; poll++) {
        uint32_t entry = read32(REG_REPLY_FIFO);
        if (entry == REPLY_FIFO_EMPTY) {
            io_wait();
            continue;
        }
        if ((entry & ADDRESS_REPLY) == 0) {
            /* Context reply: the command finished without error. */
            if (entry == context) {
                write32(REG_INTR_STATUS, 0);
                return SCSI_OK;
            }
            continue;
        }
        /* Address reply: the frame address shifted right by one. */
        uint32_t frame = entry << 1;
        uint32_t offset = frame - (uint32_t)frames_physical;
        if (offset < REPLY_OFFSET
            || offset + REPLY_FRAME_SIZE > PAGE_SIZE) {
            continue;
        }
        __asm__ volatile ("mfence" ::: "memory");
        const volatile struct scsi_io_reply *reply =
            (const volatile struct scsi_io_reply *)(frames + offset);
        uint32_t reply_context = reply->msg_context;
        uint8_t scsi_status = reply->scsi_status;
        uint16_t ioc_status = reply->ioc_status & IOCSTATUS_MASK;
        uint8_t function = reply->function;
        /* Hand the frame back to the IOC. */
        write32(REG_REPLY_FIFO, frame);
        if (reply_context != context || function != FUNCTION_SCSI_IO) {
            continue;
        }
        write32(REG_INTR_STATUS, 0);
        bool ioc_ok = ioc_status == IOCSTATUS_SUCCESS
            || ioc_status == IOCSTATUS_SCSI_RECOVERED_ERROR
            || ioc_status == IOCSTATUS_SCSI_DATA_UNDERRUN;
        if (ioc_ok && scsi_status == SCSI_STATUS_GOOD) {
            return SCSI_OK;
        }
        if (scsi_status == SCSI_STATUS_CHECK) {
            return SCSI_CHECK_CONDITION;
        }
        return SCSI_FAILED;
    }
    return SCSI_FAILED;
}

static struct scsi_host host = {
    .name = "LSI SCSI",
    .buffer_size = DATA_PAGES * PAGE_SIZE,
    .execute = execute,
};

static const uint16_t device_ids[] = {
    0x0030, /* 53C1030: VMware "LSI Logic Parallel" */
    0x0054, /* SAS1068: VMware "LSI Logic SAS", QEMU mptsas1068 */
    0x0058, /* SAS1068E */
    0x0050, /* SAS1064 */
    0x0056, /* SAS1064E */
};

static bool find_controller(struct pci_device *device) {
    for (size_t i = 0; i < sizeof(device_ids) / sizeof(device_ids[0]); i++) {
        if (pci_find_device(LSI_VENDOR_ID, device_ids[i], device)) {
            host.name = device_ids[i] == 0x0030 ? "LSI SCSI" : "LSI SAS";
            return true;
        }
    }
    return false;
}

static uint16_t find_io_base(const struct pci_device *device) {
    for (uint8_t bar = 0; bar < 6; bar++) {
        bool io_space = false;
        uint64_t address = pci_bar_address(device, bar, &io_space);
        if (address != 0 && io_space && address <= 0xff00) {
            return (uint16_t)address;
        }
    }
    return 0;
}

bool mpt_initialize(void) {
    struct pci_device device;
    if (io_base != 0 || !find_controller(&device)) {
        return false;
    }
    pci_enable_device(&device);
    io_base = find_io_base(&device);
    if (io_base == 0) {
        return false;
    }

    /* Request frames, sense and reply frames share the high 32 address
       bits the IOC is told about; keep them below 4 GiB. */
    frames_physical = physical_page_alloc();
    data_physical = physical_pages_alloc_contiguous(DATA_PAGES);
    if (frames_physical == 0 || frames_physical >= 0x100000000ULL
        || data_physical == 0) {
        return false;
    }
    frames = physical_page_to_hhdm(frames_physical);
    host.buffer = physical_page_to_hhdm(data_physical);
    if (frames == NULL || host.buffer == NULL) {
        return false;
    }
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        frames[i] = 0;
    }

    write32(REG_INTR_MASK, INTR_DOORBELL | INTR_REPLY);
    if (!reset_ioc()) {
        return false;
    }
    write32(REG_INTR_MASK, INTR_DOORBELL | INTR_REPLY);
    write32(REG_INTR_STATUS, 0);

    struct ioc_facts_request facts_request = {.function = FUNCTION_IOC_FACTS};
    struct ioc_facts_reply facts;
    if (!handshake(&facts_request, sizeof(facts_request), &facts,
                   sizeof(facts))) {
        return false;
    }

    struct ioc_init_request init = {
        .who_init = WHOINIT_HOST_DRIVER,
        .function = FUNCTION_IOC_INIT,
        .max_devices = facts.max_devices,
        .max_buses = facts.max_buses != 0 ? facts.max_buses : 1,
        .reply_frame_size = REPLY_FRAME_SIZE,
        .msg_version = facts.msg_version,
        .header_version = facts.header_version,
    };
    struct default_reply init_reply;
    if (!handshake(&init, sizeof(init), &init_reply, sizeof(init_reply))
        || !wait_state(DOORBELL_STATE_OPERATIONAL)) {
        return false;
    }

    unsigned reply_frames = REPLY_FRAMES;
    if (facts.reply_queue_depth != 0 && facts.reply_queue_depth < reply_frames) {
        reply_frames = facts.reply_queue_depth;
    }
    for (unsigned i = 0; i < reply_frames; i++) {
        write32(REG_REPLY_FIFO, (uint32_t)(frames_physical + REPLY_OFFSET
                                           + i * REPLY_FRAME_SIZE));
    }

    struct port_enable_request enable = {.function = FUNCTION_PORT_ENABLE};
    struct default_reply enable_reply;
    if (!handshake(&enable, sizeof(enable), &enable_reply,
                   sizeof(enable_reply))) {
        return false;
    }

    unsigned targets = facts.max_devices;
    if (targets == 0 || targets > MAX_TARGETS) {
        targets = MAX_TARGETS;
    }
    disk_count = scsi_scan_host(&host, targets);
    return disk_count != 0;
}

size_t mpt_disk_count(void) {
    return disk_count;
}
