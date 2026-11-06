/*
 * TrueType text: see include/opos_font.h. The rasteriser is stb_truetype
 * (sdk/stb/stb_truetype.h, public domain / MIT).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_font.h>

/* stb_truetype uses these math functions only for outlines and (unused
   here) signed distance fields; small versions are enough. */
static inline double ttf_floor(double x) {
    long i = (long)x;
    return (double)(i - ((double)i > x ? 1 : 0));
}
static inline double ttf_ceil(double x) {
    long i = (long)x;
    return (double)(i + ((double)i < x ? 1 : 0));
}
static inline double ttf_sqrt(double x) {
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}
static inline double ttf_fabs(double x) { return x < 0 ? -x : x; }
static inline double ttf_fmod(double x, double y) {
    return x - ttf_floor(x / y) * y;
}
/* Only reached by the SDF functions, which nothing here calls. */
static inline double ttf_unused(double x, double y) {
    (void)x;
    (void)y;
    return 0;
}

#define STBTT_ifloor(x) ((int)ttf_floor(x))
#define STBTT_iceil(x) ((int)ttf_ceil(x))
#define STBTT_sqrt(x) ttf_sqrt(x)
#define STBTT_fabs(x) ttf_fabs(x)
#define STBTT_fmod(x, y) ttf_fmod(x, y)
#define STBTT_pow(x, y) ttf_unused(x, y)
#define STBTT_cos(x) ttf_unused(x, 0)
#define STBTT_acos(x) ttf_unused(x, 0)
#define STBTT_malloc(size, user) malloc(size)
#define STBTT_free(pointer, user) free(pointer)
#define STBTT_assert(condition) ((void)0)
#define STBTT_strlen(text) strlen(text)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "../stb/stb_truetype.h"
#pragma GCC diagnostic pop

#define CACHE_SLOTS 4096 /* power of two */
#define CACHE_LIMIT (CACHE_SLOTS * 3 / 4)
#define FILE_MAX (8u * 1024 * 1024)

struct glyph {
    uint32_t code;      /* 0 = empty slot */
    uint16_t size;
    int16_t width;
    int16_t height;
    int16_t x_offset;
    int16_t y_offset;
    int advance;        /* 1/64 pixel */
    uint8_t *bitmap;    /* width * height coverage, or NULL if empty */
};

struct opos_font {
    stbtt_fontinfo info;
    uint8_t *data;
    struct glyph *cache;
    size_t cached;
    uint32_t fallback; /* code point drawn for missing characters */
};

struct opos_font *opos_font_load(const char *path) {
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t capacity = 128 * 1024;
    size_t length = 0;
    uint8_t *data = malloc(capacity);
    while (data != NULL) {
        if (length == capacity) {
            if (capacity >= FILE_MAX) {
                free(data);
                data = NULL;
                break;
            }
            uint8_t *grown = realloc(data, capacity * 2);
            if (grown == NULL) {
                free(data);
                data = NULL;
                break;
            }
            data = grown;
            capacity *= 2;
        }
        size_t piece = capacity - length;
        long got = opos_read((int)fd, data + length, piece > 4096 ? 4096 : piece);
        if (got < 0) {
            free(data);
            data = NULL;
            break;
        }
        if (got == 0) {
            break;
        }
        length += (size_t)got;
    }
    opos_close((int)fd);
    if (data == NULL) {
        return NULL;
    }
    struct opos_font *font = calloc(1, sizeof(*font));
    if (font == NULL) {
        free(data);
        return NULL;
    }
    font->data = data;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&font->info, data, offset)) {
        free(data);
        free(font);
        return NULL;
    }
    font->fallback = stbtt_FindGlyphIndex(&font->info, 0xfffd) != 0
        ? 0xfffd : '?';
    return font;
}

static void cache_clear(struct opos_font *font) {
    if (font->cache == NULL) {
        return;
    }
    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        if (font->cache[i].bitmap != NULL) {
            stbtt_FreeBitmap(font->cache[i].bitmap, NULL);
        }
    }
    memset(font->cache, 0, CACHE_SLOTS * sizeof(struct glyph));
    font->cached = 0;
}

void opos_font_free(struct opos_font *font) {
    if (font == NULL) {
        return;
    }
    cache_clear(font);
    free(font->cache);
    free(font->data);
    free(font);
}

static float scale_for(const struct opos_font *font, int size) {
    return stbtt_ScaleForMappingEmToPixels(&font->info, (float)size);
}

int opos_font_ascent(const struct opos_font *font, int size) {
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
    return (int)(ascent * scale_for(font, size) + 0.5f);
}

int opos_font_descent(const struct opos_font *font, int size) {
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
    return (int)(-descent * scale_for(font, size) + 0.5f);
}

int opos_font_line_height(const struct opos_font *font, int size) {
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
    return (int)((ascent - descent + gap) * scale_for(font, size) + 0.5f);
}

size_t opos_utf8_decode(const char *text, size_t length, uint32_t *code) {
    unsigned char c = (unsigned char)text[0];
    if (c < 0x80) {
        *code = c;
        return 1;
    }
    size_t need = c >= 0xc2 && c < 0xe0 ? 1 : c >= 0xe0 && c < 0xf0 ? 2
                : c >= 0xf0 && c < 0xf5 ? 3 : 0;
    if (need == 0 || need >= length) {
        *code = 0xfffd;
        return 1;
    }
    uint32_t value = c & (0x3f >> need);
    for (size_t i = 1; i <= need; i++) {
        unsigned char d = (unsigned char)text[i];
        if ((d & 0xc0) != 0x80) {
            *code = 0xfffd;
            return 1;
        }
        value = value << 6 | (d & 0x3f);
    }
    if ((need == 2 && value < 0x800) || (need == 3 && value < 0x10000)
        || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        *code = 0xfffd;
        return 1;
    }
    *code = value;
    return need + 1;
}

