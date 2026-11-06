#include "usb_storage.h"

#include "scsi.h"
#include "xhci.h"

/*
 * Bulk-Only Transport: every SCSI command is a 31-byte Command Block
 * Wrapper on the bulk OUT endpoint, an optional data stage, and a 13-byte
 * Command Status Wrapper on the bulk IN endpoint.
 */

#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u
#define CBW_LENGTH 31
#define CSW_LENGTH 13
#define CSW_PASSED 0
#define CSW_FAILED 1
#define REQUEST_MASS_STORAGE_RESET 0xff
#define SCSI_REQUEST_SENSE 0x03
#define SENSE_LENGTH 18

struct storage_unit {
    bool used;
    size_t device;      /* xHCI device index */
    uint8_t interface;
};

static struct storage_unit units[USB_STORAGE_MAX];
static uint32_t next_tag = 1;

static enum scsi_result execute(uint8_t target, const uint8_t *cdb,
                                uint8_t cdb_length,
                                enum scsi_direction direction,
                                uint32_t length);
static void location(uint8_t target, char *buffer, size_t capacity);

static struct scsi_host host = {
    .name = "USB",
    .buffer_size = XHCI_TRANSFER_BUFFER_SIZE,
    .execute = execute,
    .location = location,
    .hotplug = true,
};

static void put32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *at) {
    return at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16)
        | ((uint32_t)at[3] << 24);
}

/* Bulk-only reset recovery: class reset, then clear both halts. */
static void reset_recovery(const struct storage_unit *unit) {
    (void)xhci_control_no_data(unit->device, 0x21, REQUEST_MASS_STORAGE_RESET,
                               0, unit->interface);
    (void)xhci_clear_halt(unit->device, true);
    (void)xhci_clear_halt(unit->device, false);
}

/* One BOT command. The data stage uses the transfer buffer, or the small
   buffer when `small` (it then holds CBW, data and CSW in turn).
   Returns the CSW status, or -1 when the transport failed. */
static int transport(const struct storage_unit *unit, const uint8_t *cdb,
                     uint8_t cdb_length, enum scsi_direction direction,
                     uint32_t length, bool small) {
    uint8_t *block = xhci_small_buffer();
    uint32_t tag = next_tag++;
    for (size_t i = 0; i < CBW_LENGTH; i++) {
        block[i] = 0;
    }
    put32(block, CBW_SIGNATURE);
    put32(block + 4, tag);
    put32(block + 8, direction == SCSI_DATA_NONE ? 0 : length);
    block[12] = direction == SCSI_DATA_IN ? 0x80 : 0x00;
    block[13] = 0; /* LUN 0 */
    block[14] = cdb_length;
    for (uint8_t i = 0; i < cdb_length && i < 16; i++) {
        block[15 + i] = cdb[i];
    }
    uint32_t done;
    enum usb_result result =
        xhci_bulk(unit->device, false, true, CBW_LENGTH, &done);
    if (result != USB_OK) {
        reset_recovery(unit);
        return -1;
    }
    if (direction != SCSI_DATA_NONE && length != 0) {
        bool in = direction == SCSI_DATA_IN;
        result = xhci_bulk(unit->device, in, small, length, &done);
        if (result == USB_STALL) {
            (void)xhci_clear_halt(unit->device, in);
        } else if (result != USB_OK) {
            reset_recovery(unit);
            return -1;
        }
    }
    /* The CSW reuses the small buffer: CBW and any small data are done. */
    uint8_t status[CSW_LENGTH];
    for (unsigned attempt = 0; attempt < 2; attempt++) {
        result = xhci_bulk(unit->device, true, true, CSW_LENGTH, &done);
        if (result == USB_STALL) {
            (void)xhci_clear_halt(unit->device, true);
            continue;
        }
        break;
    }
    if (result != USB_OK || done != CSW_LENGTH) {
        reset_recovery(unit);
        return -1;
    }
    for (size_t i = 0; i < CSW_LENGTH; i++) {
        status[i] = block[i];
    }
    if (get32(status) != CSW_SIGNATURE || get32(status + 4) != tag
        || status[12] > CSW_FAILED) {
        reset_recovery(unit);
        return -1;
    }
    return status[12];
}

static enum scsi_result execute(uint8_t target, const uint8_t *cdb,
                                uint8_t cdb_length,
                                enum scsi_direction direction,
                                uint32_t length) {
    if (target >= USB_STORAGE_MAX || !units[target].used
        || length > XHCI_TRANSFER_BUFFER_SIZE) {
        return SCSI_FAILED;
    }
    const struct storage_unit *unit = &units[target];
    int status = transport(unit, cdb, cdb_length, direction, length, false);
    if (status == CSW_PASSED) {
        return SCSI_OK;
    }
    if (status != CSW_FAILED) {
        return SCSI_FAILED;
    }
    /* Fetch the sense data so the device clears its error state (many
       sticks answer UNIT ATTENTION or NOT READY until asked). */
    uint8_t sense[6] = {SCSI_REQUEST_SENSE, 0, 0, 0, SENSE_LENGTH, 0};
    (void)transport(unit, sense, 6, SCSI_DATA_IN, SENSE_LENGTH, true);
    return SCSI_CHECK_CONDITION;
}

static void location(uint8_t target, char *buffer, size_t capacity) {
    const char *prefix = "USB PORT ";
    size_t length = 0;
    if (capacity == 0) {
        return;
    }
    for (const char *c = prefix; *c != '\0' && length + 1 < capacity; c++) {
        buffer[length++] = *c;
    }
    unsigned port = target < USB_STORAGE_MAX && units[target].used
        ? xhci_device_port(units[target].device) : 0;
    if (port >= 100 && length + 1 < capacity) {
        buffer[length++] = (char)('0' + port / 100);
    }
    if (port >= 10 && length + 1 < capacity) {
        buffer[length++] = (char)('0' + port / 10 % 10);
    }
    if (length + 1 < capacity) {
        buffer[length++] = (char)('0' + port % 10);
    }
    buffer[length] = '\0';
}

bool usb_storage_attach(size_t device, uint8_t interface) {
    size_t target = 0;
    while (target < USB_STORAGE_MAX && units[target].used) {
        target++;
    }
    if (target == USB_STORAGE_MAX) {
        return false;
    }
    host.buffer = xhci_transfer_buffer();
    units[target] = (struct storage_unit){
        .used = true,
        .device = device,
        .interface = interface,
    };
    if (!scsi_probe_target(&host, (uint8_t)target)) {
        units[target].used = false;
        return false;
    }
    return true;
}

void usb_storage_detach(size_t device) {
    for (size_t target = 0; target < USB_STORAGE_MAX; target++) {
        if (units[target].used && units[target].device == device) {
            scsi_remove_target(&host, (uint8_t)target);
            units[target].used = false;
        }
    }
}
