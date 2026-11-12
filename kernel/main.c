#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

#include "interrupts.h"
#include "ahci.h"
#include "block.h"
#include "initramfs.h"
#include "io.h"
#include "keyboard.h"
#include "memory.h"
#include "mouse.h"
#include "i2c_hid.h"
#include "paging.h"
#include "power.h"
#include "scheduler.h"
#include "pci.h"
#include "shell.h"
#include "cmos.h"
#include "desktop.h"
#include "timer.h"
#include "update.h"
#include "accounts.h"
#include "package.h"
#include "user.h"
#include "ata.h"
#include "console.h"
#include "fat16.h"
#include "nvme.h"
#include "buslogic.h"
#include "mpt.h"
#include "pvscsi.h"
#include "svga.h"
#include "vmware.h"
#include "font.h"
#include "installer.h"
#include "iso9660.h"
#include "storage.h"
#include "system_root.h"
#include "vfs.h"
#include "xhci.h"
#include "optical.h"
#include "boot_display.h"
#include "sdhci.h"
#include "hda.h"
#include "net.h"
#include "sound.h"
#include "nic.h"

#define COM1 0x3f8
#define GLYPH_SCALE 2
#define TEXT_MARGIN 40
#define TEXT_LINE_HEIGHT 20
#define PAGE_SIZE 4096
#define PAGING_TEST_ADDRESS 0xffffc00000001000ULL

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 1
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memory_map_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

static struct limine_internal_module user_program_module = {
    .path = "initramfs.tar",
    .string = "initramfs",
    .flags = LIMINE_INTERNAL_MODULE_REQUIRED
};

/* Only the live ISO has install.tar; an installed disk boots without it. */
static struct limine_internal_module install_module = {
    .path = "install.tar",
    .string = "install",
    .flags = 0
};

static struct limine_internal_module *user_program_modules[] = {
    &user_program_module,
    &install_module
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 1,
    .internal_module_count = 2,
    .internal_modules = user_program_modules
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_file_request executable_file_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t limine_requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t limine_requests_end[] = LIMINE_REQUESTS_END_MARKER;

/*
 * active_framebuffer points at framebuffer_state, a copy of what Limine
 * reported, so a display driver can switch modes by changing the copy.
 * boot_framebuffer keeps the original for "boot default".
 */
static struct limine_framebuffer framebuffer_state;
static struct limine_framebuffer boot_framebuffer;
static struct limine_framebuffer *active_framebuffer;
/* Set whenever visible pixels change; display drivers poll it. */
static volatile bool display_dirty;
static uint64_t text_x = TEXT_MARGIN;
static uint64_t text_y = TEXT_MARGIN;
static int32_t cursor_x;
static int32_t cursor_y;
static bool cursor_drawn;
/*
 * Drawing target. Normally the framebuffer itself; between
 * console_frame_begin() and console_frame_present() it is an off-screen back
 * buffer so a full redraw reaches the screen in one copy without flicker.
 */
static uint8_t *draw_base;
static uint64_t draw_pitch;
static uint32_t *back_buffer;
static uint64_t back_buffer_pixels;
static bool back_buffer_failed;
static bool frame_active;
/*
 * What the framebuffer shows, kept in RAM so a present writes only the
 * pixels that changed. Video memory is slow to write (and very slow when it
 * is uncached), so copying a whole frame for every typed key used to keep
 * the processor busy. front_valid drops whenever anything draws on the
 * screen directly, which forces the next present to copy everything.
 */
static uint32_t *front_buffer;
static uint64_t front_buffer_pixels;
static bool front_buffer_failed;
static bool front_valid;
static uint64_t front_width;
static uint64_t front_height;
static const char cursor_shape[][11] = {
    "X........",
    "XX.......",
    "X.X......",
    "X..X.....",
    "X...X....",
    "X....X...",
    "X.....X..",
    "X......X.",
    "X.......X",
    "X....XXXX",
    "X...X.....",
    "X..X......",
};

static void serial_initialize(void) {
    io_out8(COM1 + 1, 0x00);
    io_out8(COM1 + 3, 0x80);
    io_out8(COM1 + 0, 0x03);
    io_out8(COM1 + 1, 0x00);
    io_out8(COM1 + 3, 0x03);
    io_out8(COM1 + 2, 0xc7);
    io_out8(COM1 + 4, 0x0b);
}

static void serial_write_character(char character) {
    for (uint32_t timeout = 0; timeout < 1000000; timeout++) {
        if ((io_in8(COM1 + 5) & 0x20) != 0) {
            io_out8(COM1, (uint8_t)character);
            return;
        }
    }
}

/*
 * The console's copy on serial is kept one line at a time and written at
 * the newline, so Backspace edits the buffered line instead of sending
 * "\b \b": a serial log read as a text file then shows the command as it
 * was finally typed. Direct serial writes flush the pending line first.
 */
static char serial_line[256];
static size_t serial_line_length;

static void serial_line_flush(void) {
    for (size_t i = 0; i < serial_line_length; i++) {
        serial_write_character(serial_line[i]);
    }
    serial_line_length = 0;
}

static void serial_line_put(char character) {
    if (serial_line_length == sizeof(serial_line)) {
        serial_line_flush();
    }
    serial_line[serial_line_length++] = character;
}

static void serial_write_string(const char *string) {
    serial_line_flush();
    while (*string != '\0') {
        if (*string == '\n') {
            serial_write_character('\r');
        }
        serial_write_character(*string++);
    }
}

void console_serial_log(const char *string) {
    serial_write_string(string);
}

static void serial_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    serial_write_string("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        serial_write_character(digits[(value >> shift) & 0x0f]);
    }
}

static uint32_t framebuffer_channel(uint8_t value, uint8_t mask_size, uint8_t mask_shift) {
    uint32_t maximum = (1u << mask_size) - 1;
    return ((uint32_t)value * maximum / 255) << mask_shift;
}

static uint32_t framebuffer_color(struct limine_framebuffer *framebuffer,
                                  uint8_t red, uint8_t green, uint8_t blue) {
    return framebuffer_channel(red, framebuffer->red_mask_size, framebuffer->red_mask_shift)
         | framebuffer_channel(green, framebuffer->green_mask_size, framebuffer->green_mask_shift)
         | framebuffer_channel(blue, framebuffer->blue_mask_size, framebuffer->blue_mask_shift);
}

static void halt(void) __attribute__((noreturn));

static void framebuffer_fill(uint32_t color) {
    for (uint64_t y = 0; y < active_framebuffer->height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)
            (draw_base + y * draw_pitch);
        for (uint64_t x = 0; x < active_framebuffer->width; x++) {
            row[x] = color;
        }
    }
}

