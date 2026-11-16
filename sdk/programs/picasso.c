/*
 * Picasso: the OPOS image editor.
 *
 * Draw with the pen, eraser and spray; draw lines, rectangles and
 * ellipses (outlined or filled); fill areas; pick colours from the
 * picture. Open PNG, JPEG and BMP pictures and save them as PNG.
 *
 * Mouse: the left button draws with the first colour, the right button
 * with the second. A palette click picks the first colour (right click:
 * the second). The wheel scrolls (Shift: sideways, Ctrl: zoom). Shift
 * keeps lines at 45 degrees and shapes square.
 * Keys: Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as,
 * Ctrl+Z undo, Ctrl+Y redo; P pen, E eraser, S spray, L line,
 * R rectangle, O ellipse, F fill, K colour picker, G filled shapes,
 * [ and ] brush size, + and - zoom, X swaps the colours, arrows scroll.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_gfx.h>
#include <opos_image.h>

#include "png_encode.h"

OPOS_MANIFEST("Picasso", "1.1", 0);

#define TOP_H 40
#define LEFT_W 76
#define BOTTOM_H 56
#define TOOL_BOX 32
#define MAX_SIDE 4096
#define NEW_WIDTH 800
#define NEW_HEIGHT 600
#define UNDO_MAX 40
#define UNDO_BYTES (160u * 1024 * 1024)
#define PATH_MAX_LENGTH 256

#define UI_BACK OPOS_RGB(30, 34, 46)
#define UI_PANEL OPOS_RGB(40, 46, 62)
#define UI_BUTTON OPOS_RGB(56, 64, 86)
#define UI_HOVER OPOS_RGB(72, 82, 110)
#define UI_ACTIVE OPOS_RGB(70, 110, 190)
#define UI_TEXT OPOS_RGB(230, 236, 248)
#define UI_DIM OPOS_RGB(130, 140, 165)
#define UI_WARN OPOS_RGB(245, 205, 110)
#define WORKSPACE OPOS_RGB(62, 66, 78)

enum tool {
    TOOL_PEN,
    TOOL_ERASER,
    TOOL_SPRAY,
    TOOL_LINE,
    TOOL_RECT,
    TOOL_ELLIPSE,
    TOOL_FILL,
    TOOL_PICKER,
    TOOL_COUNT,
};

static const char *const tool_names[TOOL_COUNT] = {
    "PEN", "ERASER", "SPRAY", "LINE", "RECTANGLE", "ELLIPSE", "FILL",
    "COLOUR PICKER",
};
static const char tool_keys[TOOL_COUNT] = {'p', 'e', 's', 'l', 'r', 'o',
                                           'f', 'k'};

static const int brush_sizes[] = {1, 2, 4, 8, 16, 32};
#define BRUSH_COUNT ((int)(sizeof(brush_sizes) / sizeof(brush_sizes[0])))

/* Zoom as a fraction: shown = image * zoom_num / zoom_den. */
static const int zoom_num[] = {1, 1, 1, 2, 4, 8};
static const int zoom_den[] = {4, 2, 1, 1, 1, 1};
#define ZOOM_COUNT 6
#define ZOOM_ONE 2

static const uint32_t palette[28] = {
    0x000000, 0x404040, 0x808080, 0xc0c0c0, 0xffffff, 0x7f1d1d,
    0xe02424, 0xff8a80, 0xb45309, 0xf59e0b, 0xfde047, 0x3f6212,
    0x22c55e, 0xa7f3d0, 0x0f766e, 0x06b6d4, 0x0c4a6e, 0x2563eb,
    0x93c5fd, 0x312e81, 0x7c3aed, 0xc4b5fd, 0x86198f, 0xec4899,
    0xfbcfe8, 0x78350f, 0xd6a77a, 0xfff7e6,
};

/* --------------------------------------------------------------- state */

struct picture {
    uint32_t *pixels;
    int width, height;
};

static struct picture image;
static struct picture undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int undo_count, redo_count;

static struct opos_canvas canvas;
static int view_width, view_height;

static enum tool tool = TOOL_PEN, tool_before_picker = TOOL_PEN;
static int brush = 2;          /* index into brush_sizes */
static bool filled;            /* shapes are filled */
static uint32_t colours[2] = {0x000000, 0xffffff};
static int zoom = ZOOM_ONE;
/* The zoom follows the window size until the person zooms. */
static bool zoom_auto = true;
static int scroll_x, scroll_y; /* shown pixels scrolled */

/* The stroke in progress: button 0 or 1, the start and the last point. */
static int drawing = -1;
static int start_x, start_y, last_x, last_y;
static bool shift_held;
/* Modifier keys held, from key state events: mouse and wheel events
   carry only the mouse buttons. */
static bool shift_down, ctrl_down;
static bool close_removes_window; /* OPOS before 0.6.6 */
static int hover_x = -1, hover_y = -1; /* picture pixel under the mouse */
static int mouse_x, mouse_y;

static char path[PATH_MAX_LENGTH];
static char file_name[64] = "Untitled";
static bool can_write;   /* path came from opos_save_file in this run */
static bool modified;
static char message[96];
static bool message_warn;

/* The question after Close, New or Open with unsaved changes. */
enum pending { PENDING_NONE, PENDING_CLOSE, PENDING_NEW, PENDING_OPEN };
static enum pending pending;

/* ------------------------------------------------------------- helpers */

static void say(const char *text, bool warn) {
    size_t length = strlen(text);
    if (length >= sizeof(message)) {
        length = sizeof(message) - 1;
    }
    memcpy(message, text, length);
    message[length] = '\0';
    message_warn = warn;
}

static void update_title(void) {
    char title[32];
    size_t used = 0;
    const char *parts[3] = {"Picasso - ", file_name, modified ? "*" : ""};
    for (int p = 0; p < 3; p++) {
        for (const char *c = parts[p]; *c != '\0' && used < 31; c++) {
            title[used++] = *c;
        }
    }
    title[used] = '\0';
    opos_window_title(title);
}

static void set_modified(bool value) {
    if (modified != value) {
        modified = value;
        update_title();
    }
}

static uint64_t random_state = 0x9e3779b97f4a7c15ull;

static uint32_t random_next(void) {
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return (uint32_t)((random_state * 0x2545f4914f6cdd1dull) >> 32);
}

