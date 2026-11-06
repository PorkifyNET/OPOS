#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "interrupts.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "scheduler.h"

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define KEYBOARD_COMMAND_SET_LEDS 0xed
#define KEYBOARD_ACK 0xfa
#define KEYBOARD_RESEND 0xfe
#define KEYBOARD_LED_NUM_LOCK 0x02
#define KEYBOARD_LED_CAPS_LOCK 0x04
#define KEYBOARD_QUEUE_SIZE 64
#define KEYBOARD_QUEUE_MASK (KEYBOARD_QUEUE_SIZE - 1)

static const char keymap[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0a] = '9', [0x0b] = '0', [0x0c] = '-', [0x0d] = '=',
    [0x0e] = '\b', [0x0f] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1a] = '[', [0x1b] = ']',
    [0x1c] = '\n',
    [0x1e] = 'a', [0x1f] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2b] = '\\',
    [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',',
    [0x34] = '.', [0x35] = '/',
    [0x39] = ' ',
};

/* Keypad keys (scan codes without the 0xe0 prefix): digits with Num Lock
   on, cursor keys with it off. */
static const char keypad_digits[128] = {
    [0x47] = '7', [0x48] = '8', [0x49] = '9',
    [0x4b] = '4', [0x4c] = '5', [0x4d] = '6',
    [0x4f] = '1', [0x50] = '2', [0x51] = '3',
    [0x52] = '0', [0x53] = '.',
};

static const char keypad_cursor[128] = {
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGE_UP,
    [0x4b] = KEY_LEFT, [0x4d] = KEY_RIGHT,
    [0x4f] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGE_DOWN,
    [0x53] = KEY_DELETE,
};

static const char shifted_keymap[128] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0a] = '(', [0x0b] = ')', [0x0c] = '_', [0x0d] = '+',
    [0x1a] = '{', [0x1b] = '}',
    [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2b] = '|',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
};

static volatile char character_queue[KEYBOARD_QUEUE_SIZE];
/* Ctrl state when each queued character was typed. */
static volatile bool ctrl_queue[KEYBOARD_QUEUE_SIZE];
static volatile bool shift_queue[KEYBOARD_QUEUE_SIZE];
static bool last_character_ctrl;
static bool last_character_shift;
static volatile uint8_t queue_head;
static volatile uint8_t queue_tail;
static volatile uint64_t dropped_characters;
static bool left_shift;
static bool right_shift;
static bool caps_lock;
static bool num_lock;
/* LED update in progress: 0xed sent, waiting for the ACK before the
   LED byte goes out. */
static bool led_command_pending;
static bool led_byte_pending;
static bool extended_scancode;
static bool super_down;
static bool super_combo;
static bool ctrl_down;
static const uint8_t keyboard_event;

static void queue_character(char character) {
    uint8_t next_head = (queue_head + 1) & KEYBOARD_QUEUE_MASK;
    if (next_head == queue_tail) {
        dropped_characters++;
        return;
    }

    character_queue[queue_head] = character;
    ctrl_queue[queue_head] = ctrl_down;
    shift_queue[queue_head] = left_shift || right_shift;
    queue_head = next_head;
}

static void keyboard_send(uint8_t value) {
    /* Wait (bounded) until the controller input buffer is empty. */
    for (int i = 0; i < 100000 && (io_in8(KEYBOARD_STATUS_PORT) & 0x02); i++) {
    }
    io_out8(KEYBOARD_DATA_PORT, value);
}

static uint8_t led_byte(void) {
    return (uint8_t)((num_lock ? KEYBOARD_LED_NUM_LOCK : 0)
                     | (caps_lock ? KEYBOARD_LED_CAPS_LOCK : 0));
}

/* Starts an LED update; the LED byte is sent when the keyboard ACKs. */
static void update_leds(void) {
    if (led_command_pending) {
        led_byte_pending = true;
        return;
    }
    led_command_pending = true;
    keyboard_send(KEYBOARD_COMMAND_SET_LEDS);
}

void keyboard_initialize(void) {
    queue_head = 0;
    queue_tail = 0;
    dropped_characters = 0;
    left_shift = false;
    right_shift = false;
    caps_lock = false;
    /* Most PCs start with Num Lock on. */
    num_lock = true;
    led_command_pending = false;
    led_byte_pending = false;
    extended_scancode = false;
    super_down = false;
    super_combo = false;
    interrupts_unmask_irq(1);
}