static void framebuffer_toggle_cursor(int32_t x, int32_t y) {
    if (active_framebuffer == NULL || x < 0 || y < 0) {
        return;
    }

    uint32_t color_mask = framebuffer_color(active_framebuffer, 255, 255, 255);
    for (size_t row = 0; row < sizeof(cursor_shape) / sizeof(cursor_shape[0]); row++) {
        uint64_t pixel_y = (uint64_t)y + row;
        if (pixel_y >= active_framebuffer->height) {
            break;
        }
        volatile uint32_t *pixels = (volatile uint32_t *)
            ((uint8_t *)active_framebuffer->address
             + pixel_y * active_framebuffer->pitch);
        for (size_t column = 0; column < sizeof(cursor_shape[row]) - 1; column++) {
            uint64_t pixel_x = (uint64_t)x + column;
            if (pixel_x >= active_framebuffer->width) {
                break;
            }
            if (cursor_shape[row][column] == 'X') {
                pixels[pixel_x] ^= color_mask;
            }
        }
    }
}

static void framebuffer_cursor_hide(void) {
    if (frame_active) {
        return;
    }
    if (cursor_drawn) {
        framebuffer_toggle_cursor(cursor_x, cursor_y);
        cursor_drawn = false;
    }
}

static void framebuffer_cursor_show(void) {
    struct mouse_state state;
    if (!frame_active) {
        /* Every direct-draw path ends here, so this marks the screen dirty. */
        display_dirty = true;
        front_valid = false;
    }
    if (!frame_active && mouse_read_state(&state)) {
        cursor_x = state.x;
        cursor_y = state.y;
        framebuffer_toggle_cursor(cursor_x, cursor_y);
        cursor_drawn = true;
    }
}

void console_cursor_refresh(void) {
    struct mouse_state state;
    if (frame_active) {
        return;
    }
    if (!mouse_read_state(&state)
        || (cursor_drawn && cursor_x == state.x && cursor_y == state.y)) {
        return;
    }
    framebuffer_cursor_hide();
    cursor_x = state.x;
    cursor_y = state.y;
    framebuffer_toggle_cursor(cursor_x, cursor_y);
    cursor_drawn = true;
    display_dirty = true;
}

/* Desktop clip rectangle, [left, right) x [top, bottom); open by default. */
static uint64_t clip_left;
static uint64_t clip_top;
static uint64_t clip_right = UINT64_MAX;
static uint64_t clip_bottom = UINT64_MAX;

void console_set_clip(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    clip_left = x;
    clip_top = y;
    clip_right = width > UINT64_MAX - x ? UINT64_MAX : x + width;
    clip_bottom = height > UINT64_MAX - y ? UINT64_MAX : y + height;
}

void console_clear_clip(void) {
    clip_left = 0;
    clip_top = 0;
    clip_right = UINT64_MAX;
    clip_bottom = UINT64_MAX;
}

static void framebuffer_draw_character_at(char character, uint64_t x, uint64_t y,
                                          uint32_t color) {
    const uint8_t *glyph = font_glyph(character);
    if (glyph == NULL) {
        return;
    }
    uint64_t glyph_width = FONT_WIDTH * GLYPH_SCALE;
    uint64_t glyph_height = FONT_HEIGHT * GLYPH_SCALE;
    if (x >= clip_right || y >= clip_bottom
        || x + glyph_width <= clip_left || y + glyph_height <= clip_top) {
        return;
    }
    bool partial = x < clip_left || y < clip_top
                || x + glyph_width > clip_right || y + glyph_height > clip_bottom;

    for (uint64_t row = 0; row < FONT_HEIGHT; row++) {
        for (uint64_t column = 0; column < FONT_WIDTH; column++) {
            if ((glyph[row] & (1u << (FONT_WIDTH - 1 - column))) == 0) {
                continue;
            }

            for (uint64_t dy = 0; dy < GLYPH_SCALE; dy++) {
                uint64_t pixel_y = y + row * GLYPH_SCALE + dy;
                if (partial && (pixel_y < clip_top || pixel_y >= clip_bottom)) {
                    continue;
                }
                volatile uint32_t *pixels = (volatile uint32_t *)
                    (draw_base + pixel_y * draw_pitch);
                for (uint64_t dx = 0; dx < GLYPH_SCALE; dx++) {
                    uint64_t pixel_x = x + column * GLYPH_SCALE + dx;
                    if (partial
                        && (pixel_x < clip_left || pixel_x >= clip_right)) {
                        continue;
                    }
                    pixels[pixel_x] = color;
                }
            }
        }
    }
}

static enum console_text_style text_style = CONSOLE_TEXT_NORMAL;

void console_set_text_style(enum console_text_style style) {
    if (style != CONSOLE_TEXT_NORMAL && !settings_get(SETTING_TERMINAL_COLORS)) {
        style = CONSOLE_TEXT_NORMAL;
    }
    text_style = style;
}

void console_write_error(const char *string) {
    console_set_text_style(CONSOLE_TEXT_ERROR);
    console_write_string(string);
    console_set_text_style(CONSOLE_TEXT_NORMAL);
}

void console_write_warning(const char *string) {
    console_set_text_style(CONSOLE_TEXT_WARNING);
    console_write_string(string);
    console_set_text_style(CONSOLE_TEXT_NORMAL);
}

static void framebuffer_draw_character(char character) {
    /* Same colours as the desktop terminal. */
    uint32_t color = text_style == CONSOLE_TEXT_ERROR
        ? framebuffer_color(active_framebuffer, 255, 110, 110)
        : text_style == CONSOLE_TEXT_WARNING
        ? framebuffer_color(active_framebuffer, 240, 205, 100)
        : framebuffer_color(active_framebuffer, 230, 238, 255);
    framebuffer_draw_character_at(character, text_x, text_y, color);
}

