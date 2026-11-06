#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts.h"
#include "io.h"
#include "mouse.h"
#include "vmware.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64
/* Status-port polls; each takes about 1 us on PCs. */
#define PS2_WAIT_LIMIT 100000
/* Touchpads can take most of a second to finish a reset. */
#define PS2_RESET_WAIT_LIMIT 1500000
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL 0x02
#define PS2_STATUS_AUX_DATA 0x20
#define PS2_CONFIG_KEYBOARD_IRQ 0x01
#define PS2_CONFIG_MOUSE_IRQ 0x02
#define PS2_CONFIG_MOUSE_CLOCK_OFF 0x20
#define PS2_ACK 0xfa
#define PS2_RESEND 0xfe
#define PS2_MOUSE_IRQ 12
#define PS2_MOUSE_SYNC 0x08
#define PS2_MOUSE_OVERFLOW 0xc0

static int32_t pointer_x;
static int32_t pointer_y;
static int32_t maximum_x;
static int32_t maximum_y;
static uint8_t packet[4];
static uint8_t packet_index;
/* 3 for a plain PS/2 mouse, 4 with an IntelliMouse wheel (ID 3). */
static uint8_t packet_size = 3;
/* Wheel notches not yet taken by the desktop; positive = away from the
   user (scroll up). */
static int32_t wheel_pending;
static uint8_t pointer_buttons;
static bool available;
static const char *failure = "not initialized";
static uint8_t device_id = 0xff;
static bool reset_ok;
/* Set by other pointer drivers (the I2C touchpad). */
static bool external_available;
static uint8_t external_buttons;

static bool wait_input_empty(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        if ((io_in8(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) == 0) {
            return true;
        }
    }
    return false;
}

static bool wait_output_full(uint32_t limit) {
    for (uint32_t i = 0; i < limit; i++) {
        if ((io_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) != 0) {
            return true;
        }
    }
    return false;
}

/* Drops bytes left over from the firmware or from keys pressed during
   boot, so the next read is the reply to OPOS's own command. */
static void flush_output(void) {
    for (int i = 0; i < 64; i++) {
        if ((io_in8(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) == 0) {
            return;
        }
        io_in8(PS2_DATA_PORT);
        for (int j = 0; j < 50; j++) {
            io_in8(0x80);
        }
    }
}

static bool controller_command(uint8_t command) {
    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_COMMAND_PORT, command);
    return true;
}

/* Reads one byte from the mouse. Keyboard bytes (no AUX flag) are
   skipped: the keyboard port is off during setup, but a byte may already
   have been queued. */
static bool read_mouse_byte(uint32_t limit, uint8_t *value) {
    for (int skipped = 0; skipped < 16; skipped++) {
        if (!wait_output_full(limit)) {
            return false;
        }
        uint8_t status = io_in8(PS2_STATUS_PORT);
        uint8_t byte = io_in8(PS2_DATA_PORT);
        if ((status & PS2_STATUS_AUX_DATA) != 0 || byte == PS2_ACK
            || byte == PS2_RESEND) {
            *value = byte;
            return true;
        }
    }
    return false;
}

/* Sends a byte to the mouse and waits for its ACK, resending up to three
   times when the device asks for it. */
static bool mouse_send_command(uint8_t command) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!controller_command(0xd4) || !wait_input_empty()) {
            return false;
        }
        io_out8(PS2_DATA_PORT, command);
        uint8_t reply;
        if (!read_mouse_byte(PS2_WAIT_LIMIT, &reply)) {
            return false;
        }
        if (reply == PS2_ACK) {
            return true;
        }
        if (reply != PS2_RESEND) {
            return false;
        }
    }
    return false;
}

static bool write_configuration(uint8_t configuration) {
    if (!controller_command(0x60) || !wait_input_empty()) {
        return false;
    }
    io_out8(PS2_DATA_PORT, configuration);
    return true;
}

/* IntelliMouse: the sample rates 200, 100, 80 in a row switch a wheel
   mouse to device ID 3 and 4-byte packets. Other mice ignore them. */
