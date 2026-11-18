/*
 * OPOS browser: window, address bar, page loading, scrolling and painting.
 *
 *   browser [URL]
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_tls.h>

#include "browser.h"

#define TABBAR_HEIGHT 28
#define TOOLBAR_HEIGHT 40
#define CHROME_HEIGHT (TABBAR_HEIGHT + TOOLBAR_HEIGHT) /* above the page */
#define STATUS_HEIGHT 20
#define SCROLLBAR_WIDTH 12
#define MARGIN PAGE_MARGIN
#define HISTORY_MAX 64
#define MAX_TABS 12
#define MAX_PAGE_BYTES (8u << 20)
#define MAX_IMAGE_BYTES (6u << 20)
#define MAX_DOWNLOAD_BYTES (1ull << 32)
#define MAX_RUNNING_DOWNLOADS 6
#define HOME_URL "https://example.com/"
#define NEW_TAB_URL "about:home"

#define COL_TOOLBAR OPOS_RGB(228, 230, 236)
#define COL_TABBAR OPOS_RGB(204, 207, 216)
#define COL_FIELD OPOS_RGB(255, 255, 255)
#define COL_BORDER OPOS_RGB(140, 144, 156)
#define COL_TEXT OPOS_RGB(30, 30, 36)
#define COL_STATUS OPOS_RGB(240, 240, 244)

static const char *font_files[FONT_COUNT] = {
    "NotoSans-Regular.ttf", "NotoSans-Bold.ttf", "NotoSans-Italic.ttf",
    "NotoSans-BoldItalic.ttf", "NotoSansMono-Regular.ttf",
    "NotoSansMono-Bold.ttf"};

/* One tab: its page, address bar, back/forward list, selection and find
   bar. */
struct tab {
    struct page page;
    bool have_page;
    int scroll;
    int script_scroll;         /* scroll offset scripts were told about */
    int script_width;          /* window width they were told about */
    bool follow_anchor;           /* keep the #fragment target in view */
    char url[OPOS_HTTP_URL_MAX];
    char title[128];              /* the page's <title>, or "" */
    char address[OPOS_HTTP_URL_MAX];
    size_t address_length;
    bool editing;
    bool address_selected;        /* the next typed key replaces it all */
    char history[HISTORY_MAX][OPOS_HTTP_URL_MAX];
    int history_count;
    int history_position;
    /* The page's text as one string (words joined with spaces and line
       breaks) and, per display item, where its text starts in it
       (NO_TEXT for items without text). Selection and find use offsets
       into this string. */
    char *text;
    size_t text_length;
    size_t *item_start;
    size_t selection_anchor, selection_cursor;
    bool finding;                 /* the find bar is open */
    char find[128];
    size_t find_length;
    size_t match;                 /* start of the current match */
    bool have_match;
    const struct opos_html_node *dropdown; /* the open <select> list */
};

/* Entries of the right-click menu and the toolbar's menu button. */
enum menu_action {
    MENU_OPEN_NEW_TAB, MENU_COPY_LINK, MENU_BOOKMARK_LINK, MENU_DOWNLOAD_LINK,
    MENU_BACK, MENU_FORWARD, MENU_RELOAD, MENU_BOOKMARK_PAGE, MENU_COPY,
    MENU_NEW_TAB, MENU_CLOSE_TAB, MENU_BOOKMARKS, MENU_HISTORY,
    MENU_DOWNLOADS, MENU_COOKIES, MENU_JAVASCRIPT, MENU_COUNT
};

struct browser {
    struct opos_font *fonts[FONT_COUNT];
    struct opos_canvas canvas;
    int width, height;            /* window content size */
    struct tab *tabs[MAX_TABS];
    int tab_count;
    int current;                  /* index of the tab shown */
    struct tab *tab;              /* tabs[current] */
    char status[OPOS_HTTP_URL_MAX + 32];
    char caption[32];             /* the window title last sent */
    bool dirty;
    bool dragging_scrollbar;
    bool selecting;               /* left button held on the page */
    bool field_selecting;         /* left button held in a form field */
    const struct opos_html_node *field_selection;
    bool pressed_moved;
    int press_x, press_y;
    /* Right-click menu: open while menu_count > 0. */
    int menu_x, menu_y, menu_count;
    int menu_hover;               /* row under the mouse, or -1 */
    enum menu_action menu[MENU_COUNT];
    char menu_link[OPOS_HTTP_URL_MAX];
    /* Where bookmarks, history and downloads are kept ("" without an
       account: then they last only while the browser runs). */
    char data_dir[160];
    char downloads_dir[160];
};

#define T (b.tab)

#define NO_TEXT ((size_t)-1)
#define COL_SELECTION OPOS_RGB(180, 213, 254)
#define COL_MATCH OPOS_RGB(255, 210, 90)

static struct browser b;

/* --------------------------------------------------------------- helpers */

static int viewport_width(void) { return b.width - SCROLLBAR_WIDTH; }
static int viewport_height(void) {
    return b.height - CHROME_HEIGHT - STATUS_HEIGHT;
}

static int max_scroll(void) {
    if (!T->have_page) {
        return 0;
    }
    int limit = T->page.content_height + 2 * MARGIN - viewport_height();
    return limit < 0 ? 0 : limit;
}

static void clamp_scroll(void) {
    if (T->scroll > max_scroll()) {
        T->scroll = max_scroll();
    }
    if (T->scroll < 0) {
        T->scroll = 0;
    }
}

static void set_status(const char *text) {
    snprintf(b.status, sizeof(b.status), "%s", text);
    b.dirty = true;
}

static void paint(void);

void browser_status(const char *text) {
    set_status(text);
    paint();
}

static void set_address(const char *url) {
    snprintf(T->address, sizeof(T->address), "%s", url);
    T->address_length = strlen(T->address);
}

/* ------------------------------------------------------------ page text */

static bool is_text_item(const struct display_item *item) {
    return item->kind == ITEM_TEXT && item->length > 0
        && !(item->length == 1 && item->text[0] == ' ');
}

static void text_put(size_t *capacity, const char *s, size_t n) {
    if (T->text == NULL) {
        return;
    }
    if (T->text_length + n + 1 > *capacity) {
        size_t grown = (*capacity + n + 1) * 2;
        char *bigger = realloc(T->text, grown);
        if (bigger == NULL) {
            free(T->text);
            T->text = NULL;
            return;
        }
        T->text = bigger;
        *capacity = grown;
    }
    memcpy(T->text + T->text_length, s, n);
    T->text_length += n;
    T->text[T->text_length] = '\0';
}

/* Rebuilds T->text and T->item_start from the display list. A word on a
   new line starts with a line break (two after a gap, as between
   paragraphs); a word after a gap on the same line starts with a space. */
static void build_text(void) {
    free(T->text);
    free(T->item_start);
    T->text = NULL;
    T->item_start = NULL;
    T->text_length = 0;
    if (!T->have_page) {
        return;
    }
    size_t count = T->page.item_count;
    T->item_start = malloc((count + 1) * sizeof(size_t));
    size_t capacity = 4096;
    T->text = malloc(capacity);
    if (T->item_start == NULL || T->text == NULL) {
        free(T->item_start);
        free(T->text);
        T->item_start = NULL;
        T->text = NULL;
        return;
    }
    T->text[0] = '\0';
    const struct display_item *previous = NULL;
    for (size_t i = 0; i < count; i++) {
        const struct display_item *item = &T->page.items[i];
        T->item_start[i] = NO_TEXT;
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
        if (T->text == NULL) {
            free(T->item_start);
            T->item_start = NULL;
            T->text_length = 0;
            return;
        }
        T->item_start[i] = T->text_length;
        text_put(&capacity, item->text, item->length);
        previous = item;
    }
    if (T->text == NULL) {
        free(T->item_start);
        T->item_start = NULL;
        T->text_length = 0;
    }
}

static size_t selection_start(void) {
    return T->selection_anchor < T->selection_cursor ? T->selection_anchor
                                                   : T->selection_cursor;
}

static size_t selection_end(void) {
    return T->selection_anchor > T->selection_cursor ? T->selection_anchor
                                                   : T->selection_cursor;
}