void console_draw_rectangle(uint64_t x, uint64_t y, uint64_t width, uint64_t height,
                            uint8_t red, uint8_t green, uint8_t blue) {
    if (active_framebuffer == NULL) {
        return;
    }
    if (x < clip_left) {
        uint64_t cut = clip_left - x;
        width = width > cut ? width - cut : 0;
        x = clip_left;
    }
    if (y < clip_top) {
        uint64_t cut = clip_top - y;
        height = height > cut ? height - cut : 0;
        y = clip_top;
    }
    if (x >= clip_right || y >= clip_bottom || width == 0 || height == 0) {
        return;
    }
    if (width > clip_right - x) {
        width = clip_right - x;
    }
    if (height > clip_bottom - y) {
        height = clip_bottom - y;
    }
    if (x >= active_framebuffer->width || y >= active_framebuffer->height) {
        return;
    }

    if (width > active_framebuffer->width - x) {
        width = active_framebuffer->width - x;
    }
    if (height > active_framebuffer->height - y) {
        height = active_framebuffer->height - y;
    }
    uint32_t color = framebuffer_color(active_framebuffer, red, green, blue);
    framebuffer_cursor_hide();
    for (uint64_t row = 0; row < height; row++) {
        volatile uint32_t *pixels = (volatile uint32_t *)
            (draw_base + (y + row) * draw_pitch);
        for (uint64_t column = 0; column < width; column++) {
            pixels[x + column] = color;
        }
    }
    framebuffer_cursor_show();
}

void console_draw_pixels(uint64_t x, uint64_t y, uint64_t width,
                         uint64_t height, const uint32_t *pixels,
                         uint64_t stride) {
    if (active_framebuffer == NULL || pixels == NULL
        || active_framebuffer->bpp != 32) {
        return;
    }
    uint64_t skip_x = 0;
    uint64_t skip_y = 0;
    if (x < clip_left) {
        skip_x = clip_left - x;
        x = clip_left;
    }
    if (y < clip_top) {
        skip_y = clip_top - y;
        y = clip_top;
    }
    if (skip_x >= width || skip_y >= height) {
        return;
    }
    width -= skip_x;
    height -= skip_y;
    uint64_t right = clip_right < active_framebuffer->width
        ? clip_right : active_framebuffer->width;
    uint64_t bottom = clip_bottom < active_framebuffer->height
        ? clip_bottom : active_framebuffer->height;
    if (x >= right || y >= bottom) {
        return;
    }
    if (width > right - x) {
        width = right - x;
    }
    if (height > bottom - y) {
        height = bottom - y;
    }
    /* Pixels are 0x00RRGGBB; most framebuffers use exactly that. */
    bool direct = active_framebuffer->red_mask_shift == 16
        && active_framebuffer->green_mask_shift == 8
        && active_framebuffer->blue_mask_shift == 0
        && active_framebuffer->red_mask_size == 8
        && active_framebuffer->green_mask_size == 8
        && active_framebuffer->blue_mask_size == 8;
    framebuffer_cursor_hide();
    for (uint64_t row = 0; row < height; row++) {
        const uint32_t *source = pixels + (skip_y + row) * stride + skip_x;
        volatile uint32_t *target = (volatile uint32_t *)
            (draw_base + (y + row) * draw_pitch) + x;
        if (direct) {
            for (uint64_t column = 0; column < width; column++) {
                target[column] = source[column] & 0x00ffffffu;
            }
        } else {
            for (uint64_t column = 0; column < width; column++) {
                uint32_t value = source[column];
                target[column] = framebuffer_color(
                    active_framebuffer, (uint8_t)(value >> 16),
                    (uint8_t)(value >> 8), (uint8_t)value);
            }
        }
    }
    framebuffer_cursor_show();
}

void console_draw_text(uint64_t x, uint64_t y, const char *text,
                       uint8_t red, uint8_t green, uint8_t blue) {
    if (active_framebuffer == NULL || text == NULL) {
        return;
    }
    uint32_t color = framebuffer_color(active_framebuffer, red, green, blue);
    uint64_t character_width = (FONT_WIDTH + 1) * GLYPH_SCALE;
    framebuffer_cursor_hide();
    while (*text != '\0') {
        if (x + character_width > active_framebuffer->width
            || y + FONT_HEIGHT * GLYPH_SCALE > active_framebuffer->height) {
            break;
        }
        framebuffer_draw_character_at(*text++, x, y, color);
        x += character_width;
    }
    framebuffer_cursor_show();
}

bool console_frame_begin(void) {
    if (active_framebuffer == NULL || frame_active) {
        return frame_active;
    }
    uint64_t pixels = active_framebuffer->width * active_framebuffer->height;
    if (back_buffer != NULL && back_buffer_pixels < pixels) {
        /* Mode grew. The contiguous allocator cannot free, so the old
           buffer is abandoned; mode changes are rare. */
        back_buffer = NULL;
        back_buffer_failed = false;
    }
    if (back_buffer == NULL && !back_buffer_failed) {
        uint64_t bytes = pixels * sizeof(uint32_t);
        uint64_t physical = 0;
        if (back_buffer_pixels != 0 && pixels < 3840ULL * 2160) {
            /* A second allocation means the mode grew (e.g. the host window
               is being resized). Reserve room up to 4K so further growth
               does not abandon yet another buffer. */
            uint64_t room = 3840ULL * 2160 * sizeof(uint32_t);
            physical = physical_pages_alloc_contiguous((room + 4095) / 4096);
            if (physical != 0) {
                pixels = 3840ULL * 2160;
            }
        }
        if (physical == 0) {
            physical = physical_pages_alloc_contiguous((bytes + 4095) / 4096);
        }
        back_buffer = physical == 0 ? NULL
                                    : (uint32_t *)physical_page_to_hhdm(physical);
        back_buffer_pixels = back_buffer == NULL ? 0 : pixels;
        back_buffer_failed = back_buffer == NULL;
    }
    if (back_buffer == NULL) {
        return false;
    }
    if (front_buffer != NULL && front_buffer_pixels < pixels) {
        front_buffer = NULL;
        front_buffer_failed = false;
        front_valid = false;
    }
    if (front_buffer == NULL && !front_buffer_failed) {
        /* Same size as the back buffer, so growth up to it needs no new
           allocation either. Without it presents copy whole frames. */
        uint64_t room = back_buffer_pixels;
        uint64_t physical = physical_pages_alloc_contiguous(
            (room * sizeof(uint32_t) + 4095) / 4096);
        front_buffer = physical == 0 ? NULL
                                     : (uint32_t *)physical_page_to_hhdm(physical);
        front_buffer_pixels = front_buffer == NULL ? 0 : room;
        front_buffer_failed = front_buffer == NULL;
        front_valid = false;
    }
    framebuffer_cursor_hide();
    frame_active = true;
    draw_base = (uint8_t *)back_buffer;
    draw_pitch = active_framebuffer->width * sizeof(uint32_t);
    return true;
}

