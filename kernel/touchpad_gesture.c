/* Precision Touchpad gestures: see touchpad_gesture.h. */

#include "touchpad_gesture.h"

/* A tap is a touch shorter than this that hardly moves. */
#define TAP_MAX_MS 200
/* How long a tap holds its button, so the desktop (which samples the
   buttons once per pass) sees both the press and the release. */
#define TAP_HOLD_MS 60
/* Pause between the release of one tap click and the next press. */
#define TAP_GAP_MS 30
/* Touching again this soon after a one-finger tap and moving drags. */
#define DRAG_GAP_MS 250
/* Contacts reported, then nothing for this long: the lift was lost.
   Precision touchpads report at their scan rate while touched, so only a
   lost report gets here; a still finger still sends frames. */
#define LOST_MS 1000
/* A frame after this long a gap does not move the pointer. */
#define STALE_MS 100

static struct tp_geometry geometry;
static struct tp_options options = {
    .tap_to_click = true, .natural_scroll = true, .speed = TP_SPEED_DEFAULT,
};
/* Screen pixels per contact unit at the chosen speed, 16.16 fixed point. */
static int64_t scale_x;
static int64_t scale_y;
static int64_t tap_slop;   /* contact units */
static int64_t notch_size; /* contact units of two-finger travel per notch */

static struct tp_contact previous[TP_MAX_CONTACTS];
static size_t previous_count;
static uint32_t last_frame_ms;
static int64_t remainder_x;
static int64_t remainder_y;
static int64_t scroll_travel;

/* The touch from the first finger down to the last one up. */
static bool touching;
static uint32_t session_start;
static size_t session_most;
static int64_t session_motion;
static bool session_pressed; /* the pad's button went down during it */
static bool drag_allowed;
static bool dragging;
static bool tap_recent; /* a one-finger tap ended at tap_end */
static uint32_t tap_end;

static bool pad_down;
static uint8_t pad_buttons;
static uint8_t tap_buttons;
static uint32_t tap_release;
static uint8_t tap_queued;
static uint32_t tap_queued_at;

static bool reached(uint32_t now, uint32_t when) {
    return (int32_t)(now - when) >= 0;
}

static int64_t magnitude(int64_t value) {
    return value < 0 ? -value : value;
}

static void compute_scale(void) {
    int64_t x_max = geometry.x_max > 0 ? geometry.x_max : 1;
    int64_t y_max = geometry.y_max > 0 ? geometry.y_max : 1;
    int64_t width = geometry.screen_width > 0 ? geometry.screen_width : 1024;
    /* At the default speed the pad's width spans 80% of the screen
       before acceleration. */
    scale_x = width * 65536 * 4 * options.speed
            / (5 * TP_SPEED_DEFAULT * x_max);
    scale_y = scale_x;
    if (geometry.x_physical > 0 && geometry.y_physical > 0) {
        /* The same pixels per millimetre on both axes. */
        scale_y = scale_x * x_max * geometry.y_physical
                / ((int64_t)geometry.x_physical * y_max);
    }
    tap_slop = x_max / 30;
    notch_size = y_max / 14;
    if (notch_size < 1) {
        notch_size = 1;
    }
}

void tp_reset(const struct tp_geometry *new_geometry) {
    geometry = *new_geometry;
    previous_count = 0;
    remainder_x = remainder_y = 0;
    scroll_travel = 0;
    touching = dragging = tap_recent = pad_down = false;
    pad_buttons = tap_buttons = tap_queued = 0;
    compute_scale();
}

void tp_set_options(const struct tp_options *new_options) {
    options = *new_options;
    if (options.speed < TP_SPEED_MIN || options.speed > TP_SPEED_MAX) {
        options.speed = TP_SPEED_DEFAULT;
    }
    compute_scale();
}

void tp_get_options(struct tp_options *out) {
    *out = options;
}

static const struct tp_contact *find_previous(uint32_t id) {
    for (size_t i = 0; i < previous_count; i++) {
        if (previous[i].id == id) {
            return &previous[i];
        }
    }
    return NULL;
}

/* Contact units to pixels, with acceleration for fast strokes. */
static void move_pointer(int64_t units_x, int64_t units_y,
                         struct tp_output *out) {
    int64_t x = units_x * scale_x;
    int64_t y = units_y * scale_y;
    int64_t speed = (magnitude(x) + magnitude(y)) >> 16; /* px per frame */
    int64_t factor = 1000 + (speed > 2 ? (speed - 2) * 100 : 0);
    if (factor > 2500) {
        factor = 2500;
    }
    x = x * factor / 1000 + remainder_x;
    y = y * factor / 1000 + remainder_y;
    out->dx = (int32_t)(x >> 16);
    out->dy = (int32_t)(y >> 16);
    remainder_x = x - ((int64_t)out->dx << 16);
    remainder_y = y - ((int64_t)out->dy << 16);
}

