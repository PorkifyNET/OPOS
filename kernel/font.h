#ifndef OPOS_FONT_H
#define OPOS_FONT_H

#include <stddef.h>
#include <stdint.h>

/* Shared bitmap font used by every framebuffer text renderer. */
#define FONT_WIDTH 5
#define FONT_HEIGHT 9
#define FONT_FIRST_CHARACTER 0x20
#define FONT_LAST_CHARACTER 0x7e
#define FONT_GLYPH_COUNT (FONT_LAST_CHARACTER - FONT_FIRST_CHARACTER + 1)

extern const uint8_t font_glyphs[FONT_GLYPH_COUNT][FONT_HEIGHT];

/* Returns the glyph rows for a printable ASCII character, or NULL. */
static inline const uint8_t *font_glyph(char character) {
    uint8_t code = (uint8_t)character;
    if (code < FONT_FIRST_CHARACTER || code > FONT_LAST_CHARACTER) {
        return NULL;
    }
    return font_glyphs[code - FONT_FIRST_CHARACTER];
}

#endif