void console_frame_present(void) {
    if (!frame_active) {
        return;
    }
    frame_active = false;
    draw_base = (uint8_t *)active_framebuffer->address;
    draw_pitch = active_framebuffer->pitch;
    uint64_t width = active_framebuffer->width;
    uint64_t height = active_framebuffer->height;
    bool shadow = front_buffer != NULL && width * height <= front_buffer_pixels;
    bool full = !shadow || !front_valid || front_width != width
        || front_height != height;
    for (uint64_t y = 0; y < height; y++) {
        const uint32_t *source = back_buffer + y * width;
        volatile uint32_t *destination = (volatile uint32_t *)
            (draw_base + y * draw_pitch);
        uint64_t first = 0;
        uint64_t last = width;
        if (!full) {
            /* Only the span between the first and last changed pixel. */
            uint32_t *shown = front_buffer + y * width;
            while (first < width && shown[first] == source[first]) {
                first++;
            }
            if (first == width) {
                continue;
            }
            while (last > first && shown[last - 1] == source[last - 1]) {
                last--;
            }
        }
        uint64_t x = first;
        if ((x & 1) != 0 && x < last) {
            destination[x] = source[x];
            x++;
        }
        for (; x + 1 < last; x += 2) {
            ((volatile uint64_t *)destination)[x / 2] =
                ((const uint64_t *)source)[x / 2];
        }
        if (x < last) {
            destination[x] = source[x];
        }
        if (shadow) {
            uint32_t *shown = front_buffer + y * width;
            for (uint64_t i = first; i < last; i++) {
                shown[i] = source[i];
            }
        }
    }
    cursor_drawn = false;
    framebuffer_cursor_show();
    /* The cursor is drawn over the frame and removed again before the next
       one, so the shadow still matches what lies under it. */
    front_valid = shadow;
    front_width = width;
    front_height = height;
}

uint64_t console_screen_width(void) {
    return active_framebuffer == NULL ? 0 : active_framebuffer->width;
}

uint64_t console_screen_height(void) {
    return active_framebuffer == NULL ? 0 : active_framebuffer->height;
}

void console_boot_mode(struct console_mode *mode) {
    if (mode == NULL) {
        return;
    }
    *mode = (struct console_mode){
        .address = boot_framebuffer.address,
        .width = boot_framebuffer.width,
        .height = boot_framebuffer.height,
        .pitch = boot_framebuffer.pitch,
        .red_shift = boot_framebuffer.red_mask_shift,
        .red_size = boot_framebuffer.red_mask_size,
        .green_shift = boot_framebuffer.green_mask_shift,
        .green_size = boot_framebuffer.green_mask_size,
        .blue_shift = boot_framebuffer.blue_mask_shift,
        .blue_size = boot_framebuffer.blue_mask_size,
    };
}

bool console_set_mode(const struct console_mode *mode) {
    if (active_framebuffer == NULL || mode == NULL || mode->address == NULL
        || frame_active
        || mode->width < TEXT_MARGIN + (FONT_WIDTH + 1) * GLYPH_SCALE
        || mode->height < TEXT_MARGIN + TEXT_LINE_HEIGHT
        || mode->width > INT32_MAX || mode->height > INT32_MAX
        || mode->pitch < mode->width * sizeof(uint32_t)
        || mode->red_size > 8 || mode->green_size > 8 || mode->blue_size > 8
        || mode->red_shift + mode->red_size > 32
        || mode->green_shift + mode->green_size > 32
        || mode->blue_shift + mode->blue_size > 32) {
        return false;
    }
    framebuffer_cursor_hide();
    cursor_drawn = false;
    framebuffer_state.address = mode->address;
    framebuffer_state.width = mode->width;
    framebuffer_state.height = mode->height;
    framebuffer_state.pitch = mode->pitch;
    framebuffer_state.red_mask_shift = mode->red_shift;
    framebuffer_state.red_mask_size = mode->red_size;
    framebuffer_state.green_mask_shift = mode->green_shift;
    framebuffer_state.green_mask_size = mode->green_size;
    framebuffer_state.blue_mask_shift = mode->blue_shift;
    framebuffer_state.blue_mask_size = mode->blue_size;
    draw_base = (uint8_t *)mode->address;
    draw_pitch = mode->pitch;
    framebuffer_fill(framebuffer_color(active_framebuffer, 12, 18, 32));
    text_x = TEXT_MARGIN;
    text_y = TEXT_MARGIN;
    mouse_set_bounds(mode->width, mode->height);
    framebuffer_cursor_show();
    return true;
}

bool console_take_dirty(void) {
    return __atomic_exchange_n(&display_dirty, false, __ATOMIC_ACQ_REL);
}

static void console_scroll_if_needed(void) {
    if (active_framebuffer == NULL
        || text_y + FONT_HEIGHT * GLYPH_SCALE <= active_framebuffer->height) {
        return;
    }

    for (uint64_t y = TEXT_MARGIN; y + TEXT_LINE_HEIGHT < active_framebuffer->height; y++) {
        volatile uint32_t *destination = (volatile uint32_t *)
            ((uint8_t *)active_framebuffer->address + y * active_framebuffer->pitch);
        volatile uint32_t *source = (volatile uint32_t *)
            ((uint8_t *)active_framebuffer->address + (y + TEXT_LINE_HEIGHT) * active_framebuffer->pitch);
        for (uint64_t x = 0; x < active_framebuffer->width; x++) {
            destination[x] = source[x];
        }
    }

    uint32_t background = framebuffer_color(active_framebuffer, 12, 18, 32);
    for (uint64_t y = active_framebuffer->height - TEXT_LINE_HEIGHT;
         y < active_framebuffer->height;
         y++) {
        volatile uint32_t *row = (volatile uint32_t *)
            ((uint8_t *)active_framebuffer->address + y * active_framebuffer->pitch);
        for (uint64_t x = 0; x < active_framebuffer->width; x++) {
            row[x] = background;
        }
    }
    text_y -= TEXT_LINE_HEIGHT;
}