static int isqrt(int value) {
    if (value <= 0) {
        return 0;
    }
    int root = 0, bit = 1 << 30;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

/* --------------------------------------------------------------- undo */

static size_t picture_bytes(const struct picture *picture) {
    return (size_t)picture->width * (size_t)picture->height * 4;
}

static void picture_free(struct picture *picture) {
    free(picture->pixels);
    picture->pixels = NULL;
}

static bool picture_copy(struct picture *to, const struct picture *from) {
    to->pixels = malloc(picture_bytes(from));
    if (to->pixels == NULL) {
        return false;
    }
    memcpy(to->pixels, from->pixels, picture_bytes(from));
    to->width = from->width;
    to->height = from->height;
    return true;
}

static void stack_drop_oldest(struct picture *stack, int *count) {
    picture_free(&stack[0]);
    for (int i = 1; i < *count; i++) {
        stack[i - 1] = stack[i];
    }
    (*count)--;
}

static void stack_clear(struct picture *stack, int *count) {
    while (*count > 0) {
        picture_free(&stack[--*count]);
    }
}

static size_t undo_bytes(void) {
    size_t total = 0;
    for (int i = 0; i < undo_count; i++) {
        total += picture_bytes(&undo_stack[i]);
    }
    for (int i = 0; i < redo_count; i++) {
        total += picture_bytes(&redo_stack[i]);
    }
    return total;
}

/* Before a change: keeps a copy to go back to. */
static bool remember(void) {
    stack_clear(redo_stack, &redo_count);
    if (undo_count == UNDO_MAX) {
        stack_drop_oldest(undo_stack, &undo_count);
    }
    while (undo_count > 0
           && undo_bytes() + picture_bytes(&image) > UNDO_BYTES) {
        stack_drop_oldest(undo_stack, &undo_count);
    }
    if (!picture_copy(&undo_stack[undo_count], &image)) {
        say("NOT ENOUGH MEMORY TO UNDO THIS", true);
        return false;
    }
    undo_count++;
    return true;
}

/* Undo (from the undo stack to the redo stack) or redo. */
static void step(struct picture *from, int *from_count, struct picture *to,
                 int *to_count) {
    if (*from_count == 0) {
        return;
    }
    if (*to_count == UNDO_MAX) {
        stack_drop_oldest(to, to_count);
    }
    to[(*to_count)++] = image;
    image = from[--*from_count];
    set_modified(true);
}

/* -------------------------------------------------------- drawing ops */

static void plot(int x, int y, uint32_t colour) {
    if (x >= 0 && y >= 0 && x < image.width && y < image.height) {
        image.pixels[(size_t)y * (size_t)image.width + (size_t)x] = colour;
    }
}

/* A round dot of the brush size centred on (x, y). */
static void stamp(int x, int y, int size, uint32_t colour) {
    if (size <= 1) {
        plot(x, y, colour);
        return;
    }
    int left = x - size / 2, top = y - size / 2;
    for (int dy = 0; dy < size; dy++) {
        int ey = 2 * dy + 1 - size;
        for (int dx = 0; dx < size; dx++) {
            int ex = 2 * dx + 1 - size;
            if (ex * ex + ey * ey <= size * size) {
                plot(left + dx, top + dy, colour);
            }
        }
    }
}

static void line(int x0, int y0, int x1, int y1, int size, uint32_t colour) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        stamp(x0, y0, size, colour);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void fill_rect(int x0, int y0, int x1, int y1, uint32_t colour) {
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
    }
    if (y0 > y1) {
        int t = y0; y0 = y1; y1 = t;
    }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            plot(x, y, colour);
        }
    }
}

static void rectangle(int x0, int y0, int x1, int y1, int size,
                      uint32_t colour) {
    if (filled) {
        fill_rect(x0, y0, x1, y1, colour);
        return;
    }
    line(x0, y0, x1, y0, size, colour);
    line(x1, y0, x1, y1, size, colour);
    line(x1, y1, x0, y1, size, colour);
    line(x0, y1, x0, y0, size, colour);
}

/* The ellipse inside the box (x0, y0)-(x1, y1). Each row's half width
   comes from the ellipse's equation; the outline joins neighbouring
   rows so it has no gaps. */
static void ellipse(int x0, int y0, int x1, int y1, int size,
                    uint32_t colour) {
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
    }
    if (y0 > y1) {
        int t = y0; y0 = y1; y1 = t;
    }
    /* Doubled coordinates keep the centre exact for even sizes. */
    int cx2 = x0 + x1, cy2 = y0 + y1;
    int rx2 = x1 - x0, ry2 = y1 - y0;
    if (rx2 == 0 || ry2 == 0) {
        line(x0, y0, x1, y1, size, colour);
        return;
    }
    int previous_left = 0, previous_right = 0, previous_y = 0;
    for (int y = y0; y <= y1; y++) {
        int dy2 = 2 * y - cy2;
        /* half width (doubled) = rx2 * sqrt(1 - dy2^2 / ry2^2) */
        long inside = (long)ry2 * ry2 - (long)dy2 * dy2;
        int half2 = inside <= 0 ? 0
            : (int)((long)rx2 * isqrt((int)(inside > 0x3fffffff ? 0x3fffffff
                                                                : inside))
                    / ry2);
        int left = (cx2 - half2 + 1) / 2, right = (cx2 + half2) / 2;
        if (filled) {
            for (int x = left; x <= right; x++) {
                plot(x, y, colour);
            }
        } else if (y == y0) {
            line(left, y, right, y, size, colour);
        } else {
            line(previous_left, previous_y, left, y, size, colour);
            line(previous_right, previous_y, right, y, size, colour);
            if (y == y1) {
                line(left, y, right, y, size, colour);
            }
        }
        previous_left = left;
        previous_right = right;
        previous_y = y;
    }
}

/* Fills the area of one colour around (x, y), row by row. */
static void flood_fill(int x, int y, uint32_t colour) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) {
        return;
    }
    uint32_t *pixels = image.pixels;
    int width = image.width, height = image.height;
    uint32_t target = pixels[(size_t)y * width + x];
    if (target == colour) {
        return;
    }
    size_t capacity = 1024, count = 0;
    int *stack = malloc(capacity * 2 * sizeof(int));
    if (stack == NULL) {
        say("NOT ENOUGH MEMORY TO FILL", true);
        return;
    }
    stack[count * 2] = x;
    stack[count * 2 + 1] = y;
    count++;
    while (count > 0) {
        count--;
        int px = stack[count * 2], py = stack[count * 2 + 1];
        uint32_t *row = pixels + (size_t)py * width;
        if (row[px] != target) {
            continue;
        }
        int left = px, right = px;
        while (left > 0 && row[left - 1] == target) {
            left--;
        }
        while (right + 1 < width && row[right + 1] == target) {
            right++;
        }
        for (int i = left; i <= right; i++) {
            row[i] = colour;
        }
        for (int side = -1; side <= 1; side += 2) {
            int ny = py + side;
            if (ny < 0 || ny >= height) {
                continue;
            }
            uint32_t *next = pixels + (size_t)ny * width;
            bool in_run = false;
            for (int i = left; i <= right; i++) {
                bool match = next[i] == target;
                if (match && !in_run) {
                    if (count == capacity) {
                        int *grown = realloc(stack,
                                             capacity * 4 * sizeof(int));
                        if (grown == NULL) {
                            free(stack);
                            say("NOT ENOUGH MEMORY TO FILL", true);
                            return;
                        }
                        stack = grown;
                        capacity *= 2;
                    }
                    stack[count * 2] = i;
                    stack[count * 2 + 1] = ny;
                    count++;
                }
                in_run = match;
            }
        }
    }
    free(stack);
}

