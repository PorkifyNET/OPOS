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
/* Other pointer drivers: attach once, then report relative motion
   (screen pixels, y down) and the button bits (1 left, 2 right, 4 middle). */
void mouse_external_attach(void);
void mouse_external_report(int32_t dx, int32_t dy, uint8_t buttons);
/* Why mouse_initialize failed, or NULL when the mouse is ready. */
const char *mouse_failure_reason(void);
/* True when the device answered a reset; *id is its PS/2 device ID. */
bool mouse_reset_info(uint8_t *id);

#endif