void console_write_string(const char *string) {
    framebuffer_cursor_hide();
    while (*string != '\0') {
        char character = *string++;
        if (character == '\n') {
            serial_line_flush();
            serial_write_character('\r');
            serial_write_character('\n');
            text_x = TEXT_MARGIN;
            text_y += TEXT_LINE_HEIGHT;
            console_scroll_if_needed();
            continue;
        }

        uint64_t character_width = (FONT_WIDTH + 1) * GLYPH_SCALE;
        if (character == '\b') {
            if (text_x > TEXT_MARGIN) {
                if (serial_line_length != 0) {
                    serial_line_length--;
                }
                text_x -= character_width;
                if (active_framebuffer != NULL
                    && text_y <= active_framebuffer->height - TEXT_LINE_HEIGHT) {
                    for (uint64_t row = 0; row < TEXT_LINE_HEIGHT; row++) {
                        volatile uint32_t *pixels = (volatile uint32_t *)
                            ((uint8_t *)active_framebuffer->address
                             + (text_y + row) * active_framebuffer->pitch);
                        for (uint64_t column = 0; column < character_width; column++) {
                            pixels[text_x + column] =
                                framebuffer_color(active_framebuffer, 12, 18, 32);
                        }
                    }
                }
            }
            continue;
        }

        serial_line_put(character);
        if (active_framebuffer == NULL) {
            continue;
        }

        if (text_x + character_width > active_framebuffer->width) {
            text_x = TEXT_MARGIN;
            text_y += TEXT_LINE_HEIGHT;
            console_scroll_if_needed();
        }
        if (text_y + FONT_HEIGHT * GLYPH_SCALE <= active_framebuffer->height) {
            framebuffer_draw_character(character);
        }
        text_x += character_width;
    }
    framebuffer_cursor_show();
}

void console_clear(void) {
    framebuffer_cursor_hide();
    serial_write_string("\x1b[2J\x1b[H");
    if (active_framebuffer != NULL) {
        framebuffer_fill(framebuffer_color(active_framebuffer, 12, 18, 32));
    }
    text_x = TEXT_MARGIN;
    text_y = TEXT_MARGIN;
    framebuffer_cursor_show();
}

void console_write_bytes(const char *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char character[2] = {data[i], '\0'};
        console_write_string(character);
    }
}

void console_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buffer[16];

    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[sizeof(buffer) - 1 - i] = digits[value & 0x0f];
        value >>= 4;
    }

    for (size_t i = 0; i < sizeof(buffer); i++) {
        char character[2] = {buffer[i], '\0'};
        console_write_string(character);
    }
}

static bool memory_allocator_self_test(void) {
    uint64_t first_page = physical_page_alloc();
    uint64_t second_page = physical_page_alloc();
    if (first_page == 0 || second_page == 0 || first_page == second_page) {
        return false;
    }

    volatile uint64_t *first = physical_page_to_hhdm(first_page);
    volatile uint64_t *second = physical_page_to_hhdm(second_page);
    if (first == NULL || second == NULL) {
        return false;
    }

    first[0] = 0x4f504f5350414745;
    first[PAGE_SIZE / sizeof(uint64_t) - 1] = 0x45474150534f504f;
    second[0] = 0x4f504f5350414745;
    second[PAGE_SIZE / sizeof(uint64_t) - 1] = 0x45474150534f504f;
    if (first[0] != 0x4f504f5350414745
        || first[PAGE_SIZE / sizeof(uint64_t) - 1] != 0x45474150534f504f
        || second[0] != 0x4f504f5350414745
        || second[PAGE_SIZE / sizeof(uint64_t) - 1] != 0x45474150534f504f) {
        return false;
    }

    if (!physical_page_free(first_page)
        || physical_page_free(first_page)
        || physical_page_free(first_page + 1)
        || !physical_page_free(second_page)) {
        return false;
    }

    uint64_t reused_second = physical_page_alloc();
    uint64_t reused_first = physical_page_alloc();
    if (reused_second != second_page || reused_first != first_page) {
        return false;
    }

    return physical_page_free(reused_first) && physical_page_free(reused_second);
}

static bool paging_self_test(void) {
    uint64_t first_page = physical_page_alloc();
    uint64_t second_page = physical_page_alloc();
    if (first_page == 0 || second_page == 0 || first_page == second_page) {
        if (first_page != 0) {
            physical_page_free(first_page);
        }
        if (second_page != 0) {
            physical_page_free(second_page);
        }
        return false;
    }

    volatile uint64_t *first = physical_page_to_hhdm(first_page);
    volatile uint64_t *alias = (volatile uint64_t *)PAGING_TEST_ADDRESS;
    uint64_t translated = 0;
    uint64_t unmapped = 0;
    bool mapped = paging_map_page(PAGING_TEST_ADDRESS, first_page, PAGING_WRITABLE);
    bool success = first != NULL && mapped;
    if (success) {
        alias[0] = 0x4f504f5350414745;
        success = first[0] == 0x4f504f5350414745
               && paging_translate(PAGING_TEST_ADDRESS + sizeof(uint64_t), &translated)
               && translated == first_page + sizeof(uint64_t)
               && !paging_map_page(PAGING_TEST_ADDRESS, second_page, PAGING_WRITABLE)
               && paging_unmap_page(PAGING_TEST_ADDRESS, &unmapped)
               && unmapped == first_page
               && !paging_translate(PAGING_TEST_ADDRESS, &translated);
    }
    if (mapped && !success) {
        paging_unmap_page(PAGING_TEST_ADDRESS, NULL);
    }

    bool first_page_freed = physical_page_free(first_page);
    bool second_page_freed = physical_page_free(second_page);
    bool pages_freed = first_page_freed && second_page_freed;

    uint64_t isolated_page = physical_page_alloc();
    uint64_t process_address_space = 0;
    bool created = isolated_page != 0
        && paging_address_space_create(&process_address_space);
    bool active = created
        && paging_address_space_activate(process_address_space);
    bool mapped_isolated_page = false;
    uintptr_t isolated_address = 0x00400000;
    uintptr_t shared_kernel_address =
        (uintptr_t)physical_page_to_hhdm(isolated_page);
    bool isolated = active
        && paging_translate(shared_kernel_address, &translated)
        && translated == isolated_page
        && !paging_translate(isolated_address, &translated);
    if (isolated) {
        mapped_isolated_page =
            paging_map_page(isolated_address, isolated_page,
                            PAGING_USER | PAGING_WRITABLE | PAGING_NO_EXECUTE);
        isolated = mapped_isolated_page
            && paging_user_range_valid(isolated_address, PAGE_SIZE, true, false)
            && paging_translate(isolated_address, &translated)
            && translated == isolated_page;
    }

    bool returned_to_kernel = !active
        || paging_address_space_activate(paging_kernel_address_space());
    if (returned_to_kernel && mapped_isolated_page
        && paging_address_space_activate(process_address_space)) {
        active = true;
        isolated = isolated
            && paging_unmap_page(isolated_address, NULL)
            && !paging_translate(isolated_address, &translated);
        returned_to_kernel =
            paging_address_space_activate(paging_kernel_address_space());
        active = !returned_to_kernel;
    } else {
        active = !returned_to_kernel;
    }

    bool address_space_destroyed = !created
        || (!active && paging_address_space_destroy(process_address_space));
    bool isolated_page_freed = isolated_page == 0
        || physical_page_free(isolated_page);
    return success && pages_freed && isolated && returned_to_kernel
        && address_space_destroyed && isolated_page_freed;
}

