#include <stdbool.h>
#include <stdint.h>

#include "interrupts.h"
#include "io.h"
#include "mouse.h"
#include "vmware.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64
#define PS2_COMMAND_PORT 0x64
#define PS2_WAIT_LIMIT 100000
#define PS2_MOUSE_IRQ 12
#define PS2_MOUSE_SYNC 0x08
#define PS2_MOUSE_OVERFLOW 0xc0

static int32_t pointer_x;
static int32_t pointer_y;
static int32_t maximum_x;
static int32_t maximum_y;
static uint8_t packet[3];
static uint8_t packet_index;
static uint8_t pointer_buttons;
static bool available;

static bool wait_input_empty(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        if ((io_in8(PS2_STATUS_PORT) & 0x02) == 0) {
            return true;
        }
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        if ((io_in8(PS2_STATUS_PORT) & 0x01) != 0) {
            return true;
        }
    }
    return false;
}

static bool mouse_send_command(uint8_t command) {
    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_COMMAND_PORT, 0xd4);
    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_DATA_PORT, command);
    if (!wait_output_full()) {
        return false;
    }
    return io_in8(PS2_DATA_PORT) == 0xfa;
}

bool mouse_initialize(uint64_t screen_width, uint64_t screen_height) {
    available = false;
    packet_index = 0;
    pointer_buttons = 0;
    if (screen_width == 0 || screen_height == 0
        || screen_width > INT32_MAX || screen_height > INT32_MAX) {
        return false;
    }
    maximum_x = (int32_t)screen_width - 1;
    maximum_y = (int32_t)screen_height - 1;
    pointer_x = (int32_t)(screen_width / 2);
    pointer_y = (int32_t)(screen_height / 2);

    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_COMMAND_PORT, 0xa7);
    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_COMMAND_PORT, 0x20);
    if (!wait_output_full()) {
        return false;
    }
    uint8_t configuration = io_in8(PS2_DATA_PORT);
    configuration = (configuration | 0x02) & (uint8_t)~0x20;
    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_COMMAND_PORT, 0x60);
    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_DATA_PORT, configuration);

    if (!wait_input_empty()) {
        return false;
    }
    io_out8(PS2_COMMAND_PORT, 0xa8);
    if (!mouse_send_command(0xf6) || !mouse_send_command(0xf4)) {
        return false;
    }

    available = true;
    interrupts_unmask_irq(PS2_MOUSE_IRQ);
    return true;
}

void mouse_irq_handler(void) {
    uint8_t status = io_in8(PS2_STATUS_PORT);
    if ((status & 0x21) != 0x21) {
        return;
    }
    uint8_t value = io_in8(PS2_DATA_PORT);
    if (vmware_mouse_active()) {
        /* The PS/2 byte only signals that absolute events are queued. */
        packet_index = 0;
        uint32_t x, y;
        uint8_t buttons;
        if (vmware_mouse_read(&x, &y, &buttons)) {
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
    if (packet_index != sizeof(packet)) {
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
    if (!available || state == 0) {
        return false;
    }
    state->x = pointer_x;
    state->y = pointer_y;
    state->buttons = pointer_buttons;
    return true;
}