static void spray(int x, int y, uint32_t colour) {
    int radius = brush_sizes[brush] * 2 + 4;
    int dots = brush_sizes[brush] * 2 + 6;
    for (int i = 0; i < dots; i++) {
        int dx = (int)(random_next() % (uint32_t)(2 * radius + 1)) - radius;
        int dy = (int)(random_next() % (uint32_t)(2 * radius + 1)) - radius;
        if (dx * dx + dy * dy <= radius * radius) {
            plot(x + dx, y + dy, colour);
        }
    }
}

/* Lines at multiples of 45 degrees, shapes with equal sides. */
static void constrain(int *x, int *y) {
    int dx = *x - start_x, dy = *y - start_y;
    int ax = abs(dx), ay = abs(dy);
    if (tool == TOOL_LINE) {
        if (ax > 2 * ay) {
            *y = start_y;
        } else if (ay > 2 * ax) {
            *x = start_x;
        } else {
            int d = ax > ay ? ax : ay;
            *x = start_x + (dx < 0 ? -d : d);
            *y = start_y + (dy < 0 ? -d : d);
        }
    } else {
        int d = ax > ay ? ax : ay;
        *x = start_x + (dx < 0 ? -d : d);
        *y = start_y + (dy < 0 ? -d : d);
    }
}

/* Lines and shapes: the picture from before the stroke, with the shape
   drawn to (x, y). */
static void shape_preview(int x, int y) {
    if (undo_count == 0) {
        return;
    }
    const struct picture *before = &undo_stack[undo_count - 1];
    memcpy(image.pixels, before->pixels, picture_bytes(&image));
    if (shift_held) {
        constrain(&x, &y);
    }
    uint32_t colour = colours[drawing];
    int size = brush_sizes[brush];
    if (tool == TOOL_LINE) {
        line(start_x, start_y, x, y, size, colour);
    } else if (tool == TOOL_RECT) {
        rectangle(start_x, start_y, x, y, size, colour);
    } else {
        ellipse(start_x, start_y, x, y, size, colour);
    }
}

/* --------------------------------------------------------------- files */

static bool new_picture(int width, int height) {
    uint32_t *pixels = malloc((size_t)width * height * 4);
    if (pixels == NULL) {
        say("NOT ENOUGH MEMORY FOR A NEW PICTURE", true);
        return false;
    }
    for (size_t i = 0; i < (size_t)width * height; i++) {
        pixels[i] = 0xffffff;
    }
    stack_clear(undo_stack, &undo_count);
    stack_clear(redo_stack, &redo_count);
    picture_free(&image);
    image.pixels = pixels;
    image.width = width;
    image.height = height;
    scroll_x = scroll_y = 0;
    return true;
}

static void set_name_from_path(void) {
    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    size_t length = strlen(name);
    if (length >= sizeof(file_name)) {
        length = sizeof(file_name) - 1;
    }
    memcpy(file_name, name, length);
    file_name[length] = '\0';
}

/* The largest zoom of at most 100% that shows the whole picture. */
static void zoom_to_fit(void) {
    int room_w = view_width - LEFT_W - 16;
    int room_h = view_height - TOP_H - BOTTOM_H - 16;
    zoom = ZOOM_ONE;
    while (zoom > 0 && (image.width * zoom_num[zoom] / zoom_den[zoom] > room_w
                        || image.height * zoom_num[zoom] / zoom_den[zoom]
                               > room_h)) {
        zoom--;
    }
    zoom_auto = true;
    scroll_x = scroll_y = 0;
}

static void do_new(void) {
    if (!new_picture(NEW_WIDTH, NEW_HEIGHT)) {
        return;
    }
    path[0] = '\0';
    strcpy(file_name, "Untitled");
    can_write = false;
    modified = true; /* so that update_title runs */
    set_modified(false);
    zoom_to_fit();
    say("NEW PICTURE, 800 X 600", false);
}