void exception_handler(uint64_t vector, uint64_t error_code,
                       uint64_t instruction_pointer, uint64_t code_segment,
                       void *exception_frame) {
    if ((code_segment & 3) == 3 && user_exception_dispatch(vector, exception_frame)) {
        console_set_text_style(CONSOLE_TEXT_ERROR);
        console_write_string("OPOS: isolated user exception\nVECTOR 0x");
        console_write_hex(vector);
        console_write_string("\nERROR 0x");
        console_write_hex(error_code);
        console_write_string("\nRIP 0x");
        console_write_hex(instruction_pointer);
        if (vector == 14) {
            uint64_t fault_address;
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));
            console_write_string("\nADDRESS 0x");
            console_write_hex(fault_address);
        }
        console_set_text_style(CONSOLE_TEXT_NORMAL);
        console_write_string("\n");
        return;
    }

    /*
     * Fatal. Report on serial first: if the fault came from drawing (e.g. a
     * bad framebuffer mapping), drawing the report would fault again, and
     * repeated nesting ends in a triple fault that resets the machine with
     * no message. A second fatal exception therefore only uses serial.
     */
    static volatile bool fatal_in_progress;
    uint64_t fault_address;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_address));
    serial_write_string("\nOPOS: CPU EXCEPTION vector ");
    serial_write_hex(vector);
    serial_write_string(" error ");
    serial_write_hex(error_code);
    serial_write_string(" rip ");
    serial_write_hex(instruction_pointer);
    serial_write_string(" cr2 ");
    serial_write_hex(fault_address);
    serial_write_string("\n");
    if (fatal_in_progress) {
        serial_write_string("OPOS: exception while reporting an exception\n");
        halt();
    }
    fatal_in_progress = true;

    /* Always red, without touching settings (which may read disk). */
    text_style = CONSOLE_TEXT_ERROR;
    console_write_string("OPOS: CPU EXCEPTION\nVECTOR 0x");
    console_write_hex(vector);
    console_write_string("\nERROR 0x");
    console_write_hex(error_code);
    console_write_string("\nRIP 0x");
    console_write_hex(instruction_pointer);
    console_write_string("\nCR2 0x");
    console_write_hex(fault_address);
    console_write_string("\n");
    halt();
}

static bool framebuffer_initialize(void) {
    if (framebuffer_request.response == NULL
        || framebuffer_request.response->framebuffer_count == 0) {
        return false;
    }

    struct limine_framebuffer *framebuffer =
        framebuffer_request.response->framebuffers[0];
    if (framebuffer == NULL || framebuffer->address == NULL
        || framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB
        || framebuffer->bpp != 32
        || framebuffer->width < TEXT_MARGIN + (FONT_WIDTH + 1) * GLYPH_SCALE
        || framebuffer->height < TEXT_MARGIN + FONT_HEIGHT * GLYPH_SCALE
        || framebuffer->pitch < framebuffer->width * sizeof(uint32_t)
        || framebuffer->red_mask_size > 8 || framebuffer->green_mask_size > 8
        || framebuffer->blue_mask_size > 8
        || framebuffer->red_mask_shift + framebuffer->red_mask_size > 32
        || framebuffer->green_mask_shift + framebuffer->green_mask_size > 32
        || framebuffer->blue_mask_shift + framebuffer->blue_mask_size > 32) {
        return false;
    }

    /* Firmware modes (revision 1 responses), for the Settings RESOLUTION row. */
    if (framebuffer_request.response->revision >= 1) {
        for (uint64_t i = 0; i < framebuffer->mode_count; i++) {
            const struct limine_video_mode *mode = framebuffer->modes[i];
            if (mode != NULL && mode->bpp == 32
                && mode->memory_model == LIMINE_FRAMEBUFFER_RGB) {
                boot_display_add_mode((uint32_t)mode->width,
                                      (uint32_t)mode->height);
            }
        }
    }
    boot_display_set_running((uint32_t)framebuffer->width,
                             (uint32_t)framebuffer->height);

    framebuffer_state = *framebuffer;
    boot_framebuffer = *framebuffer;
    active_framebuffer = &framebuffer_state;
    draw_base = (uint8_t *)framebuffer->address;
    draw_pitch = framebuffer->pitch;
    framebuffer_fill(framebuffer_color(framebuffer, 12, 18, 32));
    return true;
}

static void halt(void) {
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* Finds a loaded Limine module by its internal-module string. */
static struct limine_file *find_module(const char *name) {
    if (module_request.response == NULL
        || module_request.response->modules == NULL) {
        return NULL;
    }
    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file *file = module_request.response->modules[i];
        if (file == NULL || file->address == NULL || file->size == 0
            || file->string == NULL) {
            continue;
        }
        size_t j = 0;
        while (name[j] != '\0' && file->string[j] == name[j]) {
            j++;
        }
        if (name[j] == '\0' && file->string[j] == '\0') {
            return file;
        }
    }
    return NULL;
}

