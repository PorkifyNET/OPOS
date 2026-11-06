#ifndef OPOS_TOUCHPAD_GESTURE_H
#define OPOS_TOUCHPAD_GESTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Turns Precision Touchpad contact frames into pointer motion, clicks and
 * scrolling. Pure logic (no hardware access), so a host test can drive it.
 *
 *   one finger moving       pointer motion (with acceleration)
 *   one-finger tap          left click; tap, then touch and move = drag
 *   two-finger tap          right click
 *   three-finger tap        middle click
 *   two fingers moving      vertical scrolling
 *   pressing the pad        left click; right click with two fingers down
 *                           or in the bottom-right corner
 */

#define TP_MAX_CONTACTS 5
#define TP_SPEED_MIN 1
#define TP_SPEED_MAX 10
#define TP_SPEED_DEFAULT 5

struct tp_contact {
    uint32_t id;
    int32_t x; /* 0..x_max, left to right */
    int32_t y; /* 0..y_max, top to bottom */
};

struct tp_geometry {
    int32_t x_max;      /* logical maxima of the contact X/Y fields */
    int32_t y_max;
    int32_t x_physical; /* physical extents in any one unit, 0 if unknown */
    int32_t y_physical;
    int32_t screen_width;
};

struct tp_options {
    bool tap_to_click;
    bool natural_scroll; /* fingers down = content down (scrolls up) */
    uint8_t speed;       /* TP_SPEED_MIN..TP_SPEED_MAX */
};

struct tp_output {
    int32_t dx;       /* screen pixels */
    int32_t dy;
    int32_t scroll;   /* wheel notches, positive = up */
    uint8_t buttons;  /* held now: 1 left, 2 right, 4 middle */
};

void tp_reset(const struct tp_geometry *geometry);
void tp_set_options(const struct tp_options *options);
void tp_get_options(struct tp_options *options);
/* One complete frame: the contacts touching the pad (tip switch set,
   palms left out) and the pad's own button. *out gets the motion since
   the last call and the buttons held now. */
void tp_frame(const struct tp_contact *contacts, size_t count, bool button,
              uint32_t now_ms, struct tp_output *out);
/* Timers (tap click release, a lost lift report). Call often; *out is
   filled like tp_frame, and true means something changed. */
bool tp_tick(uint32_t now_ms, struct tp_output *out);

#endif