static void clear_selection(void) {
    T->selection_anchor = T->selection_cursor = 0;
    T->have_match = false;
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
    if (item->control == NULL || T->page.focus != item->control) {
        return false;
    }
    struct form_field *field = form_field(&T->page, item->control);
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
    if (T->item_start == NULL) {
        return 0;
    }
    long best_distance = -1;
    size_t best = 0;
    for (size_t i = 0; i < T->page.item_count; i++) {
        if (T->item_start[i] == NO_TEXT) {
            continue;
        }
        const struct display_item *item = &T->page.items[i];
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
    const struct display_item *item = &T->page.items[best];
    size_t start = T->item_start[best];
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
    if (T->item_start == NULL) {
        return NULL;
    }
    const struct display_item *found = NULL;
    for (size_t i = 0; i < T->page.item_count; i++) {
        if (T->item_start[i] == NO_TEXT) {
            continue;
        }
        if (T->item_start[i] > offset) {
            break;
        }
        found = &T->page.items[i];
    }
    return found;
}

static void copy_selection(void) {
    size_t start = selection_start(), end = selection_end();
    if (T->text == NULL || end <= start || end > T->text_length) {
        return;
    }
    long kept = opos_clipboard_set(T->text + start, end - start);
    set_status(kept < 0 ? "Could not copy" : "Copied");
}

static char fold(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool match_at(size_t at) {
    if (at + T->find_length > T->text_length) {
        return false;
    }
    for (size_t i = 0; i < T->find_length; i++) {
        char want = fold(T->find[i]);
        char have = fold(T->text[at + i]);
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
    if (top < T->scroll || top + item->height > T->scroll + viewport_height()) {
        T->scroll = top - viewport_height() / 3;
        clamp_scroll();
    }
}

/* Finds the query from offset onwards (backwards when step < 0), wrapping
   around the page, and selects the match. */
static void find_from(size_t from, int step) {
    T->have_match = false;
    if (T->text == NULL || T->find_length == 0
        || T->find_length > T->text_length) {
        b.dirty = true;
        return;
    }
    size_t last = T->text_length - T->find_length;
    if (from > last) {
        from = step > 0 ? 0 : last;
    }
    size_t at = from;
    for (size_t tried = 0; tried <= last; tried++) {
        if (match_at(at)) {
            T->have_match = true;
            T->match = at;
            T->selection_anchor = at;
            T->selection_cursor = at + T->find_length;
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
    if (T->text == NULL || T->find_length == 0) {
        return 0;
    }
    for (size_t at = 0; at + T->find_length <= T->text_length; at++) {
        if (match_at(at)) {
            count++;
            if (T->have_match && at == T->match) {
                *index = count;
            }
        }
    }
    return count;
}

/* ----------------------------------------------------------------- paint */

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
    int top = CHROME_HEIGHT;
    int bottom = top + viewport_height();
    struct opos_canvas *canvas = &b.canvas;
    uint32_t background = T->have_page ? T->page.background : 0x00ffffff;
    opos_fill_rect(canvas, 0, top, viewport_width(), viewport_height(),
                   background);
    if (!T->have_page) {
        return;
    }
    int origin_y = top + MARGIN - T->scroll;
    int fixed_y = top + MARGIN; /* fixed items do not scroll */
    int origin_x = MARGIN;
    for (size_t i = 0; i < T->page.item_count; i++) {
        const struct display_item *item = &T->page.items[i];
        int x = origin_x + item->x;
        int y = (item->fixed ? fixed_y : origin_y) + item->y;
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
            if (item->image != NULL) {
                background_paint(b.canvas.pixels, (int)b.canvas.stride,
                                 viewport_width(), b.canvas.height, item, x,
                                 y, top, bottom);
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
            size_t start = T->item_start != NULL ? T->item_start[i] : NO_TEXT;
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
                    && T->text[start - 1] == ' ') {
                    x0 -= opos_font_text_width(b.fonts[item->font],
                                               item->size, " ", 1);
                }
                bool is_match = T->have_match && low == T->match;
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
        case ITEM_BOX:
            break;
        case ITEM_IMAGE:
            if (item->image != NULL && item->image->state == 1) {
                image_blend(b.canvas.pixels, (int)b.canvas.stride,
                            viewport_width(), b.canvas.height, item->image, x,
                            y, item->width, item->height, 0, top,
                            viewport_width(), bottom);
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
    int total = T->page.content_height + 2 * MARGIN;
    if (total > viewport_height()) {
        int thumb = (int)((long long)viewport_height() * viewport_height() / total);
        if (thumb < 24) {
            thumb = 24;
        }
        int range = viewport_height() - thumb;
        int offset = max_scroll() > 0 ? (int)((long long)range * T->scroll / max_scroll()) : 0;
        opos_fill_rect(canvas, track_x + 2, top + offset, SCROLLBAR_WIDTH - 4,
                       thumb, OPOS_RGB(160, 164, 176));
    }
}

#define BUTTON_Y (TABBAR_HEIGHT + 6)
#define ADDRESS_X 106

static void draw_button(int x, const char *label, bool enabled) {
    opos_fill_rect(&b.canvas, x, BUTTON_Y, 28, 28,
                   enabled ? COL_FIELD : OPOS_RGB(214, 216, 222));
    opos_draw_rect(&b.canvas, x, BUTTON_Y, 28, 28, COL_BORDER);
    opos_draw_text(&b.canvas, x + 14 - (int)strlen(label) * 6, BUTTON_Y + 5,
                   label, enabled ? COL_TEXT : OPOS_RGB(150, 152, 160), 2);
}

/* Toolbar: < > R [address] * = */
static int address_width(void) { return b.width - ADDRESS_X - 8 - 2 * 32; }
static int star_x(void) { return ADDRESS_X + address_width() + 4; }
static int menu_button_x(void) { return star_x() + 32; }

static void draw_star(void) {
    int x = star_x();
    bool marked = T->have_page && bookmark_has(T->url);
    opos_fill_rect(&b.canvas, x, BUTTON_Y, 28, 28,
                   marked ? OPOS_RGB(255, 214, 92) : COL_FIELD);
    opos_draw_rect(&b.canvas, x, BUTTON_Y, 28, 28, COL_BORDER);
    /* U+2605 BLACK STAR, U+2606 WHITE STAR */
    const char *star = marked ? "\xe2\x98\x85" : "\xe2\x98\x86";
    struct opos_font *font = b.fonts[0];
    if (!opos_font_has_glyph(font, 0x2605)) {
        star = "*";
    }
    int width = opos_font_text_width(font, 18, star, strlen(star));
    opos_font_draw(font, &b.canvas, x + 14 - width / 2, BUTTON_Y + 21, 18,
                   star, strlen(star), COL_TEXT);
}

/* The bytes of text that fit in width pixels; *ellipsis tells whether
   "..." must follow (its width is included). */
static size_t fit_text(struct opos_font *font, int size, const char *text,
                       size_t length, int width, bool *ellipsis) {
    *ellipsis = false;
    if (opos_font_text_width(font, size, text, length) <= width) {
        return length;
    }
    *ellipsis = true;
    int dots = opos_font_text_width(font, size, "...", 3);
    size_t fits = 0;
    while (fits < length) {
        size_t next = browser_utf8_next(text, length, fits);
        if (opos_font_text_width(font, size, text, next) + dots > width) {
            break;
        }
        fits = next;
    }
    return fits;
}

static void draw_fitted(int x, int baseline, int size, int width,
                        const char *text, uint32_t colour) {
    bool ellipsis;
    size_t n = fit_text(b.fonts[0], size, text, strlen(text), width,
                        &ellipsis);
    int end = opos_font_draw(b.fonts[0], &b.canvas, x, baseline, size, text,
                             n, colour);
    if (ellipsis) {
        opos_font_draw(b.fonts[0], &b.canvas, end, baseline, size, "...", 3,
                       colour);
    }
}

/* -------------------------------------------------------------- tab bar */

/* What a tab shows: the page title, else the address. */
static const char *tab_label(const struct tab *tab) {
    if (tab->title[0] != '\0') {
        return tab->title;
    }
    if (strncmp(tab->url, "about:", 6) == 0 || tab->url[0] == '\0') {
        return "New tab";
    }
    const char *host = strstr(tab->url, "://");
    return host != NULL ? host + 3 : tab->url;
}

static int tab_width(void) {
    int width = (b.width - 8 - 32) / (b.tab_count > 0 ? b.tab_count : 1);
    return width > 220 ? 220 : width < 48 ? 48 : width;
}

static void paint_tabs(void) {
    struct opos_canvas *canvas = &b.canvas;
    opos_fill_rect(canvas, 0, 0, b.width, TABBAR_HEIGHT, COL_TABBAR);
    int width = tab_width();
    for (int i = 0; i < b.tab_count; i++) {
        int x = 4 + i * width;
        bool shown = i == b.current;
        opos_fill_rect(canvas, x, 4, width - 2, TABBAR_HEIGHT - 4,
                       shown ? COL_TOOLBAR : OPOS_RGB(216, 219, 227));
        if (!shown) {
            opos_draw_line(canvas, x, TABBAR_HEIGHT - 1, x + width - 2,
                           TABBAR_HEIGHT - 1, OPOS_RGB(190, 193, 202));
        }
        draw_fitted(x + 8, 20, 12, width - 34, tab_label(b.tabs[i]),
                    shown ? COL_TEXT : OPOS_RGB(70, 72, 82));
        /* Close button: a small cross. */
        int cx = x + width - 18, cy = 10;
        for (int d = 0; d < 8; d++) {
            opos_fill_rect(canvas, cx + d, cy + d, 2, 1, OPOS_RGB(90, 92, 100));
            opos_fill_rect(canvas, cx + 7 - d, cy + d, 2, 1,
                           OPOS_RGB(90, 92, 100));
        }
    }
    if (b.tab_count < MAX_TABS) {
        int x = 4 + b.tab_count * width + 6;
        opos_fill_rect(canvas, x + 4, 15, 13, 2, OPOS_RGB(60, 62, 70));
        opos_fill_rect(canvas, x + 10, 9, 2, 13, OPOS_RGB(60, 62, 70));
    }
}

/* The tab at x in the tab bar (-1: none, b.tab_count: the + button);
   *close is set when x is on the tab's close button. */
static int tab_at(int x, bool *close) {
    int width = tab_width();
    *close = false;
    if (x < 4) {
        return -1;
    }
    int index = (x - 4) / width;
    if (index < b.tab_count) {
        int left = 4 + index * width;
        *close = x >= left + width - 24;
        return index;
    }
    int plus = 4 + b.tab_count * width + 6;
    return b.tab_count < MAX_TABS && x >= plus && x < plus + 24
         ? b.tab_count : -1;
}

/* ---------------------------------------------------------------- menu */

#define MENU_ROW 24
#define MENU_WIDTH 230

static const char *menu_label(enum menu_action action) {
    switch (action) {
    case MENU_OPEN_NEW_TAB: return "Open link in new tab";
    case MENU_COPY_LINK: return "Copy link address";
    case MENU_BOOKMARK_LINK: return "Bookmark link";
    case MENU_DOWNLOAD_LINK: return "Download link";
    case MENU_BACK: return "Back";
    case MENU_FORWARD: return "Forward";
    case MENU_RELOAD: return "Reload";
    case MENU_BOOKMARK_PAGE:
        return bookmark_has(T->url) ? "Remove bookmark" : "Bookmark this page";
    case MENU_COPY: return "Copy";
    case MENU_NEW_TAB: return "New tab";
    case MENU_CLOSE_TAB: return "Close tab";
    case MENU_BOOKMARKS: return "Bookmarks";
    case MENU_HISTORY: return "History";
    case MENU_DOWNLOADS: return "Downloads";
    case MENU_COOKIES: return "Cookies";
    case MENU_JAVASCRIPT:
        return scripting_enabled ? "Turn JavaScript off"
                                 : "Turn JavaScript on";
    default: return "";
    }
}

static const char *menu_shortcut(enum menu_action action) {
    switch (action) {
    case MENU_BACK: return "Backspace";
    case MENU_RELOAD: return "Ctrl+R";
    case MENU_BOOKMARK_PAGE: return "Ctrl+D";
    case MENU_COPY: return "Ctrl+C";
    case MENU_NEW_TAB: return "Ctrl+T";
    case MENU_CLOSE_TAB: return "Ctrl+W";
    case MENU_BOOKMARKS: return "Ctrl+B";
    case MENU_HISTORY: return "Ctrl+H";
    case MENU_DOWNLOADS: return "Ctrl+J";
    default: return "";
    }
}

static void menu_add(enum menu_action action) {
    if (b.menu_count < MENU_COUNT) {
        b.menu[b.menu_count++] = action;
    }
}

/* Opens the menu with its top left corner near (x, y), kept on screen. */
static void menu_place(int x, int y) {
    int height = b.menu_count * MENU_ROW + 8;
    if (x + MENU_WIDTH > b.width) {
        x = b.width - MENU_WIDTH;
    }
    if (y + height > b.height) {
        y = b.height - height;
    }
    b.menu_x = x < 0 ? 0 : x;
    b.menu_y = y < 0 ? 0 : y;
    b.menu_hover = -1;
    b.dirty = true;
}

static int menu_row_at(int x, int y) {
    if (b.menu_count == 0 || x < b.menu_x || x >= b.menu_x + MENU_WIDTH
        || y < b.menu_y + 4) {
        return -1;
    }
    int row = (y - b.menu_y - 4) / MENU_ROW;
    return row < b.menu_count ? row : -1;
}

static void paint_menu(void) {
    if (b.menu_count == 0) {
        return;
    }
    struct opos_canvas *canvas = &b.canvas;
    int height = b.menu_count * MENU_ROW + 8;
    opos_fill_rect(canvas, b.menu_x + 3, b.menu_y + 3, MENU_WIDTH, height,
                   OPOS_RGB(150, 152, 160));
    opos_fill_rect(canvas, b.menu_x, b.menu_y, MENU_WIDTH, height, COL_FIELD);
    opos_draw_rect(canvas, b.menu_x, b.menu_y, MENU_WIDTH, height,
                   COL_BORDER);
    for (int i = 0; i < b.menu_count; i++) {
        int y = b.menu_y + 4 + i * MENU_ROW;
        if (i == b.menu_hover) {
            opos_fill_rect(canvas, b.menu_x + 2, y, MENU_WIDTH - 4, MENU_ROW,
                           COL_SELECTION);
        }
        const char *label = menu_label(b.menu[i]);
        opos_font_draw(b.fonts[0], canvas, b.menu_x + 12, y + 17, 13, label,
                       strlen(label), COL_TEXT);
        const char *keys = menu_shortcut(b.menu[i]);
        int width = opos_font_text_width(b.fonts[0], 12, keys, strlen(keys));
        opos_font_draw(b.fonts[0], canvas, b.menu_x + MENU_WIDTH - 12 - width,
                       y + 17, 12, keys, strlen(keys), OPOS_RGB(120, 122, 132));
    }
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
    if (T->dropdown == NULL || !T->have_page) {
        return false;
    }
    for (size_t i = 0; i < T->page.item_count; i++) {
        const struct display_item *item = &T->page.items[i];
        if (item->kind != ITEM_RECT || item->control != T->dropdown) {
            continue;
        }
        int count = form_option_count(T->dropdown);
        *rows = count < DROP_MAX_ROWS ? count : DROP_MAX_ROWS;
        *x = MARGIN + item->x;
        *width = item->width < 120 ? 120 : item->width;
        int top = CHROME_HEIGHT + MARGIN + item->y
                - (item->fixed ? 0 : T->scroll);
        int bottom = CHROME_HEIGHT + viewport_height();
        *y = top + item->height;
        if (*y + *rows * DROP_ROW + 2 > bottom && top - *rows * DROP_ROW - 2
                                                  >= CHROME_HEIGHT) {
            *y = top - *rows * DROP_ROW - 2; /* no room below: open up */
        }
        return *rows > 0;
    }
    return false;
}

/* The first option shown, so that the chosen one is in view. */
static int dropdown_first(int rows) {
    struct form_field *field = form_field(&T->page, T->dropdown);
    int chosen = field != NULL ? field->option : 0;
    int count = form_option_count(T->dropdown);
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
    struct form_field *field = form_field(&T->page, T->dropdown);
    int chosen = field != NULL ? field->option : 0;
    int first = dropdown_first(rows);
    int height = rows * DROP_ROW + 2;
    opos_fill_rect(canvas, x, y, width, height, COL_FIELD);
    opos_draw_rect(canvas, x, y, width, height, COL_BORDER);
    for (int row = 0; row < rows; row++) {
        int index = first + row;
        char label[128];
        option_label(form_option(T->dropdown, index), label, sizeof(label));
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
        struct form_field *field = form_field(&T->page, T->dropdown);
        if (field != NULL) {
            field->option = dropdown_first(rows) + (click_y - y - 1) / DROP_ROW;
        }
    }
    T->page.focus = T->dropdown;
    T->dropdown = NULL;
    refresh_controls();
    b.dirty = true;
}

static void paint(void) {
    struct opos_canvas *canvas = &b.canvas;
    /* The page first: the chrome then covers text that runs above it. */
    paint_page();
    opos_fill_rect(canvas, 0, TABBAR_HEIGHT, b.width, TOOLBAR_HEIGHT,
                   COL_TOOLBAR);
    paint_tabs();
    int field_x = ADDRESS_X;
    int field_w = address_width();
    int field_y = BUTTON_Y;
    opos_fill_rect(canvas, field_x, field_y, field_w, 28, COL_FIELD);
    int text_width = opos_font_text_width(b.fonts[0], 15, T->address,
                                          T->address_length);
    int shift = text_width > field_w - 16 ? text_width - (field_w - 16) : 0;
    if (T->editing && T->address_selected && text_width > 0) {
        int from = field_x + 8 - shift;
        int width = text_width + 2;
        if (from < field_x + 2) {
            width -= field_x + 2 - from;
            from = field_x + 2;
        }
        if (from + width > field_x + field_w - 2) {
            width = field_x + field_w - 2 - from;
        }
        opos_fill_rect(canvas, from, field_y + 3, width, 22, COL_SELECTION);
    }
    /* Draw the address, then paint the toolbar around the field over
       whatever ran past its edges. */
    opos_font_draw(b.fonts[0], canvas, field_x + 8 - shift, field_y + 19, 15,
                   T->address, T->address_length, COL_TEXT);
    opos_fill_rect(canvas, 0, TABBAR_HEIGHT, field_x, TOOLBAR_HEIGHT,
                   COL_TOOLBAR);
    opos_fill_rect(canvas, field_x + field_w, TABBAR_HEIGHT,
                   b.width - field_x - field_w, TOOLBAR_HEIGHT, COL_TOOLBAR);
    draw_button(6, "<", T->history_position > 0);
    draw_button(38, ">", T->history_position + 1 < T->history_count);
    draw_button(70, "R", T->have_page);
    draw_star();
    draw_button(menu_button_x(), "=", true);
    opos_draw_rect(canvas, field_x, field_y, field_w, 28,
                   T->editing ? OPOS_RGB(6, 69, 173) : COL_BORDER);
    if (T->editing) {
        int cursor = field_x + 8 + text_width - shift;
        opos_fill_rect(canvas, cursor + 1, field_y + 5, 1, 18, COL_TEXT);
    }
    paint_dropdown();
    int status_y = b.height - STATUS_HEIGHT;
    opos_fill_rect(canvas, 0, status_y, b.width, STATUS_HEIGHT, COL_STATUS);
    opos_draw_line(canvas, 0, status_y, b.width, status_y,
                   OPOS_RGB(200, 200, 208));
    if (T->finding) {
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
                                 12, T->find, T->find_length, COL_TEXT);
        opos_fill_rect(canvas, end + 1, status_y + 4, 1, STATUS_HEIGHT - 7,
                       COL_TEXT);
        char result[64] = "";
        size_t index;
        size_t count = count_matches(&index);
        if (T->find_length > 0) {
            if (count == 0) {
                snprintf(result, sizeof(result), "No matches");
            } else {
                snprintf(result, sizeof(result), "%u of %u", (unsigned)index,
                         (unsigned)count);
            }
        }
        opos_font_draw(b.fonts[0], canvas, box_x + box_w + 10, status_y + 15,
                       12, result, strlen(result),
                       count == 0 && T->find_length > 0 ? OPOS_RGB(180, 30, 30)
                                                       : OPOS_RGB(70, 70, 80));
    } else {
        opos_font_draw(b.fonts[0], canvas, 6, status_y + 15, 12, b.status,
                       strlen(b.status), OPOS_RGB(70, 70, 80));
    }
    paint_menu();
    opos_canvas_present(canvas);
    b.dirty = false;
}

/* --------------------------------------------------------------- loading */

static void sheets_update(struct page *page, const char *base, bool show);

static void relayout(void) {
    if (!T->have_page) {
        return;
    }
    viewport_width_px = viewport_width();
    viewport_height_px = viewport_height();
    /* Scripts may have added, changed or removed style sheets. */
    sheets_update(&T->page, T->page.url, false);
    layout_page(&T->page, b.fonts, viewport_width() - 2 * MARGIN);
    script_layout_done(&T->page);
    build_text();
    /* Images that arrive change the layout, not the text: offsets stay. */
    if (T->selection_anchor > T->text_length) {
        T->selection_anchor = T->text_length;
    }
    if (T->selection_cursor > T->text_length) {
        T->selection_cursor = T->text_length;
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

/* Style sheets a page fetches: at most MAX_SHEETS of them and
   MAX_CSS_BYTES in all (big sites link 20+ sheets). */
#define MAX_SHEETS 32
#define MAX_SHEET_BYTES (2u << 20)
#define MAX_CSS_BYTES (6u << 20)

static bool starts_with_ci(const char *text, const char *prefix);

/* The sheet's text with every relative url(...) made absolute against
   the sheet's own address (malloc'd), or NULL. */
static char *absolute_urls(const char *base, const char *text, size_t length,
                           size_t *out_length) {
    size_t capacity = length + 256, used = 0;
    char *out = malloc(capacity + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t i = 0;
    while (i < length) {
        bool url = i + 4 <= length && memcmp(text + i, "url(", 4) == 0
                && (i == 0 || !(isalnum((unsigned char)text[i - 1])
                                || text[i - 1] == '-'));
        size_t copy_from = i, copy_to = i + 1;
        char resolved[OPOS_HTTP_URL_MAX];
        bool replace = false;
        size_t after = i + 1;
        if (url) {
            size_t p = i + 4;
            while (p < length && (text[p] == ' ' || text[p] == '\t')) {
                p++;
            }
            char quote = p < length && (text[p] == '"' || text[p] == '\'')
                       ? text[p] : 0;
            size_t start = quote ? p + 1 : p;
            size_t end = start;
            while (end < length && end - start < OPOS_HTTP_URL_MAX - 1
                   && (quote ? text[end] != quote
                             : text[end] != ')' && text[end] != ' ')) {
                end++;
            }
            size_t n = end - start;
            if (end < length && n > 0 && n < OPOS_HTTP_URL_MAX - 1
                && !starts_with_ci(text + start, "data:")
                && text[start] != '#') {
                char reference[OPOS_HTTP_URL_MAX];
                memcpy(reference, text + start, n);
                reference[n] = '\0';
                if (opos_url_resolve(base, reference, resolved,
                                     sizeof(resolved))) {
                    replace = true;
                    copy_to = start;      /* "url(" and any quote */
                    after = end;          /* the closing quote or ")" on */
                }
            }
        }
        size_t piece = copy_to - copy_from;
        size_t extra = replace ? strlen(resolved) : 0;
        if (used + piece + extra > capacity) {
            capacity = (used + piece + extra) * 2;
            char *grown = realloc(out, capacity + 1);
            if (grown == NULL) {
                free(out);
                return NULL;
            }
            out = grown;
        }
        memcpy(out + used, text + copy_from, piece);
        used += piece;
        if (replace) {
            memcpy(out + used, resolved, extra);
            used += extra;
        }
        i = replace ? after : copy_to;
    }
    out[used] = '\0';
    *out_length = used;
    return out;
}

static __attribute__((noinline)) struct css_sheet *fetch_sheet(
    const char *url, int number, bool show, size_t *bytes) {
    char text[OPOS_HTTP_URL_MAX + 32];
    snprintf(text, sizeof(text), "Style %d: %s", number, url);
    set_status(text);
    if (show) {
        paint();
    }
    struct css_sheet *sheet = NULL;
    struct opos_http_response response;
    if (opos_http_get(url, MAX_SHEET_BYTES, &response) == 0) {
        if (response.status == 200) {
            *bytes += response.length;
            size_t length;
            char *absolute = absolute_urls(response.url,
                                           (const char *)response.body,
                                           response.length, &length);
            if (absolute != NULL) {
                sheet = css_parse(absolute, length);
                free(absolute);
            } else {
                sheet = css_parse((const char *)response.body,
                                  response.length);
            }
        }
        opos_http_free(&response);
    }
    return sheet;
}

/* Where a sheet of the page's chain came from: a <style> element (its
   text), a linked sheet (its address) or the adopted sheets (node NULL,
   not link). hash: of the text or the address. */
struct page_sheet {
    const struct opos_html_node *node;
    bool link;
    bool taken;                /* sheets_update: reused */
    uint32_t hash;
    struct css_sheet *sheet;   /* NULL: it failed to load */
};

struct sheet_walk {
    struct page *page;
    const char *base;
    bool show;                 /* paint the status while fetching */
    struct page_sheet *list;
    size_t count, capacity;
    bool changed;
};

static uint32_t hash_bytes(const char *text, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ (uint8_t)text[i]) * 16777619u;
    }
    return hash;
}

static bool sheet_room(struct sheet_walk *w) {
    if (w->count < w->capacity) {
        return true;
    }
    size_t capacity = w->capacity == 0 ? 16 : w->capacity * 2;
    struct page_sheet *grown = realloc(w->list, capacity * sizeof(*grown));
    if (grown == NULL) {
        return false;
    }
    w->list = grown;
    w->capacity = capacity;
    return true;
}

/* Moves the page's old entry for this sheet to the new list; false when
   there is none. */
static bool sheet_reuse(struct sheet_walk *w, const struct opos_html_node *node,
                        bool link, uint32_t hash) {
    struct page *page = w->page;
    for (size_t i = 0; i < page->sheet_count; i++) {
        struct page_sheet *old = &page->sheet_list[i];
        if (!old->taken && old->hash == hash && old->link == link
            && (link || old->node == node)) {
            if (i != w->count) {
                w->changed = true;
            }
            old->taken = true;
            w->list[w->count] = *old;
            w->list[w->count].node = node;
            w->list[w->count].taken = false;
            w->count++;
            return true;
        }
    }
    return false;
}

static void sheet_add_text(struct sheet_walk *w,
                           const struct opos_html_node *node,
                           const char *text, size_t length) {
    uint32_t hash = hash_bytes(text, length);
    if (!sheet_room(w) || sheet_reuse(w, node, false, hash)) {
        return;
    }
    w->list[w->count++] = (struct page_sheet){node, false, false, hash,
                                              css_parse(text, length)};
    w->changed = true;
}

static void sheet_add_style(struct sheet_walk *w,
                            const struct opos_html_node *node) {
    size_t length;
    const char *text = script_sheet_text(w->page, node, &length);
    if (text != NULL) {
        sheet_add_text(w, node, text, length);
        return;
    }
    size_t total = 0, count = 0;
    const struct opos_html_node *only = NULL;
    for (const struct opos_html_node *t = node->first_child; t != NULL;
         t = t->next_sibling) {
        if (t->type == HTML_NODE_TEXT) {
            total += t->text_length;
            count++;
            only = t;
        }
    }
    if (count <= 1) {
        sheet_add_text(w, node, only != NULL ? only->text : "",
                       only != NULL ? only->text_length : 0);
        return;
    }
    char *joined = malloc(total + 1);
    if (joined == NULL) {
        return;
    }
    size_t used = 0;
    for (const struct opos_html_node *t = node->first_child; t != NULL;
         t = t->next_sibling) {
        if (t->type == HTML_NODE_TEXT) {
            memcpy(joined + used, t->text, t->text_length);
            used += t->text_length;
        }
    }
    joined[used] = '\0';
    sheet_add_text(w, node, joined, used);
    free(joined);
}

static void sheet_add_link(struct sheet_walk *w,
                           const struct opos_html_node *node,
                           const char *href) {
    char url[OPOS_HTTP_URL_MAX];
    if (!opos_url_resolve(w->base, href, url, sizeof(url))) {
        return;
    }
    uint32_t hash = hash_bytes(url, strlen(url));
    for (size_t i = 0; i < w->count; i++) {
        if (w->list[i].link && w->list[i].hash == hash) {
            return; /* each address once */
        }
    }
    if (!sheet_room(w) || sheet_reuse(w, node, true, hash)) {
        return;
    }
    struct page *page = w->page;
    struct css_sheet *sheet = NULL;
    if (page->sheet_fetches < MAX_SHEETS
        && page->sheet_bytes < MAX_CSS_BYTES) {
        page->sheet_fetches++;
        sheet = fetch_sheet(url, page->sheet_fetches, w->show,
                            &page->sheet_bytes);
    }
    w->list[w->count++] = (struct page_sheet){node, true, false, hash, sheet};
    w->changed = true;
}

static void sheets_walk(struct sheet_walk *w, struct opos_html_node *node) {
    for (; node != NULL; node = node->next_sibling) {
        if (node->type != HTML_NODE_ELEMENT) {
            continue;
        }
        if (node->tag == HTML_TAG_STYLE) {
            const char *media = opos_html_attribute(node, "media");
            if (media == NULL || css_media_matches(media)) {
                sheet_add_style(w, node);
            }
            continue;
        }
        if (node->tag == HTML_TAG_LINK) {
            const char *rel = opos_html_attribute(node, "rel");
            const char *href = opos_html_attribute(node, "href");
            const char *media = opos_html_attribute(node, "media");
            if (rel != NULL && href != NULL && strstr(rel, "stylesheet") != NULL
                && strstr(rel, "alternate") == NULL
                && opos_html_attribute(node, "disabled") == NULL
                && (media == NULL || css_media_matches(media))) {
                sheet_add_link(w, node, href);
            }
            continue;
        }
        if (node->tag == HTML_TAG_TEMPLATE) {
            continue;
        }
        sheets_walk(w, node->first_child);
    }
}

/* Brings the page's sheets in line with its <style> and <link> elements,
   which scripts add, change and remove: new ones are parsed or fetched,
   unchanged ones kept, and the chain relinked when anything changed. */
static void sheets_update(struct page *page, const char *base, bool show) {
    if (page->doc == NULL) {
        return;
    }
    struct sheet_walk w;
    memset(&w, 0, sizeof(w));
    w.page = page;
    w.base = base;
    w.show = show;
    sheets_walk(&w, page->doc->root);
    size_t length;
    const char *adopted = script_sheet_text(page, NULL, &length);
    if (adopted != NULL) {
        sheet_add_text(&w, NULL, adopted, length);
    }
    bool removed = false;
    for (size_t i = 0; i < page->sheet_count; i++) {
        removed |= !page->sheet_list[i].taken;
    }
    if (!w.changed && !removed) {
        free(page->sheet_list);
        page->sheet_list = w.list;
        css_forget_matches(page->sheets);
        return;
    }
    for (size_t i = 0; i < page->sheet_count; i++) {
        struct page_sheet *old = &page->sheet_list[i];
        if (!old->taken && old->sheet != NULL) {
            css_unlink_free(old->sheet);
        }
    }
    free(page->sheet_list);
    page->sheet_list = w.list;
    page->sheet_count = w.count;
    struct css_sheet **chain = malloc((w.count + 1) * sizeof(*chain));
    if (chain == NULL) {
        /* Keep the sheets apart rather than half linked. */
        for (size_t i = 0; i < w.count; i++) {
            if (w.list[i].sheet != NULL) {
                css_unlink_free(w.list[i].sheet);
            }
        }
        page->sheet_count = 0;
        page->sheets = NULL;
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < w.count; i++) {
        if (w.list[i].sheet != NULL) {
            chain[used++] = w.list[i].sheet;
        }
    }
    page->sheets = css_relink(chain, used);
    free(chain);
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

/* A plain ASCII stand-in for a character the window frame cannot show:
   the base letter of an accented Latin letter, a dash or quote, else
   '?'. */
static char ascii_fold(uint32_t code) {
    /* U+00C0 to U+017F, two characters per letter pair where they
       alternate; '?' where nothing fits. */
    static const char latin1[] =
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii"
        "dnooooo/ouuuuyty";
    static const char extended_a[] =
        "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi"
        "IiJjJjKkkLlLlLlL" "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs"
        "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
    /* U+1EA0 to U+1EF9: Vietnamese letters with tone marks. */
    static const char vietnamese[] =
        "AaAaAaAaAaAaAaAaAaAaAaAaEeEeEeEeEeEeEeEeIiIiOoOoOoOoOoOoOoOo"
        "OoOoOoOoUuUuUuUuUuUuUuYyYyYyYy";
    if (code >= 0xc0 && code < 0x100) {
        return latin1[code - 0xc0];
    }
    if (code >= 0x100 && code < 0x180) {
        return extended_a[code - 0x100];
    }
    if (code >= 0x1ea0 && code <= 0x1ef9) {
        return vietnamese[code - 0x1ea0];
    }
    if (code >= 0x2010 && code <= 0x2015) {
        return '-';
    }
    if (code >= 0x2018 && code <= 0x201b) {
        return '\'';
    }
    if (code >= 0x201c && code <= 0x201f) {
        return '"';
    }
    if (code == 0x2022 || code == 0xb7) {
        return '-';
    }
    if (code == 0xa0) {
        return ' ';
    }
    return '?';
}

/* Sends the window a title: the page title folded to ASCII (the window
   frame shows only that), else "Browser". */
static void update_caption(void) {
    char caption[sizeof(b.caption)];
    size_t used = 0;
    const char *title = T->title[0] != '\0' ? T->title : "Browser";
    size_t length = strlen(title);
    bool space = false;
    for (size_t i = 0; i < length && used + 1 < sizeof(caption);) {
        uint32_t code;
        i += opos_utf8_decode(title + i, length - i, &code);
        char c = code < 0x80 ? (char)code : ascii_fold(code);
        if (c < ' ' || c > '~') {
            continue;
        }
        if (c == ' ' && (space || used == 0)) {
            continue;
        }
        space = c == ' ';
        caption[used++] = (char)c;
    }
    if (used + 1 == sizeof(caption) && strlen(title) > used) {
        memcpy(caption + used - 3, "...", 3);
    }
    caption[used] = '\0';
    if (strcmp(caption, b.caption) != 0) {
        memcpy(b.caption, caption, sizeof(caption));
        opos_window_title(caption);
    }
}

/* T->title from the document's <title>, white space collapsed. */
static void set_title(const char *title) {
    size_t used = 0;
    bool space = true;
    for (; title != NULL && *title != '\0' && used + 1 < sizeof(T->title);
         title++) {
        char c = *title == '\n' || *title == '\t' || *title == '\r' ? ' '
                                                                   : *title;
        if (c == ' ' && space) {
            continue;
        }
        space = c == ' ';
        T->title[used++] = c;
    }
    while (used > 0 && T->title[used - 1] == ' ') {
        used--;
    }
    /* Do not end inside a UTF-8 character. */
    while (used > 0 && (T->title[used - 1] & 0xc0) == 0x80
           && used + 1 == sizeof(T->title)) {
        used--;
    }
    T->title[used] = '\0';
    update_caption();
}

/* The element a "#fragment" names: the one with that id, or an <a> with
   that name. */
static const struct opos_html_node *find_anchor(const struct opos_html_node *root,
                                                const char *name) {
    const struct opos_html_node *node = root;
    while (node != NULL) {
        if (node->type == HTML_NODE_ELEMENT) {
            const char *id = opos_html_attribute(node, "id");
            if (id != NULL && strcmp(id, name) == 0) {
                return node;
            }
            if (node->tag == HTML_TAG_A) {
                const char *anchor = opos_html_attribute(node, "name");
                if (anchor != NULL && strcmp(anchor, name) == 0) {
                    return node;
                }
            }
        }
        if (node->first_child != NULL) {
            node = node->first_child;
            continue;
        }
        while (node != NULL && node != root && node->next_sibling == NULL) {
            node = node->parent;
        }
        if (node == NULL || node == root) {
            break;
        }
        node = node->next_sibling;
    }
    return NULL;
}

static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Scrolls to the anchor's top again (after images changed the layout). */
static void follow_anchor(void) {
    if (T->follow_anchor && T->page.anchor != NULL
        && T->page.anchor_y >= 0) {
        T->scroll = T->page.anchor_y + MARGIN - 4;
        clamp_scroll();
        b.dirty = true;
    }
}

/* Scrolls to the part of the page that url's #fragment names; the top for
   "#" and "#top" or when nothing has the name. */
static void scroll_to_fragment(const char *url) {
    const char *hash = strchr(url, '#');
    T->page.anchor = NULL;
    T->follow_anchor = false;
    T->scroll = 0;
    if (hash == NULL || !T->have_page) {
        relayout();
        return;
    }
    char name[256];
    size_t used = 0;
    for (const char *p = hash + 1; *p != '\0' && used + 1 < sizeof(name);
         p++) {
        if (*p == '%' && hex_value(p[1]) >= 0 && hex_value(p[2]) >= 0) {
            name[used++] = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
            p += 2;
        } else {
            name[used++] = *p;
        }
    }
    name[used] = '\0';
    if (used > 0) {
        T->page.anchor = find_anchor(T->page.doc->root, name);
    }
    relayout();
    T->follow_anchor = T->page.anchor != NULL;
    follow_anchor();
}

static void install_document(const char *html, size_t length,
                             const char *charset, const char *url) {
    struct opos_html_doc *doc = opos_html_parse(html, length, charset);
    if (doc == NULL) {
        set_status("Out of memory");
        return;
    }
    if (T->have_page) {
        page_destroy(&T->page);
    }
    memset(&T->page, 0, sizeof(T->page));
    T->page.doc = doc;
    css_reset_variables();
    /* @media rules are decided while the sheets are parsed. */
    viewport_width_px = viewport_width();
    viewport_height_px = viewport_height();
    snprintf(T->page.url, sizeof(T->page.url), "%s", url);
    sheets_update(&T->page, url, true);
    T->have_page = true;
    T->scroll = 0;
    T->script_scroll = 0;
    clear_selection();
    b.field_selecting = false;
    b.field_selection = NULL;
    T->dropdown = NULL;
    T->page.focus = find_autofocus(doc->body);
    /* Web pages run their scripts before the first layout. */
    if (scripting_enabled && (starts_with_ci(url, "http://")
                              || starts_with_ci(url, "https://"))) {
        script_page_start(&T->page);
        script_take_dirty(&T->page);
        script_take_title(&T->page);
    }
    set_title(T->page.doc->title);
    scroll_to_fragment(url);
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

/* ------------------------------------------------------------- downloads */

/* A file name for url: its last path segment without the query, made safe
   ("download" when there is none). */
static void download_name(const char *url, char *out, size_t capacity) {
    const char *path = strstr(url, "://");
    path = path != NULL ? strchr(path + 3, '/') : NULL;
    const char *start = "", *end = start;
    if (path != NULL) {
        end = path + strcspn(path, "?#");
        start = end;
        while (start > path && start[-1] != '/') {
            start--;
        }
    }
    size_t used = 0;
    for (const char *p = start; p < end && used + 1 < capacity; p++) {
        char c = *p;
        if (c == '%' && p + 2 < end && hex_value(p[1]) >= 0
            && hex_value(p[2]) >= 0) {
            c = (char)(hex_value(p[1]) * 16 + hex_value(p[2]));
            p += 2;
        }
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                 || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'
                 || c == ' ' || c == '+' || c == '(' || c == ')';
        out[used++] = safe ? c : '_';
    }
    while (used > 0 && (out[used - 1] == '.' || out[used - 1] == ' ')) {
        used--;
    }
    out[used] = '\0';
    if (used == 0 || out[0] == '.') {
        snprintf(out, capacity, "download");
    }
}

/* Creates a new file for name in the downloads folder, adding " (2)" and
   so on before the extension while the name is taken. */
static long download_create(const char *name, char *path, size_t capacity) {
    if (b.downloads_dir[0] == '\0') {
        return -OPOS_ENOENT;
    }
    opos_mkdir(b.downloads_dir);
    const char *dot = strrchr(name, '.');
    size_t stem = dot != NULL && dot != name ? (size_t)(dot - name)
                                             : strlen(name);
    long fd = -OPOS_EEXIST;
    for (int n = 1; n < 100 && fd == -OPOS_EEXIST; n++) {
        if (n == 1) {
            snprintf(path, capacity, "%s/%s", b.downloads_dir, name);
        } else {
            snprintf(path, capacity, "%s/%.*s (%d)%s", b.downloads_dir,
                     (int)stem, name, n, name + stem);
        }
        fd = opos_open(path, OPOS_O_WRONLY | OPOS_O_CREAT | OPOS_O_EXCL);
    }
    return fd;
}

static bool write_all(int fd, const uint8_t *data, size_t length) {
    while (length > 0) {
        size_t chunk = length < 65536 ? length : 65536;
        long written = opos_write(fd, data, chunk);
        if (written <= 0) {
            return false;
        }
        data += written;
        length -= (size_t)written;
    }
    return true;
}

static void download_finished(const char *name, const char *path,
                              unsigned long long size, bool ok) {
    download_record(name, path, size, !ok);
    char text[OPOS_HTTP_URL_MAX + 64];
    if (ok) {
        snprintf(text, sizeof(text), "Saved %s to Downloads (Ctrl+J)", name);
        opos_notify("Download complete", name);
    } else {
        snprintf(text, sizeof(text), "Download of %s failed", name);
    }
    set_status(text);
}

/* Saves a body already in memory. */
static void save_download(const char *url, const uint8_t *data,
                          size_t length) {
    char name[96], path[256];
    download_name(url, name, sizeof(name));
    long fd = download_create(name, path, sizeof(path));
    if (fd < 0) {
        download_finished(name, b.downloads_dir, 0, false);
        return;
    }
    bool ok = write_all((int)fd, data, length);
    opos_close((int)fd);
    if (!ok) {
        opos_unlink(path);
    }
    download_finished(name, path, length, ok);
}

struct download_sink {
    int fd;
    const char *name;
};

static long download_write(void *context, const uint8_t *data,
                           size_t length) {
    struct download_sink *sink = context;
    return write_all(sink->fd, data, length) ? 0 : -OPOS_EIO;
}

static void download_progress(void *context, size_t received, size_t total) {
    const struct download_sink *sink = context;
    char text[160];
    if (total > 0) {
        snprintf(text, sizeof(text), "Downloading %s: %u of %u KB", sink->name,
                 (unsigned)(received >> 10), (unsigned)(total >> 10));
    } else {
        snprintf(text, sizeof(text), "Downloading %s: %u KB", sink->name,
                 (unsigned)(received >> 10));
    }
    set_status(text);
    paint();
}

/* Downloads url straight to the file fd, however large, while the window
   waits (when no background download can be started). */
static void download_here(const char *url, long fd, const char *name,
                          const char *path) {
    struct download_sink sink = {(int)fd, name};
    struct opos_http_response response;
    opos_http_set_progress(download_progress, &sink);
    cookies_attach(false);
    long error = opos_http_download(url, (size_t)MAX_DOWNLOAD_BYTES,
                                    download_write, &sink, &response);
    cookies_detach();
    opos_http_set_progress(NULL, NULL);
    opos_close((int)fd);
    bool ok = error == 0 && response.status >= 200 && response.status < 300;
    unsigned long long size = error == 0 ? response.length : 0;
    if (error == 0) {
        opos_http_free(&response);
    }
    if (!ok) {
        opos_unlink(path);
    }
    download_finished(name, path, size, ok);
}

/*
 * Background downloads. Each runs in a forked copy of the browser, which
 * fetches into the file and reports through a pipe, one line per report:
 *   "P received total" while it runs, "D 1 size" or "D 0 0" at the end.
 * The window stays usable meanwhile. The copy also posts the "Download
 * complete" notification, so it arrives even if the browser was closed.
 */
struct running {
    bool used;
    int id;              /* the library's download id */
    long pid;
    int pipe;            /* read end, nonblocking */
    bool reported;       /* the "D" line arrived */
    bool ok;
    bool cancelled;
    unsigned long long size;
    char line[64];
    size_t line_length;
    char name[96];
    char path[256];
};

static struct running running[MAX_RUNNING_DOWNLOADS];
static bool downloads_page_stale; /* about:downloads needs a refresh */

#define LINUX_O_NONBLOCK 0x800
#define LINUX_O_CLOEXEC 0x80000
#define LINUX_EAGAIN 11
#define LINUX_SIGKILL 9

static int child_pipe = -1;      /* in a download child: the write end */
static long long child_reported; /* uptime in ms of the last "P" line */

static long long uptime_ms(void) {
    struct opos_timespec time = {0, 0};
    opos_uptime(&time);
    return (long long)time.seconds * 1000 + time.nanoseconds / 1000000;
}

static void child_send(const char *line, bool must) {
    size_t length = strlen(line);
    for (int tries = 0; tries < 3000; tries++) {
        long written = opos_write(child_pipe, line, length);
        if (written == (long)length) {
            return;
        }
        if (written != -LINUX_EAGAIN || !must) {
            return; /* a lost progress line does not matter */
        }
        opos_sleep_ms(10);
    }
}

static void child_progress(void *context, size_t received, size_t total) {
    (void)context;
    long long t = uptime_ms();
    if (t - child_reported < 300) {
        return;
    }
    child_reported = t;
    char line[64];
    snprintf(line, sizeof(line), "P %llu %llu\n",
             (unsigned long long)received, (unsigned long long)total);
    child_send(line, false);
}

static long child_write(void *context, const uint8_t *data, size_t length) {
    return write_all(*(int *)context, data, length) ? 0 : -OPOS_EIO;
}

__attribute__((noreturn)) static void child_download(const char *url, int fd,
                                                     const char *name) {
    struct opos_http_response response;
    opos_http_set_progress(child_progress, NULL);
    cookies_attach(false); /* send the site's cookies, keep no new ones */
    long error = opos_http_download(url, (size_t)MAX_DOWNLOAD_BYTES,
                                    child_write, &fd, &response);
    opos_close(fd);
    bool ok = error == 0 && response.status >= 200 && response.status < 300;
    char line[64];
    snprintf(line, sizeof(line), "D %d %llu\n", ok ? 1 : 0,
             ok ? (unsigned long long)response.length : 0ull);
    if (ok) {
        opos_notify("Download complete", name);
    }
    child_send(line, true);
    opos_close(child_pipe);
    for (;;) {
        opos_syscall1(231 /* exit_group */, ok ? 0 : 1);
    }
}

/* Downloads url to a new file in Downloads, in the background when it
   can. */
static void download_url(const char *url) {
    char name[96], path[256];
    download_name(url, name, sizeof(name));
    long fd = download_create(name, path, sizeof(path));
    if (fd < 0) {
        download_finished(name, b.downloads_dir, 0, false);
        return;
    }
    struct running *r = NULL;
    for (int i = 0; i < MAX_RUNNING_DOWNLOADS && r == NULL; i++) {
        if (!running[i].used) {
            r = &running[i];
        }
    }
    int ends[2];
    if (r == NULL
        || opos_syscall2(293 /* pipe2 */, (long)ends,
                         LINUX_O_NONBLOCK | LINUX_O_CLOEXEC) != 0) {
        download_here(url, fd, name, path);
        return;
    }
    long pid = opos_syscall0(57 /* fork */);
    if (pid == 0) {
        opos_close(ends[0]);
        for (int i = 0; i < MAX_RUNNING_DOWNLOADS; i++) {
            if (running[i].used) {
                opos_close(running[i].pipe);
            }
        }
        child_pipe = ends[1];
        child_download(url, (int)fd, name);
    }
    opos_close(ends[1]);
    if (pid < 0) {
        opos_close(ends[0]);
        download_here(url, fd, name, path);
        return;
    }
    opos_close((int)fd);
    memset(r, 0, sizeof(*r));
    r->used = true;
    r->pid = pid;
    r->pipe = ends[0];
    r->id = download_start(name, path);
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->path, sizeof(r->path), "%s", path);
    char text[160];
    snprintf(text, sizeof(text), "Downloading %s (Ctrl+J shows downloads)",
             name);
    set_status(text);
    downloads_page_stale = true;
}

static bool downloads_running(void) {
    for (int i = 0; i < MAX_RUNNING_DOWNLOADS; i++) {
        if (running[i].used) {
            return true;
        }
    }
    return false;
}

void download_cancel(int id) {
    for (int i = 0; i < MAX_RUNNING_DOWNLOADS; i++) {
        if (running[i].used && running[i].id == id && !running[i].reported) {
            running[i].cancelled = true;
            opos_syscall2(62 /* kill */, running[i].pid, LINUX_SIGKILL);
        }
    }
}

static void running_line(struct running *r) {
    char *end;
    unsigned long long one = strtoul(r->line + 2, &end, 10), two = 0;
    if (*end == ' ') {
        two = strtoul(end + 1, NULL, 10);
    }
    if (r->line[0] == 'P') {
        download_update(r->id, one, two);
    } else if (r->line[0] == 'D') {
        r->reported = true;
        r->ok = one == 1;
        r->size = two;
    }
}

/* Reads what the download children reported and collects the ones that
   ended. */
static void downloads_poll(void) {
    for (int i = 0; i < MAX_RUNNING_DOWNLOADS; i++) {
        struct running *r = &running[i];
        if (!r->used) {
            continue;
        }
        bool ended = false;
        for (;;) {
            char data[256];
            long got = opos_read(r->pipe, data, sizeof(data));
            if (got == -LINUX_EAGAIN) {
                break;
            }
            if (got <= 0) {
                ended = true; /* the child closed its end or died */
                break;
            }
            for (long k = 0; k < got; k++) {
                if (data[k] == '\n') {
                    r->line[r->line_length] = '\0';
                    running_line(r);
                    r->line_length = 0;
                } else if (r->line_length + 1 < sizeof(r->line)) {
                    r->line[r->line_length++] = data[k];
                }
            }
            downloads_page_stale = true;
        }
        if (!ended) {
            continue;
        }
        int status = 0;
        if (opos_syscall4(61 /* wait4 */, r->pid, (long)&status, 1, 0) == 0) {
            continue; /* not gone yet: look again on the next pass */
        }
        opos_close(r->pipe);
        bool ok = r->reported && r->ok && !r->cancelled;
        if (!ok) {
            opos_unlink(r->path);
        }
        download_end(r->id, ok ? r->size : 0, !ok, r->cancelled);
        char text[160];
        if (ok) {
            snprintf(text, sizeof(text), "Saved %s to Downloads (Ctrl+J)",
                     r->name);
        } else if (r->cancelled) {
            snprintf(text, sizeof(text), "Download of %s cancelled", r->name);
        } else {
            snprintf(text, sizeof(text), "Download of %s failed", r->name);
        }
        set_status(text);
        r->used = false;
        downloads_page_stale = true;
    }
}

/* --------------------------------------------------------------- loading */

enum load_result { LOAD_SHOWN, LOAD_SAVED };

/* Whether two URLs name the same document (they differ at most in the
   #fragment). */
static bool same_document(const char *one, const char *other) {
    size_t length = strcspn(one, "#");
    return length == strcspn(other, "#") && strncmp(one, other, length) == 0;
}

static void show_about(const char *url) {
    size_t length = 0;
    char *html = about_page(url, &length);
    /* Actions (?remove=, ?clear) happen once: reload shows the plain
       page. */
    char base[64];
    snprintf(base, sizeof(base), "%.*s", (int)strcspn(url, "?#"), url);
    if (html != NULL) {
        install_document(html, length, "utf-8", base);
        free(html);
    }
    snprintf(T->url, sizeof(T->url), "%s", base);
    set_address(strcmp(base, NEW_TAB_URL) == 0 ? "" : base);
    set_status("");
}

/* Shows new download progress when about:downloads is on screen, at most
   once a second. */
static void refresh_downloads_page(void) {
    static long long last;
    if (!downloads_page_stale || strcmp(T->url, "about:downloads") != 0
        || T->editing) {
        downloads_page_stale = false;
        return;
    }
    long long t = uptime_ms();
    if (t - last < 1000 && downloads_running()) {
        return;
    }
    last = t;
    downloads_page_stale = false;
    int scroll = T->scroll;
    char status[sizeof(b.status)];
    snprintf(status, sizeof(status), "%s", b.status);
    show_about("about:downloads");
    set_status(status);
    T->scroll = scroll;
    clamp_scroll();
    b.dirty = true;
}

/* After a download: keep the page, or show the downloads in an empty
   tab. */
static enum load_result saved(void) {
    if (!T->have_page) {
        show_about("about:downloads");
        return LOAD_SHOWN;
    }
    set_address(T->url);
    return LOAD_SAVED;
}

/* Fetches and shows a URL (reload: even when only the #fragment would
   change). Files that cannot be shown are saved to Downloads, keeping
   the page. */
static enum load_result load_request(const char *url, const char *post,
                                     size_t post_length, bool reload) {
    char text[OPOS_HTTP_URL_MAX + 16];
    if (strncmp(url, "about:", 6) == 0) {
        show_about(url);
        return LOAD_SHOWN;
    }
    if (!reload && post == NULL && T->have_page && strchr(url, '#') != NULL
        && same_document(url, T->url)) {
        snprintf(T->url, sizeof(T->url), "%s", url);
        set_address(url);
        scroll_to_fragment(url);
        return LOAD_SHOWN;
    }
    /* The #fragment is for the browser, not the server. */
    char fetch[OPOS_HTTP_URL_MAX];
    size_t fetch_length = strcspn(url, "#");
    snprintf(fetch, sizeof(fetch), "%.*s", (int)fetch_length, url);
    const char *fragment = url + fetch_length;
    set_address(url);
    snprintf(text, sizeof(text), "Loading %s", url);
    set_status(text);
    paint();

    struct opos_http_response response;
    opos_http_set_progress(progress, NULL);
    cookies_attach(true);
    long error = post != NULL
        ? opos_http_post(fetch, "application/x-www-form-urlencoded", post,
                         post_length, MAX_PAGE_BYTES, &response)
        : opos_http_get(fetch, MAX_PAGE_BYTES, &response);
    cookies_detach();
    opos_http_set_progress(NULL, NULL);
    if (error == -OPOS_EFBIG && post == NULL) {
        /* Too big for a page: almost surely a file to keep. */
        download_url(fetch);
        return saved();
    }
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
        snprintf(T->url, sizeof(T->url), "%s", url);
        set_status("Failed");
        return LOAD_SHOWN;
    }
    /* Where redirects ended, with the fragment asked for. */
    char final[OPOS_HTTP_URL_MAX];
    snprintf(final, sizeof(final), "%s%s", response.url,
             strchr(response.url, '#') == NULL ? fragment : "");
    char charset_buffer[40];
    const char *charset = charset_of(response.content_type, charset_buffer,
                                     sizeof(charset_buffer));
    const char *type = response.content_type;
    bool html = starts_with_ci(type, "text/html")
             || starts_with_ci(type, "application/xhtml")
             || type[0] == '\0';
    enum image_format format = image_sniff(response.body, response.length);
    bool svg = starts_with_ci(type, "image/svg")
            || (!html && image_is_svg(response.body, response.length));
    if (svg) {
        html = false;
    }
    bool shows_text = starts_with_ci(type, "text/")
                   || starts_with_ci(type, "application/json");
    if (format == IMAGE_FORMAT_UNKNOWN && !svg && !html && !shows_text
        && response.status < 400) {
        save_download(response.url, response.body, response.length);
        opos_http_free(&response);
        return saved();
    }
    if (format != IMAGE_FORMAT_UNKNOWN || svg) {
        char markup[OPOS_HTTP_URL_MAX + 64];
        snprintf(markup, sizeof(markup), "<img src=\"%s\">", response.url);
        install_document(markup, strlen(markup), "utf-8", final);
    } else if (html || starts_with_ci(type, "text/xml")) {
        install_document((const char *)response.body, response.length,
                         charset, final);
    } else if (shows_text) {
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
            install_document(markup, used, charset, final);
            free(markup);
        }
    } else {
        show_message(final, "Cannot show this file",
                     "This type of file is not supported yet.");
    }
    snprintf(T->url, sizeof(T->url), "%s", final);
    set_address(final);
    if (response.status >= 400) {
        snprintf(text, sizeof(text), "HTTP %ld", response.status);
    } else {
        snprintf(text, sizeof(text), "Done");
        if (post == NULL) {
            history_add(T->url, T->title);
        }
    }
    set_status(text);
    opos_http_free(&response);
    return LOAD_SHOWN;
}

/* Loads url in the current tab; with record, it becomes the newest entry
   of the tab's back/forward list. */
/* Makes T->url the newest entry of the tab's back/forward list. */
static void history_record(void) {
    if (T->history_position + 1 < T->history_count) {
        T->history_count = T->history_position + 1;
    }
    if (T->history_count == HISTORY_MAX) {
        memmove(T->history[0], T->history[1],
                (HISTORY_MAX - 1) * sizeof(T->history[0]));
        T->history_count--;
    }
    snprintf(T->history[T->history_count], sizeof(T->history[0]), "%s",
             T->url);
    T->history_position = T->history_count++;
}

static void navigate_request(const char *url, bool record, const char *post,
                             size_t post_length) {
    T->editing = false;
    if (load_request(url, post, post_length, false) == LOAD_SHOWN && record) {
        history_record(); /* where redirects ended up */
    }
    b.dirty = true;
}

static void load(const char *url) { load_request(url, NULL, 0, false); }

static void reload(void) {
    if (T->url[0] != '\0') {
        char url[OPOS_HTTP_URL_MAX];
        snprintf(url, sizeof(url), "%s", T->url);
        int keep = T->scroll;
        load_request(url, NULL, 0, true);
        T->scroll = keep;
        clamp_scroll();
    }
}

static void navigate(const char *url, bool record) {
    navigate_request(url, record, NULL, 0);
}

/* ----------------------------------------------------------------- forms */

/* Lays the page out again after a form control changed, keeping the
   scroll position. */
static void refresh_controls(void) {
    int keep = T->scroll;
    relayout();
    T->scroll = keep;
    clamp_scroll();
    b.dirty = true;
}

/* ------------------------------------------------------------ scripts */

static void go_history(int step);

/* Applies what the shown page's scripts asked for: a new title, a
   navigation, a new address, a changed document. */
static void scripts_apply(void) {
    if (canvas_take_painted()) {
        b.dirty = true; /* a canvas changed: paint again */
    }
    if (!T->have_page || T->page.script == NULL) {
        return;
    }
    if (script_take_title(&T->page)) {
        set_title(T->page.doc->title);
        b.dirty = true;
    }
    char url[OPOS_HTTP_URL_MAX];
    bool replace;
    if (script_take_navigation(&T->page, url, sizeof(url), &replace)) {
        navigate(url, !replace);
        return;
    }
    int step = script_take_history_step(&T->page);
    if (step != 0) {
        go_history(step);
        return;
    }
    if (script_take_url(&T->page, url, sizeof(url), &replace)) {
        /* history.pushState: a new address without loading it. Going
           back to it later loads it. */
        snprintf(T->url, sizeof(T->url), "%s", url);
        if (!T->editing) {
            set_address(url);
        }
        if (replace && T->history_position >= 0) {
            snprintf(T->history[T->history_position],
                     sizeof(T->history[0]), "%s", url);
        } else {
            history_record();
        }
        b.dirty = true;
    }
    if (script_take_dirty(&T->page)) {
        refresh_controls();
    }
}

/* Runs due timers in every tab; returns milliseconds until the next one
   (-1: none). Tabs in the background run theirs at most once a second
   and are laid out again when shown. */
static long scripts_tick(void) {
    static long long background_due;
    long long now = uptime_ms();
    bool background = now >= background_due;
    if (background) {
        background_due = now + 1000;
    }
    long next = -1;
    for (int i = 0; i < b.tab_count; i++) {
        struct tab *tab = b.tabs[i];
        if (!tab->have_page || tab->page.script == NULL
            || (tab != T && !background)) {
            continue;
        }
        long wait = script_run_timers(&tab->page);
        if (tab != T) {
            script_take_dirty(&tab->page); /* tab_switch lays it out */
            wait = wait < 0 ? -1 : 1000;
        }
        if (wait >= 0 && (next < 0 || wait < next)) {
            next = wait;
        }
    }
    scripts_apply();
    /* The shown page hears when it scrolled or the window resized. */
    if (T->have_page && T->page.script != NULL) {
        if (T->script_width != b.width) {
            bool first = T->script_width == 0;
            T->script_width = b.width;
            if (!first) {
                script_window_event(&T->page, "resize");
            }
        }
        if (T->script_scroll != T->scroll) {
            T->script_scroll = T->scroll;
            script_window_event(&T->page, "scroll");
        }
        scripts_apply();
    }
    return next;
}

/* --------------------------------------------- layout asked by scripts */

struct opos_font *browser_font(int index) {
    return index >= 0 && index < FONT_COUNT ? b.fonts[index] : NULL;
}

static struct tab *tab_of(const struct page *page) {
    for (int i = 0; i < b.tab_count; i++) {
        if (&b.tabs[i]->page == page) {
            return b.tabs[i];
        }
    }
    return NULL;
}

void browser_layout_now(struct page *page) {
    struct tab *tab = tab_of(page);
    if (tab == NULL || !tab->have_page) {
        return;
    }
    struct tab *shown = T;
    b.tab = tab;
    int keep = T->scroll;
    relayout();
    T->scroll = keep;
    clamp_scroll();
    b.tab = shown;
    b.dirty = true;
}

bool browser_element_rect(struct page *page, const struct opos_html_node *node,
                          int rect[4]) {
    struct tab *tab = tab_of(page);
    bool fixed;
    if (tab == NULL || !page_element_box(page, node, rect, &fixed)) {
        return false;
    }
    rect[0] += MARGIN;
    rect[1] += MARGIN - (fixed ? 0 : tab->scroll);
    return true;
}

void browser_page_metrics(struct page *page, int *scroll_x, int *scroll_y,
                          int *width, int *height) {
    struct tab *tab = tab_of(page);
    *scroll_x = 0;
    *scroll_y = tab != NULL ? tab->scroll : 0;
    *width = page->content_width + 2 * MARGIN;
    *height = page->content_height + 2 * MARGIN;
}

void browser_scroll_to(struct page *page, int y) {
    struct tab *tab = tab_of(page);
    if (tab == NULL) {
        return;
    }
    struct tab *shown = T;
    b.tab = tab;
    T->scroll = y;
    T->follow_anchor = false;
    clamp_scroll();
    /* The page asked for it: no scroll event back for this one. */
    T->script_scroll = T->scroll;
    b.tab = shown;
    b.dirty = true;
}

/* A click at a page element: true when a script cancelled it. (x, y) are
   window coordinates. */
static bool script_click(const struct opos_html_node *target, int x, int y) {
    if (!T->have_page || T->page.script == NULL) {
        return false;
    }
    if (target == NULL) {
        target = T->page.doc->body;
    }
    return script_event(&T->page, target, "click", x - MARGIN,
                        y - CHROME_HEIGHT, 0);
}

/* "input" and "change" after the user changed a control. */
static void script_changed(const struct opos_html_node *node, bool input,
                           bool change) {
    if (T->page.script == NULL || node == NULL) {
        return;
    }
    if (input) {
        script_event(&T->page, node, "input", 0, 0, 0);
    }
    if (change) {
        script_event(&T->page, node, "change", 0, 0, 0);
    }
}

/* The DOM's KeyboardEvent key and keyCode for an OPOS key. */
static int key_name(uint32_t key, char *out, size_t capacity) {
    static const struct {
        uint32_t key;
        const char *name;
        int code;
    } named[] = {
        {'\n', "Enter", 13}, {'\b', "Backspace", 8}, {'\t', "Tab", 9},
        {OPOS_KEY_ESCAPE, "Escape", 27}, {OPOS_KEY_UP, "ArrowUp", 38},
        {OPOS_KEY_DOWN, "ArrowDown", 40}, {OPOS_KEY_LEFT, "ArrowLeft", 37},
        {OPOS_KEY_RIGHT, "ArrowRight", 39}, {OPOS_KEY_DELETE, "Delete", 46},
        {OPOS_KEY_HOME, "Home", 36}, {OPOS_KEY_END, "End", 35},
        {OPOS_KEY_PAGE_UP, "PageUp", 33},
        {OPOS_KEY_PAGE_DOWN, "PageDown", 34},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (named[i].key == key) {
            snprintf(out, capacity, "%s", named[i].name);
            return named[i].code;
        }
    }
    char utf8[5] = {0};
    if (key < 0x80) {
        utf8[0] = (char)key;
    } else if (key < 0x800) {
        utf8[0] = (char)(0xc0 | key >> 6);
        utf8[1] = (char)(0x80 | (key & 0x3f));
    } else if (key < 0x10000) {
        utf8[0] = (char)(0xe0 | key >> 12);
        utf8[1] = (char)(0x80 | ((key >> 6) & 0x3f));
        utf8[2] = (char)(0x80 | (key & 0x3f));
    } else {
        utf8[0] = (char)(0xf0 | key >> 18);
        utf8[1] = (char)(0x80 | ((key >> 12) & 0x3f));
        utf8[2] = (char)(0x80 | ((key >> 6) & 0x3f));
        utf8[3] = (char)(0x80 | (key & 0x3f));
    }
    snprintf(out, capacity, "%s", utf8);
    if (key >= 'a' && key <= 'z') {
        return (int)key - 32;
    }
    return key == ' ' ? 32 : key < 0x80 ? (int)key : 0;
}

static void set_focus(const struct opos_html_node *node) {
    if (T->have_page && T->page.focus != node) {
        T->page.focus = node;
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
    if (T->page.script != NULL
        && script_event(&T->page, form, "submit", 0, 0, 0)) {
        return; /* the page's script handles it */
    }
    size_t length = 0;
    char *data = form_encode(&T->page, form, submitter, &length);
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
        snprintf(url, sizeof(url), "%s", T->page.url);
    } else if (!opos_url_resolve(T->page.url, action, url, sizeof(url))) {
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
    struct form_field *field = form_field(&T->page, node);
    if (field == NULL) {
        return;
    }
    if (field->kind == FIELD_CHECKBOX || field->kind == FIELD_RADIO) {
        /* The box changes first; a cancelled click puts it back. */
        bool was = field->checked;
        if (field->kind == FIELD_CHECKBOX) {
            field->checked = !field->checked;
        } else {
            form_check_radio(&T->page, node);
        }
        bool cancelled = script_click(node, b.press_x, b.press_y);
        field = form_field(&T->page, node); /* a layout may move it */
        if (field == NULL) {
            return;
        }
        if (cancelled) {
            field->checked = was;
        } else {
            script_changed(node, true, true);
        }
        T->page.focus = node;
        refresh_controls();
        return;
    }
    if (script_click(node, b.press_x, b.press_y)) {
        refresh_controls();
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
        form_check_radio(&T->page, node);
        break;
    case FIELD_SELECT: {
        T->dropdown = form_option_count(node) > 0 ? node : NULL;
        b.dirty = true;
        break;
    }
    case FIELD_SUBMIT:
        T->page.focus = NULL;
        submit_form(node, node);
        return;
    default:
        set_focus(node);
        return;
    }
    T->page.focus = node;
    refresh_controls();
}

/* Moves the focus to the next (step 1) or previous (-1) control shown on
   the page, in layout order. */
static void focus_step(int step) {
    const struct opos_html_node *order[512];
    size_t count = 0;
    for (size_t i = 0; i < T->page.item_count && count < 512; i++) {
        const struct opos_html_node *node = T->page.items[i].control;
        if (node != NULL && opos_html_attribute(node, "disabled") == NULL) {
            order[count++] = node;
        }
    }
    if (count == 0) {
        return;
    }
    size_t at = count;
    for (size_t i = 0; i < count; i++) {
        if (order[i] == T->page.focus) {
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
    for (size_t i = 0; i < T->page.item_count; i++) {
        const struct display_item *item = &T->page.items[i];
        if (item->control == order[next]) {
            int top = item->y + MARGIN;
            if (top < T->scroll
                || top + item->height > T->scroll + viewport_height()) {
                T->scroll = top - viewport_height() / 3;
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
    const struct opos_html_node *node = T->page.focus;
    struct form_field *field = form_field(&T->page, node);
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
        if (T->dropdown != NULL && (key == '\n' || key == ' '
                                   || key == OPOS_KEY_ESCAPE)) {
            T->dropdown = NULL;
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
    int target = T->history_position + step;
    if (target < 0 || target >= T->history_count) {
        return;
    }
    T->history_position = target;
    char url[OPOS_HTTP_URL_MAX];
    snprintf(url, sizeof(url), "%s", T->history[target]);
    load(url);
}

static void submit_address(void) {
    char url[OPOS_HTTP_URL_MAX];
    const char *text = T->address;
    while (*text == ' ') {
        text++;
    }
    if (*text == '\0') {
        return;
    }
    size_t length = strlen(text);
    while (length > 0 && text[length - 1] == ' ') {
        length--;
    }
    bool looks_like_address = memchr(text, ' ', length) == NULL
        && (memchr(text, '.', length) != NULL
            || starts_with_ci(text, "localhost"));
    if (starts_with_ci(text, "http://") || starts_with_ci(text, "https://")
        || starts_with_ci(text, "about:")) {
        snprintf(url, sizeof(url), "%.*s", (int)length, text);
    } else if (looks_like_address) {
        snprintf(url, sizeof(url), "https://%.*s", (int)length, text);
    } else {
        /* Words: search them (DuckDuckGo's page without JavaScript). */
        size_t used = (size_t)snprintf(url, sizeof(url),
                                       "https://lite.duckduckgo.com/lite/?q=");
        static const char hex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < length && used + 4 < sizeof(url); i++) {
            unsigned char c = (unsigned char)text[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '-' || c == '_'
                || c == '.') {
                url[used++] = (char)c;
            } else if (c == ' ') {
                url[used++] = '+';
            } else {
                url[used++] = '%';
                url[used++] = hex[c >> 4];
                url[used++] = hex[c & 15];
            }
        }
        url[used] = '\0';
    }
    T->editing = false;
    navigate(url, true);
}

/* ------------------------------------------------------------------ tabs */

static struct tab *tab_create(void) {
    struct tab *tab = calloc(1, sizeof(*tab));
    if (tab != NULL) {
        tab->history_position = -1;
    }
    return tab;
}

static void tab_destroy(struct tab *tab) {
    if (tab->have_page) {
        page_destroy(&tab->page);
    }
    free(tab->text);
    free(tab->item_start);
    free(tab);
}

/* Shows tab index: lays its page out for the window's current size. */
static void tab_switch(int index) {
    if (index < 0 || index >= b.tab_count) {
        return;
    }
    b.current = index;
    T = b.tabs[index];
    b.selecting = b.field_selecting = b.dragging_scrollbar = false;
    b.field_selection = NULL;
    b.menu_count = 0;
    int keep = T->scroll;
    relayout();
    T->scroll = keep;
    clamp_scroll();
    update_caption();
    set_status("");
    b.dirty = true;
}

/* Opens a tab after the current one with url (NULL: the new-tab page,
   address bar ready for typing); show switches to it. */
static void tab_open(const char *url, bool show) {
    if (b.tab_count == MAX_TABS) {
        set_status("Too many tabs: close one first (Ctrl+W)");
        return;
    }
    struct tab *tab = tab_create();
    if (tab == NULL) {
        set_status("Out of memory");
        return;
    }
    int index = b.tab_count == 0 ? 0 : b.current + 1;
    memmove(b.tabs + index + 1, b.tabs + index,
            (size_t)(b.tab_count - index) * sizeof(b.tabs[0]));
    b.tabs[index] = tab;
    b.tab_count++;
    int back = b.current;
    tab_switch(index);
    navigate(url != NULL ? url : NEW_TAB_URL, true);
    if (url == NULL) {
        T->editing = true;
        T->address_selected = true;
    }
    if (!show) {
        tab_switch(back);
    }
    b.dirty = true;
}

/* Closes a tab; closing the last one ends the browser. Returns false
   then. */
static bool tab_close(int index) {
    if (index < 0 || index >= b.tab_count) {
        return true;
    }
    if (b.tab_count == 1) {
        return false;
    }
    /* Leave T on a live tab while this one goes away. */
    int show = b.current;
    if (index == b.current) {
        show = index + 1 < b.tab_count ? index + 1 : index - 1;
    }
    T = b.tabs[show];
    tab_destroy(b.tabs[index]);
    memmove(b.tabs + index, b.tabs + index + 1,
            (size_t)(b.tab_count - index - 1) * sizeof(b.tabs[0]));
    b.tab_count--;
    tab_switch(show > index ? show - 1 : show);
    return true;
}

static void toggle_bookmark(const char *url, const char *title) {
    if (url[0] == '\0' || strncmp(url, "about:", 6) == 0) {
        return;
    }
    set_status(bookmark_toggle(url, title) ? "Bookmarked"
                                           : "Bookmark removed");
}

/* Fetches one pending image and lays the page out again. Returns false when
   there is nothing left to load. */
static bool load_next_image(void) {
    if (!T->have_page) {
        return false;
    }
    for (size_t i = 0; i < T->page.image_count; i++) {
        struct image_ref *ref = T->page.images[i];
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
            if (response.status == 200) {
                image_ref_decode(ref, response.body, response.length);
            }
            opos_http_free(&response);
        }
        int keep = T->scroll;
        relayout();
        T->scroll = keep;
        clamp_scroll();
        follow_anchor();
        set_status("Done");
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- events */

static void scroll_by(int delta) {
    T->scroll += delta;
    T->follow_anchor = false;
    clamp_scroll();
    b.dirty = true;
}

static bool link_target(const struct display_item *item, char *out,
                        size_t capacity) {
    if (item == NULL || item->link == NULL) {
        return false;
    }
    const char *href = opos_html_attribute(item->link, "href");
    if (href == NULL || href[0] == '\0'
        || starts_with_ci(href, "javascript:")
        || starts_with_ci(href, "mailto:")) {
        return false;
    }
    while (*href == ' ' || *href == '\n' || *href == '\t') {
        href++;
    }
    if (starts_with_ci(href, "about:") || starts_with_ci(href, "http://")
        || starts_with_ci(href, "https://")) {
        snprintf(out, capacity, "%s", href);
        return strlen(href) < capacity;
    }
    if (href[0] == '#') {
        snprintf(out, capacity, "%.*s%s", (int)strcspn(T->page.url, "#"),
                 T->page.url, href);
        return true;
    }
    return opos_url_resolve(T->page.url, href, out, capacity);
}

/* The link under the window position (x, y), if any. */
static bool link_at(int x, int y, char *out, size_t capacity) {
    if (!T->have_page || y < CHROME_HEIGHT || x >= viewport_width()
        || y >= CHROME_HEIGHT + viewport_height()) {
        return false;
    }
    const struct display_item *item = page_item_at(
        &T->page, x - MARGIN, y - CHROME_HEIGHT - MARGIN + T->scroll,
        T->scroll);
    return link_target(item, out, capacity);
}

/* The text of the link under (x, y), for its bookmark's title. */
static void link_text(int x, int y, char *out, size_t capacity) {
    out[0] = '\0';
    const struct display_item *item = page_item_at(
        &T->page, x - MARGIN, y - CHROME_HEIGHT - MARGIN + T->scroll,
        T->scroll);
    if (item == NULL || item->link == NULL) {
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < T->page.item_count && used + 2 < capacity; i++) {
        const struct display_item *part = &T->page.items[i];
        if (part->kind != ITEM_TEXT || part->link != item->link
            || part->length == 0) {
            continue;
        }
        if (used > 0) {
            out[used++] = ' ';
        }
        size_t n = part->length < capacity - used - 1 ? part->length
                                                      : capacity - used - 1;
        memcpy(out + used, part->text, n);
        used += n;
    }
    out[used] = '\0';
}

static void open_menu_at(int x, int y) {
    b.menu_count = 0;
    if (link_at(x, y, b.menu_link, sizeof(b.menu_link))) {
        menu_add(MENU_OPEN_NEW_TAB);
        menu_add(MENU_COPY_LINK);
        menu_add(MENU_BOOKMARK_LINK);
        menu_add(MENU_DOWNLOAD_LINK);
    } else {
        b.menu_link[0] = '\0';
        if (T->history_position > 0) {
            menu_add(MENU_BACK);
        }
        if (T->history_position + 1 < T->history_count) {
            menu_add(MENU_FORWARD);
        }
        menu_add(MENU_RELOAD);
        if (strncmp(T->url, "about:", 6) != 0) {
            menu_add(MENU_BOOKMARK_PAGE);
        }
        if (selection_end() > selection_start()) {
            menu_add(MENU_COPY);
        }
    }
    b.press_x = x;
    b.press_y = y;
    menu_place(x, y);
}

static void open_main_menu(void) {
    b.menu_count = 0;
    menu_add(MENU_NEW_TAB);
    menu_add(MENU_CLOSE_TAB);
    if (strncmp(T->url, "about:", 6) != 0 && T->have_page) {
        menu_add(MENU_BOOKMARK_PAGE);
    }
    menu_add(MENU_BOOKMARKS);
    menu_add(MENU_HISTORY);
    menu_add(MENU_DOWNLOADS);
    menu_add(MENU_COOKIES);
    menu_add(MENU_JAVASCRIPT);
    menu_place(menu_button_x() + 28 - MENU_WIDTH, BUTTON_Y + 30);
}

/* JavaScript is on unless the file "javascript-off" is in the browser's
   data folder. */
static void javascript_setting_path(char *out, size_t capacity) {
    snprintf(out, capacity, "%s/javascript-off", b.data_dir);
}

static void javascript_load_setting(void) {
    char path[200];
    javascript_setting_path(path, sizeof(path));
    long fd = b.data_dir[0] != '\0' ? opos_open(path, OPOS_O_RDONLY) : -1;
    scripting_enabled = fd < 0;
    if (fd >= 0) {
        opos_close((int)fd);
    }
}

static void javascript_toggle(void) {
    scripting_enabled = !scripting_enabled;
    if (b.data_dir[0] != '\0') {
        char path[200];
        javascript_setting_path(path, sizeof(path));
        if (scripting_enabled) {
            opos_unlink(path);
        } else {
            long fd = opos_open(path, OPOS_O_WRONLY | OPOS_O_CREAT
                                          | OPOS_O_TRUNC);
            if (fd >= 0) {
                opos_close((int)fd);
            }
        }
    }
    set_status(scripting_enabled ? "JavaScript is on" : "JavaScript is off");
    reload();
}

static bool quit; /* the last tab was closed */

static void menu_run(enum menu_action action) {
    char url[OPOS_HTTP_URL_MAX];
    snprintf(url, sizeof(url), "%s", b.menu_link);
    b.menu_count = 0;
    b.dirty = true;
    switch (action) {
    case MENU_OPEN_NEW_TAB: tab_open(url, false); break;
    case MENU_COPY_LINK: opos_clipboard_set(url, strlen(url)); break;
    case MENU_BOOKMARK_LINK: {
        char title[128];
        link_text(b.press_x, b.press_y, title, sizeof(title));
        toggle_bookmark(url, title);
        break;
    }
    case MENU_DOWNLOAD_LINK: download_url(url); break;
    case MENU_BACK: go_history(-1); break;
    case MENU_FORWARD: go_history(1); break;
    case MENU_RELOAD: reload(); break;
    case MENU_BOOKMARK_PAGE: toggle_bookmark(T->url, T->title); break;
    case MENU_COPY: copy_selection(); break;
    case MENU_NEW_TAB: tab_open(NULL, true); break;
    case MENU_CLOSE_TAB: quit = !tab_close(b.current); break;
    case MENU_BOOKMARKS: navigate("about:bookmarks", true); break;
    case MENU_HISTORY: navigate("about:history", true); break;
    case MENU_DOWNLOADS: navigate("about:downloads", true); break;
    case MENU_COOKIES: navigate("about:cookies", true); break;
    case MENU_JAVASCRIPT: javascript_toggle(); break;
    default: break;
    }
}

/* Maps a click in a rendered text control back to a byte offset in its
   value. For a password, each displayed star maps to one UTF-8 character. */
static size_t field_offset_at(const struct opos_html_node *node, int x, int y) {
    struct form_field *field = form_field(&T->page, node);
    if (!is_text_field(field)) {
        return 0;
    }
    const struct display_item *best = NULL;
    int best_distance = 0x7fffffff;
    for (size_t i = 0; i < T->page.item_count; i++) {
        const struct display_item *item = &T->page.items[i];
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
    if (b.menu_count > 0) {
        int row = menu_row_at(x, y);
        if (row >= 0) {
            menu_run(b.menu[row]);
        } else {
            b.menu_count = 0; /* a click elsewhere only closes it */
            b.dirty = true;
        }
        return;
    }
    if (T->dropdown != NULL) {
        dropdown_click(x, y);
        return;
    }
    if (y < TABBAR_HEIGHT) {
        bool close;
        int index = tab_at(x, &close);
        if (index == b.tab_count) {
            tab_open(NULL, true);
        } else if (index >= 0 && close) {
            quit = !tab_close(index);
        } else if (index >= 0) {
            tab_switch(index);
        }
        b.dirty = true;
        return;
    }
    if (y < CHROME_HEIGHT) {
        T->editing = false;
        if (x >= 6 && x < 34) {
            go_history(-1);
        } else if (x >= 38 && x < 66) {
            go_history(1);
        } else if (x >= 70 && x < 98) {
            reload();
        } else if (x >= star_x() && x < star_x() + 28) {
            toggle_bookmark(T->url, T->title);
        } else if (x >= menu_button_x() && x < menu_button_x() + 28) {
            open_main_menu();
        } else if (x >= ADDRESS_X && x < ADDRESS_X + address_width()) {
            T->editing = true;
            T->address_selected = true;
        }
        b.dirty = true;
        return;
    }
    T->editing = false;
    b.dirty = true;
    if (x >= viewport_width()) {
        T->follow_anchor = false;
        b.dragging_scrollbar = true;
        int range = viewport_height();
        T->scroll = (int)((long long)max_scroll() * (y - CHROME_HEIGHT) / (range > 0 ? range : 1));
        clamp_scroll();
        return;
    }
    if (!T->have_page || y >= CHROME_HEIGHT + viewport_height()) {
        return;
    }
    const struct opos_html_node *control = page_control_at(
        &T->page, x - MARGIN, y - CHROME_HEIGHT - MARGIN + T->scroll,
        T->scroll);
    b.press_x = x;
    b.press_y = y;
    if (control != NULL) {
        struct form_field *field = form_field(&T->page, control);
        bool select_field = is_text_field(field)
            && opos_html_attribute(control, "readonly") == NULL
            && opos_html_attribute(control, "disabled") == NULL;
        if (select_field) {
            clear_selection();
            T->have_match = false;
            size_t offset = field_offset_at(
                control, x - MARGIN,
                y - CHROME_HEIGHT - MARGIN + T->scroll);
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
    T->have_match = false;
    T->selection_anchor = T->selection_cursor = text_offset_at(
        x - MARGIN, y - CHROME_HEIGHT - MARGIN + T->scroll);
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
    T->selection_anchor = T->selection_cursor;
    b.dirty = true;
    const struct display_item *item = page_item_at(
        &T->page, b.press_x - MARGIN,
        b.press_y - CHROME_HEIGHT - MARGIN + T->scroll, T->scroll);
    const struct opos_html_node *link = item != NULL ? item->link : NULL;
    char url[OPOS_HTTP_URL_MAX];
    bool has_url = link_target(item, url, sizeof(url));
    if (script_click(item != NULL ? item->element : NULL, b.press_x,
                     b.press_y)) {
        return; /* a script handled the click */
    }
    const char *href = link != NULL ? opos_html_attribute(link, "href")
                                    : NULL;
    while (href != NULL && (*href == ' ' || *href == '\n' || *href == '\t')) {
        href++;
    }
    if (href != NULL && starts_with_ci(href, "javascript:")) {
        if (!script_runs_javascript_url(&T->page, href)) {
            set_status("This link needs JavaScript");
        }
        return;
    }
    if (has_url) {
        const char *target = opos_html_attribute(link, "target");
        if (opos_html_attribute(link, "download") != NULL
            && strncmp(url, "about:", 6) != 0) {
            download_url(url);
        } else if (target != NULL && strcmp(target, "_blank") == 0) {
            tab_open(url, true);
        } else {
            navigate(url, true);
        }
    }
}

static void on_move(int x, int y) {
    char url[OPOS_HTTP_URL_MAX];
    if (b.menu_count > 0) {
        int row = menu_row_at(x, y);
        if (row != b.menu_hover) {
            b.menu_hover = row;
            b.dirty = true;
        }
        return;
    }
    if (b.field_selecting && b.field_selection != NULL) {
        struct form_field *field = form_field(&T->page, b.field_selection);
        if (field != NULL) {
            field->selection_cursor = field_offset_at(
                b.field_selection, x - MARGIN,
                y - CHROME_HEIGHT - MARGIN + T->scroll);
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
        if (y < CHROME_HEIGHT) {
            scroll_by(-24);
        } else if (y >= CHROME_HEIGHT + viewport_height()) {
            scroll_by(24);
        }
        T->selection_cursor = text_offset_at(
            x - MARGIN, y - CHROME_HEIGHT - MARGIN + T->scroll);
        b.dirty = true;
        return;
    }
    if (b.dragging_scrollbar) {
        int range = viewport_height();
        T->scroll = (int)((long long)max_scroll() * (y - CHROME_HEIGHT) / (range > 0 ? range : 1));
        clamp_scroll();
        b.dirty = true;
        return;
    }
    if (T->have_page && y >= CHROME_HEIGHT && x < viewport_width()
        && y < CHROME_HEIGHT + viewport_height()) {
        const struct display_item *item = page_item_at(
            &T->page, x - MARGIN, y - CHROME_HEIGHT - MARGIN + T->scroll,
        T->scroll);
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
    bool shift = (modifiers & OPOS_MOD_SHIFT) != 0;
    if (b.menu_count > 0) {
        /* Arrows and Enter work the open menu; other keys close it. */
        if (key == OPOS_KEY_UP || key == OPOS_KEY_DOWN) {
            int step = key == OPOS_KEY_UP ? -1 : 1;
            b.menu_hover = b.menu_hover < 0
                ? (step > 0 ? 0 : b.menu_count - 1)
                : (b.menu_hover + step + b.menu_count) % b.menu_count;
        } else if (key == '\n' && b.menu_hover >= 0) {
            menu_run(b.menu[b.menu_hover]);
        } else {
            b.menu_count = 0;
        }
        b.dirty = true;
        return;
    }
    if (ctrl) {
        char lower = key < 128 ? (char)(key | 0x20) : 0;
        if (lower == 't') {
            tab_open(NULL, true);
            return;
        }
        if (lower == 'w') {
            quit = !tab_close(b.current);
            return;
        }
        if (key == '\t' || key == OPOS_KEY_PAGE_DOWN || key == OPOS_KEY_PAGE_UP) {
            int step = key == OPOS_KEY_PAGE_UP || (key == '\t' && shift) ? -1
                                                                          : 1;
            tab_switch((b.current + step + b.tab_count) % b.tab_count);
            return;
        }
        if (key >= '1' && key <= '9') {
            tab_switch(key == '9' ? b.tab_count - 1 : (int)(key - '1'));
            return;
        }
        if (lower == 'd') {
            toggle_bookmark(T->url, T->title);
            b.dirty = true;
            return;
        }
        if (lower == 'b' || lower == 'h' || lower == 'j') {
            navigate(lower == 'b' ? "about:bookmarks"
                     : lower == 'h' ? "about:history" : "about:downloads",
                     true);
            return;
        }
        if (lower == 'l') {
            T->editing = true;
            T->address_selected = true;
            b.dirty = true;
            return;
        }
        if (lower == 'r') {
            reload();
            b.dirty = true;
            return;
        }
    }
    if (ctrl && (key == 'f' || key == 'F')) {
        T->finding = true;
        T->editing = false;
        if (T->find_length > 0) {
            find_from(selection_start(), 1);
        }
        b.dirty = true;
        return;
    }
    if (T->have_page && T->page.script != NULL && !T->editing
        && !T->finding && !ctrl) {
        char name[16];
        int code = key_name(key, name, sizeof(name));
        int keys = shift ? 1 : 0;
        bool cancelled = script_key(&T->page, T->page.focus, "keydown",
                                    name, code, keys);
        if (key >= ' ' || key == '\n') {
            cancelled |= script_key(&T->page, T->page.focus, "keypress",
                                    name, code, keys);
        }
        if (cancelled) {
            script_key(&T->page, T->page.focus, "keyup", name, code, keys);
            b.dirty = true;
            return;
        }
    }
    if (T->have_page && T->page.focus != NULL && !T->editing && !T->finding
        && field_key(key, ctrl, (modifiers & OPOS_MOD_SHIFT) != 0)) {
        bool edit = key >= ' ' || key == '\b' || key == OPOS_KEY_DELETE
                 || (ctrl && (key == 'v' || key == 'V' || key == 'x'
                              || key == 'X'));
        if (edit && T->page.focus != NULL) {
            script_changed(T->page.focus, true, false);
        }
        if (!ctrl && T->page.script != NULL) {
            char name[16];
            int code = key_name(key, name, sizeof(name));
            script_key(&T->page, T->page.focus, "keyup", name, code,
                       shift ? 1 : 0);
        }
        return;
    }
    if (key == '\t' && T->have_page && !T->editing && !T->finding) {
        focus_step((modifiers & OPOS_MOD_SHIFT) != 0 ? -1 : 1);
        return;
    }
    if (ctrl && (key == 'c' || key == 'C') && !T->editing) {
        copy_selection();
        return;
    }
    if (ctrl && (key == 'a' || key == 'A') && !T->editing) {
        T->have_match = false;
        T->selection_anchor = 0;
        T->selection_cursor = T->text_length;
        b.dirty = true;
        return;
    }
    if (T->finding && !T->editing) {
        if (key == OPOS_KEY_ESCAPE) {
            T->finding = false;
            T->have_match = false;
        } else if (key == '\n') {
            size_t from = T->have_match ? T->match : selection_start();
            if (shift) {
                find_from(from == 0 ? T->text_length : from - 1, -1);
            } else {
                find_from(T->have_match ? from + 1 : from, 1);
            }
        } else if (key == '\b') {
            if (T->find_length > 0) {
                do {
                    T->find_length--;
                } while (T->find_length > 0
                         && (T->find[T->find_length] & 0xc0) == 0x80);
                T->find[T->find_length] = '\0';
                find_from(T->have_match ? T->match : 0, 1);
            }
        } else if (key >= 32 && key < 127 && !ctrl
                   && T->find_length + 1 < sizeof(T->find)) {
            T->find[T->find_length++] = (char)key;
            T->find[T->find_length] = '\0';
            /* Typing refines the search from the current match. */
            find_from(T->have_match ? T->match : selection_start(), 1);
        } else {
            goto scroll_keys;
        }
        b.dirty = true;
        return;
    }
    /* While the whole address is selected, typing, pasting or Backspace
       replaces it; other keys keep it and drop the selection. */
    if (T->editing && T->address_selected) {
        bool replaces = (ctrl && (key == 'v' || key == 'V')) || key == '\b'
                     || (key >= 32 && key < 127 && !ctrl);
        if (replaces) {
            T->address_length = 0;
            T->address[0] = '\0';
            if (key == '\b') {
                T->address_selected = false;
                b.dirty = true;
                return;
            }
        }
        if (key != '\n' && !(ctrl && (key == 'c' || key == 'C'))) {
            T->address_selected = false;
        }
    }
    if (T->editing && ctrl && (key == 'c' || key == 'C')) {
        opos_clipboard_set(T->address, T->address_length);
        return;
    }
    if (T->editing && ctrl && (key == 'v' || key == 'V')) {
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
            if (ch >= 32 && T->address_length + 1 < sizeof(T->address)) {
                T->address[T->address_length++] = (char)ch;
            }
        }
        T->address[T->address_length] = '\0';
        b.dirty = true;
        return;
    }
    if (T->editing) {
        if (key == '\n') {
            submit_address();
        } else if (key == OPOS_KEY_ESCAPE) {
            T->editing = false;
            set_address(T->url);
        } else if (key == '\b') {
            if (T->address_length > 0) {
                /* Remove a whole UTF-8 character. */
                do {
                    T->address_length--;
                } while (T->address_length > 0
                         && (T->address[T->address_length] & 0xc0) == 0x80);
                T->address[T->address_length] = '\0';
            }
        } else if (key >= 32 && key < 127 && !ctrl
                   && T->address_length + 1 < sizeof(T->address)) {
            T->address[T->address_length++] = (char)key;
            T->address[T->address_length] = '\0';
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
    case OPOS_KEY_HOME: scroll_by(-T->scroll); break;
    case OPOS_KEY_END: scroll_by(max_scroll()); break;
    case '\b': go_history(-1); break;
    default: break;
    }
}

static bool resize(int width, int height) {
    if (width < 200 || height < CHROME_HEIGHT + STATUS_HEIGHT + 60) {
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

/* JavaScript (React and the like) recurses deeply: a 4 MiB stack instead
   of the SDK's 128 KiB (crt0.S). */
__attribute__((used)) const unsigned long opos_stack_size = 4ul << 20;

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

    /* Bookmarks and history live in the account's AppData, downloads in
       its Downloads folder. */
    struct opos_user_info user;
    if (opos_user_info(&user) == 0 && (user.flags & OPOS_USER_ACCOUNT)
        && user.home[0] != '\0') {
        char app_data[160];
        snprintf(app_data, sizeof(app_data), "%s/AppData", user.home);
        opos_mkdir(app_data);
        snprintf(b.data_dir, sizeof(b.data_dir), "%s/Browser", app_data);
        opos_mkdir(b.data_dir);
        snprintf(b.downloads_dir, sizeof(b.downloads_dir), "%s/Downloads",
                 user.home);
    }
    library_open(b.data_dir);
    cookies_open(b.data_dir);
    script_set_data_dir(b.data_dir);
    javascript_load_setting();

    tab_open(argc > 1 ? argv[1] : NULL, true);
    if (T == NULL) {
        return 3;
    }

    struct opos_event event;
    while (!quit) {
        long timer_wait = scripts_tick();
        if (b.dirty) {
            paint();
        }
        long got = opos_window_event(&event, 0);
        if (got == 0) {
            if (downloads_running() || downloads_page_stale) {
                downloads_poll();
                refresh_downloads_page();
                if (b.dirty) {
                    continue; /* paint before waiting */
                }
            }
            if (load_next_image()) {
                continue;
            }
            /* While downloads run, wake up to show their progress; wake
               up for the next script timer too. */
            long wait = downloads_running() ? 250 : -1;
            if (timer_wait >= 0 && (wait < 0 || timer_wait < wait)) {
                wait = timer_wait < 4 ? 4 : timer_wait;
            }
            got = opos_window_event(&event, wait);
        }
        if (got <= 0) {
            if (got < 0) {
                break;
            }
            continue;
        }
        switch (event.type) {
        case OPOS_EVENT_CLOSE:
            /* Running downloads finish on their own and notify. */
            cookies_save();
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
            } else if (event.button == OPOS_BUTTON_RIGHT) {
                if (b.menu_count > 0 || T->dropdown != NULL) {
                    b.menu_count = 0;
                    b.dirty = true;
                } else if (event.y >= CHROME_HEIGHT
                           && event.x < viewport_width()) {
                    open_menu_at(event.x, event.y);
                }
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
            b.menu_count = 0;
            scroll_by(-OPOS_WHEEL_NOTCHES(event) * 48);
            break;
        default:
            break;
        }
    }
    return 0;
}