void kmain(void) {
    serial_initialize();
    interrupts_initialize();

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        serial_write_string("OPOS: unsupported Limine base revision\n");
        halt();
    }

    if (!framebuffer_initialize()) {
        serial_write_string("OPOS: no supported framebuffer available\n");
        halt();
    }

    if (hhdm_request.response == NULL || memory_map_request.response == NULL
        || !memory_initialize(memory_map_request.response, hhdm_request.response->offset)) {
        console_write_string("OPOS: physical memory initialization failed\n");
        halt();
    }
    struct limine_file *initramfs_module = find_module("initramfs");
    if (initramfs_module == NULL
        || !initramfs_initialize(initramfs_module->address,
                                 (size_t)initramfs_module->size)) {
        console_write_string("OPOS: required initramfs module unavailable or invalid\n");
        halt();
    }
    struct limine_file *install_module_file = find_module("install");
    struct limine_file *kernel_file = executable_file_request.response != NULL
        ? executable_file_request.response->executable_file : NULL;
    installer_set_payload(
        install_module_file != NULL ? install_module_file->address : NULL,
        install_module_file != NULL ? (size_t)install_module_file->size : 0,
        kernel_file != NULL ? kernel_file->address : NULL,
        kernel_file != NULL ? (size_t)kernel_file->size : 0,
        initramfs_module->address, (size_t)initramfs_module->size);
    if (installer_available()) {
        serial_write_string("OPOS: running from installation media\n");
    }
    struct initramfs_file elf_test;
    if (!initramfs_find_file("tests/elf-test", &elf_test)
        || !user_set_elf_image(elf_test.data, elf_test.size)) {
        console_write_string("OPOS: /tests/elf-test missing from initramfs\n");
        halt();
    }
    bool acpi_shutdown_ready = rsdp_request.response != NULL
        && power_initialize(rsdp_request.response->address);
    if (acpi_shutdown_ready) {
        serial_write_string("OPOS: ACPI shutdown ready\n");
    } else {
        serial_write_string("OPOS: ACPI shutdown unavailable\n");
    }
    if (!paging_nx_supported()) {
        console_write_string("OPOS: CPU does not support required NX page protection\n");
        halt();
    }
    if (!paging_initialize()) {
        console_write_string("OPOS: page-table manager initialization failed\n");
        halt();
    }
    serial_write_string(paging_smep_enabled() ? "OPOS: SMEP on\n"
                                              : "OPOS: SMEP unavailable\n");
    serial_write_string(paging_umip_enabled() ? "OPOS: UMIP on\n"
                                              : "OPOS: UMIP unavailable\n");

    if (!interrupts_route_legacy_pic()) {
        console_write_string("OPOS: legacy PIC interrupt routing failed\n");
        halt();
    }

    console_write_string("OPOS: x86-64 kernel entered\n");
    console_write_string("OPOS: Limine base revision supported\n");
    console_write_string("OPOS: framebuffer console ready\n");
    console_write_string("OPOS: CPU exception handlers ready\n");
    console_write_string("OPOS: usable pages 0x");
    console_write_hex(memory_total_pages());
    console_write_string("\n");
    if (!memory_allocator_self_test()) {
        console_write_string("OPOS: physical page allocator test failed\n");
        halt();
    }
    console_write_string("OPOS: physical page allocator ready\n");
    console_write_string("OPOS: page-table manager ready\n");
    console_write_string("OPOS: initramfs loaded with /tests/elf-test\n");
    if (!paging_self_test()) {
        console_write_string("OPOS: page-table manager test failed\n");
        halt();
    }
    console_write_string("OPOS: page-table map, translate, and unmap ready\n");
    console_write_string("OPOS: isolated process address spaces ready\n");

    pci_initialize();
    struct pci_storage_controllers pci_storage;
    pci_get_storage_controllers(&pci_storage);
    if (pci_storage.ahci_count != 0 || pci_storage.nvme_count != 0) {
        console_write_string("OPOS: PCI storage controllers enumerated\n");
    } else {
        console_write_string("OPOS: no AHCI or NVMe PCI controller found\n");
    }
    if (ahci_initialize()) {
        struct ahci_status ahci;
        ahci_get_status(&ahci);
        console_write_string("OPOS: AHCI SATA devices found: ");
        console_write_hex(ahci.sata_devices);
        console_write_string("\n");
    } else if (pci_storage.ahci_count != 0) {
        console_write_string("OPOS: AHCI controller could not be initialized\n");
    }

    if (pci_storage.nvme_count != 0) {
        if (nvme_initialize()) {
            struct nvme_status nvme;
            nvme_get_status(&nvme);
            console_write_string("OPOS: NVMe namespace ready (");
            console_write_hex(nvme.namespace_blocks);
            console_write_string(" blocks)\n");
        } else {
            console_write_string("OPOS: NVMe controller could not be initialized\n");
        }
    }

    if (pvscsi_initialize()) {
        console_write_string("OPOS: PVSCSI disks found: ");
        console_write_hex(pvscsi_disk_count());
        console_write_string("\n");
    }
    if (mpt_initialize()) {
        console_write_string("OPOS: LSI MPT disks found: ");
        console_write_hex(mpt_disk_count());
        console_write_string("\n");
    }
    if (buslogic_initialize()) {
        console_write_string("OPOS: BusLogic disks found: ");
        console_write_hex(buslogic_disk_count());
        console_write_string("\n");
    }
    if (sdhci_initialize()) {
        console_write_string("OPOS: SD/eMMC cards found: ");
        console_write_hex(sdhci_disk_count());
        console_write_string("\n");
    }

    /* Limine reports the disk it loaded the kernel from; that disk is
       mounted at /disk even when other disks come first. */
    if (kernel_file != NULL) {
        block_set_boot_identity(kernel_file->mbr_disk_id,
                                (const uint8_t *)&kernel_file->gpt_disk_uuid);
    }
    enum block_device disk = block_initialize();
    struct block_disk_info active_info;
    if (kernel_file != NULL
        && block_disk_info(block_active_disk(), &active_info)
        && active_info.boot_disk) {
        /* Mount the partition OPOS booted from, not the first FAT one. */
        fat16_prefer_partition(kernel_file->partition_index);
    }
    for (size_t i = 0; i < block_disk_count(); i++) {
        struct block_disk_info info;
        if (block_disk_info(i, &info)) {
            serial_write_string("OPOS: disk ");
            serial_write_string(info.location);
            serial_write_string(" \"");
            serial_write_string(info.model);
            serial_write_string(info.boot_disk ? "\" (boot disk)\n" : "\"\n");
        }
    }
    if (disk != BLOCK_DEVICE_NONE) {
        console_write_string("OPOS: ");
        console_write_string(block_device_name(disk));
        console_write_string(" selected as disk (");
        console_write_hex(block_sector_count());
        console_write_string(" sectors)\n");
        if (fat16_initialize()) {
            console_write_string(fat16_volume_type() == FAT_VOLUME_FAT32
                ? "OPOS: writable FAT32 volume mounted\n"
                : "OPOS: writable FAT16 volume mounted\n");
        } else {
            console_write_string("OPOS: no supported FAT volume found: ");
            console_write_string(
                fat16_disk_state_description(fat16_disk_state()));
            console_write_string("\n");
        }
        fat16_mount_others();
        for (size_t i = 1; i < FAT16_MAX_VOLUMES; i++) {
            struct fat16_volume_info volume;
            if (fat16_volume_info(i, &volume)) {
                console_write_string("OPOS: extra volume /disk");
                char number = (char)('1' + i);
                console_write_bytes(&number, 1);
                console_write_string(volume.fat32 ? " (FAT32)\n"
                                                  : " (FAT16)\n");
            }
        }
    } else {
        console_write_string("OPOS: no usable IDE, SATA, NVMe or SCSI disk found\n");
    }

    /* An update cut short by a power loss is finished first. */
    update_resume();
    /* Then a package install or removal; this also lists the installed
       package folders, including each account's, for permissions.c. */
    accounts_load();
    package_resume();

    bool root_selection_failed = false;
    const char *cmdline = cmdline_request.response != NULL
        ? cmdline_request.response->cmdline : NULL;
    static const char root_option[] = "opos.root=";
    bool root_requested = cmdline != NULL;
    for (size_t i = 0; root_requested && i < sizeof(root_option) - 1; i++) {
        root_requested = cmdline[i] != '\0'
            && cmdline[i] == root_option[i];
    }
    if (!installer_available() && fat16_system_mounted()) {
        char generation[64];
        char root[128];
        bool known = system_root_generation(generation, sizeof(generation))
            && system_root_path(root, sizeof(root));
        size_t generation_length = 0;
        while (known && generation[generation_length] != '\0') {
            generation_length++;
        }
        if (root_requested) {
            const char *pin = cmdline + sizeof(root_option) - 1;
            size_t pin_length = 0;
            while (pin[pin_length] != '\0') pin_length++;
            bool matches = known && pin_length == generation_length;
            for (size_t i = 0; matches && i < pin_length; i++) {
                matches = pin[i] == generation[i];
            }
            if (!matches) {
                size_t old_size;
                if (!vfs_file_size("/disk/boot/old/kernel.elf", 25,
                                   &old_size)) {
                    console_write_error("OPOS: disk system root pin does not match this kernel; recovery shell will start\n");
                    root_selection_failed = true;
                } else {
                    console_write_string("OPOS: old root pin belongs to previous boot entry; selecting current generation\n");
                }
            }
        }
        if (!root_selection_failed && known) {
            size_t root_length = 0;
            while (root[root_length] != '\0') root_length++;
            if (!system_root_ready(root, root_length)) {
                const char *error = NULL;
                console_write_string("OPOS: staging disk system root\n");
                if (!installer_stage_system_root(&error)) {
                    console_write_error("OPOS: disk system root staging failed: ");
                    console_write_error(error != NULL ? error : "UNKNOWN");
                    console_write_error("\n");
                    if (root_requested) root_selection_failed = true;
                    known = false;
                }
            }
            if (known && system_root_select(generation, generation_length)) {
                console_write_string("OPOS: verified disk system root selected\n");
            } else if (known) {
                console_write_error("OPOS: disk system root verification failed; recovery shell will start\n");
                root_selection_failed = true;
            }
        }
    } else if (root_requested && !installer_available()) {
        console_write_error("OPOS: disk system root unavailable; recovery shell will start\n");
        root_selection_failed = true;
    }

    /* CD/DVD drives, and the disc in each (mounted at /cdrom...). */
    optical_initialize();
    iso_refresh();
    for (size_t i = 0; i < optical_drive_count(); i++) {
        struct optical_drive_info info;
        if (optical_drive_info(i, &info)) {
            serial_write_string("OPOS: CD/DVD drive ");
            serial_write_string(info.location);
            serial_write_string(" \"");
            serial_write_string(info.model);
            serial_write_string(iso_mounted(i) ? "\" (disc \"" : "\"");
            if (iso_mounted(i)) {
                serial_write_string(iso_label(i));
                serial_write_string("\")");
            }
            serial_write_string("\n");
        }
    }

    /* After the disk, so the saved resolution can come from OPOS.CFG. */
    if (svga_initialize()) {
        console_write_string("OPOS: VMware SVGA II display found\n");
        size_t mode = settings_display_mode();
        if (mode != 0 && mode < svga_mode_count()) {
            if (svga_set_mode(mode)) {
                console_write_string("OPOS: display mode set from settings\n");
            } else {
                console_write_string("OPOS: saved display mode not supported\n");
            }
        }
    }

    scheduler_initialize();
    timer_initialize();
    console_write_string("OPOS: preemptive kernel scheduler ready\n");
    console_write_string("OPOS: PIT timer ready at 100 HZ\n");
    keyboard_initialize();
    console_write_string("OPOS: PS/2 keyboard ready\n");
    if (mouse_initialize(active_framebuffer->width, active_framebuffer->height)) {
        console_write_string("OPOS: PS/2 mouse ready\n");
    } else {
        console_write_string("OPOS: PS/2 mouse unavailable: ");
        console_write_string(mouse_failure_reason());
        console_write_string("\n");
    }
    vmware_initialize();
    vmware_set_autofit(settings_display_mode() == VMWARE_DISPLAY_AUTOFIT);
    if (vmware_mouse_active()) {
        console_write_string("OPOS: VMware absolute mouse ready\n");
    }
    interrupts_enable();
    keyboard_start_leds();

    /* USB (xHCI, only under a hypervisor) and the sticks plugged in.
       After interrupts are on: the driver times itself with the timer. */
    storage_initialize();
    serial_write_string(xhci_ready() ? "OPOS: USB controller ready\n"
                                     : "OPOS: no USB controller used\n");
    /* Network card; DHCP starts in the background (net_poll). */
    net_initialize();
    console_write_string(nic_ready() ? "OPOS: network card ready\n"
                                     : "OPOS: no network card found\n");
    /* Sound (docs/sound.md); the timer refills its buffers. */
    if (!hda_initialize()) {
        console_write_string("OPOS: no sound output found\n");
    }
    sound_initialize();
    /* I2C touchpad (laptops). Its reset waits about 150 ms. */
    if (i2c_hid_initialize()) {
        console_write_string("OPOS: I2C touchpad ready\n");
    } else if (i2c_hid_present()) {
        console_write_string("OPOS: I2C touchpad unavailable: ");
        console_write_string(i2c_hid_stage());
        console_write_string("\n");
    }
    for (size_t i = 0; i < block_disk_count(); i++) {
        struct block_disk_info info;
        if (block_disk_info(i, &info) && info.removable) {
            serial_write_string("OPOS: removable disk ");
            serial_write_string(info.location);
            serial_write_string(" \"");
            serial_write_string(info.model);
            serial_write_string("\"\n");
        }
    }

    uint64_t reported_dropped_characters = 0;
    bool boot_shell = root_selection_failed || settings_take_boot_shell_once();
    /* Installation media always starts the desktop with the installer. */
    if (!boot_shell
        && (settings_get(SETTING_AUTOSTART_DESKTOP) || installer_available())) {
        desktop_run();
    }
    shell_initialize();
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
        char character;
        while (keyboard_read_character(&character)) {
            shell_handle_character(character);
        }
        vmware_tools_poll();
        i2c_hid_poll();
        net_poll();
        console_cursor_refresh();

        uint64_t dropped = keyboard_dropped_characters();
        if (dropped != reported_dropped_characters) {
            shell_report_dropped_input(dropped - reported_dropped_characters);
            reported_dropped_characters = dropped;
        }
    }
}
