/*
 * fit.h: shows a game drawn for a minimum size in a smaller window.
 *
 * The program draws into fit.drawing, fit.width x fit.height pixels:
 * the window's size, or larger with the same shape when the window is
 * smaller than the game needs (a tiled window, a small screen). Then
 * fit_present shrinks it to the window, averaging pixels, and fit_x and
 * fit_y turn mouse positions into drawing positions.
 */
#ifndef OPOS_PROGRAMS_FIT_H
#define OPOS_PROGRAMS_FIT_H

#include <stdbool.h>
#include <stdint.h>

#include <opos.h>
#include <opos_gfx.h>

struct fit {
    int window_width, window_height; /* the content area */
    int width, height;               /* the drawing */
    /* window = drawing * scale / FIT_ONE */
    int scale;
    struct opos_canvas drawing;
    struct opos_canvas window;       /* only while shrinking */
};

#define FIT_ONE 4096

static bool fit_canvas(struct opos_canvas *canvas, int width, int height) {
    if (canvas->pixels != 0 && canvas->width >= width
        && canvas->height >= height) {
        return true;
    }
    opos_canvas_destroy(canvas);
    *canvas = opos_canvas_create(width, height);
    return canvas->pixels != 0;
}

/* After a RESIZE event, or when the game needs another size. Returns
   false when out of memory. */
static bool fit_layout(struct fit *fit, int window_width, int window_height,
                       int need_width, int need_height) {
    if (window_width < 1) {
        window_width = 1;
    }
    if (window_height < 1) {
        window_height = 1;
    }
    fit->window_width = window_width;
    fit->window_height = window_height;
    int scale_x = (int)((int64_t)window_width * FIT_ONE / need_width);
    int scale_y = (int)((int64_t)window_height * FIT_ONE / need_height);
    int scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale >= FIT_ONE) {
        scale = FIT_ONE;
    } else if (scale < FIT_ONE / 4) {
        scale = FIT_ONE / 4;
    }
    fit->scale = scale;
    fit->width = (int)((int64_t)window_width * FIT_ONE / scale);
    fit->height = (int)((int64_t)window_height * FIT_ONE / scale);
    if (!fit_canvas(&fit->drawing, fit->width, fit->height)) {
        return false;
    }
    return scale == FIT_ONE
        || fit_canvas(&fit->window, window_width, window_height);
}

/* Before the first fit_layout the scale is 0: positions stay. */
static int fit_x(const struct fit *fit, int x) {
    return fit->scale == 0 ? x : (int)((int64_t)x * FIT_ONE / fit->scale);
}

static int fit_y(const struct fit *fit, int y) {
    return fit->scale == 0 ? y : (int)((int64_t)y * FIT_ONE / fit->scale);
}

static long fit_present(struct fit *fit) {
    if (fit->scale == FIT_ONE) {
        return opos_canvas_present_area(&fit->drawing, fit->width,
                                        fit->height);
    }
    const struct opos_canvas *from = &fit->drawing;
    struct opos_canvas *to = &fit->window;
    for (int y = 0; y < fit->window_height; y++) {
        int y0 = fit_y(fit, y), y1 = fit_y(fit, y + 1);
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (y1 > from->height) {
            y1 = from->height;
        }
        for (int x = 0; x < fit->window_width; x++) {
            int x0 = fit_x(fit, x), x1 = fit_x(fit, x + 1);
            if (x1 <= x0) {
                x1 = x0 + 1;
            }
            if (x1 > from->width) {
                x1 = from->width;
            }
            uint32_t red = 0, green = 0, blue = 0, count = 0;
            for (int sy = y0; sy < y1; sy++) {
                const uint32_t *row = from->pixels + (size_t)sy
                                    * (size_t)from->stride;
                for (int sx = x0; sx < x1; sx++) {
                    uint32_t pixel = row[sx];
                    red += pixel >> 16 & 0xff;
                    green += pixel >> 8 & 0xff;
                    blue += pixel & 0xff;
                    count++;
                }
            }
            if (count == 0) {
                count = 1;
            }
            to->pixels[(size_t)y * (size_t)to->stride + (size_t)x] =
                OPOS_RGB(red / count, green / count, blue / count);
        }
    }
    return opos_canvas_present_area(to, fit->window_width,
                                    fit->window_height);
}

#endif
