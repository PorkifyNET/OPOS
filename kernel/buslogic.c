#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buslogic.h"
#include "io.h"
#include "memory.h"
#include "pci.h"
#include "scsi.h"

/*
 * BusLogic MultiMaster host driver (BT-958, VMware's "BusLogic" SCSI
 * controller). Register bits, adapter commands, CCB and 32-bit mailbox
 * layouts follow Linux's drivers/scsi/BusLogic.h. The driver hard-resets
 * the adapter, sets up extended (32-bit) mailboxes and runs one CCB at a
 * time, polling the incoming mailbox. All addresses given to the adapter
 * are 32-bit, so its memory stays below 4 GiB. FlashPoint adapters have no
 * mailbox firmware and are not supported.
 */

#define PAGE_SIZE 4096

#define BUSLOGIC_VENDOR_ID 0x104b
#define BUSLOGIC_MULTIMASTER 0x1040
#define BUSLOGIC_MULTIMASTER_NC 0x0140

#define REG_CONTROL 0       /* write */
#define REG_STATUS 0        /* read */
#define REG_COMMAND 1       /* write: command and parameter bytes */
#define REG_DATA_IN 1       /* read */
#define REG_INTERRUPT 2     /* read */

#define CONTROL_INTERRUPT_RESET 0x20
#define CONTROL_HARD_RESET 0x80

#define STATUS_COMMAND_INVALID 0x01
#define STATUS_DATA_IN_READY 0x04
#define STATUS_COMMAND_BUSY 0x08
#define STATUS_ADAPTER_READY 0x10
#define STATUS_DIAGNOSTIC_FAILED 0x40
#define STATUS_DIAGNOSTIC_ACTIVE 0x80

#define INTERRUPT_COMMAND_COMPLETE 0x04

#define COMMAND_EXECUTE_MAILBOX 0x02
#define COMMAND_INIT_EXTENDED_MAILBOX 0x81

#define CCB_INITIATOR 0x00
#define CCB_DATA_UNCHECKED (0u << 3)
#define CCB_DATA_IN (1u << 3)
#define CCB_DATA_OUT (2u << 3)

#define ACTION_START 0x01
#define COMPLETION_FREE 0x00
#define COMPLETION_GOOD 0x01

#define ADAPTER_NORMAL 0x00
#define ADAPTER_LINKED_COMPLETE 0x0a
#define ADAPTER_LINKED_COMPLETE_FLAG 0x0b
#define ADAPTER_DATA_UNDERRUN 0x0c
/* MultiMaster firmware reports both overrun and underrun as 0x12. */
#define ADAPTER_DATA_OVER_UNDERRUN 0x12

#define TARGET_GOOD 0x00
#define TARGET_CHECK_CONDITION 0x02

#define MAILBOXES 4
/* Memory page: outgoing mailboxes, incoming mailboxes, CCB, sense. */
#define OUTGOING_OFFSET 0
#define INCOMING_OFFSET (MAILBOXES * 8)
#define CCB_OFFSET 128
#define SENSE_OFFSET 256
#define SENSE_SIZE 32

/* Data buffer: 64 KiB of contiguous pages, 128 sectors per command. */
#define DATA_PAGES 16
#define MAX_TARGETS 16
#define POLL_LIMIT 50000000u
#define COMMAND_POLL_LIMIT 2000000u

struct outgoing_mailbox {
    uint32_t ccb;
    uint8_t reserved[3];
    uint8_t action;
} __attribute__((packed));

struct incoming_mailbox {
    uint32_t ccb;
    uint8_t adapter_status;
    uint8_t target_status;
    uint8_t reserved;
    uint8_t completion;
} __attribute__((packed));

struct ccb {
    uint8_t opcode;
    uint8_t control;        /* data direction in bits 3-4 */
    uint8_t cdb_length;
    uint8_t sense_length;
    uint32_t data_length;
    uint32_t data;
    uint8_t reserved[2];
    uint8_t adapter_status;
    uint8_t target_status;
    uint8_t target;
    uint8_t lun;
    uint8_t cdb[12];
    uint8_t reserved1[2];
    uint32_t reserved2;
    uint32_t sense;
} __attribute__((packed));

_Static_assert(sizeof(struct outgoing_mailbox) == 8, "outgoing mailbox");
_Static_assert(sizeof(struct incoming_mailbox) == 8, "incoming mailbox");
_Static_assert(sizeof(struct ccb) == 40, "BusLogic CCB");