static uint8_t held_buttons(void) {
    return (uint8_t)(pad_buttons | tap_buttons | (dragging ? 1 : 0));
}

static void tap_click(uint8_t buttons, uint32_t now) {
    if (tap_buttons != 0) {
        /* The last tap is still held: release it first, press again a
           moment later (a double tap). */
        tap_buttons = 0;
        tap_queued = buttons;
        tap_queued_at = now + TAP_GAP_MS;
        return;
    }
    tap_buttons = buttons;
    tap_release = now + TAP_HOLD_MS;
}

void tp_frame(const struct tp_contact *contacts, size_t count, bool button,
              uint32_t now, struct tp_output *out) {
    *out = (struct tp_output){0};
    if (count > TP_MAX_CONTACTS) {
        count = TP_MAX_CONTACTS;
    }
    if (reached(now, last_frame_ms + STALE_MS)) {
        /* Old positions: measuring from them would make the pointer jump. */
        previous_count = 0;
    }
    last_frame_ms = now;

    if (!touching && count > 0) {
        touching = true;
        session_start = now;
        session_most = 0;
        session_motion = 0;
        session_pressed = false;
        drag_allowed = tap_recent && options.tap_to_click
                    && !reached(now, tap_end + DRAG_GAP_MS);
        tap_recent = false;
        scroll_travel = 0;
        remainder_x = remainder_y = 0;
    }
    if (count > session_most) {
        session_most = count;
    }

    /* The pad's own button: decided when it goes down, kept until up. */
    if (button && !pad_down) {
        pad_down = true;
        session_pressed = true;
        pad_buttons = 1;
        if (count >= 2) {
            pad_buttons = 2;
        } else if (count == 1 && contacts[0].x > geometry.x_max / 2
                   && contacts[0].y > geometry.y_max / 5 * 4) {
            pad_buttons = 2;
        }
    } else if (!button && pad_down) {
        pad_down = false;
        pad_buttons = 0;
    }

    /* Travel of the contacts seen in the last frame too. */
    int64_t travel = 0;
    for (size_t i = 0; i < count; i++) {
        const struct tp_contact *before = find_previous(contacts[i].id);
        if (before != NULL) {
            travel += magnitude(contacts[i].x - before->x)
                    + magnitude(contacts[i].y - before->y);
        }
    }
    if (count > 0) {
        session_motion += travel / (int64_t)count;
    }

    if (count == 1 || (pad_down && count >= 2)) {
        /* With the pad pressed, the highest finger moves the pointer
           (the lower one is the thumb on the button area). */
        const struct tp_contact *pointer = &contacts[0];
        for (size_t i = 1; i < count; i++) {
            if (contacts[i].y < pointer->y) {
                pointer = &contacts[i];
            }
        }
        const struct tp_contact *before = find_previous(pointer->id);
        if (before != NULL) {
            move_pointer(pointer->x - before->x, pointer->y - before->y, out);
        }
    } else if (count == 2) {
        const struct tp_contact *a = find_previous(contacts[0].id);
        const struct tp_contact *b = find_previous(contacts[1].id);
        if (a != NULL && b != NULL) {
            scroll_travel += ((contacts[0].y - a->y) + (contacts[1].y - b->y))
                           / 2;
            int64_t notches = scroll_travel / notch_size;
            scroll_travel -= notches * notch_size;
            out->scroll = (int32_t)(options.natural_scroll ? notches
                                                           : -notches);
        }
    }

    if (drag_allowed && !dragging && count == 1
        && session_motion > tap_slop) {
        dragging = true;
    }

    if (touching && count == 0) {
        touching = false;
        if (dragging) {
            dragging = false;
        } else if (options.tap_to_click && !session_pressed
                   && !reached(now, session_start + TAP_MAX_MS + 1)
                   && session_motion <= tap_slop) {
            tap_click(session_most == 1 ? 1 : session_most == 2 ? 2 : 4, now);
            if (session_most == 1) {
                tap_recent = true;
                tap_end = now;
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        previous[i] = contacts[i];
    }
    previous_count = count;
    out->buttons = held_buttons();
}

bool tp_tick(uint32_t now, struct tp_output *out) {
    *out = (struct tp_output){0};
    bool changed = false;
    if (tap_queued != 0 && reached(now, tap_queued_at)) {
        tap_buttons = tap_queued;
        tap_release = now + TAP_HOLD_MS;
        tap_queued = 0;
        changed = true;
    } else if (tap_buttons != 0 && reached(now, tap_release)) {
        tap_buttons = 0;
        changed = true;
    }
    if (touching && reached(now, last_frame_ms + LOST_MS)) {
        /* No lift report came: end the touch without a tap. */
        touching = false;
        dragging = false;
        previous_count = 0;
        changed = true;
    }
    out->buttons = held_buttons();
    return changed;
}