bool opos_font_has_glyph(const struct opos_font *font, uint32_t code) {
    return stbtt_FindGlyphIndex(&font->info, (int)code) != 0;
}

/* Finds or renders the glyph for (code, size). */
static struct glyph *glyph_get(struct opos_font *font, int size,
                               uint32_t code) {
    if (font->cache == NULL) {
        font->cache = calloc(CACHE_SLOTS, sizeof(struct glyph));
        if (font->cache == NULL) {
            return NULL;
        }
    }
    if (font->cached >= CACHE_LIMIT) {
        cache_clear(font);
    }
    size_t slot = (code * 2654435761u + (uint32_t)size * 40503u)
                & (CACHE_SLOTS - 1);
    for (;;) {
        struct glyph *g = &font->cache[slot];
        if (g->code == 0) {
            break;
        }
        if (g->code == code && g->size == size) {
            return g;
        }
        slot = (slot + 1) & (CACHE_SLOTS - 1);
    }
    uint32_t render = code;
    if (stbtt_FindGlyphIndex(&font->info, (int)code) == 0) {
        render = font->fallback;
    }
    float scale = scale_for(font, size);
    struct glyph *g = &font->cache[slot];
    int advance, bearing;
    stbtt_GetCodepointHMetrics(&font->info, (int)render, &advance, &bearing);
    int width = 0, height = 0, x_offset = 0, y_offset = 0;
    uint8_t *bitmap = stbtt_GetCodepointBitmap(&font->info, scale, scale,
                                               (int)render, &width, &height,
                                               &x_offset, &y_offset);
    g->code = code;
    g->size = (uint16_t)size;
    g->advance = (int)(advance * scale * 64.0f + 0.5f);
    g->bitmap = bitmap;
    g->width = (int16_t)(bitmap != NULL ? width : 0);
    g->height = (int16_t)(bitmap != NULL ? height : 0);
    g->x_offset = (int16_t)x_offset;
    g->y_offset = (int16_t)y_offset;
    font->cached++;
    return g;
}

int opos_font_advance(struct opos_font *font, int size, uint32_t code) {
    struct glyph *g = glyph_get(font, size, code);
    return g != NULL ? g->advance : 0;
}

static int kerning(struct opos_font *font, int size, uint32_t left,
                   uint32_t right) {
    int units = stbtt_GetCodepointKernAdvance(&font->info, (int)left,
                                              (int)right);
    if (units == 0) {
        return 0;
    }
    float scaled = units * scale_for(font, size) * 64.0f;
    return (int)(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
}

int opos_font_text_width(struct opos_font *font, int size, const char *text,
                         size_t length) {
    long pen = 0;
    uint32_t previous = 0;
    for (size_t i = 0; i < length;) {
        uint32_t code;
        i += opos_utf8_decode(text + i, length - i, &code);
        if (code < 0x20 || code == 0x7f) {
            continue;
        }
        if (previous != 0) {
            pen += kerning(font, size, previous, code);
        }
        pen += opos_font_advance(font, size, code);
        previous = code;
    }
    return (int)((pen + 32) >> 6);
}

int opos_font_draw(struct opos_font *font, struct opos_canvas *canvas, int x,
                   int baseline, int size, const char *text, size_t length,
                   uint32_t colour) {
    long pen = (long)x * 64;
    uint32_t previous = 0;
    int red = (int)(colour >> 16 & 0xff);
    int green = (int)(colour >> 8 & 0xff);
    int blue = (int)(colour & 0xff);
    for (size_t i = 0; i < length;) {
        uint32_t code;
        i += opos_utf8_decode(text + i, length - i, &code);
        if (code < 0x20 || code == 0x7f) {
            continue;
        }
        if (previous != 0) {
            pen += kerning(font, size, previous, code);
        }
        previous = code;
        struct glyph *g = glyph_get(font, size, code);
        if (g == NULL) {
            continue;
        }
        if (g->bitmap != NULL) {
            int left = (int)((pen + 32) >> 6) + g->x_offset;
            int top = baseline + g->y_offset;
            for (int row = 0; row < g->height; row++) {
                int py = top + row;
                if (py < 0 || py >= canvas->height) {
                    continue;
                }
                uint32_t *line = canvas->pixels + (size_t)py * canvas->stride;
                const uint8_t *source = g->bitmap + (size_t)row * g->width;
                for (int column = 0; column < g->width; column++) {
                    int px = left + column;
                    unsigned alpha = source[column];
                    if (alpha == 0 || px < 0 || px >= canvas->width) {
                        continue;
                    }
                    if (alpha == 255) {
                        line[px] = colour;
                        continue;
                    }
                    uint32_t under = line[px];
                    int r = (int)(under >> 16 & 0xff);
                    int gr = (int)(under >> 8 & 0xff);
                    int b = (int)(under & 0xff);
                    r += (red - r) * (int)alpha / 255;
                    gr += (green - gr) * (int)alpha / 255;
                    b += (blue - b) * (int)alpha / 255;
                    line[px] = OPOS_RGB(r, gr, b);
                }
            }
        }
        pen += g->advance;
    }
    return (int)((pen + 32) >> 6);
}
