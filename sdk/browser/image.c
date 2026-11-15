/*
 * Page images: decoding what was fetched (PNG, JPEG, BMP or SVG), data:
 * URLs, and the scaled copy that painting uses. An SVG is drawn again at
 * the size it is shown at instead of being stretched.
 */
#include <stdlib.h>
#include <string.h>

#include "browser.h"

/* True when the bytes look like an SVG document. */
bool image_is_svg(const uint8_t *data, size_t length) {
    size_t limit = length < 1024 ? length : 1024;
    size_t i = 0;
    if (limit >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        i = 3;
    }
    while (i < limit && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n'
                         || data[i] == '\r')) {
        i++;
    }
    if (i >= limit || data[i] != '<') {
        return false;
    }
    /* "<svg", or "<?xml ...", "<!DOCTYPE svg ...", "<!-- ... -->" before it. */
    for (; i + 4 <= limit; i++) {
        if (data[i] == '<' && data[i + 1] == 's' && data[i + 2] == 'v'
            && data[i + 3] == 'g') {
            return true;
        }
    }
    return false;
}

static void set_svg_size(struct image_ref *ref) {
    int width, height;
    svg_size(ref->svg, &width, &height);
    ref->width = width;
    ref->height = height;
}

bool image_ref_decode(struct image_ref *ref, const uint8_t *data,
                      size_t length) {
    if (image_is_svg(data, length)) {
        struct opos_html_doc *doc = opos_html_parse((const char *)data, length,
                                                    "utf-8");
        const struct opos_html_node *svg = doc != NULL
                                         ? svg_root(doc->root) : NULL;
        if (svg == NULL) {
            opos_html_free(doc);
            return false;
        }
        ref->svg_doc = doc;
        ref->svg = svg;
        set_svg_size(ref);
        ref->state = 1;
        return true;
    }
    struct image_info info;
    if (image_probe(data, length, &info) != IMAGE_OK) {
        return false;
    }
    void *scratch = malloc(info.scratch_bytes + 16);
    if (scratch == NULL) {
        return false;
    }
    void *aligned = (void *)(((uintptr_t)scratch + 15) & ~(uintptr_t)15);
    struct image image;
    bool ok = false;
    if (image_decode(data, length, aligned, info.scratch_bytes, &image)
        == IMAGE_OK) {
        size_t bytes = (size_t)image.width * image.height * 4;
        ref->pixels = malloc(bytes);
        if (ref->pixels != NULL) {
            memcpy(ref->pixels, image.pixels, bytes);
            ref->width = (int)image.width;
            ref->height = (int)image.height;
            ref->state = 1;
            ok = true;
        }
    }
    free(scratch);
    return ok;
}

void image_ref_inline_svg(struct image_ref *ref,
                          const struct opos_html_node *svg) {
    ref->svg = svg;
    set_svg_size(ref);
    ref->state = 1;
}

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint8_t *data_url_decode(const char *url, size_t *length) {
    if (strncmp(url, "data:", 5) != 0) {
        return NULL;
    }
    const char *comma = strchr(url, ',');
    if (comma == NULL) {
        return NULL;
    }
    bool base64 = false;
    for (const char *p = url + 5; p + 7 <= comma; p++) {
        if (strncmp(p, ";base64", 7) == 0) {
            base64 = true;
        }
    }
    const char *in = comma + 1;
    size_t n = strlen(in);
    uint8_t *out = malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t used = 0;
    if (base64) {
        uint32_t bits = 0;
        int count = 0;
        for (size_t i = 0; i < n; i++) {
            char ch = in[i];
            if (ch == '%' && i + 2 < n && hex_digit(in[i + 1]) >= 0
                && hex_digit(in[i + 2]) >= 0) {
                /* "%3D" padding written percent-encoded. */
                ch = (char)(hex_digit(in[i + 1]) * 16 + hex_digit(in[i + 2]));
                i += 2;
            }
            int v = base64_value(ch);
            if (v < 0) {
                continue; /* spaces, line breaks and '=' padding */
            }
            bits = bits << 6 | (uint32_t)v;
            count++;
            if (count == 4) {
                out[used++] = (uint8_t)(bits >> 16);
                out[used++] = (uint8_t)(bits >> 8);
                out[used++] = (uint8_t)bits;
                bits = 0;
                count = 0;
            }
        }
        if (count == 2) {
            out[used++] = (uint8_t)(bits >> 4);
        } else if (count == 3) {
            out[used++] = (uint8_t)(bits >> 10);
            out[used++] = (uint8_t)(bits >> 2);
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            if (in[i] == '%' && i + 2 < n + 0 && hex_digit(in[i + 1]) >= 0
                && hex_digit(in[i + 2]) >= 0) {
                out[used++] = (uint8_t)(hex_digit(in[i + 1]) * 16
                                        + hex_digit(in[i + 2]));
                i += 2;
            } else {
                out[used++] = (uint8_t)in[i];
            }
        }
    }
    out[used] = 0;
    *length = used;
    return out;
}

