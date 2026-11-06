/*
 * TrueType text for OPOS applications, on top of stb_truetype (public
 * domain, sdk/stb/). Fonts are ordinary .ttf files; OPOS ships Noto Sans
 * and Noto Sans Mono (SIL Open Font License, see data/fonts/OFL.txt) in
 * /share/fonts.
 *
 *   struct opos_font *font = opos_font_load("/share/fonts/NotoSans-Regular.ttf");
 *   int width = opos_font_text_width(font, 16, "Hello", 5);
 *   opos_font_draw(font, &canvas, x, baseline, 16, "Hello", 5, OPOS_RGB(0,0,0));
 *
 * Sizes are pixel heights (the em square). Text is UTF-8. Glyph bitmaps
 * are cached per font, size and character, so drawing the same text again
 * is cheap. Glyphs are placed at whole pixels; widths are measured in
 * fractional pixels and rounded, so a line measures as it draws.
 */
#ifndef OPOS_FONT_H
#define OPOS_FONT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opos_gfx.h>

struct opos_font;

/* Reads the file and parses it. NULL when the file is missing or is not a
   TrueType font (OpenType with CFF outlines is not supported). */
struct opos_font *opos_font_load(const char *path);
void opos_font_free(struct opos_font *font);

/* Vertical metrics at a size, in pixels: ascent above the baseline,
   descent below it (positive), and the distance between baselines. */
int opos_font_ascent(const struct opos_font *font, int size);
int opos_font_descent(const struct opos_font *font, int size);
int opos_font_line_height(const struct opos_font *font, int size);

/* Decodes one UTF-8 character at text; returns the bytes used (at least 1,
   at most length) and stores the code point, or U+FFFD for bad bytes. */
size_t opos_utf8_decode(const char *text, size_t length, uint32_t *code);

/* True when the font has a glyph for the code point. */
bool opos_font_has_glyph(const struct opos_font *font, uint32_t code);

/* Advance of one character in 1/64 pixel. */
int opos_font_advance(struct opos_font *font, int size, uint32_t code);

/* Width of UTF-8 text in whole pixels (control characters have no width;
   kerning pairs are applied). */
int opos_font_text_width(struct opos_font *font, int size, const char *text,
                         size_t length);

/* Draws UTF-8 text with its baseline at y and returns the x after it.
   Everything clips to the canvas. colour is 0x00RRGGBB. */
int opos_font_draw(struct opos_font *font, struct opos_canvas *canvas, int x,
                   int baseline, int size, const char *text, size_t length,
                   uint32_t colour);

#endif
