#ifndef OPOS_MOUSE_H
#define OPOS_MOUSE_H

#include <stdbool.h>
#include <stdint.h>

struct mouse_state {
    int32_t x;
    int32_t y;
    uint8_t buttons;
};

bool mouse_initialize(uint64_t screen_width, uint64_t screen_height);
void mouse_irq_handler(void);
/* Updates the pointer limits after a resolution change. */
void mouse_set_bounds(uint64_t screen_width, uint64_t screen_height);
bool mouse_read_state(struct mouse_state *state);

#endif
