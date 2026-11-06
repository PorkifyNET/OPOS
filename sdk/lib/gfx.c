/* Canvas drawing for OPOS programs (see include/opos_gfx.h). */
#include <stdlib.h>

#include <opos.h>
#include <opos_gfx.h>

struct opos_canvas opos_canvas_create(int width, int height) {
    struct opos_canvas canvas = {0};
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return canvas;
    }
    canvas.pixels = opos_alloc((size_t)width * (size_t)height
                               * sizeof(uint32_t));
    if (canvas.pixels != NULL) {
        canvas.width = width;
        canvas.height = height;
        canvas.stride = width;
    }
    return canvas;
}

void opos_canvas_destroy(struct opos_canvas *canvas) {
    if (canvas->pixels != NULL) {
        opos_free(canvas->pixels, (size_t)canvas->stride
                  * (size_t)canvas->height * sizeof(uint32_t));
    }
    *canvas = (struct opos_canvas){0};
}

long opos_canvas_present_area(const struct opos_canvas *canvas, int width,
                              int height) {
    if (width > canvas->width) {
        width = canvas->width;
    }
    if (height > canvas->height) {
        height = canvas->height;
    }
    if (canvas->pixels == NULL || width <= 0 || height <= 0) {
        return 0;
    }
    return opos_window_present(canvas->pixels, (uint32_t)width,
                               (uint32_t)height, (uint32_t)canvas->stride);
}

long opos_canvas_present(const struct opos_canvas *canvas) {
    return opos_canvas_present_area(canvas, canvas->width, canvas->height);
}

void opos_fill_rect(struct opos_canvas *canvas, int x, int y, int width,
                    int height, uint32_t colour) {
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > canvas->width) {
        width = canvas->width - x;
    }
    if (y + height > canvas->height) {
        height = canvas->height - y;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int row = y; row < y + height; row++) {
        uint32_t *line = canvas->pixels + (size_t)row * canvas->stride;
        for (int column = x; column < x + width; column++) {
            line[column] = colour;
        }
    }
}

void opos_draw_rect(struct opos_canvas *canvas, int x, int y, int width,
                    int height, uint32_t colour) {
    opos_fill_rect(canvas, x, y, width, 1, colour);
    opos_fill_rect(canvas, x, y + height - 1, width, 1, colour);
    opos_fill_rect(canvas, x, y, 1, height, colour);
    opos_fill_rect(canvas, x + width - 1, y, 1, height, colour);
}

static void plot(struct opos_canvas *canvas, int x, int y, uint32_t colour) {
    if (x >= 0 && y >= 0 && x < canvas->width && y < canvas->height) {
        canvas->pixels[(size_t)y * canvas->stride + x] = colour;
    }
}

void opos_draw_line(struct opos_canvas *canvas, int x0, int y0, int x1,
                    int y1, uint32_t colour) {
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int step_x = x0 < x1 ? 1 : -1;
    int step_y = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        plot(canvas, x0, y0, colour);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += step_x;
        }
        if (twice <= dx) {
            error += dx;
            y0 += step_y;
        }
    }
}

void opos_draw_char(struct opos_canvas *canvas, int x, int y, char character,
                    uint32_t colour, int scale) {
    unsigned char code = (unsigned char)character;
    if (code < OPOS_FONT_FIRST || code > OPOS_FONT_LAST || scale <= 0) {
        return;
    }
    const uint8_t *glyph = opos_font_glyphs[code - OPOS_FONT_FIRST];
    for (int row = 0; row < OPOS_FONT_HEIGHT; row++) {
        for (int column = 0; column < OPOS_FONT_WIDTH; column++) {
            if ((glyph[row] & (0x10 >> column)) != 0) {
                opos_fill_rect(canvas, x + column * scale, y + row * scale,
                               scale, scale, colour);
            }
        }
    }
}

int opos_draw_text_n(struct opos_canvas *canvas, int x, int y,
                     const char *text, size_t length, uint32_t colour,
                     int scale) {
    for (size_t i = 0; i < length && text[i] != '\0'; i++) {
        if (x >= canvas->width) {
            x += opos_char_width(scale);
            continue;
        }
        opos_draw_char(canvas, x, y, text[i], colour, scale);
        x += opos_char_width(scale);
    }
    return x;
}

int opos_draw_text(struct opos_canvas *canvas, int x, int y,
                   const char *text, uint32_t colour, int scale) {
    return opos_draw_text_n(canvas, x, y, text, (size_t)-1, colour, scale);
}