static uint8_t *read_whole_file(const char *file, size_t *size) {
    long fd = opos_open(file, OPOS_O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t capacity = 65536, used = 0;
    uint8_t *data = malloc(capacity);
    while (data != NULL) {
        if (used == capacity) {
            if (capacity >= 256u * 1024 * 1024) {
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
        size_t chunk = capacity - used < 4096 ? capacity - used : 4096;
        long got = opos_read((int)fd, data + used, chunk);
        if (got < 0) {
            free(data);
            data = NULL;
        } else if (got == 0) {
            break;
        } else {
            used += (size_t)got;
        }
    }
    opos_close((int)fd);
    *size = used;
    return data;
}

static void do_open(void) {
    char chosen[PATH_MAX_LENGTH];
    long picked = opos_pick_file(chosen, sizeof(chosen));
    if (picked == -OPOS_ECANCELED) {
        return;
    }
    if (picked < 0) {
        say("COULD NOT CHOOSE A FILE", true);
        return;
    }
    size_t size;
    uint8_t *data = read_whole_file(chosen, &size);
    if (data == NULL) {
        say("COULD NOT READ THAT FILE", true);
        return;
    }
    struct image_info info;
    enum image_result result = image_probe(data, size, &info);
    if (result == IMAGE_OK
        && (info.width > MAX_SIDE || info.height > MAX_SIDE)) {
        result = IMAGE_TOO_LARGE;
    }
    void *scratch = NULL;
    struct image decoded;
    if (result == IMAGE_OK) {
        scratch = malloc(info.scratch_bytes + 16);
        result = scratch == NULL ? IMAGE_NO_MEMORY
            : image_decode(data, size,
                           (void *)(((uintptr_t)scratch + 15) & ~(uintptr_t)15),
                           info.scratch_bytes, &decoded);
    }
    free(data);
    if (result != IMAGE_OK) {
        free(scratch);
        say(result == IMAGE_TOO_LARGE ? "PICTURES UP TO 4096 X 4096 OPEN"
            : result == IMAGE_UNKNOWN_FORMAT
                ? "PICASSO OPENS PNG, JPEG AND BMP PICTURES"
                : image_result_text(result),
            true);
        return;
    }
    if (!new_picture((int)decoded.width, (int)decoded.height)) {
        free(scratch);
        return;
    }
    /* Transparent parts go on white. */
    for (size_t i = 0; i < (size_t)image.width * image.height; i++) {
        uint32_t p = decoded.pixels[i];
        uint32_t a = p >> 24;
        uint32_t r = ((p >> 16 & 0xff) * a + 255 * (255 - a)) / 255;
        uint32_t g = ((p >> 8 & 0xff) * a + 255 * (255 - a)) / 255;
        uint32_t b = ((p & 0xff) * a + 255 * (255 - a)) / 255;
        image.pixels[i] = r << 16 | g << 8 | b;
    }
    free(scratch);
    memcpy(path, chosen, strlen(chosen) + 1);
    set_name_from_path();
    can_write = false; /* reading a file does not allow writing it */
    modified = true;
    set_modified(false);
    zoom_to_fit();
    say("OPENED", false);
}

static bool write_png(const char *file) {
    size_t size;
    uint8_t *png = png_encode(image.pixels, (uint32_t)image.width,
                              (uint32_t)image.height, (uint32_t)image.width,
                              &size);
    if (png == NULL) {
        say("NOT ENOUGH MEMORY TO SAVE", true);
        return false;
    }
    long fd = opos_open(file, OPOS_O_WRONLY | OPOS_O_CREAT | OPOS_O_TRUNC);
    bool ok = fd >= 0;
    size_t done = 0;
    while (ok && done < size) {
        long wrote = opos_write((int)fd, png + done, size - done);
        if (wrote <= 0) {
            ok = false;
        } else {
            done += (size_t)wrote;
        }
    }
    if (fd >= 0 && opos_close((int)fd) < 0) {
        ok = false;
    }
    free(png);
    if (!ok) {
        say("COULD NOT SAVE THE PICTURE", true);
    }
    return ok;
}

static bool do_save_as(void) {
    /* The picture's own path, so Files starts in its folder. */
    char suggested[PATH_MAX_LENGTH + 8];
    const char *from = path[0] == '/' ? path : file_name;
    size_t length = strlen(from);
    memcpy(suggested, from, length + 1);
    /* Saving always writes PNG: suggest a .png name. */
    char *dot = strrchr(suggested, '.');
    char *slash = strrchr(suggested, '/');
    if (dot != NULL && dot != suggested && (slash == NULL || dot > slash + 1)) {
        *dot = '\0';
        length = (size_t)(dot - suggested);
    }
    if (length + 5 < sizeof(suggested)) {
        memcpy(suggested + length, ".png", 5);
    }
    char chosen[PATH_MAX_LENGTH];
    long result = opos_save_file(suggested, chosen, sizeof(chosen));
    if (result == -OPOS_ECANCELED) {
        return false;
    }
    if (result == -OPOS_ENOSYS) {
        say("THIS OPOS CANNOT SAVE FOR PROGRAMS YET; PLEASE UPDATE", true);
        return false;
    }
    if (result < 0) {
        say("COULD NOT CHOOSE WHERE TO SAVE", true);
        return false;
    }
    if (!write_png(chosen)) {
        return false;
    }
    memcpy(path, chosen, strlen(chosen) + 1);
    set_name_from_path();
    can_write = true;
    modified = true;
    set_modified(false);
    say("SAVED", false);
    return true;
}

static bool do_save(void) {
    if (!can_write) {
        return do_save_as();
    }
    if (!write_png(path)) {
        return false;
    }
    set_modified(false);
    say("SAVED", false);
    return true;
}

/* -------------------------------------------------------------- layout */

struct box {
    int x, y, w, h;
};

static bool inside(struct box box, int x, int y) {
    return x >= box.x && y >= box.y && x < box.x + box.w
        && y < box.y + box.h;
}

enum button {
    BUTTON_NEW,
    BUTTON_OPEN,
    BUTTON_SAVE,
    BUTTON_SAVE_AS,
    BUTTON_UNDO,
    BUTTON_REDO,
    BUTTON_ZOOM_OUT,
    BUTTON_ZOOM_IN,
    BUTTON_COUNT,
};

static const char *const button_labels[BUTTON_COUNT] = {
    "NEW", "OPEN", "SAVE", "SAVE AS", "UNDO", "REDO", "-", "+",
};

static struct box button_box(int index) {
    int x = 8;
    for (int i = 0; i < index; i++) {
        x += opos_text_width(button_labels[i], 2) + 20 + 6;
        if (i == BUTTON_SAVE_AS || i == BUTTON_REDO) {
            x += 14;
        }
        if (i == BUTTON_ZOOM_OUT) {
            x += opos_text_width("800%", 2) + 12;
        }
    }
    return (struct box){x, 6, opos_text_width(button_labels[index], 2) + 20,
                        TOP_H - 12};
}

static struct box tool_box(int index) {
    return (struct box){8 + (index % 2) * (TOOL_BOX + 4),
                        TOP_H + 8 + (index / 2) * (TOOL_BOX + 4), TOOL_BOX,
                        TOOL_BOX};
}

static struct box filled_box(void) {
    struct box last = tool_box(TOOL_COUNT - 1);
    return (struct box){8, last.y + TOOL_BOX + 10, 2 * TOOL_BOX + 4, 26};
}

static struct box brush_box(int index) {
    struct box above = filled_box();
    int size = 2 * TOOL_BOX + 4;
    return (struct box){8, above.y + above.h + 10 + index * 30, size, 26};
}

static struct box colour_box(int which) {
    int top = view_height - BOTTOM_H;
    return which == 0 ? (struct box){12, top + 8, 30, 30}
                      : (struct box){28, top + 22, 30, 30};
}

static struct box palette_box(int index) {
    int top = view_height - BOTTOM_H + 6;
    return (struct box){LEFT_W + (index % 14) * 24, top + (index / 14) * 24,
                        22, 22};
}

static struct box work_area(void) {
    return (struct box){LEFT_W, TOP_H, view_width - LEFT_W,
                        view_height - TOP_H - BOTTOM_H};
}

/* Where the picture's top-left corner is on screen. A picture smaller
   than the work area is centred. */
static void picture_origin(int *x, int *y) {
    struct box area = work_area();
    int shown_w = image.width * zoom_num[zoom] / zoom_den[zoom];
    int shown_h = image.height * zoom_num[zoom] / zoom_den[zoom];
    *x = shown_w < area.w ? area.x + (area.w - shown_w) / 2
                          : area.x - scroll_x;
    *y = shown_h < area.h ? area.y + (area.h - shown_h) / 2
                          : area.y - scroll_y;
}

static void clamp_scroll(void) {
    struct box area = work_area();
    int shown_w = image.width * zoom_num[zoom] / zoom_den[zoom];
    int shown_h = image.height * zoom_num[zoom] / zoom_den[zoom];
    int max_x = shown_w - area.w, max_y = shown_h - area.h;
    scroll_x = scroll_x > max_x ? max_x : scroll_x;
    scroll_y = scroll_y > max_y ? max_y : scroll_y;
    scroll_x = scroll_x < 0 ? 0 : scroll_x;
    scroll_y = scroll_y < 0 ? 0 : scroll_y;
}

/* Screen to picture pixels (may be outside the picture). */
static void to_picture(int sx, int sy, int *px, int *py) {
    int ox, oy;
    picture_origin(&ox, &oy);
    int dx = (sx - ox) * zoom_den[zoom], dy = (sy - oy) * zoom_den[zoom];
    *px = dx >= 0 ? dx / zoom_num[zoom] : -((-dx + zoom_num[zoom] - 1)
                                            / zoom_num[zoom]);
    *py = dy >= 0 ? dy / zoom_num[zoom] : -((-dy + zoom_num[zoom] - 1)
                                            / zoom_num[zoom]);
}

/* Zooms keeping the picture point under (sx, sy) in place. */
static void set_zoom(int level, int sx, int sy) {
    if (level < 0 || level >= ZOOM_COUNT || level == zoom) {
        return;
    }
    int px, py;
    to_picture(sx, sy, &px, &py);
    struct box area = work_area();
    zoom = level;
    zoom_auto = false;
    scroll_x = px * zoom_num[zoom] / zoom_den[zoom] - (sx - area.x);
    scroll_y = py * zoom_num[zoom] / zoom_den[zoom] - (sy - area.y);
    clamp_scroll();
}

static void zoom_centre(int level) {
    struct box area = work_area();
    set_zoom(level, area.x + area.w / 2, area.y + area.h / 2);
}

/* ------------------------------------------------------------- drawing */

static struct opos_canvas *const screen = &canvas;

static void fill(struct box box, uint32_t colour) {
    opos_fill_rect(screen, box.x, box.y, box.w, box.h, colour);
}

static void draw_button(struct box box, const char *label, bool active,
                        bool enabled) {
    bool hover = inside(box, mouse_x, mouse_y);
    fill(box, active ? UI_ACTIVE : hover && enabled ? UI_HOVER : UI_BUTTON);
    int width = opos_text_width(label, 2);
    opos_draw_text(screen, box.x + (box.w - width) / 2 + 1,
                   box.y + (box.h - opos_char_height(2)) / 2, label,
                   enabled ? UI_TEXT : UI_DIM, 2);
}

/* Small pictures for the tools, in a 32 x 32 box. */
static void draw_tool_icon(enum tool which, struct box box, uint32_t ink) {
    int x = box.x, y = box.y;
    switch (which) {
    case TOOL_PEN:
        for (int i = 0; i < 16; i++) {
            opos_fill_rect(screen, x + 9 + i, y + 23 - i, 3, 3, ink);
        }
        opos_fill_rect(screen, x + 7, y + 24, 3, 3, OPOS_RGB(250, 210, 90));
        break;
    case TOOL_ERASER:
        opos_fill_rect(screen, x + 7, y + 11, 18, 11, OPOS_RGB(240, 140, 160));
        opos_fill_rect(screen, x + 7, y + 17, 18, 5, ink);
        opos_draw_rect(screen, x + 7, y + 11, 18, 11, ink);
        break;
    case TOOL_SPRAY:
        opos_fill_rect(screen, x + 16, y + 12, 9, 14, ink);
        opos_fill_rect(screen, x + 18, y + 9, 5, 3, ink);
        for (int i = 0; i < 9; i++) {
            opos_fill_rect(screen, x + 5 + (i * 5) % 9, y + 6 + (i * 7) % 12,
                           2, 2, ink);
        }
        break;
    case TOOL_LINE:
        for (int i = 0; i < 20; i++) {
            opos_fill_rect(screen, x + 6 + i, y + 25 - i, 2, 2, ink);
        }
        break;
    case TOOL_RECT:
        opos_draw_rect(screen, x + 6, y + 9, 20, 14, ink);
        opos_draw_rect(screen, x + 7, y + 10, 18, 12, ink);
        break;
    case TOOL_ELLIPSE:
        for (int dx = -10; dx <= 10; dx++) {
            int dy = isqrt(100 - dx * dx) * 7 / 10;
            opos_fill_rect(screen, x + 15 + dx, y + 15 - dy, 2, 2, ink);
            opos_fill_rect(screen, x + 15 + dx, y + 15 + dy, 2, 2, ink);
        }
        break;
    case TOOL_FILL:
        for (int i = 0; i < 9; i++) {
            opos_fill_rect(screen, x + 6 + i, y + 14 - i, 12, 2, ink);
            opos_fill_rect(screen, x + 6 + i, y + 14 + i, 12, 2, ink);
        }
        opos_fill_rect(screen, x + 22, y + 18, 4, 7, OPOS_RGB(90, 160, 255));
        break;
    case TOOL_PICKER:
        for (int i = 0; i < 12; i++) {
            opos_fill_rect(screen, x + 7 + i, y + 25 - i, 3, 3, ink);
        }
        opos_fill_rect(screen, x + 18, y + 6, 8, 8, ink);
        break;
    case TOOL_COUNT:
        break;
    }
}

static void draw_toolbar(void) {
    fill((struct box){0, 0, view_width, TOP_H}, UI_PANEL);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        bool enabled = !(i == BUTTON_UNDO && undo_count == 0)
                    && !(i == BUTTON_REDO && redo_count == 0)
                    && !(i == BUTTON_ZOOM_OUT && zoom == 0)
                    && !(i == BUTTON_ZOOM_IN && zoom == ZOOM_COUNT - 1);
        draw_button(button_box(i), button_labels[i], false, enabled);
    }
    char percent[8];
    int value = zoom_num[zoom] * 100 / zoom_den[zoom];
    int used = 0;
    if (value >= 100) {
        if (value >= 1000) {
            percent[used++] = (char)('0' + value / 1000);
        }
        percent[used++] = (char)('0' + value / 100 % 10);
    }
    percent[used++] = (char)('0' + value / 10 % 10);
    percent[used++] = (char)('0' + value % 10);
    percent[used++] = '%';
    percent[used] = '\0';
    struct box minus = button_box(BUTTON_ZOOM_OUT);
    int room = button_box(BUTTON_ZOOM_IN).x - (minus.x + minus.w);
    opos_draw_text(screen,
                   minus.x + minus.w + (room - opos_text_width(percent, 2)) / 2,
                   minus.y + (minus.h - opos_char_height(2)) / 2, percent,
                   UI_TEXT, 2);
}

static void draw_tools(void) {
    fill((struct box){0, TOP_H, LEFT_W, view_height - TOP_H - BOTTOM_H},
         UI_PANEL);
    for (int i = 0; i < TOOL_COUNT; i++) {
        struct box box = tool_box(i);
        bool hover = inside(box, mouse_x, mouse_y);
        fill(box, tool == (enum tool)i ? UI_ACTIVE
                  : hover ? UI_HOVER : UI_BUTTON);
        draw_tool_icon((enum tool)i, box, UI_TEXT);
    }
    /* Filled or outlined shapes: a square drawn that way. */
    struct box fill_toggle = filled_box();
    draw_button(fill_toggle, "", filled, true);
    int square_x = fill_toggle.x + (fill_toggle.w - 30) / 2;
    if (filled) {
        opos_fill_rect(screen, square_x, fill_toggle.y + 5, 30, 16, UI_TEXT);
    } else {
        opos_draw_rect(screen, square_x, fill_toggle.y + 5, 30, 16, UI_TEXT);
        opos_draw_rect(screen, square_x + 1, fill_toggle.y + 6, 28, 14,
                       UI_TEXT);
    }
    for (int i = 0; i < BRUSH_COUNT; i++) {
        struct box box = brush_box(i);
        if (box.y + box.h > view_height - BOTTOM_H) {
            break;
        }
        bool hover = inside(box, mouse_x, mouse_y);
        fill(box, brush == i ? UI_ACTIVE : hover ? UI_HOVER : UI_BUTTON);
        int size = brush_sizes[i] > 20 ? 20 : brush_sizes[i];
        int thick = size < 2 ? 1 : size;
        opos_fill_rect(screen, box.x + 8, box.y + (box.h - thick) / 2,
                       box.w - 16, thick < 18 ? thick : 18, UI_TEXT);
    }
}

static void draw_palette(void) {
    int top = view_height - BOTTOM_H;
    fill((struct box){0, top, view_width, BOTTOM_H}, UI_PANEL);
    /* The second colour behind the first. */
    struct box second = colour_box(1), first = colour_box(0);
    fill((struct box){second.x - 1, second.y - 1, second.w + 2, second.h + 2},
         UI_TEXT);
    fill(second, colours[1]);
    fill((struct box){first.x - 1, first.y - 1, first.w + 2, first.h + 2},
         UI_TEXT);
    fill(first, colours[0]);
    for (int i = 0; i < 28; i++) {
        struct box box = palette_box(i);
        fill((struct box){box.x - 1, box.y - 1, box.w + 2, box.h + 2},
             OPOS_RGB(20, 22, 30));
        fill(box, palette[i]);
    }
    /* Status: position, size, file name or a message. */
    char status[96];
    int used = 0;
    const char *tool_name = tool_names[tool];
    while (*tool_name != '\0' && used < 30) {
        status[used++] = *tool_name++;
    }
    status[used++] = ' ';
    status[used++] = ' ';
    int numbers[4] = {hover_x, hover_y, image.width, image.height};
    for (int n = 0; n < 4; n++) {
        if (n < 2 && hover_x < 0) {
            continue;
        }
        char digits[12];
        int count = 0, value = numbers[n];
        do {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value > 0);
        while (count > 0) {
            status[used++] = digits[--count];
        }
        const char *after = n == 0 ? "," : n == 1 ? "   " : n == 2 ? "X" : "";
        while (*after != '\0') {
            status[used++] = *after++;
        }
    }
    status[used] = '\0';
    int text_x = palette_box(13).x + 36;
    int line1 = top + 8, line2 = top + 30;
    if (text_x < view_width - 60) {
        opos_draw_text(screen, text_x, line1, status, UI_DIM, 2);
        opos_draw_text(screen, text_x, line2,
                       message[0] != '\0' ? message : file_name,
                       message_warn ? UI_WARN : UI_TEXT, 2);
    }
}

/* The picture at its zoom, a checker-free grey around it, and a thin
   frame. */
static void draw_picture(void) {
    struct box area = work_area();
    fill(area, WORKSPACE);
    int ox, oy;
    picture_origin(&ox, &oy);
    int shown_w = image.width * zoom_num[zoom] / zoom_den[zoom];
    int shown_h = image.height * zoom_num[zoom] / zoom_den[zoom];
    int left = ox > area.x ? ox : area.x;
    int top = oy > area.y ? oy : area.y;
    int right = ox + shown_w < area.x + area.w ? ox + shown_w
                                               : area.x + area.w;
    int bottom = oy + shown_h < area.y + area.h ? oy + shown_h
                                                : area.y + area.h;
    if (left >= right || top >= bottom) {
        return;
    }
    if (ox > area.x || oy > area.y) {
        opos_draw_rect(screen, ox - 1, oy - 1, shown_w + 2, shown_h + 2,
                       OPOS_RGB(20, 22, 30));
    }
    static int *columns;
    static int column_capacity;
    if (column_capacity < right - left) {
        free(columns);
        columns = malloc((size_t)(right - left) * sizeof(int));
        column_capacity = columns != NULL ? right - left : 0;
        if (columns == NULL) {
            return;
        }
    }
    for (int x = left; x < right; x++) {
        int px = (x - ox) * zoom_den[zoom] / zoom_num[zoom];
        columns[x - left] = px < image.width ? px : image.width - 1;
    }
    for (int y = top; y < bottom; y++) {
        int py = (y - oy) * zoom_den[zoom] / zoom_num[zoom];
        if (py >= image.height) {
            py = image.height - 1;
        }
        const uint32_t *from = image.pixels + (size_t)py * image.width;
        uint32_t *to = canvas.pixels + (size_t)y * canvas.stride;
        for (int x = left; x < right; x++) {
            to[x] = from[columns[x - left]];
        }
    }
    /* Grid lines between pixels at 800%. */
    if (zoom == ZOOM_COUNT - 1) {
        uint32_t grid = OPOS_RGB(200, 200, 210);
        for (int x = ox + (left - ox + 7) / 8 * 8; x < right; x += 8) {
            for (int y = top; y < bottom; y += 2) {
                canvas.pixels[(size_t)y * canvas.stride + x] = grid;
            }
        }
        for (int y = oy + (top - oy + 7) / 8 * 8; y < bottom; y += 8) {
            for (int x = left; x < right; x += 2) {
                canvas.pixels[(size_t)y * canvas.stride + x] = grid;
            }
        }
    }
    /* Scroll bars when the picture is larger than the work area. */
    uint32_t bar = OPOS_RGB(150, 158, 180);
    if (shown_w > area.w) {
        int length = area.w * area.w / shown_w;
        int at = area.x + scroll_x * area.w / shown_w;
        opos_fill_rect(screen, at, area.y + area.h - 5, length, 4, bar);
    }
    if (shown_h > area.h) {
        int length = area.h * area.h / shown_h;
        int at = area.y + scroll_y * area.h / shown_h;
        opos_fill_rect(screen, area.x + area.w - 5, at, 4, length, bar);
    }
}

/* The brush outline under the mouse, for pen, eraser and spray. */
static void draw_cursor(void) {
    if (hover_x < 0 || drawing >= 0
        || !(tool == TOOL_PEN || tool == TOOL_ERASER || tool == TOOL_SPRAY)) {
        return;
    }
    int size = tool == TOOL_SPRAY ? brush_sizes[brush] * 4 + 9
                                  : brush_sizes[brush];
    int shown = size * zoom_num[zoom] / zoom_den[zoom];
    if (shown < 5) {
        return;
    }
    int ox, oy;
    picture_origin(&ox, &oy);
    int cx = ox + hover_x * zoom_num[zoom] / zoom_den[zoom];
    int cy = oy + hover_y * zoom_num[zoom] / zoom_den[zoom];
    opos_draw_rect(screen, cx - shown / 2, cy - shown / 2, shown, shown,
                   OPOS_RGB(255, 255, 255));
    opos_draw_rect(screen, cx - shown / 2 - 1, cy - shown / 2 - 1,
                   shown + 2, shown + 2, OPOS_RGB(0, 0, 0));
}

static struct box prompt_button(int index) {
    int width = 150, gap = 12;
    int total = 3 * width + 2 * gap;
    return (struct box){(view_width - total) / 2 + index * (width + gap),
                        view_height / 2 + 6, width, 32};
}

static void draw_prompt(void) {
    if (pending == PENDING_NONE) {
        return;
    }
    int width = 3 * 150 + 2 * 12 + 40;
    struct box panel = {(view_width - width) / 2, view_height / 2 - 50,
                        width, 100};
    fill((struct box){panel.x - 2, panel.y - 2, panel.w + 4, panel.h + 4},
         UI_ACTIVE);
    fill(panel, UI_PANEL);
    char question[96] = "SAVE CHANGES TO ";
    strncat(question, file_name, 40);
    strcat(question, "?");
    opos_draw_text(screen, panel.x + 20, panel.y + 16, question, UI_TEXT, 2);
    static const char *const labels[3] = {"SAVE", "DON'T SAVE", "CANCEL"};
    for (int i = 0; i < 3; i++) {
        draw_button(prompt_button(i), labels[i], i == 0, true);
    }
}

static void draw(void) {
    if (canvas.pixels == NULL) {
        return;
    }
    draw_picture();
    draw_cursor();
    draw_toolbar();
    draw_tools();
    draw_palette();
    draw_prompt();
    opos_canvas_present_area(&canvas, view_width, view_height);
}

/* -------------------------------------------------------------- input */

static void finish_pending(bool saved_or_dropped) {
    enum pending was = pending;
    pending = PENDING_NONE;
    if (!saved_or_dropped) {
        return;
    }
    if (was == PENDING_CLOSE) {
        opos_exit(0);
    } else if (was == PENDING_NEW) {
        do_new();
    } else if (was == PENDING_OPEN) {
        do_open();
    }
}

static void prompt_answer(int answer) {
    if (answer == 0) {
        finish_pending(do_save());
    } else if (answer == 1) {
        finish_pending(true);
    } else {
        finish_pending(false);
    }
}

/* New, Open and Close ask first when there are unsaved changes. */
static void request(enum pending what) {
    if (modified) {
        pending = what;
        return;
    }
    pending = what;
    finish_pending(true);
}

static void undo(void) {
    step(undo_stack, &undo_count, redo_stack, &redo_count);
}

static void redo(void) {
    step(redo_stack, &redo_count, undo_stack, &undo_count);
}

static void choose_tool(enum tool which) {
    if (which == TOOL_PICKER && tool != TOOL_PICKER) {
        tool_before_picker = tool;
    }
    tool = which;
}

static void press_button(int index) {
    switch (index) {
    case BUTTON_NEW:
        request(PENDING_NEW);
        break;
    case BUTTON_OPEN:
        request(PENDING_OPEN);
        break;
    case BUTTON_SAVE:
        do_save();
        break;
    case BUTTON_SAVE_AS:
        do_save_as();
        break;
    case BUTTON_UNDO:
        undo();
        break;
    case BUTTON_REDO:
        redo();
        break;
    case BUTTON_ZOOM_OUT:
        zoom_centre(zoom - 1);
        break;
    case BUTTON_ZOOM_IN:
        zoom_centre(zoom + 1);
        break;
    }
}

static void stroke_begin(int button, int px, int py) {
    if (tool == TOOL_PICKER) {
        if (px >= 0 && py >= 0 && px < image.width && py < image.height) {
            colours[button] =
                image.pixels[(size_t)py * image.width + (size_t)px];
            tool = tool_before_picker;
        }
        return;
    }
    if (tool == TOOL_FILL) {
        if (px < 0 || py < 0 || px >= image.width || py >= image.height) {
            return;
        }
        if (remember()) {
            flood_fill(px, py, colours[button]);
            set_modified(true);
        }
        return;
    }
    if (!remember()) {
        return;
    }
    drawing = button;
    start_x = last_x = px;
    start_y = last_y = py;
    uint32_t colour = tool == TOOL_ERASER ? colours[1] : colours[button];
    if (tool == TOOL_PEN || tool == TOOL_ERASER) {
        stamp(px, py, brush_sizes[brush], colour);
    } else if (tool == TOOL_SPRAY) {
        spray(px, py, colours[button]);
    } else {
        shape_preview(px, py);
    }
    set_modified(true);
}

static void stroke_move(int px, int py) {
    if (drawing < 0) {
        return;
    }
    if (tool == TOOL_PEN || tool == TOOL_ERASER) {
        uint32_t colour = tool == TOOL_ERASER ? colours[1] : colours[drawing];
        line(last_x, last_y, px, py, brush_sizes[brush], colour);
    } else if (tool == TOOL_SPRAY) {
        spray(px, py, colours[drawing]);
    } else {
        shape_preview(px, py);
    }
    last_x = px;
    last_y = py;
}

static void mouse_down(const struct opos_event *event) {
    int button = event->button == OPOS_BUTTON_RIGHT ? 1 : 0;
    int x = event->x, y = event->y;
    if (pending != PENDING_NONE) {
        for (int i = 0; i < 3; i++) {
            if (inside(prompt_button(i), x, y)) {
                prompt_answer(i);
            }
        }
        return;
    }
    say("", false);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (inside(button_box(i), x, y)) {
            press_button(i);
            return;
        }
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        if (inside(tool_box(i), x, y)) {
            choose_tool((enum tool)i);
            return;
        }
    }
    if (inside(filled_box(), x, y)) {
        filled = !filled;
        return;
    }
    for (int i = 0; i < BRUSH_COUNT; i++) {
        if (inside(brush_box(i), x, y)) {
            brush = i;
            return;
        }
    }
    for (int i = 0; i < 28; i++) {
        if (inside(palette_box(i), x, y)) {
            colours[button] = palette[i];
            return;
        }
    }
    if (inside(colour_box(0), x, y) || inside(colour_box(1), x, y)) {
        uint32_t swap = colours[0];
        colours[0] = colours[1];
        colours[1] = swap;
        return;
    }
    if (inside(work_area(), x, y)) {
        int px, py;
        to_picture(x, y, &px, &py);
        shift_held = shift_down;
        stroke_begin(button, px, py);
    }
}

static void key(const struct opos_event *event) {
    uint32_t c = event->key;
    bool ctrl = (event->buttons & OPOS_MOD_CTRL) != 0;
    bool shift = (event->buttons & OPOS_MOD_SHIFT) != 0;
    if (pending != PENDING_NONE) {
        if (c == '\n' || c == 'y' || c == 'Y') {
            prompt_answer(0);
        } else if (c == 'n' || c == 'N') {
            prompt_answer(1);
        } else if (c == OPOS_KEY_ESCAPE) {
            prompt_answer(2);
        }
        return;
    }
    say("", false);
    if (ctrl) {
        switch (c | 0x20) {
        case 'n':
            request(PENDING_NEW);
            break;
        case 'o':
            request(PENDING_OPEN);
            break;
        case 's':
            if (shift) {
                do_save_as();
            } else {
                do_save();
            }
            break;
        case 'z':
            undo();
            break;
        case 'y':
            redo();
            break;
        }
        return;
    }
    for (int i = 0; i < TOOL_COUNT; i++) {
        if ((c | 0x20) == (uint32_t)tool_keys[i]) {
            choose_tool((enum tool)i);
            return;
        }
    }
    int step_size = 64;
    if (c == 'g' || c == 'G') {
        filled = !filled;
    } else if (c == 'x' || c == 'X') {
        uint32_t swap = colours[0];
        colours[0] = colours[1];
        colours[1] = swap;
    } else if (c == '[' && brush > 0) {
        brush--;
    } else if (c == ']' && brush + 1 < BRUSH_COUNT) {
        brush++;
    } else if (c == '+' || c == '=') {
        zoom_centre(zoom + 1);
    } else if (c == '-') {
        zoom_centre(zoom - 1);
    } else if (c == OPOS_KEY_LEFT) {
        scroll_x -= step_size;
    } else if (c == OPOS_KEY_RIGHT) {
        scroll_x += step_size;
    } else if (c == OPOS_KEY_UP) {
        scroll_y -= step_size;
    } else if (c == OPOS_KEY_DOWN) {
        scroll_y += step_size;
    } else if (c == OPOS_KEY_PAGE_UP) {
        scroll_y -= work_area().h * 3 / 4;
    } else if (c == OPOS_KEY_PAGE_DOWN) {
        scroll_y += work_area().h * 3 / 4;
    } else if (c == OPOS_KEY_ESCAPE && drawing >= 0) {
        /* Esc drops the stroke being drawn. */
        undo();
        stack_clear(redo_stack, &redo_count);
        drawing = -1;
    }
    clamp_scroll();
}

static void wheel(const struct opos_event *event) {
    int notches = OPOS_WHEEL_NOTCHES(*event);
    if (ctrl_down) {
        set_zoom(zoom + (notches > 0 ? 1 : -1), event->x, event->y);
        return;
    }
    if (shift_down) {
        scroll_x -= notches * 48;
    } else {
        scroll_y -= notches * 48;
    }
    clamp_scroll();
}

static void track_hover(int x, int y) {
    mouse_x = x;
    mouse_y = y;
    int px, py;
    to_picture(x, y, &px, &py);
    if (inside(work_area(), x, y) && px >= 0 && py >= 0 && px < image.width
        && py < image.height) {
        hover_x = px;
        hover_y = py;
    } else {
        hover_x = hover_y = -1;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    random_state ^= (uint64_t)high << 32 | low;
    if (opos_window_create("Picasso", 1100, 760) < 0) {
        opos_print("picasso: no window; run it from the desktop\n");
        return 1;
    }
    if (!new_picture(NEW_WIDTH, NEW_HEIGHT)) {
        return 1;
    }
    /* Older OPOS versions have no ASK_CLOSE: their close button removes
       the window before CLOSE arrives, so Picasso cannot ask then. */
    if (opos_window_options(OPOS_WINDOW_KEY_STATE | OPOS_WINDOW_ASK_CLOSE)
        < 0) {
        opos_window_options(OPOS_WINDOW_KEY_STATE);
        close_removes_window = true;
    }
    update_title();
    struct opos_event event;
    for (;;) {
        /* The spray keeps spraying while the button is held. */
        long timeout = drawing >= 0 && tool == TOOL_SPRAY ? 40 : -1;
        long got = opos_window_event(&event, timeout);
        if (got < 0) {
            continue;
        }
        if (got == 0) {
            if (drawing >= 0 && tool == TOOL_SPRAY) {
                spray(last_x, last_y, colours[drawing]);
                draw();
            }
            continue;
        }
        switch (event.type) {
        case OPOS_EVENT_RESIZE: {
            bool first = view_width == 0;
            view_width = (int)event.width;
            view_height = (int)event.height;
            if (canvas.pixels == NULL || canvas.width < view_width
                || canvas.height < view_height) {
                opos_canvas_destroy(&canvas);
                canvas = opos_canvas_create(view_width, view_height);
                if (canvas.pixels == NULL) {
                    opos_print("picasso: out of memory\n");
                    return 1;
                }
            }
            if (first || zoom_auto) {
                zoom_to_fit();
            }
            clamp_scroll();
            break;
        }
        case OPOS_EVENT_MOUSE_DOWN:
            track_hover(event.x, event.y);
            mouse_down(&event);
            break;
        case OPOS_EVENT_MOUSE_MOVE: {
            track_hover(event.x, event.y);
            if (drawing >= 0) {
                int px, py;
                to_picture(event.x, event.y, &px, &py);
                shift_held = shift_down;
                stroke_move(px, py);
            }
            break;
        }
        case OPOS_EVENT_MOUSE_UP:
            track_hover(event.x, event.y);
            drawing = -1;
            break;
        case OPOS_EVENT_WHEEL:
            wheel(&event);
            track_hover(event.x, event.y);
            break;
        case OPOS_EVENT_KEY:
            key(&event);
            break;
        case OPOS_EVENT_KEY_STATE: {
            /* Scan codes: Shift 0x2a and 0x36, Ctrl 0x1d (right: 0x9d). */
            bool down = event.button != 0;
            if (event.key == 0x2a || event.key == 0x36) {
                shift_down = down;
            } else if (event.key == 0x1d || event.key == 0x9d) {
                ctrl_down = down;
            }
            if (drawing >= 0 && event.key != 0x1d && event.key != 0x9d) {
                /* Shift changes a line or shape at once. */
                shift_held = shift_down;
                stroke_move(last_x, last_y);
            }
            break;
        }
        case OPOS_EVENT_CLOSE:
            if (close_removes_window) {
                opos_exit(0);
            }
            drawing = -1;
            request(PENDING_CLOSE);
            break;
        default:
            break;
        }
        draw();
    }
}