const uint32_t *image_ref_scaled(struct image_ref *ref, int width,
                                 int height) {
    if (ref->state != 1 || width <= 0 || height <= 0
        || (size_t)width * (size_t)height > 16u * 1024 * 1024) {
        return NULL;
    }
    if (ref->scaled != NULL && ref->scaled_width == width
        && ref->scaled_height == height && ref->scaled_colour == ref->colour) {
        return ref->scaled;
    }
    free(ref->scaled);
    ref->scaled = NULL;
    if (ref->svg != NULL) {
        ref->scaled = svg_render(ref->svg, width, height, ref->colour);
    } else if (ref->pixels != NULL) {
        ref->scaled = malloc((size_t)width * height * 4);
        if (ref->scaled != NULL) {
            if (width == ref->width && height == ref->height) {
                memcpy(ref->scaled, ref->pixels, (size_t)width * height * 4);
            } else {
                image_resample(ref->pixels, (uint32_t)ref->width, 0, 0,
                               (uint32_t)ref->width, (uint32_t)ref->height,
                               ref->scaled, (uint32_t)width, (uint32_t)width,
                               (uint32_t)height);
            }
        }
    }
    if (ref->scaled == NULL) {
        return NULL;
    }
    ref->scaled_width = width;
    ref->scaled_height = height;
    ref->scaled_colour = ref->colour;
    return ref->scaled;
}

void image_ref_free(struct image_ref *ref) {
    free(ref->pixels);
    free(ref->scaled);
    free(ref->data_url);
    opos_html_free(ref->svg_doc);
    free(ref);
}

/* tint: draw only the image's shape (its alpha) in that colour. */
static void blend(uint32_t *target, int stride, int target_width,
                  int target_height, struct image_ref *ref, int x, int y,
                  int width, int height, int clip_left, int clip_top,
                  int clip_right, int clip_bottom, bool tinted,
                  uint32_t tint) {
    if (clip_left < 0) clip_left = 0;
    if (clip_top < 0) clip_top = 0;
    if (clip_right > target_width) clip_right = target_width;
    if (clip_bottom > target_height) clip_bottom = target_height;
    if (x >= clip_right || y >= clip_bottom || x + width <= clip_left
        || y + height <= clip_top) {
        return;
    }
    const uint32_t *pixels = image_ref_scaled(ref, width, height);
    if (pixels == NULL) {
        return;
    }
    int first_row = clip_top > y ? clip_top - y : 0;
    int last_row = clip_bottom < y + height ? clip_bottom - y : height;
    int first_col = clip_left > x ? clip_left - x : 0;
    int last_col = clip_right < x + width ? clip_right - x : width;
    for (int row = first_row; row < last_row; row++) {
        const uint32_t *source = pixels + (size_t)row * width;
        uint32_t *line = target + (size_t)(y + row) * stride + x;
        for (int col = first_col; col < last_col; col++) {
            uint32_t p = source[col];
            if (tinted) {
                p = (p & 0xff000000u) | (tint & 0x00ffffffu);
            }
            uint32_t alpha = p >> 24;
            if (alpha == 255) {
                line[col] = p & 0x00ffffff;
            } else if (alpha != 0) {
                uint32_t q = line[col];
                uint32_t r = (((p >> 16) & 255) * alpha
                              + ((q >> 16) & 255) * (255 - alpha)) / 255;
                uint32_t g = (((p >> 8) & 255) * alpha
                              + ((q >> 8) & 255) * (255 - alpha)) / 255;
                uint32_t b = ((p & 255) * alpha + (q & 255) * (255 - alpha))
                           / 255;
                line[col] = r << 16 | g << 8 | b;
            }
        }
    }
}