static void enable_wheel(void) {
    packet_size = 3;
    static const uint8_t rates[3] = {200, 100, 80};
    for (int i = 0; i < 3; i++) {
        if (!mouse_send_command(0xf3) || !mouse_send_command(rates[i])) {
            return;
        }
    }
    uint8_t id;
    if (mouse_send_command(0xf2) && read_mouse_byte(PS2_WAIT_LIMIT, &id)) {
        device_id = id;
        if (id == 3 || id == 4) {
            packet_size = 4;
        }
    }
    /* Back to the default rate. */
    if (mouse_send_command(0xf3)) {
        (void)mouse_send_command(100);
    }
}

/* Laptop touchpads (Synaptics, ELAN, ALPS, ...) start in a basic PS/2
   mouse mode and send the standard 3-byte packets this driver reads.
   Real controllers need more care than emulated ones: stale bytes, slow
   resets and resend requests. */
static bool setup_device(void) {
    /* Both ports off while setting up, so no key press gets in between. */
    if (!controller_command(0xad) || !controller_command(0xa7)) {
        failure = "controller does not accept commands";
        return false;
    }
    flush_output();

    if (!controller_command(0x20) || !wait_output_full(PS2_WAIT_LIMIT)) {
        failure = "controller configuration unreadable";
        controller_command(0xae);
        return false;
    }
    uint8_t configuration = io_in8(PS2_DATA_PORT);
    /* Interrupts off until the mouse is ready; mouse clock on. */
    uint8_t setup = configuration
        & (uint8_t)~(PS2_CONFIG_KEYBOARD_IRQ | PS2_CONFIG_MOUSE_IRQ
                     | PS2_CONFIG_MOUSE_CLOCK_OFF);
    bool ready = write_configuration(setup) && controller_command(0xa8);
    if (!ready) {
        failure = "controller configuration not written";
    }
    flush_output();

    /* Reset: ACK, then 0xAA (self-test passed) and the device ID. A device
       that rejects the reset may still work, so this is not fatal. */
    reset_ok = false;
    device_id = 0xff;
    if (ready && mouse_send_command(0xff)) {
        uint8_t result;
        if (read_mouse_byte(PS2_RESET_WAIT_LIMIT, &result) && result == 0xaa) {
            uint8_t id;
            if (read_mouse_byte(PS2_WAIT_LIMIT, &id)) {
                device_id = id;
            }
            reset_ok = true;
        }
    }
    flush_output();

    if (ready) {
        if (!mouse_send_command(0xf6)) {
            ready = false;
            failure = reset_ok ? "no ACK to set defaults (0xF6)"
                               : "no PS/2 mouse or touchpad answered";
        } else if (enable_wheel(), !mouse_send_command(0xf4)) {
            ready = false;
            failure = "no ACK to enable (0xF4)";
        }
    }

    /* Restore the controller whatever happened: the keyboard needs it. */
    configuration |= PS2_CONFIG_KEYBOARD_IRQ;
    if (ready) {
        configuration = (configuration | PS2_CONFIG_MOUSE_IRQ)
            & (uint8_t)~PS2_CONFIG_MOUSE_CLOCK_OFF;
    } else {
        configuration = (configuration & (uint8_t)~PS2_CONFIG_MOUSE_IRQ)
            | PS2_CONFIG_MOUSE_CLOCK_OFF;
    }
    flush_output();
    write_configuration(configuration);
    if (!ready) {
        controller_command(0xa7);
    }
    controller_command(0xae);
    return ready;
}

bool mouse_initialize(uint64_t screen_width, uint64_t screen_height) {
    available = false;
    packet_index = 0;
    pointer_buttons = 0;
    if (screen_width == 0 || screen_height == 0
        || screen_width > INT32_MAX || screen_height > INT32_MAX) {
        failure = "bad screen size";
        return false;
    }
    maximum_x = (int32_t)screen_width - 1;
    maximum_y = (int32_t)screen_height - 1;
    pointer_x = (int32_t)(screen_width / 2);
    pointer_y = (int32_t)(screen_height / 2);

    if (!setup_device()) {
        return false;
    }
    failure = NULL;
    available = true;
    interrupts_unmask_irq(PS2_MOUSE_IRQ);
    return true;
}

