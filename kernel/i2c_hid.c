/* I2C HID touchpad (HID over I2C, PNP0C50) on an Intel LPSS I2C
   controller, which is a Synopsys DesignWare core.

   The ACPI tables give the touchpad's I2C address, its HID descriptor
   register (_DSM) and the controller's PCI address (_ADR). The touchpad
   starts in mouse mode (relative motion and buttons), which is all this
   driver reads: it parses the report descriptor for the mouse
   collection. OPOS routes no IOAPIC interrupts yet, so the touchpad is
   polled every timer tick; HID over I2C devices answer a read without
   pending data with a zero length. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi_dump.h"
#include "console.h"
#include "i2c_hid.h"
#include "interrupts.h"
#include "io.h"
#include "mouse.h"
#include "paging.h"
#include "pci.h"
#include "timer.h"

#define I2C_MMIO_BASE 0xffffc00000180000ULL
#define I2C_MAX_CONTROLLERS 12

/* DesignWare I2C registers. */
#define DW_CON 0x00
#define DW_TAR 0x04
#define DW_DATA_CMD 0x10
#define DW_SS_SCL_HCNT 0x14
#define DW_SS_SCL_LCNT 0x18
#define DW_FS_SCL_HCNT 0x1c
#define DW_FS_SCL_LCNT 0x20
#define DW_INTR_MASK 0x30
#define DW_RAW_INTR_STAT 0x34
#define DW_RX_TL 0x38
#define DW_TX_TL 0x3c
#define DW_CLR_INTR 0x40
#define DW_CLR_TX_ABRT 0x54
#define DW_CLR_STOP_DET 0x60
#define DW_ENABLE 0x6c
#define DW_STATUS 0x70
#define DW_TXFLR 0x74
#define DW_RXFLR 0x78
#define DW_SDA_HOLD 0x7c
#define DW_TX_ABRT_SOURCE 0x80
#define DW_ENABLE_STATUS 0x9c
#define DW_COMP_PARAM_1 0xf4
#define DW_COMP_TYPE 0xfc
#define DW_COMP_TYPE_VALUE 0x44570140u

#define DW_CON_MASTER 0x01
#define DW_CON_SPEED_STANDARD 0x02
#define DW_CON_SPEED_FAST 0x04
#define DW_CON_RESTART_ENABLE 0x20
#define DW_CON_SLAVE_DISABLE 0x40
#define DW_CMD_READ 0x100
#define DW_CMD_STOP 0x200
#define DW_CMD_RESTART 0x400
#define DW_INTR_TX_ABRT 0x40
#define DW_INTR_STOP_DET 0x200
#define DW_STATUS_ACTIVITY 0x01

/* HID over I2C. */
#define HID_DESCRIPTOR_LENGTH 30
#define HID_OPCODE_RESET 0x01
#define HID_OPCODE_SET_POWER 0x08
#define HID_MAX_REPORT_DESCRIPTOR 4096
#define HID_MAX_INPUT 512

struct field {
    bool present;
    uint16_t offset;    /* bits from the start of the report data */
    uint8_t size;       /* bits */
    bool is_signed;
    bool relative;
};

static struct {
    const char *stage;
    struct acpi_i2c_hid acpi;
    bool acpi_found;
    bool controller_found;
    struct pci_device pci;
    int previous_power_state;
    uint32_t comp_type;
    uint32_t tx_depth;
    uint32_t rx_depth;
    uint32_t last_abort;
    uint16_t descriptor_register;
    uint8_t descriptor[HID_DESCRIPTOR_LENGTH];
    uint16_t report_descriptor_length;
    uint16_t report_register;
    uint16_t input_register;
    uint16_t max_input;
    uint16_t command_register;
    uint16_t vendor;
    uint16_t product;
    bool report_ids;
    uint8_t mouse_report_id;
    bool mouse_found;
    struct field buttons[3];
    struct field x;
    struct field y;
    bool active;
    uint64_t reports;
    uint64_t empty_reads;
    uint64_t failed_reads;
    uint64_t last_poll_tick;
    uint8_t last_report[16];
    uint8_t last_report_length;
    const char *irq_error;
    bool irq_mode;
    uint64_t irq_count;
    uint64_t repeats_dropped;
    size_t previous_length;
} touchpad = {.stage = "not started"};