static uint16_t io_base;
static uint8_t *memory;
static uint64_t memory_physical;
static uint64_t data_physical;
static unsigned next_outgoing;
static unsigned next_incoming;
static size_t disk_count;

static uint8_t read_register(uint16_t offset) {
    return io_in8((uint16_t)(io_base + offset));
}

static void write_register(uint16_t offset, uint8_t value) {
    io_out8((uint16_t)(io_base + offset), value);
}

static bool hard_reset(void) {
    write_register(REG_CONTROL, CONTROL_HARD_RESET);
    /* Diagnostics may finish before the first status read (emulated
       adapters), so seeing Diagnostic Active is not required. */
    for (uint32_t poll = 0; poll < 100000u; poll++) {
        if ((read_register(REG_STATUS) & STATUS_DIAGNOSTIC_ACTIVE) != 0) {
            break;
        }
        io_wait();
    }
    for (uint32_t poll = 0; poll < POLL_LIMIT; poll++) {
        uint8_t status = read_register(REG_STATUS);
        if ((status & STATUS_DIAGNOSTIC_ACTIVE) == 0
            && (status & (STATUS_DIAGNOSTIC_FAILED | STATUS_ADAPTER_READY))
                   != 0) {
            return (status & STATUS_DIAGNOSTIC_FAILED) == 0;
        }
        io_wait();
    }
    return false;
}

/* Runs an adapter command through the command/parameter register. */
static bool adapter_command(uint8_t opcode, const uint8_t *parameters,
                            size_t parameter_count) {
    uint32_t poll = 0;
    while ((read_register(REG_STATUS)
            & (STATUS_ADAPTER_READY | STATUS_COMMAND_BUSY))
           != STATUS_ADAPTER_READY) {
        if (++poll >= COMMAND_POLL_LIMIT) {
            return false;
        }
        io_wait();
    }
    write_register(REG_COMMAND, opcode);
    for (size_t i = 0; i < parameter_count; i++) {
        poll = 0;
        while ((read_register(REG_STATUS) & STATUS_COMMAND_BUSY) != 0) {
            if ((read_register(REG_INTERRUPT) & INTERRUPT_COMMAND_COMPLETE)
                != 0) {
                /* Completed early: the adapter rejected a parameter. */
                write_register(REG_CONTROL, CONTROL_INTERRUPT_RESET);
                return false;
            }
            if (++poll >= COMMAND_POLL_LIMIT) {
                return false;
            }
            io_wait();
        }
        write_register(REG_COMMAND, parameters[i]);
    }
    for (poll = 0; poll < COMMAND_POLL_LIMIT; poll++) {
        if ((read_register(REG_INTERRUPT) & INTERRUPT_COMMAND_COMPLETE) != 0) {
            break;
        }
        if ((read_register(REG_STATUS) & STATUS_DATA_IN_READY) != 0) {
            (void)read_register(REG_DATA_IN);
        }
        io_wait();
    }
    if (poll >= COMMAND_POLL_LIMIT) {
        return false;
    }
    write_register(REG_CONTROL, CONTROL_INTERRUPT_RESET);
    return (read_register(REG_STATUS) & STATUS_COMMAND_INVALID) == 0;
}