void keyboard_irq_handler(void) {
    /* Real 8042s can raise IRQ 1 while the byte waiting is the
       touchpad's (AUX flag set): hand it to the mouse driver instead of
       reading it as a key. An empty buffer means a spurious IRQ. */
    uint8_t status = io_in8(KEYBOARD_STATUS_PORT);
    if ((status & 0x01) == 0) {
        return;
    }
    if ((status & 0x20) != 0) {
        mouse_irq_handler();
        return;
    }
    uint8_t scancode = io_in8(KEYBOARD_DATA_PORT);
    if (scancode == KEYBOARD_ACK || scancode == KEYBOARD_RESEND) {
        if (led_command_pending) {
            if (scancode == KEYBOARD_RESEND) {
                led_command_pending = false;
                return;
            }
            /* ACK of 0xed: send the LED byte (its own ACK is ignored). */
            led_command_pending = false;
            keyboard_send(led_byte());
            if (led_byte_pending) {
                led_byte_pending = false;
                update_leds();
            }
        }
        return;
    }
    if (scancode == 0xe0) {
        extended_scancode = true;
        return;
    }
    bool released = (scancode & 0x80) != 0;
    uint8_t key = scancode & 0x7f;
    char special = '\0';
    if (extended_scancode) {
        extended_scancode = false;
        if (key == 0x5b || key == 0x5c) {
            /* A lone Super tap is reported on release; Super+key is a combo. */
            if (!released) {
                if (!super_down) {
                    super_combo = false;
                }
                super_down = true;
            } else {
                if (super_down && !super_combo) {
                    queue_character(KEY_SUPER);
                    scheduler_wake_event(&keyboard_event);
                }
                super_down = false;
            }
            return;
        }
        if (key == 0x1d) {
            ctrl_down = !released;
            return;
        }
        if (released) {
            return;
        }
        switch (key) {
        case 0x48: special = KEY_UP; break;
        case 0x50: special = KEY_DOWN; break;
        case 0x4b: special = KEY_LEFT; break;
        case 0x4d: special = KEY_RIGHT; break;
        case 0x53: special = KEY_DELETE; break;
        case 0x47: special = KEY_HOME; break;
        case 0x4f: special = KEY_END; break;
        case 0x49: special = KEY_PAGE_UP; break;
        case 0x51: special = KEY_PAGE_DOWN; break;
        case 0x1c: special = '\n'; break;
        case 0x35: special = '/'; break; /* keypad / */
        case 0x5d: special = KEY_MENU; break;
        default: return;
        }
    }
    if (key == 0x1d) {
        ctrl_down = !released;
        return;
    }
    if (key == 0x2a) {
        left_shift = !released;
        return;
    }
    if (key == 0x36) {
        right_shift = !released;
        return;
    }
    if (released) {
        return;
    }
    if (key == 0x01) {
        special = KEY_ESCAPE;
    }
    if (key == 0x44 && special == '\0' && (left_shift || right_shift)) {
        special = KEY_MENU; /* Shift+F10 */
    }
    if (special == '\0' && key == 0x3a) {
        caps_lock = !caps_lock;
        update_leds();
        return;
    }
    if (special == '\0' && key == 0x45) {
        num_lock = !num_lock;
        update_leds();
        return;
    }
    if (special == '\0') {
        if (keypad_digits[key] != '\0') {
            special = num_lock ? keypad_digits[key] : keypad_cursor[key];
            if (special == '\0') {
                return; /* keypad 5 and 0 with Num Lock off */
            }
        } else if (key == 0x37) {
            special = '*';
        } else if (key == 0x4a) {
            special = '-';
        } else if (key == 0x4e) {
            special = '+';
        }
    }

    char character = special != '\0' ? special : keymap[key];
    if (character >= 'a' && character <= 'z') {
        if ((left_shift || right_shift) != caps_lock) {
            character -= 'a' - 'A';
        }
    } else if (special == '\0' && (left_shift || right_shift)
               && shifted_keymap[key] != '\0') {
        character = shifted_keymap[key];
    }

    if (character != '\0') {
        if (super_down) {
            super_combo = true;
            queue_character(KEY_SUPER_PREFIX);
        }
        queue_character(character);
        scheduler_wake_event(&keyboard_event);
    }
}

bool keyboard_read_character(char *character) {
    if (character == NULL || queue_tail == queue_head) {
        return false;
    }

    *character = character_queue[queue_tail];
    last_character_ctrl = ctrl_queue[queue_tail];
    last_character_shift = shift_queue[queue_tail];
    queue_tail = (queue_tail + 1) & KEYBOARD_QUEUE_MASK;
    return true;
}

bool keyboard_wait_character(char *character) {
    if (character == NULL) {
        return false;
    }

    for (;;) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        if (keyboard_read_character(character)) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return true;
        }
        if (!scheduler_block_current_locked(&keyboard_event)) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return false;
        }
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
}

bool keyboard_last_ctrl(void) {
    return last_character_ctrl;
}

bool keyboard_last_shift(void) {
    return last_character_shift;
}

bool keyboard_num_lock(void) {
    return num_lock;
}

void keyboard_start_leds(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    update_leds();
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

bool keyboard_ctrl_held(void) {
    return ctrl_down;
}

bool keyboard_shift_held(void) {
    return left_shift || right_shift;
}

uint64_t keyboard_dropped_characters(void) {
    return dropped_characters;
}