static volatile uint8_t *regs;
static uint8_t report_descriptor[HID_MAX_REPORT_DESCRIPTOR];
static uint8_t input_buffer[HID_MAX_INPUT];
static uint8_t previous_report[HID_MAX_INPUT];
static volatile bool irq_pending;

/* Interrupt context: hold the (level) line masked until the report is
   read, which drops it. */
static void touchpad_irq(void) {
    interrupts_gsi_mask(true);
    irq_pending = true;
    touchpad.irq_count++;
}

static void delay_us(uint32_t microseconds) {
    for (uint32_t i = 0; i < microseconds; i++) {
        io_in8(0x80);
    }
}

static uint32_t rd(uint32_t offset) {
    return *(volatile uint32_t *)(regs + offset);
}

static void wr(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(regs + offset) = value;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

/* ------------------------------------------------ DesignWare master */

static bool dw_set_enabled(bool enabled) {
    for (int i = 0; i < 1000; i++) {
        wr(DW_ENABLE, enabled ? 1 : 0);
        if ((rd(DW_ENABLE_STATUS) & 1) == (enabled ? 1u : 0u)) {
            return true;
        }
        delay_us(25);
    }
    return false;
}

static bool dw_setup(uint32_t speed) {
    if (!dw_set_enabled(false)) {
        return false;
    }
    /* SCL counts sized for the fastest LPSS input clock (133 MHz); a
       slower clock (100 MHz on Atom) only makes the bus slower. */
    wr(DW_SS_SCL_HCNT, 540);
    wr(DW_SS_SCL_LCNT, 630);
    wr(DW_FS_SCL_HCNT, 110);
    wr(DW_FS_SCL_LCNT, 220);
    wr(DW_SDA_HOLD, 40);
    wr(DW_CON, DW_CON_MASTER | DW_CON_RESTART_ENABLE | DW_CON_SLAVE_DISABLE
                   | (speed >= 400000 ? DW_CON_SPEED_FAST
                                      : DW_CON_SPEED_STANDARD));
    wr(DW_INTR_MASK, 0);
    wr(DW_RX_TL, 0);
    wr(DW_TX_TL, 0);
    (void)rd(DW_CLR_INTR);
    uint32_t parameters = rd(DW_COMP_PARAM_1);
    touchpad.rx_depth = ((parameters >> 8) & 0xff) + 1;
    touchpad.tx_depth = ((parameters >> 16) & 0xff) + 1;
    if (parameters == 0 || parameters == 0xffffffffu) {
        touchpad.rx_depth = touchpad.tx_depth = 8;
    }
    return true;
}

/* Writes write_length bytes, then (repeated start) reads read_length
   bytes, then stops. Runs with interrupts off: the controller sends a
   stop on its own when its transmit FIFO runs dry. */
static bool dw_transfer(uint16_t address, const uint8_t *out,
                        size_t write_length, uint8_t *in, size_t read_length) {
    if (!dw_set_enabled(false)) {
        return false;
    }
    wr(DW_TAR, address & 0x3ff);
    (void)rd(DW_CLR_INTR);
    if (!dw_set_enabled(true)) {
        return false;
    }
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");

    size_t total = write_length + read_length;
    size_t sent = 0;
    size_t received = 0;
    bool ok = true;
    /* About 30 us per byte at 300-400 kHz; allow plenty. */
    uint32_t budget = 20000 + (uint32_t)total * 200;
    while (received < read_length || sent < total) {
        uint32_t raw = rd(DW_RAW_INTR_STAT);
        if ((raw & DW_INTR_TX_ABRT) != 0) {
            touchpad.last_abort = rd(DW_TX_ABRT_SOURCE);
            (void)rd(DW_CLR_TX_ABRT);
            ok = false;
            break;
        }
        bool progress = false;
        while (sent < total && rd(DW_TXFLR) < touchpad.tx_depth
               && (sent < write_length
                   || (sent - write_length) - received < touchpad.rx_depth)) {
            uint32_t command;
            if (sent < write_length) {
                command = out[sent];
            } else {
                command = DW_CMD_READ;
                if (sent == write_length && write_length != 0) {
                    command |= DW_CMD_RESTART;
                }
            }
            if (sent + 1 == total) {
                command |= DW_CMD_STOP;
            }
            wr(DW_DATA_CMD, command);
            sent++;
            progress = true;
        }
        while (received < read_length && rd(DW_RXFLR) > 0) {
            in[received++] = (uint8_t)rd(DW_DATA_CMD);
            progress = true;
        }
        if (!progress) {
            if (budget-- == 0) {
                ok = false;
                touchpad.last_abort = 0xffffffffu; /* timeout */
                break;
            }
            delay_us(1);
        }
    }
    /* Wait for the stop condition and an idle controller. */
    for (int i = 0; i < 5000; i++) {
        uint32_t raw = rd(DW_RAW_INTR_STAT);
        if ((raw & DW_INTR_TX_ABRT) != 0) {
            touchpad.last_abort = rd(DW_TX_ABRT_SOURCE);
            (void)rd(DW_CLR_TX_ABRT);
            ok = false;
        }
        if ((raw & DW_INTR_STOP_DET) != 0
            && (rd(DW_STATUS) & DW_STATUS_ACTIVITY) == 0) {
            break;
        }
        delay_us(2);
    }
    (void)rd(DW_CLR_STOP_DET);
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
    dw_set_enabled(false);
    return ok;
}

/* ------------------------------------------------- controller lookup */

static bool lpss_byt_cht(uint16_t device_id) {
    return (device_id >= 0x0f41 && device_id <= 0x0f47)
        || (device_id >= 0x22c1 && device_id <= 0x22c7);
}

static size_t mapped_controllers;

/* Maps BAR0, powers the function up and takes it out of LPSS reset.
   True when it is a DesignWare I2C core. */
static bool open_controller(const struct pci_device *device) {
    if (mapped_controllers >= I2C_MAX_CONTROLLERS) {
        return false;
    }
    bool io_space = false;
    uint64_t bar = pci_bar_address(device, 0, &io_space);
    if (bar == 0 || io_space || (bar & 0xfff) != 0) {
        return false;
    }
    touchpad.previous_power_state = pci_set_power_d0(device);
    pci_enable_device(device);
    uintptr_t window = I2C_MMIO_BASE + mapped_controllers * 0x1000;
    if (!paging_map_page(window, bar, PAGING_WRITABLE | PAGING_CACHE_DISABLE
                                          | PAGING_NO_EXECUTE)) {
        return false;
    }
    mapped_controllers++;
    regs = (volatile uint8_t *)window;
    uint32_t type = rd(DW_COMP_TYPE);
    if (type != DW_COMP_TYPE_VALUE && device->vendor_id == 0x8086) {
        /* LPSS private registers: function and DMA reset, active low. */
        uint32_t resets = lpss_byt_cht(device->device_id) ? 0x804 : 0x204;
        wr(resets, 0);
        delay_us(10);
        wr(resets, lpss_byt_cht(device->device_id) ? 0x3 : 0x7);
        delay_us(1000);
        type = rd(DW_COMP_TYPE);
    }
    touchpad.comp_type = type;
    return type == DW_COMP_TYPE_VALUE;
}

static bool read_hid_descriptor(uint16_t address, uint16_t reg) {
    uint8_t command[2] = {(uint8_t)reg, (uint8_t)(reg >> 8)};
    if (!dw_transfer(address, command, 2, touchpad.descriptor,
                     HID_DESCRIPTOR_LENGTH)) {
        return false;
    }
    /* wHIDDescLength 30, bcdVersion 1.00 */
    return le16(touchpad.descriptor) == HID_DESCRIPTOR_LENGTH
        && le16(touchpad.descriptor + 2) == 0x0100;
}

/* Tries the touchpad on one controller, at the _DSM register or the two
   values firmware uses most. */
static bool probe(const struct pci_device *device) {
    if (!open_controller(device) || !dw_setup(touchpad.acpi.speed)) {
        return false;
    }
    touchpad.controller_found = true;
    touchpad.pci = *device;
    uint16_t candidates[3] = {touchpad.acpi.descriptor_register, 0x0001, 0x0020};
    for (int i = 0; i < 3; i++) {
        if (candidates[i] == 0 || (i > 0 && candidates[i] == candidates[0])) {
            continue;
        }
        if (read_hid_descriptor(touchpad.acpi.address, candidates[i])) {
            touchpad.descriptor_register = candidates[i];
            return true;
        }
    }
    return false;
}

static bool find_controller(void) {
    static struct pci_function_info functions[96];
    size_t total = pci_list(functions, 96);
    if (total > 96) {
        total = 96;
    }
    /* First the function the ACPI _ADR names (bus 0). */
    if (touchpad.acpi.controller_adr_valid) {
        uint8_t dev = (uint8_t)(touchpad.acpi.controller_adr >> 16);
        uint8_t fn = (uint8_t)touchpad.acpi.controller_adr;
        for (size_t i = 0; i < total; i++) {
            const struct pci_device *d = &functions[i].device;
            if (d->bus == 0 && d->device == dev && d->function == fn) {
                if (probe(d)) {
                    return true;
                }
                break;
            }
        }
    }
    /* Otherwise every Intel serial-bus function (class 0C80 or 1180). */
    for (size_t i = 0; i < total; i++) {
        const struct pci_device *d = &functions[i].device;
        uint32_t class_code = functions[i].class_code >> 8;
        if (d->vendor_id != 0x8086
            || (class_code != 0x0c80 && class_code != 0x1180)) {
            continue;
        }
        if (touchpad.acpi.controller_adr_valid && d->bus == 0
            && d->device == (uint8_t)(touchpad.acpi.controller_adr >> 16)
            && d->function == (uint8_t)touchpad.acpi.controller_adr) {
            continue; /* already tried */
        }
        if (probe(d)) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------ report descriptor */

static void set_field(struct field *f, uint32_t offset, uint32_t size,
                      int32_t logical_minimum, uint32_t flags) {
    if (f->present || size == 0 || size > 32 || offset > 0xffff) {
        return;
    }
    f->present = true;
    f->offset = (uint16_t)offset;
    f->size = (uint8_t)size;
    f->is_signed = logical_minimum < 0;
    f->relative = (flags & 0x04) != 0;
}

/* Finds the first Generic Desktop / Mouse application collection and
   the bit positions of its buttons and X/Y in its input report. */
static bool parse_report_descriptor(const uint8_t *d, size_t length) {
    static uint32_t bit_offset[256];
    for (int i = 0; i < 256; i++) {
        bit_offset[i] = 0;
    }
    uint32_t usage_page = 0;
    uint32_t report_size = 0;
    uint32_t report_count = 0;
    int32_t logical_minimum = 0;
    uint8_t report_id = 0;
    uint32_t usages[16];
    size_t usage_count = 0;
    uint32_t usage_minimum = 0;
    uint32_t usage_maximum = 0;
    bool have_range = false;
    int depth = 0;
    int mouse_depth = -1;
    bool done = false;
    struct {
        uint32_t usage_page, report_size, report_count;
        int32_t logical_minimum;
        uint8_t report_id;
    } stack[4];
    int stack_depth = 0;

    for (size_t i = 0; i < length && !done;) {
        uint8_t prefix = d[i++];
        if (prefix == 0xfe) { /* long item */
            if (i + 1 >= length) {
                break;
            }
            i += 2 + d[i];
            continue;
        }
        size_t size = prefix & 3;
        if (size == 3) {
            size = 4;
        }
        if (i + size > length) {
            break;
        }
        uint32_t value = 0;
        for (size_t k = 0; k < size; k++) {
            value |= (uint32_t)d[i + k] << (8 * k);
        }
        int32_t signed_value = (int32_t)value;
        if (size == 1) {
            signed_value = (int8_t)value;
        } else if (size == 2) {
            signed_value = (int16_t)value;
        }
        i += size;
        uint8_t type = (prefix >> 2) & 3;
        uint8_t tag = prefix >> 4;

        if (type == 1) { /* global */
            switch (tag) {
            case 0x0: usage_page = value; break;
            case 0x1: logical_minimum = signed_value; break;
            case 0x7: report_size = value; break;
            case 0x8:
                report_id = (uint8_t)value;
                touchpad.report_ids = true;
                break;
            case 0x9: report_count = value; break;
            case 0xa:
                if (stack_depth < 4) {
                    stack[stack_depth].usage_page = usage_page;
                    stack[stack_depth].report_size = report_size;
                    stack[stack_depth].report_count = report_count;
                    stack[stack_depth].logical_minimum = logical_minimum;
                    stack[stack_depth].report_id = report_id;
                    stack_depth++;
                }
                break;
            case 0xb:
                if (stack_depth > 0) {
                    stack_depth--;
                    usage_page = stack[stack_depth].usage_page;
                    report_size = stack[stack_depth].report_size;
                    report_count = stack[stack_depth].report_count;
                    logical_minimum = stack[stack_depth].logical_minimum;
                    report_id = stack[stack_depth].report_id;
                }
                break;
            default: break;
            }
            continue;
        }
        if (type == 2) { /* local */
            uint32_t full = size == 4 ? value : (usage_page << 16 | value);
            if (tag == 0x0 && usage_count < 16) {
                usages[usage_count++] = full;
            } else if (tag == 0x1) {
                usage_minimum = full;
                have_range = true;
            } else if (tag == 0x2) {
                usage_maximum = full;
                have_range = true;
            }
            continue;
        }
        if (type != 0) {
            continue;
        }
        /* main */
        if (tag == 0xa) { /* collection */
            if (value == 1 && mouse_depth < 0 && usage_count > 0
                && usages[usage_count - 1] == 0x00010002) {
                mouse_depth = depth;
                touchpad.mouse_report_id = report_id;
            }
            depth++;
        } else if (tag == 0xc) { /* end collection */
            depth--;
            if (mouse_depth >= 0 && depth == mouse_depth) {
                done = true;
            }
        } else if (tag == 0x8) { /* input */
            uint32_t bits = report_size * report_count;
            if (mouse_depth >= 0 && (value & 1) == 0) {
                touchpad.mouse_report_id = report_id;
                for (uint32_t n = 0; n < report_count; n++) {
                    uint32_t usage;
                    if (have_range) {
                        usage = usage_minimum + n;
                        if (usage > usage_maximum) {
                            break;
                        }
                    } else if (usage_count > 0) {
                        usage = usages[n < usage_count ? n : usage_count - 1];
                    } else {
                        break;
                    }
                    uint32_t at = bit_offset[report_id] + n * report_size;
                    if (usage >= 0x00090001 && usage <= 0x00090003) {
                        set_field(&touchpad.buttons[usage - 0x00090001], at,
                                  report_size, 0, value);
                    } else if (usage == 0x00010030) {
                        set_field(&touchpad.x, at, report_size,
                                  logical_minimum, value);
                    } else if (usage == 0x00010031) {
                        set_field(&touchpad.y, at, report_size,
                                  logical_minimum, value);
                    }
                }
            }
            bit_offset[report_id] += bits;
        } else if (tag == 0x9 || tag == 0xb) {
            /* output / feature: separate reports, no input bits */
        }
        usage_count = 0;
        have_range = false;
    }
    touchpad.mouse_found = mouse_depth >= 0 && touchpad.x.present
        && touchpad.y.present;
    return touchpad.mouse_found;
}

static int32_t read_field(const struct field *f, const uint8_t *data,
                          size_t length) {
    if (!f->present || ((size_t)f->offset + f->size + 7) / 8 > length) {
        return 0;
    }
    uint32_t value = 0;
    for (uint32_t b = 0; b < f->size; b++) {
        uint32_t bit = f->offset + b;
        if ((data[bit / 8] >> (bit % 8)) & 1) {
            value |= 1u << b;
        }
    }
    if (f->is_signed && f->size < 32 && (value & (1u << (f->size - 1))) != 0) {
        value |= ~0u << f->size;
    }
    return (int32_t)value;
}

/* ----------------------------------------------------------- startup */

static bool hid_command(uint8_t opcode, uint8_t argument) {
    uint8_t command[4] = {
        (uint8_t)touchpad.command_register,
        (uint8_t)(touchpad.command_register >> 8), argument, opcode,
    };
    return dw_transfer(touchpad.acpi.address, command, 4, NULL, 0);
}

static void read_input(size_t *length) {
    *length = 0;
    size_t want = touchpad.max_input;
    if (want < 2 || want > HID_MAX_INPUT) {
        want = HID_MAX_INPUT < 64 ? HID_MAX_INPUT : 64;
    }
    if (!dw_transfer(touchpad.acpi.address, NULL, 0, input_buffer, want)) {
        touchpad.failed_reads++;
        return;
    }
    size_t reported = le16(input_buffer);
    if (reported <= 2 || reported == 0xffff) {
        touchpad.empty_reads++;
        return;
    }
    *length = reported < want ? reported : want;
}

bool i2c_hid_initialize(void) {
    touchpad.stage = "no HID-over-I2C device in ACPI";
    touchpad.acpi_found = acpi_find_i2c_hid(&touchpad.acpi);
    if (!touchpad.acpi_found || !touchpad.acpi.address_valid) {
        return false;
    }
    touchpad.stage = "no I2C controller answered";
    if (!find_controller()) {
        if (touchpad.controller_found) {
            touchpad.stage = "controller found, HID descriptor not read";
        }
        return false;
    }
    const uint8_t *desc = touchpad.descriptor;
    touchpad.report_descriptor_length = le16(desc + 4);
    touchpad.report_register = le16(desc + 6);
    touchpad.input_register = le16(desc + 8);
    touchpad.max_input = le16(desc + 10);
    touchpad.command_register = le16(desc + 16);
    touchpad.vendor = le16(desc + 20);
    touchpad.product = le16(desc + 22);

    /* The touchpad's interrupt line says when a report is waiting;
       without it, a read returns the last report again. */
    touchpad.irq_error = "no interrupt in ACPI";
    if (touchpad.acpi.gsi_valid) {
        touchpad.irq_error = interrupts_route_gsi(
            touchpad.acpi.gsi, touchpad.acpi.level, touchpad.acpi.active_low,
            touchpad_irq);
    }
    irq_pending = false;

    touchpad.stage = "power on failed";
    if (!hid_command(HID_OPCODE_SET_POWER, 0)) {
        return false;
    }
    delay_us(2000);
    touchpad.stage = "reset failed";
    if (!hid_command(HID_OPCODE_RESET, 0)) {
        return false;
    }
    /* Reset completes within about 100 ms: the device then raises its
       interrupt and returns a zero-length input report. That interrupt
       also proves the routing works. */
    if (touchpad.irq_error == NULL) {
        for (int ms = 0; ms < 1000 && !irq_pending; ms++) {
            delay_us(1000);
        }
        if (irq_pending) {
            touchpad.irq_mode = true;
        } else {
            touchpad.irq_error = "no interrupt after reset";
            interrupts_gsi_mask(true);
        }
    } else {
        delay_us(150000);
    }
    for (int i = 0; i < (touchpad.irq_mode ? 1 : 4); i++) {
        size_t length;
        read_input(&length);
    }
    if (touchpad.irq_mode) {
        irq_pending = false;
        interrupts_gsi_mask(false);
    }

    touchpad.stage = "report descriptor not read";
    size_t length = touchpad.report_descriptor_length;
    if (length == 0 || length > HID_MAX_REPORT_DESCRIPTOR) {
        return false;
    }
    uint8_t command[2] = {(uint8_t)touchpad.report_register,
                          (uint8_t)(touchpad.report_register >> 8)};
    if (!dw_transfer(touchpad.acpi.address, command, 2, report_descriptor,
                     length)) {
        return false;
    }
    touchpad.stage = "no mouse collection in the report descriptor";
    if (!parse_report_descriptor(report_descriptor, length)) {
        return false;
    }
    if (!touchpad.x.relative) {
        touchpad.stage = "mouse collection reports absolute X/Y";
        return false;
    }
    touchpad.stage = touchpad.irq_mode ? "ready (interrupt)"
                                       : "ready (polled, repeats dropped)";
    touchpad.active = true;
    mouse_external_attach();
    return true;
}

/* Applies one input report from input_buffer. */
static void handle_report(size_t length) {
    const uint8_t *data = input_buffer + 2;
    size_t data_length = length - 2;
    if (!touchpad.irq_mode) {
        /* Polled: a report identical to the last one is the same report
           read again, not new motion. */
        bool same = length == touchpad.previous_length;
        for (size_t i = 0; same && i < length && i < HID_MAX_INPUT; i++) {
            same = previous_report[i] == input_buffer[i];
        }
        if (same) {
            touchpad.repeats_dropped++;
            return;
        }
        for (size_t i = 0; i < length && i < HID_MAX_INPUT; i++) {
            previous_report[i] = input_buffer[i];
        }
        touchpad.previous_length = length;
    }
    touchpad.last_report_length = (uint8_t)(length < 16 ? length : 16);
    for (size_t i = 0; i < touchpad.last_report_length; i++) {
        touchpad.last_report[i] = input_buffer[i];
    }
    if (touchpad.report_ids) {
        if (data[0] != touchpad.mouse_report_id) {
            return;
        }
        data++;
        data_length--;
    }
    touchpad.reports++;
    uint8_t buttons = 0;
    for (int b = 0; b < 3; b++) {
        if (read_field(&touchpad.buttons[b], data, data_length) != 0) {
            buttons |= (uint8_t)(1u << b);
        }
    }
    mouse_external_report(read_field(&touchpad.x, data, data_length),
                          read_field(&touchpad.y, data, data_length),
                          buttons);
}

void i2c_hid_poll(void) {
    if (!touchpad.active) {
        return;
    }
    if (touchpad.irq_mode) {
        /* One read per interrupt. The handler masked the line; unmasking
           re-raises it at once while more reports wait. */
        for (int round = 0; round < 8 && irq_pending; round++) {
            irq_pending = false;
            size_t length;
            read_input(&length);
            if (length > 2) {
                handle_report(length);
            }
            interrupts_gsi_mask(false);
            delay_us(30);
        }
        return;
    }
    uint64_t tick = timer_ticks();
    if (tick == touchpad.last_poll_tick) {
        return;
    }
    touchpad.last_poll_tick = tick;
    size_t length;
    read_input(&length);
    if (length > 2) {
        handle_report(length);
    } else {
        /* An empty read ends the repeat: the next report is new. */
        touchpad.previous_length = 0;
    }
}

bool i2c_hid_active(void) {
    return touchpad.active;
}

const char *i2c_hid_stage(void) {
    return touchpad.stage;
}

static void write_decimal(uint64_t value) {
    char digits[21];
    size_t length = 0;
    do {
        digits[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (length > 0) {
        console_write_bytes(&digits[--length], 1);
    }
}

static void write_field(const char *name, const struct field *f) {
    console_write_string(name);
    if (!f->present) {
        console_write_string(" -");
        return;
    }
    console_write_string(" @");
    write_decimal(f->offset);
    console_write_string("/");
    write_decimal(f->size);
    console_write_string(f->relative ? "r" : "a");
}

void i2c_hid_write_status(void) {
    console_write_string("Driver: ");
    console_write_string(touchpad.stage);
    console_write_string("\n");
    if (!touchpad.acpi_found) {
        return;
    }
    console_write_string("  ACPI: ");
    console_write_string(touchpad.acpi.hid);
    console_write_string(" addr 0x");
    console_write_hex(touchpad.acpi.address);
    console_write_string(" on ");
    console_write_string(touchpad.acpi.controller);
    console_write_string(touchpad.acpi.controller_adr_valid ? " _ADR " : " no _ADR");
    if (touchpad.acpi.controller_adr_valid) {
        console_write_hex(touchpad.acpi.controller_adr);
    }
    console_write_string(" dsm ");
    console_write_hex(touchpad.acpi.descriptor_register);
    console_write_string("\n");
    if (!touchpad.controller_found) {
        console_write_string("  last COMP_TYPE ");
        console_write_hex(touchpad.comp_type);
        console_write_string("\n");
        return;
    }
    console_write_string("  PCI ");
    console_write_hex(touchpad.pci.bus);
    console_write_string(":");
    console_write_hex(touchpad.pci.device);
    console_write_string(".");
    console_write_hex(touchpad.pci.function);
    console_write_string(" ");
    console_write_hex(touchpad.pci.vendor_id);
    console_write_string(":");
    console_write_hex(touchpad.pci.device_id);
    console_write_string(" was D");
    write_decimal(touchpad.previous_power_state < 0
                      ? 9 : (uint64_t)touchpad.previous_power_state);
    console_write_string(" fifo ");
    write_decimal(touchpad.tx_depth);
    console_write_string("/");
    write_decimal(touchpad.rx_depth);
    console_write_string(" abort ");
    console_write_hex(touchpad.last_abort);
    console_write_string("\n");
    if (touchpad.descriptor_register == 0) {
        return;
    }
    console_write_string("  HID ");
    console_write_hex(touchpad.vendor);
    console_write_string(":");
    console_write_hex(touchpad.product);
    console_write_string(" reg ");
    console_write_hex(touchpad.descriptor_register);
    console_write_string(" rdesc ");
    write_decimal(touchpad.report_descriptor_length);
    console_write_string(" maxin ");
    write_decimal(touchpad.max_input);
    console_write_string("\n  mouse id ");
    write_decimal(touchpad.mouse_report_id);
    console_write_string(" ");
    write_field("b1", &touchpad.buttons[0]);
    console_write_string(" ");
    write_field("b2", &touchpad.buttons[1]);
    console_write_string(" ");
    write_field("x", &touchpad.x);
    console_write_string(" ");
    write_field("y", &touchpad.y);
    console_write_string("\n  irq ");
    if (touchpad.acpi.gsi_valid) {
        write_decimal(touchpad.acpi.gsi);
    }
    console_write_string(touchpad.irq_mode ? " in use" : " not used: ");
    if (!touchpad.irq_mode && touchpad.irq_error != NULL) {
        console_write_string(touchpad.irq_error);
    }
    console_write_string(" count ");
    write_decimal(touchpad.irq_count);
    console_write_string(" repeats dropped ");
    write_decimal(touchpad.repeats_dropped);
    console_write_string("\n  reports ");
    write_decimal(touchpad.reports);
    console_write_string(" empty ");
    write_decimal(touchpad.empty_reads);
    console_write_string(" failed ");
    write_decimal(touchpad.failed_reads);
    console_write_string("\n  last:");
    for (size_t i = 0; i < touchpad.last_report_length; i++) {
        static const char hex[] = "0123456789abcdef";
        char text[3] = {hex[touchpad.last_report[i] >> 4],
                        hex[touchpad.last_report[i] & 0xf], '\0'};
        console_write_string(" ");
        console_write_string(text);
    }
    console_write_string("\n");
}

/* For the host test: parse a descriptor and read one report. */
#ifdef I2C_HID_HOST_TEST
bool i2c_hid_test_parse(const uint8_t *d, size_t length) {
    touchpad = (__typeof__(touchpad)){.stage = "test"};
    return parse_report_descriptor(d, length);
}
void i2c_hid_test_report(const uint8_t *data, size_t length, int32_t *dx,
                         int32_t *dy, uint8_t *buttons) {
    if (touchpad.report_ids) {
        data++;
        length--;
    }
    *buttons = 0;
    for (int b = 0; b < 3; b++) {
        if (read_field(&touchpad.buttons[b], data, length) != 0) {
            *buttons |= (uint8_t)(1u << b);
        }
    }
    *dx = read_field(&touchpad.x, data, length);
    *dy = read_field(&touchpad.y, data, length);
}
#endif

bool i2c_hid_present(void) {
    return touchpad.acpi_found && touchpad.acpi.address_valid;
}