static enum scsi_result execute(uint8_t target, const uint8_t *cdb,
                                uint8_t cdb_length,
                                enum scsi_direction direction,
                                uint32_t length) {
    if (cdb_length > 12) {
        return SCSI_FAILED;
    }
    volatile struct ccb *ccb = (volatile struct ccb *)(memory + CCB_OFFSET);
    volatile uint8_t *bytes = (volatile uint8_t *)ccb;
    for (size_t i = 0; i < sizeof(*ccb); i++) {
        bytes[i] = 0;
    }
    if (direction == SCSI_DATA_NONE) {
        length = 0;
    }
    ccb->opcode = CCB_INITIATOR;
    ccb->control = direction == SCSI_DATA_IN ? CCB_DATA_IN
        : direction == SCSI_DATA_OUT ? CCB_DATA_OUT : CCB_DATA_UNCHECKED;
    ccb->cdb_length = cdb_length;
    ccb->sense_length = SENSE_SIZE;
    ccb->data_length = length;
    ccb->data = length != 0 ? (uint32_t)data_physical : 0;
    ccb->target = target;
    for (size_t i = 0; i < cdb_length; i++) {
        ccb->cdb[i] = cdb[i];
    }
    ccb->sense = (uint32_t)(memory_physical + SENSE_OFFSET);

    uint32_t ccb_physical = (uint32_t)(memory_physical + CCB_OFFSET);
    volatile struct outgoing_mailbox *outgoing =
        (volatile struct outgoing_mailbox *)(memory + OUTGOING_OFFSET)
        + next_outgoing;
    next_outgoing = (next_outgoing + 1) % MAILBOXES;
    outgoing->ccb = ccb_physical;
    __asm__ volatile ("mfence" ::: "memory");
    outgoing->action = ACTION_START;
    __asm__ volatile ("mfence" ::: "memory");
    write_register(REG_COMMAND, COMMAND_EXECUTE_MAILBOX);

    for (uint32_t poll = 0; poll < POLL_LIMIT; poll++) {
        volatile struct incoming_mailbox *incoming =
            (volatile struct incoming_mailbox *)(memory + INCOMING_OFFSET)
            + next_incoming;
        __asm__ volatile ("mfence" ::: "memory");
        uint8_t completion = incoming->completion;
        if (completion == COMPLETION_FREE) {
            io_wait();
            continue;
        }
        uint32_t done = incoming->ccb;
        uint8_t adapter_status = incoming->adapter_status;
        uint8_t target_status = incoming->target_status;
        incoming->completion = COMPLETION_FREE;
        next_incoming = (next_incoming + 1) % MAILBOXES;
        write_register(REG_CONTROL, CONTROL_INTERRUPT_RESET);
        if (done != ccb_physical) {
            continue;
        }
        if (completion == COMPLETION_GOOD) {
            return SCSI_OK;
        }
        bool adapter_ok = adapter_status == ADAPTER_NORMAL
            || adapter_status == ADAPTER_LINKED_COMPLETE
            || adapter_status == ADAPTER_LINKED_COMPLETE_FLAG
            || adapter_status == ADAPTER_DATA_UNDERRUN
            || adapter_status == ADAPTER_DATA_OVER_UNDERRUN;
        if (adapter_ok && target_status == TARGET_GOOD) {
            return SCSI_OK;
        }
        if (adapter_ok && target_status == TARGET_CHECK_CONDITION) {
            return SCSI_CHECK_CONDITION;
        }
        return SCSI_FAILED;
    }
    return SCSI_FAILED;
}

static struct scsi_host host = {
    .name = "BUSLOGIC",
    .buffer_size = DATA_PAGES * PAGE_SIZE,
    .execute = execute,
};

static uint16_t find_io_base(const struct pci_device *device) {
    for (uint8_t bar = 0; bar < 6; bar++) {
        bool io_space = false;
        uint64_t address = pci_bar_address(device, bar, &io_space);
        if (address != 0 && io_space && address <= 0xfff0) {
            return (uint16_t)address;
        }
    }
    return 0;
}

bool buslogic_initialize(void) {
    struct pci_device device;
    if (io_base != 0
        || (!pci_find_device(BUSLOGIC_VENDOR_ID, BUSLOGIC_MULTIMASTER,
                             &device)
            && !pci_find_device(BUSLOGIC_VENDOR_ID, BUSLOGIC_MULTIMASTER_NC,
                                &device))) {
        return false;
    }
    pci_enable_device(&device);
    io_base = find_io_base(&device);
    if (io_base == 0) {
        return false;
    }

    memory_physical = physical_page_alloc();
    data_physical = physical_pages_alloc_contiguous(DATA_PAGES);
    if (memory_physical == 0 || memory_physical >= 0x100000000ULL
        || data_physical == 0
        || data_physical + DATA_PAGES * PAGE_SIZE > 0x100000000ULL) {
        return false;
    }
    memory = physical_page_to_hhdm(memory_physical);
    host.buffer = physical_page_to_hhdm(data_physical);
    if (memory == NULL || host.buffer == NULL) {
        return false;
    }
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        memory[i] = 0;
    }

    if (!hard_reset()) {
        return false;
    }
    uint32_t mailboxes = (uint32_t)memory_physical + OUTGOING_OFFSET;
    uint8_t parameters[5] = {
        MAILBOXES,
        (uint8_t)mailboxes, (uint8_t)(mailboxes >> 8),
        (uint8_t)(mailboxes >> 16), (uint8_t)(mailboxes >> 24),
    };
    if (!adapter_command(COMMAND_INIT_EXTENDED_MAILBOX, parameters,
                         sizeof(parameters))) {
        return false;
    }
    next_outgoing = 0;
    next_incoming = 0;

    disk_count = scsi_scan_host(&host, MAX_TARGETS);
    return disk_count != 0;
}

size_t buslogic_disk_count(void) {
    return disk_count;
}
