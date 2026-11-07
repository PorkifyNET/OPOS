/*
 * OPOS browser: window, address bar, page loading, scrolling and painting.
 *
 *   browser [URL]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_tls.h>

#include "browser.h"

#define TOOLBAR_HEIGHT 40
#define STATUS_HEIGHT 20
#define SCROLLBAR_WIDTH 12
#define MARGIN 8
#define HISTORY_MAX 64
#define MAX_PAGE_BYTES (8u << 20)
#define MAX_IMAGE_BYTES (6u << 20)
#define HOME_URL "https://example.com/"

#define COL_TOOLBAR OPOS_RGB(228, 230, 236)
#define COL_FIELD OPOS_RGB(255, 255, 255)
#define COL_BORDER OPOS_RGB(140, 144, 156)
#define COL_TEXT OPOS_RGB(30, 30, 36)
#define COL_STATUS OPOS_RGB(240, 240, 244)

static const char *font_files[FONT_COUNT] = {
    "NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoSans-Italic.ttf",
    "NotoSans-BoldItalic.ttf", "NotoSansMono-Regular.ttf",
    "NotoSansMono-Bold.ttf"};

struct browser {
    struct opos_font *fonts[FONT_COUNT];
    struct opos_canvas canvas;
    int width, height;            /* window content size */
    struct page page;
    bool have_page;
    int scroll;
    char url[OPOS_HTTP_URL_MAX];
    char address[OPOS_HTTP_URL_MAX];
    size_t address_length;
    bool editing;
    bool address_selected;        /* the next typed key replaces it all */
    char status[OPOS_HTTP_URL_MAX + 32];
    char history[HISTORY_MAX][OPOS_HTTP_URL_MAX];
    int history_count;
    int history_position;
    bool dirty;
    bool dragging_scrollbar;
    /* The page's text as one string (words joined with spaces and line
       breaks) and, per display item, where its text starts in it
       (NO_TEXT for items without text). Selection and find use offsets
       into this string. */
    char *text;
    size_t text_length;
    size_t *item_start;
    size_t selection_anchor, selection_cursor;
    bool selecting;               /* left button held on the page */
    bool field_selecting;         /* left button held in a form field */
    const struct opos_html_node *field_selection;
    bool pressed_moved;
    int press_x, press_y;
    bool finding;                 /* the find bar is open */
    char find[128];
    size_t find_length;
    size_t match;                 /* start of the current match */
    bool have_match;
    const struct opos_html_node *dropdown; /* the open <select> list */
};

#define NO_TEXT ((size_t)-1)
#define COL_SELECTION OPOS_RGB(180, 213, 254)
#define COL_MATCH OPOS_RGB(255, 210, 90)

static struct browser b;

/* --------------------------------------------------------------- helpers */

static int viewport_width(void) { return b.width - SCROLLBAR_WIDTH; }
static int viewport_height(void) {
    return b.height - TOOLBAR_HEIGHT - STATUS_HEIGHT;
}

static int max_scroll(void) {
    if (!b.have_page) {
        return 0;
    }
    int limit = b.page.content_height + 2 * MARGIN - viewport_height();
    return limit < 0 ? 0 : limit;
}

static void clamp_scroll(void) {
    if (b.scroll > max_scroll()) {
        b.scroll = max_scroll();
    }
    if (b.scroll < 0) {
        b.scroll = 0;
    }
}

static void set_status(const char *text) {
    snprintf(b.status, sizeof(b.status), "%s", text);
    b.dirty = true;
}

static void set_address(const char *url) {
    snprintf(b.address, sizeof(b.address), "%s", url);
    b.address_length = strlen(b.address);
}

/* ------------------------------------------------------------ page text */

static bool is_text_item(const struct display_item *item) {
    return item->kind == ITEM_TEXT && item->length > 0
        && !(item->length == 1 && item->text[0] == ' ');
}

static void text_put(size_t *capacity, const char *s, size_t n) {
    if (b.text == NULL) {
        return;
    }
    if (b.text_length + n + 1 > *capacity) {
        size_t grown = (*capacity + n + 1) * 2;
        char *bigger = realloc(b.text, grown);
        if (bigger == NULL) {
            free(b.text);
            b.text = NULL;
            return;
        }
        b.text = bigger;
        *capacity = grown;
    }
    memcpy(b.text + b.text_length, s, n);
    b.text_length += n;
    b.text[b.text_length] = '\0';
}

/* Rebuilds b.text and b.item_start from the display list. A word on a
   new line starts with a line break (two after a gap, as between
   paragraphs); a word after a gap on the same line starts with a space. */
static void build_text(void) {
    free(b.text);
    free(b.item_start);
    b.text = NULL;
    b.item_start = NULL;
    b.text_length = 0;
    if (!b.have_page) {
        return;
    }
    size_t count = b.page.item_count;
    b.item_start = malloc((count + 1) * sizeof(size_t));
    size_t capacity = 4096;
    b.text = malloc(capacity);
    if (b.item_start == NULL || b.text == NULL) {
        free(b.item_start);
        free(b.text);
        b.item_start = NULL;
        b.text = NULL;
        return;
    }
    b.text[0] = '\0';
    const struct display_item *previous = NULL;
    for (size_t i = 0; i < count; i++) {
        const struct display_item *item = &b.page.items[i];
        b.item_start[i] = NO_TEXT;
        if (!is_text_item(item)) {
            continue;
        }
        if (previous != NULL) {
            int previous_bottom = previous->y + previous->height;
            if (item->y >= previous->y + previous->height / 2
                || item->y + item->height <= previous->y) {
                bool gap = item->y > previous_bottom + previous->height / 2;
                text_put(&capacity, "\n\n", gap ? 2 : 1);
            } else if (item->x > previous->x + previous->width
                       || item->x < previous->x) {
                text_put(&capacity, " ", 1);
            }
        }
        if (b.text == NULL) {
            free(b.item_start);
            b.item_start = NULL;
            b.text_length = 0;
            return;
        }
        b.item_start[i] = b.text_length;
        text_put(&capacity, item->text, item->length);
        previous = item;
    }
    if (b.text == NULL) {
        free(b.item_start);
        b.item_start = NULL;
        b.text_length = 0;
    }
}

static size_t selection_start(void) {
    return b.selection_anchor < b.selection_cursor ? b.selection_anchor
                                                   : b.selection_cursor;
}

static size_t selection_end(void) {
    return b.selection_anchor > b.selection_cursor ? b.selection_anchor
                                                   : b.selection_cursor;
}

static void clear_selection(void) {
    b.selection_anchor = b.selection_cursor = 0;
    b.have_match = false;
}

/* Width of the first n bytes of a text item. */
static int item_prefix_width(const struct display_item *item, size_t n) {
    if (n >= item->length) {
        return item->width;
    }
    return opos_font_text_width(b.fonts[item->font], item->size, item->text,
                                n);
}

static bool field_item_selection(const struct display_item *item,
                                 size_t *from, size_t *to) {
    if (item->control == NULL || b.page.focus != item->control) {
        return false;
    }
    struct form_field *field = form_field(&b.page, item->control);
    if (field == NULL || (field->kind != FIELD_TEXT
                          && field->kind != FIELD_PASSWORD
                          && field->kind != FIELD_TEXTAREA)) {
        return false;
    }
    size_t low = field->selection_anchor < field->selection_cursor
               ? field->selection_anchor : field->selection_cursor;
    size_t high = field->selection_anchor > field->selection_cursor
                ? field->selection_anchor : field->selection_cursor;
    if (low == high) {
        return false;
    }
    size_t end = field->kind == FIELD_PASSWORD
        ? browser_utf8_offset(field->value + item->field_offset,
                              field->length - item->field_offset,
                              item->length) + item->field_offset
        : item->field_offset + item->length;
    if (high <= item->field_offset || low >= end) {
        return false;
    }
    size_t clipped_low = low > item->field_offset ? low : item->field_offset;
    size_t clipped_high = high < end ? high : end;
    if (field->kind == FIELD_PASSWORD) {
        *from = browser_utf8_count(field->value + item->field_offset,
                                   clipped_low - item->field_offset);
        *to = browser_utf8_count(field->value + item->field_offset,
                                 clipped_high - item->field_offset);
    } else {
        *from = clipped_low - item->field_offset;
        *to = clipped_high - item->field_offset;
    }
    return *to > *from;
}

