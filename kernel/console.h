#ifndef OPOS_CONSOLE_H
#define OPOS_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void console_write_string(const char *string);
/* To the serial port only: for logs that must not draw over the desktop. */
void console_serial_log(const char *string);
void console_write_bytes(const char *data, size_t length);
void console_write_hex(uint64_t value);
void console_clear(void);

/*
 * Text colour for the kernel console. Error and warning colours apply only
 * while the "coloured terminal messages" setting is on; otherwise text stays
 * the normal colour.
 */
enum console_text_style {
    CONSOLE_TEXT_NORMAL,
    CONSOLE_TEXT_ERROR,
    CONSOLE_TEXT_WARNING,
};
void console_set_text_style(enum console_text_style style);
/* Write a whole message in the error/warning colour, then back to normal. */
void console_write_error(const char *string);
void console_write_warning(const char *string);
void console_draw_rectangle(uint64_t x, uint64_t y, uint64_t width, uint64_t height,
                            uint8_t red, uint8_t green, uint8_t blue);
void console_draw_text(uint64_t x, uint64_t y, const char *text,
                       uint8_t red, uint8_t green, uint8_t blue);
/* Copies 0x00RRGGBB pixels (stride in pixels) to the screen, clipped. */
void console_draw_pixels(uint64_t x, uint64_t y, uint64_t width,
                         uint64_t height, const uint32_t *pixels,
                         uint64_t stride);
/* Limit desktop drawing (rectangles and text) to one rectangle. */
void console_set_clip(uint64_t x, uint64_t y, uint64_t width, uint64_t height);
void console_clear_clip(void);
uint64_t console_screen_width(void);
uint64_t console_screen_height(void);
void console_cursor_refresh(void);
/*
 * Redirect drawing to an off-screen back buffer until console_frame_present()
 * copies it to the screen. Returns false (drawing stays direct) if no buffer.
 */
bool console_frame_begin(void);
void console_frame_present(void);

/* A 32 bpp linear framebuffer mode. */
struct console_mode {
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint8_t red_shift, red_size;
    uint8_t green_shift, green_size;
    uint8_t blue_shift, blue_size;
};
/* The mode Limine set up at boot. */
void console_boot_mode(struct console_mode *mode);
/* Switches drawing to a new framebuffer, clears it and resets the text
 * cursor. Not allowed between console_frame_begin and _present. */
bool console_set_mode(const struct console_mode *mode);
/* True (once) if visible pixels changed since the last call. */
bool console_take_dirty(void);
uint64_t console_mode_changes(void);

#endif
