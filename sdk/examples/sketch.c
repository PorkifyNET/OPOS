/*
 * Sketch: a small drawing program that shows the window API.
 *
 * Left button draws, right button erases. Click a colour in the palette
 * at the top to pick it. C clears the page, Escape (or closing the
 * window) quits. Run with "run /disk/sketch [width height]".
 */
#include <stdbool.h>

#include <opos.h>

/* Asks for nothing beyond the defaults. Needed in a package. */
OPOS_MANIFEST("Sketch", "1.0", 0);

#define CANVAS_WIDTH 1024
#define CANVAS_HEIGHT 768
#define PALETTE_HEIGHT 28
#define SWATCH_WIDTH 36
#define PAPER 0x00f4f1e8u
#define BAR 0x00303846u

static const uint32_t palette[] = {
    0x00202020u, 0x00d03a3au, 0x00e8902au, 0x00e8d23au,
    0x0040a050u, 0x003a78d0u, 0x008a4ad0u, 0x00ffffffu,
};
#define PALETTE_COUNT (sizeof(palette) / sizeof(palette[0]))

static uint32_t *pixels;
static uint32_t view_width;
static uint32_t view_height;
static uint32_t colour = 0x00202020u;

static void fill(int x, int y, int width, int height, uint32_t value) {
    for (int row = y; row < y + height; row++) {
        if (row < 0 || row >= CANVAS_HEIGHT) {
            continue;
        }
        for (int column = x; column < x + width; column++) {
            if (column >= 0 && column < CANVAS_WIDTH) {
                pixels[row * CANVAS_WIDTH + column] = value;
            }
        }
    }
}

static void draw_palette(void) {
    fill(0, 0, CANVAS_WIDTH, PALETTE_HEIGHT, BAR);
    for (unsigned i = 0; i < PALETTE_COUNT; i++) {
        int x = 4 + (int)i * (SWATCH_WIDTH + 4);
        if (palette[i] == colour) {
            fill(x - 2, 2, SWATCH_WIDTH + 4, PALETTE_HEIGHT - 4, 0x00ffffffu);
        }
        fill(x, 4, SWATCH_WIDTH, PALETTE_HEIGHT - 8, palette[i]);
    }
}

static void clear_page(void) {
    fill(0, PALETTE_HEIGHT, CANVAS_WIDTH, CANVAS_HEIGHT - PALETTE_HEIGHT,
         PAPER);
}

/* A round brush, kept below the palette. */
static void dab(int x, int y, int radius, uint32_t value) {
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (dx * dx + dy * dy <= radius * radius && py >= PALETTE_HEIGHT
                && py < CANVAS_HEIGHT && px >= 0 && px < CANVAS_WIDTH) {
                pixels[py * CANVAS_WIDTH + px] = value;
            }
        }
    }
}

static void stroke(int x0, int y0, int x1, int y1, int radius,
                   uint32_t value) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int steps = dx > dy ? dx : dy;
    if (steps == 0) {
        dab(x0, y0, radius, value);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        dab(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, radius,
            value);
    }
}

static void present(void) {
    uint32_t width = view_width < CANVAS_WIDTH ? view_width : CANVAS_WIDTH;
    uint32_t height = view_height < CANVAS_HEIGHT ? view_height
                                                  : CANVAS_HEIGHT;
    if (width != 0 && height != 0) {
        opos_window_present(pixels, width, height, CANVAS_WIDTH);
    }
}

static uint32_t parse_number(const char *text, uint32_t fallback) {
    uint32_t value = 0;
    if (text == 0 || *text == '\0') {
        return fallback;
    }
    for (; *text != '\0'; text++) {
        if (*text < '0' || *text > '9' || value > 100000) {
            return fallback;
        }
        value = value * 10 + (uint32_t)(*text - '0');
    }
    return value;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    uint32_t width = argc > 1 ? parse_number(argv[1], 480) : 480;
    uint32_t height = argc > 2 ? parse_number(argv[2], 320) : 320;

    pixels = opos_alloc(CANVAS_WIDTH * CANVAS_HEIGHT * sizeof(uint32_t));
    if (pixels == 0) {
        opos_print("sketch: out of memory\n");
        return 1;
    }
    long created = opos_window_create("SKETCH", width, height);
    if (created < 0) {
        opos_print("sketch: no window (");
        opos_print_number(created);
        opos_print("); run it from the desktop\n");
        return 1;
    }
    clear_page();
    draw_palette();

    int last_x = 0;
    int last_y = 0;
    struct opos_event event;
    for (;;) {
        if (opos_window_event(&event, -1) <= 0) {
            continue;
        }
        switch (event.type) {
        case OPOS_EVENT_RESIZE:
            view_width = event.width;
            view_height = event.height;
            break;
        case OPOS_EVENT_MOUSE_DOWN:
            if (event.y < PALETTE_HEIGHT) {
                unsigned i = (unsigned)(event.x - 4) / (SWATCH_WIDTH + 4);
                if (event.x >= 4 && i < PALETTE_COUNT) {
                    colour = palette[i];
                    draw_palette();
                }
            } else {
                bool erase = event.button == OPOS_BUTTON_RIGHT;
                stroke(event.x, event.y, event.x, event.y, erase ? 8 : 2,
                       erase ? PAPER : colour);
            }
            last_x = event.x;
            last_y = event.y;
            break;
        case OPOS_EVENT_MOUSE_MOVE:
            if (event.buttons != 0) {
                bool erase = (event.buttons & OPOS_BUTTON_RIGHT) != 0;
                stroke(last_x, last_y, event.x, event.y, erase ? 8 : 2,
                       erase ? PAPER : colour);
            }
            last_x = event.x;
            last_y = event.y;
            break;
        case OPOS_EVENT_KEY:
            if (event.key == OPOS_KEY_ESCAPE) {
                return 0;
            }
            if (event.key == 'c' || event.key == 'C') {
                clear_page();
            }
            break;
        case OPOS_EVENT_CLOSE:
            return 0;
        default:
            break;
        }
        present();
    }
}
