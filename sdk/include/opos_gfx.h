/*
 * Drawing into a pixel buffer, with the OPOS system font. Pair it with
 * the window calls in opos.h:
 *
 *     struct opos_canvas canvas = opos_canvas_create(800, 600);
 *     opos_fill_rect(&canvas, 0, 0, 800, 600, OPOS_RGB(20, 24, 36));
 *     opos_draw_text(&canvas, 10, 10, "Hello", OPOS_RGB(255, 255, 255), 2);
 *     opos_canvas_present(&canvas);
 *
 * Everything clips to the canvas. Colours are 0x00RRGGBB.
 */
#ifndef OPOS_GFX_H
#define OPOS_GFX_H

#include <stddef.h>
#include <stdint.h>

#define OPOS_RGB(r, g, b) \
    ((uint32_t)(((r) & 0xff) << 16 | ((g) & 0xff) << 8 | ((b) & 0xff)))

/* The font: 5x9 pixels plus one column of spacing, drawn at any scale.
   The desktop uses scale 2: 12 x 18 pixels per character. */
#define OPOS_FONT_WIDTH 5
#define OPOS_FONT_HEIGHT 9
#define OPOS_FONT_FIRST 0x20
#define OPOS_FONT_LAST 0x7e
#define OPOS_FONT_GLYPH_COUNT (OPOS_FONT_LAST - OPOS_FONT_FIRST + 1)

extern const uint8_t opos_font_glyphs[OPOS_FONT_GLYPH_COUNT][OPOS_FONT_HEIGHT];

struct opos_canvas {
    uint32_t *pixels;
    int width;
    int height;
    int stride; /* pixels per row */
};

/* Allocates a zeroed width x height canvas; pixels is NULL on failure. */
struct opos_canvas opos_canvas_create(int width, int height);
void opos_canvas_destroy(struct opos_canvas *canvas);

/* Shows the top-left width x height part of the canvas in the window. */
long opos_canvas_present_area(const struct opos_canvas *canvas, int width,
                              int height);
long opos_canvas_present(const struct opos_canvas *canvas);

void opos_fill_rect(struct opos_canvas *canvas, int x, int y, int width,
                    int height, uint32_t colour);
/* A one-pixel outline. */
void opos_draw_rect(struct opos_canvas *canvas, int x, int y, int width,
                    int height, uint32_t colour);
void opos_draw_line(struct opos_canvas *canvas, int x0, int y0, int x1,
                    int y1, uint32_t colour);

/* Draws one character with its top-left corner at x, y. Characters
   outside printable ASCII draw nothing. */
void opos_draw_char(struct opos_canvas *canvas, int x, int y, char character,
                    uint32_t colour, int scale);
/* Draws text on one line (newlines are not interpreted) and returns the
   x just after it. */
int opos_draw_text(struct opos_canvas *canvas, int x, int y,
                   const char *text, uint32_t colour, int scale);
/* Like opos_draw_text for the first length characters. */
int opos_draw_text_n(struct opos_canvas *canvas, int x, int y,
                     const char *text, size_t length, uint32_t colour,
                     int scale);

/* Size of one character cell at a scale, including spacing. */
static inline int opos_char_width(int scale) {
    return (OPOS_FONT_WIDTH + 1) * scale;
}

static inline int opos_char_height(int scale) {
    return OPOS_FONT_HEIGHT * scale;
}

static inline int opos_text_width(const char *text, int scale) {
    int width = 0;
    while (*text++ != '\0') {
        width += opos_char_width(scale);
    }
    return width;
}

#endif