void image_blend(uint32_t *target, int stride, int target_width,
                 int target_height, struct image_ref *ref, int x, int y,
                 int width, int height, int clip_left, int clip_top,
                 int clip_right, int clip_bottom) {
    blend(target, stride, target_width, target_height, ref, x, y, width,
          height, clip_left, clip_top, clip_right, clip_bottom, false, 0);
}

/* Where the background image's tiles go in a box: the first tile's
   position and size, and whether it repeats each way. */
void background_tiles(const struct background *bg, struct image_ref *ref,
                      int box_x, int box_y, int box_width, int box_height,
                      int *tile_x, int *tile_y, int *tile_width,
                      int *tile_height) {
    int iw = ref->width > 0 ? ref->width : 1;
    int ih = ref->height > 0 ? ref->height : 1;
    int w = iw, h = ih;
    if (bg->size_kind == BG_SIZE_COVER || bg->size_kind == BG_SIZE_CONTAIN) {
        /* Scale (in 1/1024ths) to cover or fit the box. */
        long sx = (long)box_width * 1024 / iw, sy = (long)box_height * 1024 / ih;
        long s = bg->size_kind == BG_SIZE_COVER ? (sx > sy ? sx : sy)
                                                : (sx < sy ? sx : sy);
        w = (int)((iw * s + 1023) / 1024);
        h = (int)((ih * s + 1023) / 1024);
    } else if (bg->size_kind == BG_SIZE_EXPLICIT) {
        int sw = bg->size[0] < 0 ? -1
               : bg->size_pct[0] ? box_width * bg->size[0] / 100 : bg->size[0];
        int sh = bg->size[1] < 0 ? -1
               : bg->size_pct[1] ? box_height * bg->size[1] / 100 : bg->size[1];
        if (sw >= 0 && sh >= 0) {
            w = sw;
            h = sh;
        } else if (sw >= 0) {
            w = sw;
            h = (int)((long)sw * ih / iw);
        } else if (sh >= 0) {
            h = sh;
            w = (int)((long)sh * iw / ih);
        }
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    /* A percentage lines the same point of image and box up. */
    int px = bg->position_pct[0] ? (box_width - w) * bg->position[0] / 100
                                 : bg->position[0];
    int py = bg->position_pct[1] ? (box_height - h) * bg->position[1] / 100
                                 : bg->position[1];
    *tile_x = box_x + px;
    *tile_y = box_y + py;
    *tile_width = w;
    *tile_height = h;
}

void background_paint(uint32_t *target, int stride, int target_width,
                      int target_height, const struct display_item *item,
                      int x, int y, int clip_top, int clip_bottom) {
    struct image_ref *ref = item->image;
    if (ref == NULL || ref->state != 1 || item->bg == NULL
        || item->width <= 0 || item->height <= 0) {
        return;
    }
    int tx, ty, tw, th;
    background_tiles(item->bg, ref, x, y, item->width, item->height, &tx,
                     &ty, &tw, &th);
    int left = x, right = x + item->width;
    int top = y > clip_top ? y : clip_top;
    int bottom = y + item->height < clip_bottom ? y + item->height
                                                : clip_bottom;
    if (top >= bottom) {
        return;
    }
    /* The first and last tile columns and rows that touch the box. */
    int x0 = tx, x1 = tx + tw;
    if (!item->bg->no_repeat_x) {
        while (x0 > left) x0 -= tw;
        while (x0 + tw <= left) x0 += tw;
        x1 = right;
    }
    int y0 = ty, y1 = ty + th;
    if (!item->bg->no_repeat_y) {
        while (y0 > top) y0 -= th;
        while (y0 + th <= top) y0 += th;
        y1 = bottom;
    }
    int tiles = 0;
    for (int yy = y0; yy < y1 && yy < bottom; yy += th) {
        for (int xx = x0; xx < x1 && xx < right; xx += tw) {
            blend(target, stride, target_width, target_height, ref, xx, yy,
                  tw, th, left, top, right, bottom, item->bg->mask,
                  item->bg->mask_colour);
            if (++tiles > 4096) {
                return; /* tiny tiles over a huge box */
            }
        }
    }
}