const char *mouse_failure_reason(void) {
    return failure;
}

bool mouse_reset_info(uint8_t *id) {
    *id = device_id;
    return reset_ok;
}

void mouse_irq_handler(void) {
    uint8_t status = io_in8(PS2_STATUS_PORT);
    if ((status & (PS2_STATUS_AUX_DATA | PS2_STATUS_OUTPUT_FULL))
        != (PS2_STATUS_AUX_DATA | PS2_STATUS_OUTPUT_FULL)) {
        return;
    }
    uint8_t value = io_in8(PS2_DATA_PORT);
    if (vmware_mouse_active()) {
        /* The PS/2 byte only signals that absolute events are queued. */
        packet_index = 0;
        uint32_t x, y;
        uint8_t buttons;
        int32_t wheel = 0;
        if (vmware_mouse_read(&x, &y, &buttons, &wheel)) {
            wheel_pending += wheel;
            pointer_x = (int32_t)(((uint64_t)x * (uint64_t)(maximum_x + 1)) >> 16);
            pointer_y = (int32_t)(((uint64_t)y * (uint64_t)(maximum_y + 1)) >> 16);
            pointer_buttons = buttons;
        }
        return;
    }
    if (packet_index == 0 && (value & PS2_MOUSE_SYNC) == 0) {
        return;
    }
    packet[packet_index++] = value;
    if (packet_index != packet_size) {
        return;
    }
    packet_index = 0;

    if ((packet[0] & PS2_MOUSE_OVERFLOW) != 0) {
        return;
    }

    pointer_x += (int8_t)packet[1];
    pointer_y -= (int8_t)packet[2];
    if (pointer_x < 0) {
        pointer_x = 0;
    } else if (pointer_x > maximum_x) {
        pointer_x = maximum_x;
    }
    if (pointer_y < 0) {
        pointer_y = 0;
    } else if (pointer_y > maximum_y) {
        pointer_y = maximum_y;
    }
    pointer_buttons = packet[0] & 0x07;
    if (packet_size == 4) {
        /* ID 3: a signed byte; ID 4 keeps it in the low 4 bits. Positive
           means the wheel turned toward the user. */
        int8_t z = (int8_t)packet[3];
        if (device_id == 4) {
            z = (int8_t)((packet[3] & 0x0f) << 4) >> 4;
        }
        wheel_pending -= z;
    }
}

void mouse_set_bounds(uint64_t screen_width, uint64_t screen_height) {
    if (screen_width == 0 || screen_height == 0
        || screen_width > INT32_MAX || screen_height > INT32_MAX) {
        return;
    }
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    maximum_x = (int32_t)screen_width - 1;
    maximum_y = (int32_t)screen_height - 1;
    if (pointer_x > maximum_x) {
        pointer_x = maximum_x;
    }
    if (pointer_y > maximum_y) {
        pointer_y = maximum_y;
    }
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

bool mouse_read_state(struct mouse_state *state) {
    if ((!available && !external_available) || state == 0) {
        return false;
    }
    state->x = pointer_x;
    state->y = pointer_y;
    state->buttons = pointer_buttons | external_buttons;
    return true;
}

/* Other pointer drivers (the I2C touchpad) feed relative motion here. */
void mouse_external_attach(void) {
    external_available = true;
}

int32_t mouse_take_wheel(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    int32_t notches = wheel_pending;
    wheel_pending = 0;
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return notches;
}

void mouse_external_scroll(int32_t notches) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    wheel_pending += notches;
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

int32_t mouse_screen_width(void) {
    return maximum_x + 1;
}

bool mouse_has_wheel(void) {
    return packet_size == 4;
}

void mouse_external_report(int32_t dx, int32_t dy, uint8_t buttons) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    int64_t x = (int64_t)pointer_x + dx;
    int64_t y = (int64_t)pointer_y + dy;
    pointer_x = x < 0 ? 0 : x > maximum_x ? maximum_x : (int32_t)x;
    pointer_y = y < 0 ? 0 : y > maximum_y ? maximum_y : (int32_t)y;
    external_buttons = buttons & 0x07;
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}