/* The text offset nearest to a point in page coordinates. */
static size_t text_offset_at(int x, int y) {
    if (b.item_start == NULL) {
        return 0;
    }
    long best_distance = -1;
    size_t best = 0;
    for (size_t i = 0; i < b.page.item_count; i++) {
        if (b.item_start[i] == NO_TEXT) {
            continue;
        }
        const struct display_item *item = &b.page.items[i];
        long dy = y < item->y ? item->y - y
                : y >= item->y + item->height ? y - (item->y + item->height) + 1
                : 0;
        long dx = x < item->x ? item->x - x
                : x >= item->x + item->width ? x - (item->x + item->width) + 1
                : 0;
        long distance = dy * 65536 + dx;
        if (best_distance < 0 || distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    if (best_distance < 0) {
        return 0;
    }
    const struct display_item *item = &b.page.items[best];
    size_t start = b.item_start[best];
    if (x <= item->x) {
        return start;
    }
    if (x >= item->x + item->width) {
        return start + item->length;
    }
    /* Walk the characters; stop at the boundary nearest to x. */
    int previous_width = 0;
    size_t at = 0;
    while (at < item->length) {
        size_t next = at + 1;
        while (next < item->length && (item->text[next] & 0xc0) == 0x80) {
            next++;
        }
        int width = item_prefix_width(item, next);
        if (item->x + (previous_width + width) / 2 > x) {
            return start + at;
        }
        previous_width = width;
        at = next;
    }
    return start + item->length;
}

/* The display item whose text holds offset, or NULL. */
static const struct display_item *item_at_offset(size_t offset) {
    if (b.item_start == NULL) {
        return NULL;
    }
    const struct display_item *found = NULL;
    for (size_t i = 0; i < b.page.item_count; i++) {
        if (b.item_start[i] == NO_TEXT) {
            continue;
        }
        if (b.item_start[i] > offset) {
            break;
        }
        found = &b.page.items[i];
    }
    return found;
}

static void copy_selection(void) {
    size_t start = selection_start(), end = selection_end();
    if (b.text == NULL || end <= start || end > b.text_length) {
        return;
    }
    long kept = opos_clipboard_set(b.text + start, end - start);
    set_status(kept < 0 ? "Could not copy" : "Copied");
}

static char fold(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool match_at(size_t at) {
    if (at + b.find_length > b.text_length) {
        return false;
    }
    for (size_t i = 0; i < b.find_length; i++) {
        char want = fold(b.find[i]);
        char have = fold(b.text[at + i]);
        if (want == ' ' ? (have != ' ' && have != '\n') : want != have) {
            return false;
        }
    }
    return true;
}

/* Scrolls so that the text at offset is in view. */
static void reveal_offset(size_t offset) {
    const struct display_item *item = item_at_offset(offset);
    if (item == NULL) {
        return;
    }
    int top = item->y + MARGIN;
    if (top < b.scroll || top + item->height > b.scroll + viewport_height()) {
        b.scroll = top - viewport_height() / 3;
        clamp_scroll();
    }
}

/* Finds the query from offset onwards (backwards when step < 0), wrapping
   around the page, and selects the match. */
static void find_from(size_t from, int step) {
    b.have_match = false;
    if (b.text == NULL || b.find_length == 0
        || b.find_length > b.text_length) {
        b.dirty = true;
        return;
    }
    size_t last = b.text_length - b.find_length;
    if (from > last) {
        from = step > 0 ? 0 : last;
    }
    size_t at = from;
    for (size_t tried = 0; tried <= last; tried++) {
        if (match_at(at)) {
            b.have_match = true;
            b.match = at;
            b.selection_anchor = at;
            b.selection_cursor = at + b.find_length;
            reveal_offset(at);
            break;
        }
        if (step > 0) {
            at = at == last ? 0 : at + 1;
        } else {
            at = at == 0 ? last : at - 1;
        }
    }
    b.dirty = true;
}

/* Number of matches, and which one the current match is (1-based). */
static size_t count_matches(size_t *index) {
    size_t count = 0;
    *index = 0;
    if (b.text == NULL || b.find_length == 0) {
        return 0;
    }
    for (size_t at = 0; at + b.find_length <= b.text_length; at++) {
        if (match_at(at)) {
            count++;
            if (b.have_match && at == b.match) {
                *index = count;
            }
        }
    }
    return count;
}

/* ----------------------------------------------------------------- paint */

static void blend_image(struct image_ref *ref, int x, int y, int width,
                        int height, int clip_top, int clip_bottom) {
    if (ref->scaled == NULL || ref->scaled_width != width
        || ref->scaled_height != height) {
        free(ref->scaled);
        ref->scaled = malloc((size_t)width * height * 4);
        ref->scaled_width = width;
        ref->scaled_height = height;
        if (ref->scaled == NULL) {
            return;
        }
        if (width == ref->width && height == ref->height) {
            memcpy(ref->scaled, ref->pixels, (size_t)width * height * 4);
        } else {
            image_resample(ref->pixels, (uint32_t)ref->width, 0, 0,
                           (uint32_t)ref->width, (uint32_t)ref->height,
                           ref->scaled, (uint32_t)width, (uint32_t)width,
                           (uint32_t)height);
        }
    }
    struct opos_canvas *canvas = &b.canvas;
    for (int row = 0; row < height; row++) {
        int dy = y + row;
        if (dy < clip_top || dy >= clip_bottom || dy < 0
            || dy >= canvas->height) {
            continue;
        }
        const uint32_t *source = ref->scaled + (size_t)row * width;
        uint32_t *target = canvas->pixels + (size_t)dy * canvas->stride;
        for (int col = 0; col < width; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= viewport_width()) {
                continue;
            }
            uint32_t p = source[col];
            uint32_t alpha = p >> 24;
            if (alpha == 255) {
                target[dx] = p & 0x00ffffff;
            } else if (alpha != 0) {
                uint32_t q = target[dx];
                uint32_t r = (((p >> 16) & 255) * alpha
                              + ((q >> 16) & 255) * (255 - alpha)) / 255;
                uint32_t g = (((p >> 8) & 255) * alpha
                              + ((q >> 8) & 255) * (255 - alpha)) / 255;
                uint32_t bl = ((p & 255) * alpha
                               + (q & 255) * (255 - alpha)) / 255;
                target[dx] = r << 16 | g << 8 | bl;
            }
        }
    }
}

static void fill_clipped(int x, int y, int width, int height, uint32_t colour,
                         int top, int bottom) {
    if (y < top) {
        height -= top - y;
        y = top;
    }
    if (y + height > bottom) {
        height = bottom - y;
    }
    if (height > 0 && x < viewport_width()) {
        if (x + width > viewport_width()) {
            width = viewport_width() - x;
        }
        opos_fill_rect(&b.canvas, x, y, width, height, colour);
    }
}

/* Fills a rectangle with colour at alpha (1-254) over what is there. */
static void blend_clipped(int x, int y, int width, int height,
                          uint32_t colour, int alpha, int top, int bottom) {
    if (y < top) {
        height -= top - y;
        y = top;
    }
    if (y + height > bottom) {
        height = bottom - y;
    }
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (x + width > viewport_width()) {
        width = viewport_width() - x;
    }
    if (width <= 0 || height <= 0 || y + height > b.canvas.height) {
        return;
    }
    uint32_t r = (colour >> 16) & 255, g = (colour >> 8) & 255,
             bl = colour & 255;
    uint32_t keep = 255 - (uint32_t)alpha;
    for (int row = 0; row < height; row++) {
        uint32_t *p = b.canvas.pixels + (size_t)(y + row) * b.canvas.stride
                    + x;
        for (int col = 0; col < width; col++) {
            uint32_t q = p[col];
            p[col] = ((r * alpha + ((q >> 16) & 255) * keep) / 255) << 16
                   | ((g * alpha + ((q >> 8) & 255) * keep) / 255) << 8
                   | (bl * alpha + (q & 255) * keep) / 255;
        }
    }
}

static void paint_page(void) {
    int top = TOOLBAR_HEIGHT;
    int bottom = top + viewport_height();
    struct opos_canvas *canvas = &b.canvas;
    uint32_t background = b.have_page ? b.page.background : 0x00ffffff;
    opos_fill_rect(canvas, 0, top, viewport_width(), viewport_height(),
                   background);
    if (!b.have_page) {
        return;
    }
    int origin_y = top + MARGIN - b.scroll;
    int origin_x = MARGIN;
    for (size_t i = 0; i < b.page.item_count; i++) {
        const struct display_item *item = &b.page.items[i];
        int x = origin_x + item->x;
        int y = origin_y + item->y;
        if (y + item->height < top || y > bottom) {
            continue;
        }
        if (item->round
            && (item->kind == ITEM_RECT || item->kind == ITEM_OUTLINE)) {
            /* A disc, or a one-pixel ring: per row, the span between
               the circle's edges (for a ring, only those edges). */
            int size = item->width;
            for (int row = 0; row < size; row++) {
                int in = round_inset(size, row);
                int span = size - 2 * in;
                if (item->kind == ITEM_RECT) {
                    fill_clipped(x + in, y + row, span, 1, item->colour,
                                 top, bottom);
                    continue;
                }
                int above = round_inset(size, row - 1);
                int below = round_inset(size, row + 1);
                int inner = above > below ? above : below;
                int edge = inner > in ? inner - in : 1;
                if (above < 0 || below < 0) {
                    edge = span; /* top and bottom rows are solid */
                }
                fill_clipped(x + in, y + row, edge, 1, item->colour, top,
                             bottom);
                fill_clipped(x + size - in - edge, y + row, edge, 1,
                             item->colour, top, bottom);
            }
            continue;
        }
        switch (item->kind) {
        case ITEM_RECT:
            if (item->transparency == 0) {
                fill_clipped(x, y, item->width, item->height, item->colour,
                             top, bottom);
            } else if (item->transparency < 255) {
                blend_clipped(x, y, item->width, item->height, item->colour,
                              255 - item->transparency, top, bottom);
            }
            break;
        case ITEM_OUTLINE:
            fill_clipped(x, y, item->width, 1, item->colour, top, bottom);
            fill_clipped(x, y + item->height - 1, item->width, 1,
                         item->colour, top, bottom);
            fill_clipped(x, y, 1, item->height, item->colour, top, bottom);
            fill_clipped(x + item->width - 1, y, 1, item->height,
                         item->colour, top, bottom);
            break;
        case ITEM_TEXT: {
            int baseline = y + item->baseline;
            if (baseline - item->size > bottom || baseline + item->size < top) {
                break;
            }
            size_t field_from, field_to;
            if (field_item_selection(item, &field_from, &field_to)) {
                int x0 = x + item_prefix_width(item, field_from);
                int x1 = x + item_prefix_width(item, field_to);
                fill_clipped(x0, y, x1 - x0, item->height, COL_SELECTION,
                             top, bottom);
            }
            size_t start = b.item_start != NULL ? b.item_start[i] : NO_TEXT;
            size_t low = selection_start(), high = selection_end();
            if (start != NO_TEXT && high > low && high > start
                && low < start + item->length) {
                size_t from = low > start ? low - start : 0;
                size_t to = high - start < item->length ? high - start
                                                        : item->length;
                int x0 = x + item_prefix_width(item, from);
                int x1 = x + item_prefix_width(item, to);
                /* Cover the space before the word too when the selection
                   runs on from the previous word on the same line. */
                if (from == 0 && low < start && start > 0
                    && b.text[start - 1] == ' ') {
                    x0 -= opos_font_text_width(b.fonts[item->font],
                                               item->size, " ", 1);
                }
                bool is_match = b.have_match && low == b.match;
                fill_clipped(x0, y, x1 - x0, item->height,
                             is_match ? COL_MATCH : COL_SELECTION, top,
                             bottom);
            }
            opos_font_draw(b.fonts[item->font], canvas, x, baseline,
                           item->size, item->text, item->length,
                           item->colour);
            if (item->underline) {
                fill_clipped(x, baseline + 2, item->width, 1, item->colour,
                             top, bottom);
            }
            if (item->strike) {
                fill_clipped(x, baseline - item->size / 3, item->width, 1,
                             item->colour, top, bottom);
            }
            break;
        }
        case ITEM_IMAGE:
            if (item->image != NULL && item->image->state == 1
                && item->image->pixels != NULL) {
                blend_image(item->image, x, y, item->width, item->height, top,
                            bottom);
            } else {
                fill_clipped(x, y, item->width, item->height, 0x00ececec, top,
                             bottom);
                fill_clipped(x, y, item->width, 1, 0x00b0b0b0, top, bottom);
                fill_clipped(x, y + item->height - 1, item->width, 1,
                             0x00b0b0b0, top, bottom);
            }
            break;
        }
    }
    /* Scrollbar. */
    int track_x = viewport_width();
    opos_fill_rect(canvas, track_x, top, SCROLLBAR_WIDTH, viewport_height(),
                   OPOS_RGB(236, 236, 240));
    int total = b.page.content_height + 2 * MARGIN;
    if (total > viewport_height()) {
        int thumb = (int)((long long)viewport_height() * viewport_height() / total);
        if (thumb < 24) {
            thumb = 24;
        }
        int range = viewport_height() - thumb;
        int offset = max_scroll() > 0 ? (int)((long long)range * b.scroll / max_scroll()) : 0;
        opos_fill_rect(canvas, track_x + 2, top + offset, SCROLLBAR_WIDTH - 4,
                       thumb, OPOS_RGB(160, 164, 176));
    }
}

static void draw_button(int x, const char *label, bool enabled) {
    opos_fill_rect(&b.canvas, x, 6, 28, 28, enabled ? COL_FIELD
                                                   : OPOS_RGB(214, 216, 222));
    opos_draw_rect(&b.canvas, x, 6, 28, 28, COL_BORDER);
    opos_draw_text(&b.canvas, x + 14 - (int)strlen(label) * 6, 11, label,
                   enabled ? COL_TEXT : OPOS_RGB(150, 152, 160), 2);
}

/* ------------------------------------------------------ select drop-down */

static void refresh_controls(void);

#define DROP_ROW 22
#define DROP_TEXT 14
#define DROP_MAX_ROWS 12

/* An <option>'s text with white space collapsed. */
static void option_label(const struct opos_html_node *node, char *out,
                         size_t size) {
    size_t used = 0;
    bool space = true;
    const struct opos_html_node *stack[16];
    int depth = 0;
    const struct opos_html_node *n = node != NULL ? node->first_child : NULL;
    while (n != NULL && used + 1 < size) {
        if (n->type == HTML_NODE_TEXT) {
            for (size_t i = 0; i < n->text_length && used + 1 < size; i++) {
                char ch = n->text[i];
                bool ws = ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r';
                if (ws && !space) {
                    out[used++] = ' ';
                } else if (!ws) {
                    out[used++] = ch;
                }
                space = ws;
            }
        } else if (n->first_child != NULL && depth < 16) {
            stack[depth++] = n;
            n = n->first_child;
            continue;
        }
        while (n->next_sibling == NULL && depth > 0) {
            n = stack[--depth];
        }
        n = n->next_sibling;
    }
    while (used > 0 && out[used - 1] == ' ') {
        used--;
    }
    out[used] = '\0';
}

/* The open list's place on the screen; false if the select is not shown. */
static bool dropdown_rect(int *x, int *y, int *width, int *rows) {
    if (b.dropdown == NULL || !b.have_page) {
        return false;
    }
    for (size_t i = 0; i < b.page.item_count; i++) {
        const struct display_item *item = &b.page.items[i];
        if (item->kind != ITEM_RECT || item->control != b.dropdown) {
            continue;
        }
        int count = form_option_count(b.dropdown);
        *rows = count < DROP_MAX_ROWS ? count : DROP_MAX_ROWS;
        *x = MARGIN + item->x;
        *width = item->width < 120 ? 120 : item->width;
        int top = TOOLBAR_HEIGHT + MARGIN + item->y - b.scroll;
        int bottom = TOOLBAR_HEIGHT + viewport_height();
        *y = top + item->height;
        if (*y + *rows * DROP_ROW + 2 > bottom && top - *rows * DROP_ROW - 2
                                                  >= TOOLBAR_HEIGHT) {
            *y = top - *rows * DROP_ROW - 2; /* no room below: open up */
        }
        return *rows > 0;
    }
    return false;
}

/* The first option shown, so that the chosen one is in view. */
static int dropdown_first(int rows) {
    struct form_field *field = form_field(&b.page, b.dropdown);
    int chosen = field != NULL ? field->option : 0;
    int count = form_option_count(b.dropdown);
    int first = chosen - rows + 1;
    if (first < 0) {
        first = 0;
    }
    if (first > count - rows) {
        first = count - rows;
    }
    return first < 0 ? 0 : first;
}

static void paint_dropdown(void) {
    int x, y, width, rows;
    if (!dropdown_rect(&x, &y, &width, &rows)) {
        return;
    }
    struct opos_canvas *canvas = &b.canvas;
    struct form_field *field = form_field(&b.page, b.dropdown);
    int chosen = field != NULL ? field->option : 0;
    int first = dropdown_first(rows);
    int height = rows * DROP_ROW + 2;
    opos_fill_rect(canvas, x, y, width, height, COL_FIELD);
    opos_draw_rect(canvas, x, y, width, height, COL_BORDER);
    for (int row = 0; row < rows; row++) {
        int index = first + row;
        char label[128];
        option_label(form_option(b.dropdown, index), label, sizeof(label));
        int row_y = y + 1 + row * DROP_ROW;
        uint32_t colour = COL_TEXT;
        if (index == chosen) {
            opos_fill_rect(canvas, x + 1, row_y, width - 2, DROP_ROW,
                           OPOS_RGB(6, 69, 173));
            colour = OPOS_RGB(255, 255, 255);
        }
        opos_font_draw(b.fonts[0], canvas, x + 6, row_y + 16, DROP_TEXT,
                       label, strlen(label), colour);
    }
}

/* A click while the list is open: picks the option under it, if any,
   and closes the list. */
static void dropdown_click(int click_x, int click_y) {
    int x, y, width, rows;
    if (dropdown_rect(&x, &y, &width, &rows) && click_x >= x
        && click_x < x + width && click_y > y && click_y < y + rows * DROP_ROW) {
        struct form_field *field = form_field(&b.page, b.dropdown);
        if (field != NULL) {
            field->option = dropdown_first(rows) + (click_y - y - 1) / DROP_ROW;
        }
    }
    b.page.focus = b.dropdown;
    b.dropdown = NULL;
    refresh_controls();
    b.dirty = true;
}

static void paint(void) {
    struct opos_canvas *canvas = &b.canvas;
    opos_fill_rect(canvas, 0, 0, b.width, b.height, COL_TOOLBAR);
    draw_button(6, "<", b.history_position > 0);
    draw_button(38, ">", b.history_position + 1 < b.history_count);
    draw_button(70, "R", b.have_page);
    int field_x = 106;
    int field_w = b.width - field_x - 8;
    opos_fill_rect(canvas, field_x, 6, field_w, 28, COL_FIELD);
    opos_draw_rect(canvas, field_x, 6, field_w, 28,
                   b.editing ? OPOS_RGB(6, 69, 173) : COL_BORDER);
    int text_width = opos_font_text_width(b.fonts[0], 15, b.address,
                                          b.address_length);
    int shift = text_width > field_w - 16 ? text_width - (field_w - 16) : 0;
    if (b.editing && b.address_selected && text_width > 0) {
        int from = field_x + 8 - shift;
        int width = text_width + 2;
        if (from < field_x + 2) {
            width -= field_x + 2 - from;
            from = field_x + 2;
        }
        if (from + width > field_x + field_w - 2) {
            width = field_x + field_w - 2 - from;
        }
        opos_fill_rect(canvas, from, 9, width, 22, COL_SELECTION);
    }
    /* Draw the address, then repaint the field edges over the overflow. */
    opos_font_draw(b.fonts[0], canvas, field_x + 8 - shift, 25, 15,
                   b.address, b.address_length, COL_TEXT);
    if (shift > 0) {
        opos_fill_rect(canvas, 0, 0, field_x, TOOLBAR_HEIGHT, COL_TOOLBAR);
        opos_fill_rect(canvas, field_x + field_w, 0, 8, TOOLBAR_HEIGHT,
                       COL_TOOLBAR);
        draw_button(6, "<", b.history_position > 0);
        draw_button(38, ">", b.history_position + 1 < b.history_count);
        draw_button(70, "R", b.have_page);
        opos_draw_rect(canvas, field_x, 6, field_w, 28,
                       b.editing ? OPOS_RGB(6, 69, 173) : COL_BORDER);
    }
    if (b.editing) {
        int cursor = field_x + 8 + text_width - shift;
        opos_fill_rect(canvas, cursor + 1, 11, 1, 18, COL_TEXT);
    }
    paint_page();
    paint_dropdown();
    int status_y = b.height - STATUS_HEIGHT;
    opos_fill_rect(canvas, 0, status_y, b.width, STATUS_HEIGHT, COL_STATUS);
    opos_draw_line(canvas, 0, status_y, b.width, status_y,
                   OPOS_RGB(200, 200, 208));
    if (b.finding) {
        /* Find bar: "Find: query|   3 of 12". */
        int label_end = opos_font_draw(b.fonts[1], canvas, 6, status_y + 15,
                                       12, "Find:", 5, OPOS_RGB(70, 70, 80));
        int box_x = label_end + 6;
        int box_w = 260;
        opos_fill_rect(canvas, box_x, status_y + 2, box_w, STATUS_HEIGHT - 3,
                       COL_FIELD);
        opos_draw_rect(canvas, box_x, status_y + 2, box_w, STATUS_HEIGHT - 3,
                       OPOS_RGB(6, 69, 173));
        int end = opos_font_draw(b.fonts[0], canvas, box_x + 4, status_y + 15,
                                 12, b.find, b.find_length, COL_TEXT);
        opos_fill_rect(canvas, end + 1, status_y + 4, 1, STATUS_HEIGHT - 7,
                       COL_TEXT);
        char result[64] = "";
        size_t index;
        size_t count = count_matches(&index);
        if (b.find_length > 0) {
            if (count == 0) {
                snprintf(result, sizeof(result), "No matches");
            } else {
                snprintf(result, sizeof(result), "%u of %u", (unsigned)index,
                         (unsigned)count);
            }
        }
        opos_font_draw(b.fonts[0], canvas, box_x + box_w + 10, status_y + 15,
                       12, result, strlen(result),
                       count == 0 && b.find_length > 0 ? OPOS_RGB(180, 30, 30)
                                                       : OPOS_RGB(70, 70, 80));
    } else {
        opos_font_draw(b.fonts[0], canvas, 6, status_y + 15, 12, b.status,
                       strlen(b.status), OPOS_RGB(70, 70, 80));
    }
    opos_canvas_present(canvas);
    b.dirty = false;
}

/* --------------------------------------------------------------- loading */

static void relayout(void) {
    if (!b.have_page) {
        return;
    }
    viewport_width_px = viewport_width();
    viewport_height_px = viewport_height();
    layout_page(&b.page, b.fonts, viewport_width() - 2 * MARGIN);
    build_text();
    /* Images that arrive change the layout, not the text: offsets stay. */
    if (b.selection_anchor > b.text_length) {
        b.selection_anchor = b.text_length;
    }
    if (b.selection_cursor > b.text_length) {
        b.selection_cursor = b.text_length;
    }
    clamp_scroll();
    b.dirty = true;
}

static void append(char **out, size_t *used, size_t *capacity, const char *s,
                   size_t length) {
    if (*used + length + 1 > *capacity) {
        size_t grown = (*capacity + length + 1) * 2;
        char *bigger = realloc(*out, grown);
        if (bigger == NULL) {
            return;
        }
        *out = bigger;
        *capacity = grown;
    }
    memcpy(*out + *used, s, length);
    *used += length;
    (*out)[*used] = '\0';
}

/* Style sheets of one page load: each address once, at most MAX_SHEETS
   of them and MAX_CSS_BYTES in all (big sites link 20+ sheets). */
#define MAX_SHEETS 32
#define MAX_SHEET_BYTES (2u << 20)
#define MAX_CSS_BYTES (6u << 20)

struct sheet_loads {
    int count;
    size_t bytes;
    char urls[MAX_SHEETS][OPOS_HTTP_URL_MAX];
};

static __attribute__((noinline)) void fetch_sheet(const char *base,
                                                      const char *href,
                                                      struct css_sheet **chain,
                                                      struct sheet_loads *loads) {
    char url[OPOS_HTTP_URL_MAX];
    if (!opos_url_resolve(base, href, url, sizeof(url))) {
        return;
    }
    for (int i = 0; i < loads->count; i++) {
        if (strcmp(loads->urls[i], url) == 0) {
            return;
        }
    }
    if (loads->count == MAX_SHEETS || loads->bytes >= MAX_CSS_BYTES) {
        return;
    }
    snprintf(loads->urls[loads->count++], OPOS_HTTP_URL_MAX, "%s", url);
    char text[OPOS_HTTP_URL_MAX + 32];
    snprintf(text, sizeof(text), "Style %d: %s", loads->count, url);
    set_status(text);
    paint();
    struct opos_http_response response;
    if (opos_http_get(url, MAX_SHEET_BYTES, &response) == 0) {
        if (response.status == 200) {
            loads->bytes += response.length;
            *chain = css_append(*chain, css_parse((const char *)response.body,
                                                  response.length));
        }
        opos_http_free(&response);
    }
}

static void collect_sheets(struct opos_html_node *node, struct css_sheet **chain,
                           const char *base, struct sheet_loads *loads) {
    for (; node != NULL; node = node->next_sibling) {
        if (node->type != HTML_NODE_ELEMENT) {
            continue;
        }
        if (node->tag == HTML_TAG_STYLE) {
            const char *media = opos_html_attribute(node, "media");
            if (media != NULL && !css_media_matches(media)) {
                continue;
            }
            for (struct opos_html_node *t = node->first_child; t != NULL;
                 t = t->next_sibling) {
                if (t->type == HTML_NODE_TEXT) {
                    *chain = css_append(*chain, css_parse(t->text,
                                                         t->text_length));
                }
            }
        } else if (node->tag == HTML_TAG_LINK) {
            const char *rel = opos_html_attribute(node, "rel");
            const char *href = opos_html_attribute(node, "href");
            const char *media = opos_html_attribute(node, "media");
            if (rel != NULL && href != NULL && strstr(rel, "stylesheet") != NULL
                && strstr(rel, "alternate") == NULL
                && (media == NULL || css_media_matches(media))) {
                fetch_sheet(base, href, chain, loads);
            }
        }
        collect_sheets(node->first_child, chain, base, loads);
    }
}

/* The first enabled control with the autofocus attribute, or NULL. */
static const struct opos_html_node *find_autofocus(
    const struct opos_html_node *node) {
    for (const struct opos_html_node *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        if (child->type != HTML_NODE_ELEMENT) {
            continue;
        }
        enum field_kind kind = form_kind(child);
        if (kind != FIELD_NONE && kind != FIELD_HIDDEN
            && opos_html_attribute(child, "autofocus") != NULL
            && opos_html_attribute(child, "disabled") == NULL) {
            return child;
        }
        const struct opos_html_node *found = find_autofocus(child);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

static void install_document(const char *html, size_t length,
                             const char *charset, const char *url) {
    struct opos_html_doc *doc = opos_html_parse(html, length, charset);
    if (doc == NULL) {
        set_status("Out of memory");
        return;
    }
    if (b.have_page) {
        page_destroy(&b.page);
    }
    memset(&b.page, 0, sizeof(b.page));
    b.page.doc = doc;
    css_reset_variables();
    /* @media rules are decided while the sheets are parsed. */
    viewport_width_px = viewport_width();
    viewport_height_px = viewport_height();
    static struct sheet_loads loads;
    loads.count = 0;
    loads.bytes = 0;
    collect_sheets(doc->root, &b.page.sheets, url, &loads);
    snprintf(b.page.url, sizeof(b.page.url), "%s", url);
    b.have_page = true;
    b.scroll = 0;
    clear_selection();
    b.field_selecting = false;
    b.field_selection = NULL;
    b.dropdown = NULL;
    b.page.focus = find_autofocus(doc->body);
    relayout();
}

static void show_message(const char *url, const char *title,
                         const char *detail) {
    char html[1400];
    snprintf(html, sizeof(html),
             "<html><body><h1>%s</h1><p>%s</p><p>%s</p></body></html>", title,
             detail, url);
    install_document(html, strlen(html), "utf-8", url);
}

static const char *charset_of(const char *content_type, char *out,
                              size_t capacity) {
    const char *p = strstr(content_type, "charset=");
    if (p == NULL) {
        return NULL;
    }
    p += 8;
    if (*p == '"') {
        p++;
    }
    size_t n = 0;
    while (p[n] != '\0' && p[n] != ';' && p[n] != '"' && p[n] != ' '
           && n + 1 < capacity) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n > 0 ? out : NULL;
}

static bool starts_with_ci(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        char a = *text++;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + 32);
        }
        if (a != *prefix++) {
            return false;
        }
    }
    return true;
}

static void progress(void *context, size_t received, size_t total) {
    (void)context;
    char text[96];
    if (total > 0) {
        snprintf(text, sizeof(text), "Loading... %u of %u KB",
                 (unsigned)(received >> 10), (unsigned)(total >> 10));
    } else {
        snprintf(text, sizeof(text), "Loading... %u KB",
                 (unsigned)(received >> 10));
    }
    set_status(text);
    paint();
}

/* Fetches and shows a URL. */
static void load_request(const char *url, const char *post, size_t post_length) {
    char text[OPOS_HTTP_URL_MAX + 16];
    set_address(url);
    snprintf(text, sizeof(text), "Loading %s", url);
    set_status(text);
    paint();

    struct opos_http_response response;
    opos_http_set_progress(progress, NULL);
    long error = post != NULL
        ? opos_http_post(url, "application/x-www-form-urlencoded", post,
                         post_length, MAX_PAGE_BYTES, &response)
        : opos_http_get(url, MAX_PAGE_BYTES, &response);
    opos_http_set_progress(NULL, NULL);
    if (error != 0) {
        char detail[320];
        const char *title = "Cannot reach this page";
        /* BearSSL's certificate errors are 33 to 63 (BR_ERR_X509_*). */
        long tls_code = -error - OPOS_TLS_ERROR_BASE;
        if (error == -OPOS_EFBIG) {
            snprintf(detail, sizeof(detail), "The page is too large.");
        } else if (tls_code > 32 && tls_code < 64) {
            title = "This connection is not secure";
            snprintf(detail, sizeof(detail),
                     "The site's certificate could not be checked: %s. "
                     "Someone may be trying to impersonate the site, so the"
                     " browser stopped before sending or loading anything.",
                     opos_tls_error_text(error));
        } else {
            snprintf(detail, sizeof(detail),
                     "Could not load the page: %s (error %ld). Check the"
                     " address and the network.",
                     opos_tls_error_text(error), -error);
        }
        show_message(url, title, detail);
        snprintf(b.url, sizeof(b.url), "%s", url);
        set_status("Failed");
        return;
    }
    snprintf(b.url, sizeof(b.url), "%s", response.url);
    set_address(response.url);
    char charset_buffer[40];
    const char *charset = charset_of(response.content_type, charset_buffer,
                                     sizeof(charset_buffer));
    const char *type = response.content_type;
    bool html = starts_with_ci(type, "text/html")
             || starts_with_ci(type, "application/xhtml")
             || type[0] == '\0';
    enum image_format format = image_sniff(response.body, response.length);
    if (format != IMAGE_FORMAT_UNKNOWN) {
        char markup[OPOS_HTTP_URL_MAX + 64];
        snprintf(markup, sizeof(markup), "<img src=\"%s\">", response.url);
        install_document(markup, strlen(markup), "utf-8", response.url);
    } else if (html || starts_with_ci(type, "text/xml")) {
        install_document((const char *)response.body, response.length,
                         charset, response.url);
    } else if (starts_with_ci(type, "text/")
               || starts_with_ci(type, "application/json")) {
        char *markup = NULL;
        size_t used = 0, capacity = 0;
        append(&markup, &used, &capacity, "<pre>", 5);
        for (size_t i = 0; i < response.length; i++) {
            const char *piece = NULL;
            char ch = (char)response.body[i];
            if (ch == '<') {
                piece = "&lt;";
            } else if (ch == '&') {
                piece = "&amp;";
            }
            if (piece != NULL) {
                append(&markup, &used, &capacity, piece, strlen(piece));
            } else {
                append(&markup, &used, &capacity, &ch, 1);
            }
        }
        if (markup != NULL) {
            install_document(markup, used, charset, response.url);
            free(markup);
        }
    } else {
        show_message(response.url, "Cannot show this file",
                     "This type of file is not supported yet.");
    }
    if (response.status >= 400) {
        snprintf(text, sizeof(text), "HTTP %ld", response.status);
    } else {
        snprintf(text, sizeof(text), "Done");
    }
    set_status(text);
    opos_http_free(&response);
}

static void navigate_request(const char *url, bool record, const char *post,
                             size_t post_length) {
    if (record) {
        if (b.history_position + 1 < b.history_count) {
            b.history_count = b.history_position + 1;
        }
        if (b.history_count == HISTORY_MAX) {
            memmove(b.history[0], b.history[1],
                    (HISTORY_MAX - 1) * sizeof(b.history[0]));
            b.history_count--;
        }
        snprintf(b.history[b.history_count], sizeof(b.history[0]), "%s", url);
        b.history_position = b.history_count++;
    }
    load_request(url, post, post_length);
    if (record && b.history_position >= 0) {
        /* Remember where redirects ended up. */
        snprintf(b.history[b.history_position], sizeof(b.history[0]), "%s",
                 b.url);
    }
    b.dirty = true;
}

static void load(const char *url) { load_request(url, NULL, 0); }

static void navigate(const char *url, bool record) {
    navigate_request(url, record, NULL, 0);
}

/* ----------------------------------------------------------------- forms */

/* Lays the page out again after a form control changed, keeping the
   scroll position. */
static void refresh_controls(void) {
    int keep = b.scroll;
    relayout();
    b.scroll = keep;
    clamp_scroll();
    b.dirty = true;
}

static void set_focus(const struct opos_html_node *node) {
    if (b.have_page && b.page.focus != node) {
        b.page.focus = node;
        refresh_controls();
    }
}

/* Submits the form that holds control; submitter is the button pressed
   (or NULL when Enter was pressed in a text field). */
static void submit_form(const struct opos_html_node *control,
                        const struct opos_html_node *submitter) {
    const struct opos_html_node *form = form_of(control);
    if (form == NULL) {
        return;
    }
    size_t length = 0;
    char *data = form_encode(&b.page, form, submitter, &length);
    if (data == NULL) {
        set_status("Out of memory");
        return;
    }
    const char *action = opos_html_attribute(form, "action");
    const char *method = opos_html_attribute(form, "method");
    if (submitter != NULL) {
        /* <button formaction=...> and formmethod override the form's. */
        const char *override = opos_html_attribute(submitter, "formaction");
        if (override != NULL) {
            action = override;
        }
        override = opos_html_attribute(submitter, "formmethod");
        if (override != NULL) {
            method = override;
        }
    }
    char url[OPOS_HTTP_URL_MAX];
    if (action == NULL || action[0] == '\0') {
        snprintf(url, sizeof(url), "%s", b.page.url);
    } else if (!opos_url_resolve(b.page.url, action, url, sizeof(url))) {
        free(data);
        set_status("The form's address is not valid");
        return;
    }
    if (starts_with_ci(url, "javascript:")) {
        free(data);
        set_status("This form needs JavaScript");
        return;
    }
    bool post = method != NULL && starts_with_ci(method, "post");
    if (post) {
        navigate_request(url, true, data, length);
    } else {
        /* GET: the data replaces the query of the action address. */
        url[strcspn(url, "?#")] = '\0';
        size_t base = strlen(url);
        if (base + 1 + length >= sizeof(url)) {
            set_status("The form's data is too long for an address");
        } else {
            url[base] = '?';
            memcpy(url + base + 1, data, length + 1);
            navigate(url, true);
        }
    }
    free(data);
}

/* A click on a form control. */
static void click_control(const struct opos_html_node *node) {
    if (opos_html_attribute(node, "disabled") != NULL) {
        return;
    }
    struct form_field *field = form_field(&b.page, node);
    if (field == NULL) {
        return;
    }
    switch (field->kind) {
    case FIELD_TEXT: case FIELD_PASSWORD: case FIELD_TEXTAREA:
        if (opos_html_attribute(node, "readonly") == NULL) {
            set_focus(node);
        }
        return;
    case FIELD_CHECKBOX:
        field->checked = !field->checked;
        break;
    case FIELD_RADIO:
        form_check_radio(&b.page, node);
        break;
    case FIELD_SELECT: {
        b.dropdown = form_option_count(node) > 0 ? node : NULL;
        b.dirty = true;
        break;
    }
    case FIELD_SUBMIT:
        b.page.focus = NULL;
        submit_form(node, node);
        return;
    default:
        set_focus(node);
        return;
    }
    b.page.focus = node;
    refresh_controls();
}

/* Moves the focus to the next (step 1) or previous (-1) control shown on
   the page, in layout order. */
static void focus_step(int step) {
    const struct opos_html_node *order[512];
    size_t count = 0;
    for (size_t i = 0; i < b.page.item_count && count < 512; i++) {
        const struct opos_html_node *node = b.page.items[i].control;
        if (node != NULL && opos_html_attribute(node, "disabled") == NULL) {
            order[count++] = node;
        }
    }
    if (count == 0) {
        return;
    }
    size_t at = count;
    for (size_t i = 0; i < count; i++) {
        if (order[i] == b.page.focus) {
            at = i;
            break;
        }
    }
    size_t next;
    if (at == count) {
        next = step > 0 ? 0 : count - 1;
    } else {
        next = (at + count + (size_t)(step > 0 ? 1 : count - 1)) % count;
    }
    set_focus(order[next]);
    /* Bring it into view. */
    for (size_t i = 0; i < b.page.item_count; i++) {
        const struct display_item *item = &b.page.items[i];
        if (item->control == order[next]) {
            int top = item->y + MARGIN;
            if (top < b.scroll
                || top + item->height > b.scroll + viewport_height()) {
                b.scroll = top - viewport_height() / 3;
                clamp_scroll();
            }
            break;
        }
    }
}

static bool is_text_field(const struct form_field *field) {
    return field != NULL && (field->kind == FIELD_TEXT
        || field->kind == FIELD_PASSWORD || field->kind == FIELD_TEXTAREA);
}

static size_t field_selection_start(const struct form_field *field) {
    return field->selection_anchor < field->selection_cursor
         ? field->selection_anchor : field->selection_cursor;
}

static size_t field_selection_end(const struct form_field *field) {
    return field->selection_anchor > field->selection_cursor
         ? field->selection_anchor : field->selection_cursor;
}

static void field_move(struct form_field *field, size_t position,
                       bool extend) {
    if (position > field->length) {
        position = field->length;
    }
    if (!extend) {
        field->selection_anchor = position;
    }
    field->selection_cursor = position;
}

static bool field_replace_range(struct form_field *field,
                                const struct opos_html_node *node,
                                size_t start, size_t end,
                                const char *insert, size_t insert_length) {
    const char *maximum = opos_html_attribute(node, "maxlength");
    long max = maximum != NULL ? strtol(maximum, NULL, 10) : -1;
    if (max >= 0) {
        size_t outside = browser_utf8_count(field->value, start)
            + browser_utf8_count(field->value + end, field->length - end);
        size_t allowed = outside < (size_t)max ? (size_t)max - outside : 0;
        insert_length = browser_utf8_offset(insert, insert_length, allowed);
    }
    size_t new_length = start + insert_length + field->length - end;
    char *value = malloc(new_length + 1);
    if (value == NULL) {
        return false;
    }
    memcpy(value, field->value, start);
    memcpy(value + start, insert, insert_length);
    memcpy(value + start + insert_length, field->value + end,
           field->length - end);
    value[new_length] = '\0';
    free(field->value);
    field->value = value;
    field->length = new_length;
    field->selection_anchor = field->selection_cursor = start + insert_length;
    return true;
}

static size_t field_line_start(const struct form_field *field, size_t at) {
    while (at > 0 && field->value[at - 1] != '\n') {
        at--;
    }
    return at;
}

static size_t field_line_end(const struct form_field *field, size_t at) {
    while (at < field->length && field->value[at] != '\n') {
        at = browser_utf8_next(field->value, field->length, at);
    }
    return at;
}

static size_t field_vertical_position(const struct form_field *field,
                                      int direction) {
    size_t current_start = field_line_start(field, field->selection_cursor);
    size_t column = browser_utf8_count(field->value + current_start,
                                       field->selection_cursor - current_start);
    size_t target_start;
    if (direction < 0) {
        if (current_start == 0) {
            return 0;
        }
        target_start = field_line_start(field, current_start - 1);
    } else {
        size_t current_end = field_line_end(field, field->selection_cursor);
        if (current_end >= field->length) {
            return field->length;
        }
        target_start = current_end + 1;
    }
    size_t target_end = field_line_end(field, target_start);
    return target_start + browser_utf8_offset(
        field->value + target_start, target_end - target_start, column);
}

/* Keys while a form control has focus. Returns false for keys the page
   should handle instead (scrolling). */
static bool field_key(uint32_t key, bool ctrl, bool shift) {
    const struct opos_html_node *node = b.page.focus;
    struct form_field *field = form_field(&b.page, node);
    if (field == NULL) {
        return false;
    }
    if (field->kind == FIELD_SELECT) {
        /* Up and Down pick the previous or next option; Enter, Space
           and Escape close an open list. */
        int count = form_option_count(node);
        if (key == OPOS_KEY_UP || key == OPOS_KEY_DOWN) {
            int option = field->option + (key == OPOS_KEY_UP ? -1 : 1);
            if (option >= 0 && option < count) {
                field->option = option;
                refresh_controls();
            }
            b.dirty = true;
            return true;
        }
        if (b.dropdown != NULL && (key == '\n' || key == ' '
                                   || key == OPOS_KEY_ESCAPE)) {
            b.dropdown = NULL;
            b.dirty = true;
            return true;
        }
    }
    if (key == OPOS_KEY_ESCAPE) {
        set_focus(NULL);
        return true;
    }
    if (key == '\t') {
        focus_step(shift ? -1 : 1);
        return true;
    }
    if (!is_text_field(field)) {
        /* Buttons, boxes and lists: Space presses, Enter submits. */
        if (key == ' ' || (key == '\n' && field->kind == FIELD_SUBMIT)) {
            click_control(node);
            return true;
        }
        if (key == '\n') {
            submit_form(node, NULL);
            return true;
        }
        return false;
    }
    if (key == '\n' && field->kind != FIELD_TEXTAREA) {
        /* Enter in a text field submits its form. */
        submit_form(node, NULL);
        return true;
    }
    size_t low = field_selection_start(field);
    size_t high = field_selection_end(field);
    bool changed = false;
    bool read_only = opos_html_attribute(node, "readonly") != NULL;
    if (ctrl && (key == 'a' || key == 'A')) {
        field->selection_anchor = 0;
        field->selection_cursor = field->length;
    } else if (ctrl && (key == 'c' || key == 'C')) {
        if (field->kind != FIELD_PASSWORD && high > low) {
            opos_clipboard_set(field->value + low, high - low);
        }
        return true;
    } else if (ctrl && (key == 'x' || key == 'X')) {
        if (!read_only && high > low) {
            if (field->kind != FIELD_PASSWORD) {
                opos_clipboard_set(field->value + low, high - low);
            }
            changed = field_replace_range(field, node, low, high, "", 0);
        }
    } else if (ctrl && (key == 'v' || key == 'V')) {
        if (read_only) {
            return true;
        }
        char pasted[4096];
        long got = opos_clipboard_get(pasted, sizeof(pasted));
        if (got <= 0) {
            return true;
        }
        if (got > (long)sizeof(pasted)) {
            got = sizeof(pasted);
        }
        char clean[4096];
        size_t used = 0;
        for (long i = 0; i < got; i++) {
            char ch = pasted[i];
            if (ch == '\r') {
                continue;
            }
            if (ch == '\n' && field->kind != FIELD_TEXTAREA) {
                ch = ' ';
            }
            if ((unsigned char)ch >= 32 || ch == '\n') {
                clean[used++] = ch;
            }
        }
        changed = field_replace_range(field, node, low, high, clean, used);
    } else if (key == '\b') {
        if (read_only) {
            return true;
        }
        if (low == high && low > 0) {
            low = browser_utf8_previous(field->value, low);
        }
        if (high > low) {
            changed = field_replace_range(field, node, low, high, "", 0);
        }
    } else if (key == OPOS_KEY_DELETE) {
        if (read_only) {
            return true;
        }
        if (low == high && high < field->length) {
            high = browser_utf8_next(field->value, field->length, high);
        }
        if (high > low) {
            changed = field_replace_range(field, node, low, high, "", 0);
        }
    } else if (key == OPOS_KEY_LEFT || key == OPOS_KEY_RIGHT) {
        size_t position;
        if (!shift && low != high) {
            position = key == OPOS_KEY_LEFT ? low : high;
        } else if (key == OPOS_KEY_LEFT) {
            position = browser_utf8_previous(field->value,
                                             field->selection_cursor);
        } else {
            position = browser_utf8_next(field->value, field->length,
                                         field->selection_cursor);
        }
        field_move(field, position, shift);
    } else if (key == OPOS_KEY_HOME || key == OPOS_KEY_END) {
        size_t position = field->kind == FIELD_TEXTAREA
            ? (key == OPOS_KEY_HOME
                ? field_line_start(field, field->selection_cursor)
                : field_line_end(field, field->selection_cursor))
            : (key == OPOS_KEY_HOME ? 0 : field->length);
        field_move(field, position, shift);
    } else if (field->kind == FIELD_TEXTAREA
               && (key == OPOS_KEY_UP || key == OPOS_KEY_DOWN)) {
        field_move(field, field_vertical_position(
            field, key == OPOS_KEY_UP ? -1 : 1), shift);
    } else if ((key >= 32 && key < 127 && !ctrl) || key == '\n') {
        if (read_only) {
            return true;
        }
        char ch = (char)key;
        changed = field_replace_range(field, node, low, high, &ch, 1);
    } else {
        return false;
    }
    (void)changed;
    refresh_controls();
    return true;
}

static void go_history(int step) {
    int target = b.history_position + step;
    if (target < 0 || target >= b.history_count) {
        return;
    }
    b.history_position = target;
    char url[OPOS_HTTP_URL_MAX];
    snprintf(url, sizeof(url), "%s", b.history[target]);
    load(url);
}

static void submit_address(void) {
    char url[OPOS_HTTP_URL_MAX];
    const char *text = b.address;
    while (*text == ' ') {
        text++;
    }
    if (*text == '\0') {
        return;
    }
    if (starts_with_ci(text, "http://") || starts_with_ci(text, "https://")) {
        snprintf(url, sizeof(url), "%s", text);
    } else {
        snprintf(url, sizeof(url), "https://%s", text);
    }
    b.editing = false;
    navigate(url, true);
}

/* Fetches one pending image and lays the page out again. Returns false when
   there is nothing left to load. */
static bool load_next_image(void) {
    if (!b.have_page) {
        return false;
    }
    for (size_t i = 0; i < b.page.image_count; i++) {
        struct image_ref *ref = b.page.images[i];
        if (ref->state != 0) {
            continue;
        }
        ref->state = 2;
        char text[OPOS_HTTP_URL_MAX + 16];
        snprintf(text, sizeof(text), "Image %s", ref->url);
        set_status(text);
        paint();
        struct opos_http_response response;
        if (opos_http_get(ref->url, MAX_IMAGE_BYTES, &response) == 0) {
            struct image_info info;
            if (response.status == 200
                && image_probe(response.body, response.length, &info)
                       == IMAGE_OK) {
                void *scratch = malloc(info.scratch_bytes + 16);
                if (scratch != NULL) {
                    void *aligned = (void *)(((uintptr_t)scratch + 15)
                                             & ~(uintptr_t)15);
                    struct image image;
                    if (image_decode(response.body, response.length, aligned,
                                     info.scratch_bytes, &image)
                        == IMAGE_OK) {
                        size_t bytes = (size_t)image.width * image.height * 4;
                        ref->pixels = malloc(bytes);
                        if (ref->pixels != NULL) {
                            memcpy(ref->pixels, image.pixels, bytes);
                            ref->width = (int)image.width;
                            ref->height = (int)image.height;
                            ref->state = 1;
                        }
                    }
                    free(scratch);
                }
            }
            opos_http_free(&response);
        }
        int keep = b.scroll;
        relayout();
        b.scroll = keep;
        clamp_scroll();
        set_status("Done");
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- events */

static void scroll_by(int delta) {
    b.scroll += delta;
    clamp_scroll();
    b.dirty = true;
}

static bool link_target(const struct display_item *item, char *out,
                        size_t capacity) {
    if (item == NULL || item->link == NULL) {
        return false;
    }
    const char *href = opos_html_attribute(item->link, "href");
    if (href == NULL || href[0] == '\0' || href[0] == '#'
        || starts_with_ci(href, "javascript:")
        || starts_with_ci(href, "mailto:")) {
        return false;
    }
    return opos_url_resolve(b.page.url, href, out, capacity);
}

/* Maps a click in a rendered text control back to a byte offset in its
   value. For a password, each displayed star maps to one UTF-8 character. */
static size_t field_offset_at(const struct opos_html_node *node, int x, int y) {
    struct form_field *field = form_field(&b.page, node);
    if (!is_text_field(field)) {
        return 0;
    }
    const struct display_item *best = NULL;
    int best_distance = 0x7fffffff;
    for (size_t i = 0; i < b.page.item_count; i++) {
        const struct display_item *item = &b.page.items[i];
        if (item->kind != ITEM_TEXT || item->control != node) {
            continue;
        }
        int distance = y < item->y ? item->y - y
                     : y >= item->y + item->height
                         ? y - (item->y + item->height - 1) : 0;
        if (best == NULL || distance < best_distance) {
            best = item;
            best_distance = distance;
        }
    }
    if (best == NULL) {
        return field->length;
    }
    if (best->length == 0) {
        return best->field_offset < field->length ? best->field_offset
                                                   : field->length;
    }
    int relative = x - best->x;
    size_t display = 0;
    if (relative > 0) {
        while (display < best->length) {
            size_t next = browser_utf8_next(best->text, best->length,
                                            display);
            int left = item_prefix_width(best, display);
            int right = item_prefix_width(best, next);
            if (relative < (left + right) / 2) {
                break;
            }
            display = next;
        }
    }
    size_t offset = best->field_offset;
    if (field->kind == FIELD_PASSWORD) {
        offset += browser_utf8_offset(field->value + offset,
                                      field->length - offset,
                                      browser_utf8_count(best->text,
                                                         display));
    } else {
        offset += display;
    }
    return offset < field->length ? offset : field->length;
}

static void on_click(int x, int y) {
    if (b.dropdown != NULL) {
        dropdown_click(x, y);
        return;
    }
    if (y < TOOLBAR_HEIGHT) {
        b.editing = false;
        if (x >= 6 && x < 34) {
            go_history(-1);
        } else if (x >= 38 && x < 66) {
            go_history(1);
        } else if (x >= 70 && x < 98) {
            if (b.url[0] != '\0') {
                char url[OPOS_HTTP_URL_MAX];
                snprintf(url, sizeof(url), "%s", b.url);
                load(url);
            }
        } else if (x >= 106) {
            b.editing = true;
            b.address_selected = true;
        }
        b.dirty = true;
        return;
    }
    b.editing = false;
    b.dirty = true;
    if (x >= viewport_width()) {
        b.dragging_scrollbar = true;
        int range = viewport_height();
        b.scroll = (int)((long long)max_scroll() * (y - TOOLBAR_HEIGHT) / (range > 0 ? range : 1));
        clamp_scroll();
        return;
    }
    if (!b.have_page || y >= TOOLBAR_HEIGHT + viewport_height()) {
        return;
    }
    const struct opos_html_node *control = page_control_at(
        &b.page, x - MARGIN, y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
    if (control != NULL) {
        struct form_field *field = form_field(&b.page, control);
        bool select_field = is_text_field(field)
            && opos_html_attribute(control, "readonly") == NULL
            && opos_html_attribute(control, "disabled") == NULL;
        if (select_field) {
            clear_selection();
            b.have_match = false;
            size_t offset = field_offset_at(
                control, x - MARGIN,
                y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
            field->selection_anchor = field->selection_cursor = offset;
            b.field_selecting = true;
            b.field_selection = control;
        }
        click_control(control);
        if (select_field) {
            refresh_controls();
        }
        return;
    }
    set_focus(NULL);
    /* Start a selection; a release without a drag follows a link. */
    b.selecting = true;
    b.pressed_moved = false;
    b.press_x = x;
    b.press_y = y;
    b.have_match = false;
    b.selection_anchor = b.selection_cursor = text_offset_at(
        x - MARGIN, y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
}

static void on_release(void) {
    b.dragging_scrollbar = false;
    b.field_selecting = false;
    b.field_selection = NULL;
    if (!b.selecting) {
        return;
    }
    b.selecting = false;
    if (b.pressed_moved) {
        return;
    }
    b.selection_anchor = b.selection_cursor;
    b.dirty = true;
    const struct display_item *item = page_item_at(
        &b.page, b.press_x - MARGIN,
        b.press_y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
    char url[OPOS_HTTP_URL_MAX];
    if (link_target(item, url, sizeof(url))) {
        navigate(url, true);
    }
}

static void on_move(int x, int y) {
    char url[OPOS_HTTP_URL_MAX];
    if (b.field_selecting && b.field_selection != NULL) {
        struct form_field *field = form_field(&b.page, b.field_selection);
        if (field != NULL) {
            field->selection_cursor = field_offset_at(
                b.field_selection, x - MARGIN,
                y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
            refresh_controls();
        }
        return;
    }
    if (b.selecting) {
        int dx = x - b.press_x, dy = y - b.press_y;
        if (!b.pressed_moved && dx * dx + dy * dy < 16) {
            return;
        }
        b.pressed_moved = true;
        /* Dragging past the top or bottom scrolls the page. */
        if (y < TOOLBAR_HEIGHT) {
            scroll_by(-24);
        } else if (y >= TOOLBAR_HEIGHT + viewport_height()) {
            scroll_by(24);
        }
        b.selection_cursor = text_offset_at(
            x - MARGIN, y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
        b.dirty = true;
        return;
    }
    if (b.dragging_scrollbar) {
        int range = viewport_height();
        b.scroll = (int)((long long)max_scroll() * (y - TOOLBAR_HEIGHT) / (range > 0 ? range : 1));
        clamp_scroll();
        b.dirty = true;
        return;
    }
    if (b.have_page && y >= TOOLBAR_HEIGHT && x < viewport_width()
        && y < TOOLBAR_HEIGHT + viewport_height()) {
        const struct display_item *item = page_item_at(
            &b.page, x - MARGIN, y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
        if (link_target(item, url, sizeof(url))) {
            if (strcmp(b.status, url) != 0) {
                set_status(url);
            }
            return;
        }
    }
    if (b.status[0] != '\0' && strncmp(b.status, "http", 4) == 0) {
        set_status("");
    }
}

static void on_key(uint32_t key, uint32_t modifiers) {
    bool ctrl = (modifiers & OPOS_MOD_CTRL) != 0;
    if (ctrl && (key == 'l' || key == 'L')) {
        b.editing = true;
        b.address_selected = true;
        b.dirty = true;
        return;
    }
    if (ctrl && (key == 'r' || key == 'R')) {
        on_click(80, 10);
        return;
    }
    if (ctrl && (key == 'f' || key == 'F')) {
        b.finding = true;
        b.editing = false;
        if (b.find_length > 0) {
            find_from(selection_start(), 1);
        }
        b.dirty = true;
        return;
    }
    if (b.have_page && b.page.focus != NULL && !b.editing && !b.finding
        && field_key(key, ctrl, (modifiers & OPOS_MOD_SHIFT) != 0)) {
        return;
    }
    if (key == '\t' && b.have_page && !b.editing && !b.finding) {
        focus_step((modifiers & OPOS_MOD_SHIFT) != 0 ? -1 : 1);
        return;
    }
    if (ctrl && (key == 'c' || key == 'C') && !b.editing) {
        copy_selection();
        return;
    }
    if (ctrl && (key == 'a' || key == 'A') && !b.editing) {
        b.have_match = false;
        b.selection_anchor = 0;
        b.selection_cursor = b.text_length;
        b.dirty = true;
        return;
    }
    if (b.finding && !b.editing) {
        bool shift = (modifiers & OPOS_MOD_SHIFT) != 0;
        if (key == OPOS_KEY_ESCAPE) {
            b.finding = false;
            b.have_match = false;
        } else if (key == '\n') {
            size_t from = b.have_match ? b.match : selection_start();
            if (shift) {
                find_from(from == 0 ? b.text_length : from - 1, -1);
            } else {
                find_from(b.have_match ? from + 1 : from, 1);
            }
        } else if (key == '\b') {
            if (b.find_length > 0) {
                do {
                    b.find_length--;
                } while (b.find_length > 0
                         && (b.find[b.find_length] & 0xc0) == 0x80);
                b.find[b.find_length] = '\0';
                find_from(b.have_match ? b.match : 0, 1);
            }
        } else if (key >= 32 && key < 127 && !ctrl
                   && b.find_length + 1 < sizeof(b.find)) {
            b.find[b.find_length++] = (char)key;
            b.find[b.find_length] = '\0';
            /* Typing refines the search from the current match. */
            find_from(b.have_match ? b.match : selection_start(), 1);
        } else {
            goto scroll_keys;
        }
        b.dirty = true;
        return;
    }
    /* While the whole address is selected, typing, pasting or Backspace
       replaces it; other keys keep it and drop the selection. */
    if (b.editing && b.address_selected) {
        bool replaces = (ctrl && (key == 'v' || key == 'V')) || key == '\b'
                     || (key >= 32 && key < 127 && !ctrl);
        if (replaces) {
            b.address_length = 0;
            b.address[0] = '\0';
            if (key == '\b') {
                b.address_selected = false;
                b.dirty = true;
                return;
            }
        }
        if (key != '\n' && !(ctrl && (key == 'c' || key == 'C'))) {
            b.address_selected = false;
        }
    }
    if (b.editing && ctrl && (key == 'c' || key == 'C')) {
        opos_clipboard_set(b.address, b.address_length);
        return;
    }
    if (b.editing && ctrl && (key == 'v' || key == 'V')) {
        char pasted[OPOS_HTTP_URL_MAX];
        long length = opos_clipboard_get(pasted, sizeof(pasted));
        if (length > (long)sizeof(pasted)) {
            length = sizeof(pasted);
        }
        for (long i = 0; i < length; i++) {
            unsigned char ch = (unsigned char)pasted[i];
            if (ch == '\n' || ch == '\t') {
                ch = ' ';
            }
            if (ch >= 32 && b.address_length + 1 < sizeof(b.address)) {
                b.address[b.address_length++] = (char)ch;
            }
        }
        b.address[b.address_length] = '\0';
        b.dirty = true;
        return;
    }
    if (b.editing) {
        if (key == '\n') {
            submit_address();
        } else if (key == OPOS_KEY_ESCAPE) {
            b.editing = false;
            set_address(b.url);
        } else if (key == '\b') {
            if (b.address_length > 0) {
                /* Remove a whole UTF-8 character. */
                do {
                    b.address_length--;
                } while (b.address_length > 0
                         && (b.address[b.address_length] & 0xc0) == 0x80);
                b.address[b.address_length] = '\0';
            }
        } else if (key >= 32 && key < 127 && !ctrl
                   && b.address_length + 1 < sizeof(b.address)) {
            b.address[b.address_length++] = (char)key;
            b.address[b.address_length] = '\0';
        }
        b.dirty = true;
        return;
    }
scroll_keys:;
    int page = viewport_height() - 40;
    switch (key) {
    case OPOS_KEY_UP: scroll_by(-48); break;
    case OPOS_KEY_DOWN: scroll_by(48); break;
    case OPOS_KEY_PAGE_UP: scroll_by(-page); break;
    case OPOS_KEY_PAGE_DOWN:
    case ' ': scroll_by((modifiers & OPOS_MOD_SHIFT) ? -page : page); break;
    case OPOS_KEY_HOME: scroll_by(-b.scroll); break;
    case OPOS_KEY_END: scroll_by(max_scroll()); break;
    case '\b': go_history(-1); break;
    default: break;
    }
}

static bool resize(int width, int height) {
    if (width < 200 || height < TOOLBAR_HEIGHT + STATUS_HEIGHT + 60) {
        return false;
    }
    opos_canvas_destroy(&b.canvas);
    b.canvas = opos_canvas_create(width, height);
    if (b.canvas.pixels == NULL) {
        return false;
    }
    b.width = width;
    b.height = height;
    relayout();
    b.dirty = true;
    return true;
}

int main(int argc, char **argv) {
    if (opos_window_create("BROWSER", 900, 640) < 0) {
        return 1;
    }
    for (int i = 0; i < FONT_COUNT; i++) {
        char path[96];
        snprintf(path, sizeof(path), "/share/fonts/%s", font_files[i]);
        b.fonts[i] = opos_font_load(path);
        if (b.fonts[i] == NULL) {
            b.fonts[i] = i > 0 ? b.fonts[0] : NULL;
        }
    }
    if (b.fonts[0] == NULL) {
        return 2;
    }
    uint32_t w = 900, h = 640;
    opos_window_size(&w, &h);
    b.canvas = opos_canvas_create((int)w, (int)h);
    b.width = (int)w;
    b.height = (int)h;
    b.history_position = -1;
    paint();

    navigate(argc > 1 ? argv[1] : HOME_URL, true);

    struct opos_event event;
    for (;;) {
        if (b.dirty) {
            paint();
        }
        long got = opos_window_event(&event, 0);
        if (got == 0) {
            if (load_next_image()) {
                continue;
            }
            got = opos_window_event(&event, -1);
        }
        if (got <= 0) {
            if (got < 0) {
                break;
            }
            continue;
        }
        switch (event.type) {
        case OPOS_EVENT_CLOSE:
            return 0;
        case OPOS_EVENT_RESIZE:
            resize((int)event.width, (int)event.height);
            break;
        case OPOS_EVENT_KEY:
            on_key(event.key, event.buttons);
            break;
        case OPOS_EVENT_MOUSE_DOWN:
            if (event.button == OPOS_BUTTON_LEFT) {
                on_click(event.x, event.y);
            }
            break;
        case OPOS_EVENT_MOUSE_UP:
            if (event.button == OPOS_BUTTON_LEFT) {
                on_release();
            }
            break;
        case OPOS_EVENT_MOUSE_MOVE:
            on_move(event.x, event.y);
            break;
        case OPOS_EVENT_WHEEL:
            scroll_by(-OPOS_WHEEL_NOTCHES(event) * 48);
            break;
        default:
            break;
        }
    }
    return 0;
}
