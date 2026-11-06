/*
 * Browser layout: turns the HTML tree into a display list.
 *
 * Blocks stack vertically (with collapsing sibling margins), inline content
 * is broken into lines, lists get markers and tables are sized from the
 * minimum and maximum content width of their cells. Everything is placed in
 * absolute page coordinates, so painting is a walk over the list.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browser.h"

#define WIDE 100000 /* "unlimited" width when measuring max-content */
#define MAX_COLUMNS 48
#define MAX_CELLS 4000
#define MAX_DEPTH 200
#define MEASURE_SLOTS 8192

/* ----------------------------------------------------------------- arena */

struct arena_chunk {
    struct arena_chunk *next;
    size_t used;
    size_t size;
};

static void *arena_alloc(struct page *page, size_t size) {
    size = (size + 15) & ~(size_t)15;
    struct arena_chunk *chunk = page->arena;
    if (chunk == NULL || chunk->size - chunk->used < size) {
        size_t capacity = size > 32768 ? size : 32768;
        struct arena_chunk *fresh = malloc(sizeof(*fresh) + capacity);
        if (fresh == NULL) {
            return NULL;
        }
        fresh->next = page->arena;
        fresh->used = 0;
        fresh->size = capacity;
        page->arena = fresh;
        chunk = fresh;
    }
    void *result = (char *)(chunk + 1) + chunk->used;
    chunk->used += size;
    return result;
}

/* --------------------------------------------------------------- context */

enum inline_kind { IT_WORD, IT_SPACE, IT_BREAK, IT_IMAGE, IT_BOX };

enum box_kind {
    BOX_TEXT, BOX_BUTTON, BOX_CHECKBOX, BOX_RADIO, BOX_TEXTAREA, BOX_SELECT
};

struct inline_item {
    enum inline_kind kind;
    const char *text;
    size_t length;
    const struct style *style;
    int width;
    int height;
    struct image_ref *image;
    enum box_kind box;
    bool checked;
    bool placeholder;
    bool focused;
    const struct opos_html_node *node;
};

struct placed {
    const struct inline_item *item;
    const char *text;
    size_t length;
    int x;
    int width;
};

struct measure_entry {
    const void *node;
    int min;
    int max;
};

#define MAX_FLOATS 12

struct float_box {
    int side;              /* 1 left, 2 right */
    int x, width;          /* margin box */
    int top, bottom;
};

struct ctx {
    struct page *page;
    struct opos_font **fonts;
    int bx;                 /* left edge of the current block's content */
    int bwidth;             /* its width */
    int y;                  /* next free y */
    int pending_margin;
    enum align align;
    struct inline_item *inl;
    size_t inl_count;
    size_t inl_capacity;
    struct placed *placed;
    size_t placed_capacity;
    bool measuring;         /* min/max-content pass: nothing is kept */
    bool measure_min;
    int max_right;
    int depth;
    char marker[16];
    const struct style *marker_style;
    bool marker_pending;
    struct measure_entry *measures;
    bool failed;
    struct float_box floats[MAX_FLOATS];
    int float_count;
    int float_extent;
};

static int clamp_min(int value, int minimum) {
    return value < minimum ? minimum : value;
}

static int max_int(int a, int b) { return a > b ? a : b; }
static int min_int(int a, int b) { return a < b ? a : b; }

/* ------------------------------------------------------------- display list */

static struct display_item *add_item(struct ctx *c, enum item_kind kind,
                                     int x, int y, int width, int height) {
    struct page *page = c->page;
    if (page->item_count == page->item_capacity) {
        size_t capacity = page->item_capacity == 0 ? 512
                        : page->item_capacity * 2;
        struct display_item *grown = realloc(page->items,
                                             capacity * sizeof(*grown));
        if (grown == NULL) {
            c->failed = true;
            return NULL;
        }
        page->items = grown;
        page->item_capacity = capacity;
    }
    struct display_item *item = &page->items[page->item_count++];
    memset(item, 0, sizeof(*item));
    item->kind = kind;
    item->x = x;
    item->y = y;
    item->width = width;
    item->height = height;
    return item;
}

static void note_right(struct ctx *c, int right) {
    if (right > c->max_right) {
        c->max_right = right;
    }
}

/* ------------------------------------------------------------------ fonts */

static int font_index(const struct style *style) {
    if (style->mono) {
        return style->bold ? 5 : 4;
    }
    if (style->bold && style->italic) {
        return 3;
    }
    return style->bold ? 1 : style->italic ? 2 : 0;
}

static struct opos_font *font_for(struct ctx *c, const struct style *style) {
    return c->fonts[font_index(style)];
}

static int text_width(struct ctx *c, const struct style *style,
                      const char *text, size_t length) {
    return opos_font_text_width(font_for(c, style), style->font_size, text,
                                length);
}

static int space_width(struct ctx *c, const struct style *style) {
    int advance = opos_font_advance(font_for(c, style), style->font_size, ' ');
    return (advance + 32) >> 6;
}

/* Line box metrics of text in this style. */
static void line_metrics(struct ctx *c, const struct style *style,
                         int *ascent, int *descent) {
    struct opos_font *font = font_for(c, style);
    int a = opos_font_ascent(font, style->font_size);
    int d = opos_font_descent(font, style->font_size);
    int height = opos_font_line_height(font, style->font_size);
    if (height < a + d) {
        height = a + d;
    }
    int lead = (height - (a + d)) / 2;
    *ascent = a + lead;
    *descent = height - *ascent;
}

/* ---------------------------------------------------------- inline buffer */

static struct inline_item *inline_push(struct ctx *c) {
    if (c->inl_count == c->inl_capacity) {
        size_t capacity = c->inl_capacity == 0 ? 256 : c->inl_capacity * 2;
        struct inline_item *grown = realloc(c->inl, capacity * sizeof(*grown));
        if (grown == NULL) {
            c->failed = true;
            return NULL;
        }
        c->inl = grown;
        c->inl_capacity = capacity;
    }
    struct inline_item *item = &c->inl[c->inl_count++];
    memset(item, 0, sizeof(*item));
    return item;
}

static void add_word(struct ctx *c, const char *text, size_t length,
                     const struct style *style) {
    struct inline_item *item = inline_push(c);
    if (item == NULL) {
        return;
    }
    item->kind = IT_WORD;
    item->text = text;
    item->length = length;
    item->style = style;
    item->width = text_width(c, style, text, length);
}

static void add_space(struct ctx *c, const struct style *style) {
    if (c->inl_count > 0 && c->inl[c->inl_count - 1].kind == IT_SPACE) {
        return;
    }
    struct inline_item *item = inline_push(c);
    if (item == NULL) {
        return;
    }
    item->kind = IT_SPACE;
    item->style = style;
    item->width = space_width(c, style);
}

static void add_break(struct ctx *c, const struct style *style) {
    struct inline_item *item = inline_push(c);
    if (item != NULL) {
        item->kind = IT_BREAK;
        item->style = style;
    }
}

static bool is_ws(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* A preformatted segment with tabs expanded (to 8 columns). */
static const char *expand_tabs(struct ctx *c, const char *text, size_t *length) {
    size_t tabs = 0;
    for (size_t i = 0; i < *length; i++) {
        tabs += text[i] == '\t';
    }
    if (tabs == 0) {
        return text;
    }
    char *copy = arena_alloc(c->page, *length + tabs * 8 + 1);
    if (copy == NULL) {
        c->failed = true;
        return text;
    }
    size_t used = 0;
    for (size_t i = 0; i < *length; i++) {
        if (text[i] == '\t') {
            do {
                copy[used++] = ' ';
            } while (used % 8 != 0);
        } else {
            copy[used++] = text[i];
        }
    }
    *length = used;
    return copy;
}

static void add_text(struct ctx *c, const char *text, size_t length,
                     const struct style *style) {
    if (style->pre) {
        size_t start = 0;
        for (size_t i = 0; i <= length; i++) {
            if (i == length || text[i] == '\n' || text[i] == '\r') {
                if (i > start) {
                    size_t segment = i - start;
                    const char *expanded = expand_tabs(c, text + start,
                                                       &segment);
                    add_word(c, expanded, segment, style);
                }
                if (i < length) {
                    if (text[i] == '\r' && i + 1 < length
                        && text[i + 1] == '\n') {
                        i++;
                    }
                    add_break(c, style);
                }
                start = i + 1;
            }
        }
        return;
    }
    size_t i = 0;
    while (i < length) {
        if (is_ws(text[i])) {
            while (i < length && is_ws(text[i])) {
                i++;
            }
            add_space(c, style);
        } else {
            size_t start = i;
            while (i < length && !is_ws(text[i])) {
                i++;
            }
            add_word(c, text + start, i - start, style);
        }
    }
}

/* ---------------------------------------------------------------- images */

static struct image_ref *image_for(struct ctx *c,
                                   const struct opos_html_node *node) {
    struct page *page = c->page;
    for (size_t i = 0; i < page->image_count; i++) {
        if (page->images[i]->node == node) {
            return page->images[i];
        }
    }
    const char *source = opos_html_attribute(node, "src");
    if (source == NULL || source[0] == '\0') {
        return NULL;
    }
    if (page->image_count == page->image_capacity) {
        size_t capacity = page->image_capacity == 0 ? 32
                        : page->image_capacity * 2;
        struct image_ref **grown = realloc(page->images,
                                           capacity * sizeof(*grown));
        if (grown == NULL) {
            c->failed = true;
            return NULL;
        }
        page->images = grown;
        page->image_capacity = capacity;
    }
    struct image_ref *ref = malloc(sizeof(*ref));
    if (ref == NULL) {
        c->failed = true;
        return NULL;
    }
    page->images[page->image_count++] = ref;
    memset(ref, 0, sizeof(*ref));
    ref->node = node;
    if (strncmp(source, "data:", 5) == 0
        || !opos_url_resolve(page->url, source, ref->url, sizeof(ref->url))) {
        ref->state = 2;
    }
    return ref;
}

static int attribute_size(const struct opos_html_node *node, const char *name,
                          int percent_of) {
    const char *value = opos_html_attribute(node, name);
    int number;
    bool percent;
    if (value == NULL || !parse_length(value, &number, &percent)
        || number < 0) {
        return -1;
    }
    return percent ? percent_of * number / 100 : number;
}

static void add_image(struct ctx *c, const struct opos_html_node *node,
                      const struct style *style) {
    struct image_ref *ref = image_for(c, node);
    int width = attribute_size(node, "width", c->bwidth);
    int height = attribute_size(node, "height", c->bwidth);
    const char *alt = opos_html_attribute(node, "alt");
    if (ref != NULL && ref->state == 1) {
        if (width < 0 && height < 0) {
            width = ref->width;
            height = ref->height;
        } else if (width < 0) {
            width = height * ref->width / clamp_min(ref->height, 1);
        } else if (height < 0) {
            height = width * ref->height / clamp_min(ref->width, 1);
        }
    } else if (width < 0 || height < 0) {
        /* Not loaded (or failed) and no size: show the alternative text. */
        if (alt != NULL && alt[0] != '\0') {
            add_text(c, alt, strlen(alt), style);
        }
        if (ref != NULL && ref->state == 0 && width < 0 && height < 0
            && (alt == NULL || alt[0] == '\0')) {
            return;
        }
        if (width < 0 || height < 0) {
            return;
        }
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    if (!c->measuring && width > c->bwidth && c->bwidth > 0) {
        height = height * c->bwidth / width;
        width = c->bwidth;
        if (height < 1) {
            height = 1;
        }
    }
    struct inline_item *item = inline_push(c);
    if (item == NULL) {
        return;
    }
    item->kind = IT_IMAGE;
    item->style = style;
    item->width = width;
    item->height = height;
    item->image = ref;
}

/* ---------------------------------------------------------- form controls */

static void append_text(const struct opos_html_node *node, char *out,
                        size_t capacity, size_t *used) {
    for (const struct opos_html_node *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        if (child->type == HTML_NODE_TEXT) {
            for (size_t i = 0; i < child->text_length && *used + 1 < capacity;
                 i++) {
                char ch = child->text[i];
                out[(*used)++] = is_ws(ch) ? ' ' : ch;
            }
        } else if (child->tag != HTML_TAG_SCRIPT
                   && child->tag != HTML_TAG_STYLE) {
            append_text(child, out, capacity, used);
        }
    }
    out[*used] = '\0';
}

/* Number of UTF-8 characters in text. */
static size_t utf8_count(const char *text, size_t length) {
    size_t count = 0;
    for (size_t i = 0; i < length; i++) {
        if (((unsigned char)text[i] & 0xc0) != 0x80) {
            count++;
        }
    }
    return count;
}

static void add_control(struct ctx *c, const struct opos_html_node *node,
                        const struct style *style) {
    enum field_kind kind = form_kind(node);
    if (kind == FIELD_NONE || kind == FIELD_HIDDEN) {
        return;
    }
    struct form_field *field = form_field(c->page, node);
    bool focused = c->page->focus == node;
    int ascent, descent;
    line_metrics(c, style, &ascent, &descent);
    int line = ascent + descent;
    enum box_kind box = BOX_TEXT;
    int width = 0;
    int height = line + 8;
    bool checked = field != NULL ? field->checked
                 : opos_html_attribute(node, "checked") != NULL;
    bool placeholder = false;
    const char *text = "";
    char *owned = NULL;   /* text when it had to be built (malloc'd) */
    char buffer[200];
    buffer[0] = '\0';

    switch (kind) {
    case FIELD_CHECKBOX:
    case FIELD_RADIO:
        box = kind == FIELD_CHECKBOX ? BOX_CHECKBOX : BOX_RADIO;
        width = height = 14;
        break;
    case FIELD_SUBMIT:
    case FIELD_BUTTON:
    case FIELD_RESET:
        box = BOX_BUTTON;
        if (node->tag == HTML_TAG_BUTTON) {
            size_t used = 0;
            append_text(node, buffer, sizeof(buffer), &used);
            text = buffer;
        } else {
            const char *value = opos_html_attribute(node, "value");
            const char *type = opos_html_attribute(node, "type");
            text = value != NULL && value[0] != '\0' ? value
                 : kind == FIELD_RESET ? "Reset"
                 : kind == FIELD_SUBMIT ? "Submit"
                 : type != NULL && strcmp(type, "file") == 0 ? "Choose file"
                 : "Button";
        }
        break;
    case FIELD_TEXT:
    case FIELD_PASSWORD:
    case FIELD_TEXTAREA: {
        if (kind == FIELD_TEXTAREA) {
            box = BOX_TEXTAREA;
            int cols = attribute_size(node, "cols", 0);
            int rows = attribute_size(node, "rows", 0);
            width = (cols > 0 ? cols : 20) * (style->font_size / 2 + 1) + 12;
            height = (rows > 0 ? rows : 2) * line + 8;
            if (width > 600) {
                width = 600;
            }
            if (height > 400) {
                height = 400;
            }
        } else {
            int size = attribute_size(node, "size", 0);
            width = (size > 0 ? size : 20) * (style->font_size / 2 + 1) + 12;
            if (width > 480) {
                width = 480;
            }
        }
        if (style->width > 0) {
            width = style->width;
        } else if (style->width_pct > 0 && !c->measuring) {
            width = c->bwidth * style->width_pct / 100;
        }
        if (width < 40) {
            width = 40;
        }
        if (width > c->bwidth && c->bwidth > 40 && !c->measuring) {
            width = c->bwidth;
        }
        text = field != NULL ? field->value
             : opos_html_attribute(node, "value") != NULL
                 ? opos_html_attribute(node, "value") : "";
        if (text[0] == '\0') {
            const char *hint = opos_html_attribute(node, "placeholder");
            if (hint != NULL) {
                text = hint;
                placeholder = true;
            }
        }
        if (kind == FIELD_PASSWORD && !placeholder) {
            size_t n = utf8_count(text, strlen(text));
            owned = malloc(n + 1);
            if (owned == NULL) {
                c->failed = true;
                return;
            }
            memset(owned, '*', n);
            owned[n] = '\0';
            text = owned;
        }
        break;
    }
    case FIELD_SELECT: {
        box = BOX_SELECT;
        const struct opos_html_node *option = form_option(
            node, field != NULL ? field->option : 0);
        if (option != NULL) {
            size_t used = 0;
            append_text(option, buffer, sizeof(buffer), &used);
        }
        text = buffer;
        break;
    }
    default:
        return;
    }
    size_t length = 0;
    char *label = NULL;
    if (box != BOX_CHECKBOX && box != BOX_RADIO) {
        length = strlen(text);
        label = arena_alloc(c->page, length + 1);
        if (label == NULL) {
            free(owned);
            c->failed = true;
            return;
        }
        size_t used = 0;
        if (box == BOX_TEXT || box == BOX_TEXTAREA) {
            /* Typed text keeps its spaces; line breaks show as spaces. */
            for (size_t i = 0; i < length; i++) {
                label[used++] = is_ws(text[i]) ? ' ' : text[i];
            }
        } else {
            /* Collapse white space in button and option labels. */
            bool space = true;
            for (size_t i = 0; i < length; i++) {
                if (is_ws(text[i])) {
                    if (!space) {
                        label[used++] = ' ';
                    }
                    space = true;
                } else {
                    label[used++] = text[i];
                    space = false;
                }
            }
            while (used > 0 && label[used - 1] == ' ') {
                used--;
            }
        }
        label[used] = '\0';
        length = used;
        if (box == BOX_BUTTON) {
            width = text_width(c, style, label, length) + 24;
        } else if (box == BOX_SELECT) {
            width = text_width(c, style, label, length) + 36;
            if (width < 60) {
                width = 60;
            }
        } else if (!c->measuring) {
            /* Fit the text in the box: the end of it while typing,
               otherwise the start. */
            int room = width - 14;
            while (length > 0 && text_width(c, style, label, length) > room) {
                if (focused && !placeholder) {
                    size_t skip = 1;
                    while (skip < length && (label[skip] & 0xc0) == 0x80) {
                        skip++;
                    }
                    label += skip;
                    length -= skip;
                } else {
                    do {
                        length--;
                    } while (length > 0 && (label[length] & 0xc0) == 0x80);
                }
            }
        }
    }
    free(owned);
    struct inline_item *item = inline_push(c);
    if (item == NULL) {
        return;
    }
    item->kind = IT_BOX;
    item->style = style;
    item->text = label;
    item->length = length;
    item->width = width;
    item->height = height;
    item->box = box;
    item->checked = checked;
    item->placeholder = placeholder;
    item->focused = focused;
    item->node = node;
}

/* -------------------------------------------------------------- line flush */

/* The horizontal room for a line at y, beside the active floats. */
static void line_bounds(struct ctx *c, int y, int *left, int *right) {
    *left = c->bx;
    *right = c->bx + c->bwidth;
    for (int i = 0; i < c->float_count; i++) {
        const struct float_box *f = &c->floats[i];
        if (y >= f->top && y < f->bottom) {
            if (f->side == 1 && f->x + f->width > *left) {
                *left = f->x + f->width;
            } else if (f->side == 2 && f->x < *right) {
                *right = f->x;
            }
        }
    }
}

/* Moves y below the nearest float edge, if a float is in the way. */
static bool float_clear(struct ctx *c) {
    int next = 0;
    for (int i = 0; i < c->float_count; i++) {
        const struct float_box *f = &c->floats[i];
        if (c->y >= f->top && c->y < f->bottom
            && (next == 0 || f->bottom < next)) {
            next = f->bottom;
        }
    }
    if (next == 0) {
        return false;
    }
    c->y = next;
    return true;
}

static bool placed_reserve(struct ctx *c, size_t count) {
    if (count <= c->placed_capacity) {
        return true;
    }
    size_t capacity = c->placed_capacity == 0 ? 256 : c->placed_capacity;
    while (capacity < count) {
        capacity *= 2;
    }
    struct placed *grown = realloc(c->placed, capacity * sizeof(*grown));
    if (grown == NULL) {
        c->failed = true;
        return false;
    }
    c->placed = grown;
    c->placed_capacity = capacity;
    return true;
}

/* Bytes of the longest prefix of text that fits in room pixels (at least
   one character). */
static size_t fit_prefix(struct ctx *c, const struct style *style,
                         const char *text, size_t length, int room) {
    size_t best = 0;
    size_t i = 0;
    while (i < length) {
        uint32_t code;
        size_t step = opos_utf8_decode(text + i, length - i, &code);
        if (i > 0 && text_width(c, style, text, i + step) > room) {
            break;
        }
        i += step;
        best = i;
    }
    return best > 0 ? best : 1;
}

static void emit_marker(struct ctx *c, int baseline, int line_top) {
    if (!c->marker_pending) {
        return;
    }
    c->marker_pending = false;
    if (c->measuring) {
        return;
    }
    const struct style *style = c->marker_style;
    size_t length = strlen(c->marker);
    char *text = arena_alloc(c->page, length + 1);
    if (text == NULL) {
        c->failed = true;
        return;
    }
    memcpy(text, c->marker, length + 1);
    int width = text_width(c, style, text, length);
    struct display_item *item = add_item(c, ITEM_TEXT, c->bx - width - 8,
                                         line_top, width,
                                         0);
    if (item == NULL) {
        return;
    }
    int ascent, descent;
    line_metrics(c, style, &ascent, &descent);
    item->height = ascent + descent;
    item->text = text;
    item->length = length;
    item->font = font_index(style);
    item->size = style->font_size;
    item->baseline = baseline - line_top;
    item->colour = style->colour;
}

static void emit_line(struct ctx *c, size_t count, int used,
                      const struct inline_item *break_item) {
    int line_ascent = 0;
    int line_descent = 0;
    for (size_t i = 0; i < count; i++) {
        const struct inline_item *item = c->placed[i].item;
        int ascent, descent;
        if (item->kind == IT_IMAGE) {
            ascent = item->height;
            descent = 0;
        } else if (item->kind == IT_BOX) {
            ascent = item->height - 4;
            descent = 4;
        } else {
            line_metrics(c, item->style, &ascent, &descent);
        }
        line_ascent = max_int(line_ascent, ascent);
        line_descent = max_int(line_descent, descent);
    }
    if (count == 0 && break_item != NULL) {
        line_metrics(c, break_item->style, &line_ascent, &line_descent);
    }
    int top = c->y;
    int baseline = top + line_ascent;
    int offset = 0;
    if (!c->measuring) {
        if (c->align == ALIGN_CENTER) {
            offset = (c->bwidth - used) / 2;
        } else if (c->align == ALIGN_RIGHT) {
            offset = c->bwidth - used;
        }
        if (offset < 0) {
            offset = 0;
        }
    }
    emit_marker(c, baseline, top);
    for (size_t i = 0; i < count; i++) {
        const struct placed *p = &c->placed[i];
        const struct inline_item *item = p->item;
        const struct style *style = item->style;
        int x = c->bx + offset + p->x;
        note_right(c, x + p->width);
        if (c->measuring) {
            continue;
        }
        switch (item->kind) {
        case IT_WORD:
        case IT_SPACE: {
            int ascent, descent;
            line_metrics(c, style, &ascent, &descent);
            if (style->has_background) {
                struct display_item *back = add_item(
                    c, ITEM_RECT, x, baseline - ascent, p->width,
                    ascent + descent);
                if (back != NULL) {
                    back->colour = style->background;
                }
            }
            if (item->kind == IT_SPACE && !style->underline
                && !style->strike) {
                break;
            }
            struct display_item *text = add_item(c, ITEM_TEXT, x,
                                                 baseline - ascent, p->width,
                                                 ascent + descent);
            if (text == NULL) {
                break;
            }
            text->text = item->kind == IT_SPACE ? " " : p->text;
            text->length = item->kind == IT_SPACE ? 1 : p->length;
            text->font = font_index(style);
            text->size = style->font_size;
            text->baseline = ascent;
            text->colour = style->colour;
            text->underline = style->underline;
            text->strike = style->strike;
            text->link = style->link;
            break;
        }
        case IT_IMAGE: {
            struct display_item *image = add_item(
                c, ITEM_IMAGE, x, baseline - item->height, item->width,
                item->height);
            if (image != NULL) {
                image->image = item->image;
                image->link = style->link;
                image->colour = 0x00d8d8d8;
            }
            break;
        }
        case IT_BOX: {
            int box_top = baseline - (item->height - 4);
            bool field = item->box == BOX_TEXT || item->box == BOX_TEXTAREA
                      || item->box == BOX_CHECKBOX || item->box == BOX_RADIO;
            struct display_item *fill = add_item(c, ITEM_RECT, x, box_top,
                                                 item->width, item->height);
            if (fill != NULL) {
                fill->colour = field ? 0x00ffffff : 0x00e9e9ed;
                fill->control = item->node;
            }
            struct display_item *outline = add_item(c, ITEM_OUTLINE, x,
                                                    box_top, item->width,
                                                    item->height);
            if (outline != NULL) {
                outline->colour = item->focused ? 0x000645ad : 0x00767676;
            }
            if (item->focused) {
                struct display_item *ring = add_item(c, ITEM_OUTLINE, x - 1,
                                                     box_top - 1,
                                                     item->width + 2,
                                                     item->height + 2);
                if (ring != NULL) {
                    ring->colour = 0x000645ad;
                }
            }
            if (item->box == BOX_CHECKBOX || item->box == BOX_RADIO) {
                if (item->checked) {
                    struct display_item *mark = add_item(
                        c, ITEM_RECT, x + 3, box_top + 3, item->width - 6,
                        item->height - 6);
                    if (mark != NULL) {
                        mark->colour = 0x000645ad;
                    }
                }
                break;
            }
            if (item->length > 0) {
                int ascent, descent;
                line_metrics(c, style, &ascent, &descent);
                int label_width = text_width(c, style, item->text,
                                             item->length);
                int label_x = item->box == BOX_BUTTON
                            ? x + (item->width - label_width) / 2 : x + 6;
                struct display_item *label = add_item(
                    c, ITEM_TEXT, label_x,
                    box_top + (item->height - ascent - descent) / 2,
                    label_width, ascent + descent);
                if (label != NULL) {
                    label->text = item->text;
                    label->length = item->length;
                    label->font = font_index(style);
                    label->size = style->font_size;
                    label->baseline = ascent;
                    label->colour = item->placeholder ? 0x00757575
                                                      : style->colour;
                }
            }
            if (item->focused
                && (item->box == BOX_TEXT || item->box == BOX_TEXTAREA)) {
                /* The caret, after the text (before a placeholder). */
                int ascent, descent;
                line_metrics(c, style, &ascent, &descent);
                int caret_x = x + 6;
                if (!item->placeholder && item->length > 0) {
                    caret_x += text_width(c, style, item->text, item->length);
                }
                struct display_item *caret = add_item(
                    c, ITEM_RECT, caret_x,
                    box_top + (item->height - ascent - descent) / 2, 1,
                    ascent + descent);
                if (caret != NULL) {
                    caret->colour = style->colour;
                }
            }
            break;
        }
        default:
            break;
        }
    }
    c->y = top + line_ascent + line_descent;
}

static void flush_inline(struct ctx *c) {
    size_t n = c->inl_count;
    size_t i = 0;
    const int base_bx = c->bx, base_width = c->bwidth;
    while (i < n && !c->failed) {
        /* A new line: leading white space is dropped. */
        while (i < n && c->inl[i].kind == IT_SPACE) {
            i++;
        }
        if (i >= n) {
            break;
        }
        size_t line_start = i;
        bool restart = false;
        {
            int left, right;
            line_bounds(c, c->y, &left, &right);
            c->bx = left;
            c->bwidth = clamp_min(right - left, 1);
        }
        size_t count = 0;
        int x = 0;
        bool content = false;
        const struct inline_item *break_item = NULL;
        while (i < n) {
            struct inline_item *item = &c->inl[i];
            if (item->kind == IT_BREAK) {
                break_item = item;
                i++;
                break;
            }
            if (!placed_reserve(c, count + 1)) {
                return;
            }
            if (item->kind == IT_SPACE) {
                c->placed[count++] = (struct placed){item, NULL, 0, x,
                                                     item->width};
                x += item->width;
                i++;
                continue;
            }
            int width = item->width;
            bool pre = item->style->pre;
            if (!pre && content && x + width > c->bwidth) {
                break; /* does not fit: the line ends before it */
            }
            if (!pre && !content && x + width > c->bwidth && float_clear(c)) {
                restart = true;
                break;
            }
            if (!pre && !content && item->kind == IT_WORD
                && x + width > c->bwidth && !c->measure_min
                && item->length > 1) {
                /* One word wider than the line: split it. */
                size_t take = fit_prefix(c, item->style, item->text,
                                         item->length, c->bwidth - x);
                if (take < item->length) {
                    int part = text_width(c, item->style, item->text, take);
                    c->placed[count++] = (struct placed){item, item->text,
                                                         take, x, part};
                    x += part;
                    item->text += take;
                    item->length -= take;
                    item->width = text_width(c, item->style, item->text,
                                             item->length);
                    content = true;
                    break;
                }
            }
            c->placed[count++] = (struct placed){item, item->text,
                                                 item->length, x, width};
            x += width;
            content = true;
            i++;
        }
        if (restart) {
            i = line_start;
            c->bx = base_bx;
            c->bwidth = base_width;
            continue;
        }
        /* Trailing white space does not count. */
        while (count > 0 && c->placed[count - 1].item->kind == IT_SPACE) {
            x -= c->placed[count - 1].width;
            count--;
        }
        if (count == 0 && break_item == NULL) {
            c->bx = base_bx;
            c->bwidth = base_width;
            continue;
        }
        emit_line(c, count, x, break_item);
        c->bx = base_bx;
        c->bwidth = base_width;
    }
    c->inl_count = 0;
}

/* ----------------------------------------------------------------- blocks */

static void process_node(struct ctx *c, const struct opos_html_node *node,
                         struct style *parent);

static void process_children(struct ctx *c, const struct opos_html_node *node,
                             struct style *style) {
    if (c->depth >= MAX_DEPTH) {
        return;
    }
    c->depth++;
    for (const struct opos_html_node *child = node->first_child;
         child != NULL && !c->failed; child = child->next_sibling) {
        process_node(c, child, style);
    }
    c->depth--;
}

static void layout_hr(struct ctx *c, const struct style *style) {
    flush_inline(c);
    c->y += max_int(c->pending_margin, style->margin[TOP]);
    c->pending_margin = 0;
    if (!c->measuring) {
        struct display_item *dark = add_item(c, ITEM_RECT, c->bx, c->y,
                                             c->bwidth, 1);
        if (dark != NULL) {
            dark->colour = 0x009a9a9a;
        }
        struct display_item *light = add_item(c, ITEM_RECT, c->bx, c->y + 1,
                                              c->bwidth, 1);
        if (light != NULL) {
            light->colour = 0x00e8e8e8;
        }
    }
    c->y += 2;
    c->pending_margin = style->margin[BOTTOM];
}

static void layout_table(struct ctx *c, const struct opos_html_node *node,
                         struct style *st);
static void layout_grid_children(struct ctx *c,
                                 const struct opos_html_node *node,
                                 struct style *st);
static void layout_flex_children(struct ctx *c,
                                 const struct opos_html_node *node,
                                 struct style *st);
static int measure_content(struct ctx *c, const struct opos_html_node *node,
                           struct style *style, bool minimum);

static void layout_block(struct ctx *c, const struct opos_html_node *node,
                         struct style *style) {
    flush_inline(c);
    int saved_x = c->bx;
    int saved_width = c->bwidth;
    enum align saved_align = c->align;
    int ml = style->margin[LEFT], mr = style->margin[RIGHT];
    int frame = 2 * style->border + style->padding[LEFT] + style->padding[RIGHT];
    int avail = c->bwidth;
    int cw = avail - ml - mr - frame;
    if (style->width >= 0) {
        cw = style->width;
    } else if (style->width_pct >= 0) {
        cw = avail * style->width_pct / 100 - ml - mr - frame;
    }
    if (style->max_width >= 0 && cw > style->max_width) {
        cw = style->max_width;
    }
    if (style->max_width_pct >= 0
        && cw > avail * style->max_width_pct / 100 - frame) {
        cw = avail * style->max_width_pct / 100 - frame;
    }
    cw = clamp_min(cw, 1);
    int leftover = avail - ml - mr - frame - cw;
    if (leftover > 0) {
        if (style->margin_auto_left && style->margin_auto_right) {
            ml += leftover / 2;
            mr += leftover - leftover / 2;
        } else if (style->margin_auto_left) {
            ml += leftover;
        }
    }
    int box_x = c->bx + ml;
    int box_width = clamp_min(cw + frame, 1);
    c->bx += ml + style->border + style->padding[LEFT];
    c->bwidth = cw;

    bool boxed = style->has_background || style->border > 0;
    bool collapse_top = !boxed && style->padding[TOP] == 0;
    if (collapse_top) {
        c->pending_margin = max_int(c->pending_margin, style->margin[TOP]);
    } else {
        c->y += max_int(c->pending_margin, style->margin[TOP]);
        c->pending_margin = 0;
    }
    int box_top = c->y;
    size_t fill_index = (size_t)-1;
    size_t outline_index = (size_t)-1;
    if (boxed && !c->measuring) {
        if (style->has_background) {
            if (add_item(c, ITEM_RECT, box_x, box_top, box_width, 0) != NULL) {
                fill_index = c->page->item_count - 1;
                c->page->items[fill_index].colour = style->background;
            }
        }
        if (style->border > 0) {
            if (add_item(c, ITEM_OUTLINE, box_x, box_top, box_width, 0)
                != NULL) {
                outline_index = c->page->item_count - 1;
                c->page->items[outline_index].colour = style->border_colour;
            }
        }
    }
    c->y += style->border + style->padding[TOP];
    c->align = style->align;

    if (style->display == DISPLAY_LIST_ITEM && !style->no_marker) {
        const char *bullet = "\xe2\x80\xa2"; /* U+2022 */
        if (style->ordered) {
            snprintf(c->marker, sizeof(c->marker), "%d.", style->list_index);
        } else {
            snprintf(c->marker, sizeof(c->marker), "%s", bullet);
        }
        c->marker_style = style;
        c->marker_pending = true;
    }
    if (style->flex) {
        layout_flex_children(c, node, style);
    } else if (style->grid_count > 0) {
        layout_grid_children(c, node, style);
    } else {
        process_children(c, node, style);
    }
    flush_inline(c);
    c->marker_pending = false;

    bool collapse_bottom = !boxed && style->padding[BOTTOM] == 0;
    if (collapse_bottom) {
        c->pending_margin = max_int(c->pending_margin, style->margin[BOTTOM]);
    } else {
        c->y += c->pending_margin + style->padding[BOTTOM] + style->border;
        c->pending_margin = style->margin[BOTTOM];
        if (fill_index != (size_t)-1) {
            c->page->items[fill_index].height = c->y - box_top;
        }
        if (outline_index != (size_t)-1) {
            c->page->items[outline_index].height = c->y - box_top;
        }
    }
    c->bx = saved_x;
    c->bwidth = saved_width;
    c->align = saved_align;
}

/* ----------------------------------------------------------------- tables */

struct cell {
    const struct opos_html_node *node;
    struct style style;
    enum align align;
    int valign;          /* 0 top, 1 middle, 2 bottom */
    int row, column;
    int colspan, rowspan;
    int min, max;        /* content widths including padding */
    int spec;            /* width attribute in pixels, or -percent */
    int height;
    size_t first_item, last_item;
    size_t fill_index, outline_index;
    bool nowrap;
};

struct row {
    const struct opos_html_node *node;
    struct style style;
    int top, height;
};

static size_t measure_slot(const void *node) {
    return (size_t)(((uintptr_t)node >> 4) * 2654435761u) & (MEASURE_SLOTS - 1);
}

static bool measure_lookup(struct ctx *c, const void *node, int *min,
                           int *max) {
    if (c->measures == NULL) {
        return false;
    }
    size_t slot = measure_slot(node);
    for (size_t n = 0; n < MEASURE_SLOTS; n++) {
        struct measure_entry *entry = &c->measures[(slot + n)
                                                   & (MEASURE_SLOTS - 1)];
        if (entry->node == NULL) {
            return false;
        }
        if (entry->node == node) {
            *min = entry->min;
            *max = entry->max;
            return true;
        }
    }
    return false;
}

static void measure_store(struct ctx *c, const void *node, int min, int max) {
    if (c->measures == NULL) {
        c->measures = calloc(MEASURE_SLOTS, sizeof(*c->measures));
        if (c->measures == NULL) {
            return;
        }
    }
    size_t slot = measure_slot(node);
    for (size_t n = 0; n < MEASURE_SLOTS; n++) {
        struct measure_entry *entry = &c->measures[(slot + n)
                                                   & (MEASURE_SLOTS - 1)];
        if (entry->node == NULL) {
            entry->node = node;
            entry->min = min;
            entry->max = max;
            return;
        }
    }
}

/* Lays the cell's content out for measuring and discards the result. */
static int measure_content(struct ctx *c, const struct opos_html_node *node,
                           struct style *style, bool minimum) {
    int saved_x = c->bx, saved_width = c->bwidth, saved_y = c->y;
    int saved_margin = c->pending_margin, saved_right = c->max_right;
    bool saved_measuring = c->measuring, saved_min = c->measure_min;
    enum align saved_align = c->align;
    size_t saved_items = c->page->item_count;
    bool saved_marker = c->marker_pending;
    int saved_floats = c->float_count;
    c->float_count = 0;

    c->measuring = true;
    c->measure_min = minimum;
    c->bx = 0;
    c->bwidth = minimum ? 1 : WIDE;
    c->y = 0;
    c->pending_margin = 0;
    c->max_right = 0;
    c->align = ALIGN_LEFT;
    c->marker_pending = false;
    if (style->display == DISPLAY_TABLE) {
        layout_table(c, node, style);
    } else if (style->flex) {
        layout_flex_children(c, node, style);
    } else if (style->grid_count > 0) {
        layout_grid_children(c, node, style);
    } else {
        process_children(c, node, style);
    }
    flush_inline(c);
    int result = c->max_right;

    c->page->item_count = saved_items;
    c->bx = saved_x;
    c->bwidth = saved_width;
    c->y = saved_y;
    c->pending_margin = saved_margin;
    c->max_right = saved_right;
    c->measuring = saved_measuring;
    c->measure_min = saved_min;
    c->align = saved_align;
    c->float_count = saved_floats;
    c->marker_pending = saved_marker;
    return result;
}

static void measure_cell(struct ctx *c, struct cell *cell, int extra) {
    int min, max;
    if (!measure_lookup(c, cell->node, &min, &max)) {
        max = measure_content(c, cell->node, &cell->style, false);
        min = measure_content(c, cell->node, &cell->style, true);
        if (min > max) {
            min = max;
        }
        measure_store(c, cell->node, min, max);
    }
    if (cell->nowrap) {
        min = max;
    }
    cell->min = min + extra;
    cell->max = max + extra;
}

static int int_attribute(const struct opos_html_node *node, const char *name,
                         int fallback, int low, int high) {
    const char *value = opos_html_attribute(node, name);
    if (value == NULL) {
        return fallback;
    }
    int number;
    bool percent;
    if (!parse_length(value, &number, &percent)) {
        return fallback;
    }
    return number < low ? low : number > high ? high : number;
}

static bool ieq(const char *a, const char *b) {
    while (*a != 0 && tolower((unsigned char)*a) == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

static int align_attribute(const struct opos_html_node *node, int fallback) {
    const char *value = opos_html_attribute(node, "align");
    if (value == NULL) {
        return fallback;
    }
    if (ieq(value, "center") || ieq(value, "middle")) {
        return ALIGN_CENTER;
    }
    if (ieq(value, "right")) {
        return ALIGN_RIGHT;
    }
    if (ieq(value, "left")) {
        return ALIGN_LEFT;
    }
    return fallback;
}

static int valign_attribute(const struct opos_html_node *node, int fallback) {
    const char *value = opos_html_attribute(node, "valign");
    if (value == NULL) {
        return fallback;
    }
    if (ieq(value, "top") || ieq(value, "baseline")) {
        return 0;
    }
    if (ieq(value, "bottom")) {
        return 2;
    }
    return 1;
}

struct table_data {
    struct row *rows;
    size_t row_count, row_capacity;
    struct cell *cells;
    size_t cell_count, cell_capacity;
    bool failed;
};

static void table_add_row(struct table_data *t, const struct opos_html_node *tr,
                          const struct style *table_style, int cell_padding) {
    if (t->row_count == t->row_capacity) {
        size_t capacity = t->row_capacity == 0 ? 16 : t->row_capacity * 2;
        struct row *grown = realloc(t->rows, capacity * sizeof(*grown));
        if (grown == NULL) {
            t->failed = true;
            return;
        }
        t->rows = grown;
        t->row_capacity = capacity;
    }
    size_t index = t->row_count++;
    struct row *row = &t->rows[index];
    memset(row, 0, sizeof(*row));
    row->node = tr;
    style_compute(tr, table_style, &row->style);
    for (const struct opos_html_node *td = tr->first_child; td != NULL;
         td = td->next_sibling) {
        if (td->type != HTML_NODE_ELEMENT
            || (td->tag != HTML_TAG_TD && td->tag != HTML_TAG_TH)
            || t->cell_count >= MAX_CELLS) {
            continue;
        }
        if (t->cell_count == t->cell_capacity) {
            size_t capacity = t->cell_capacity == 0 ? 64
                            : t->cell_capacity * 2;
            struct cell *grown = realloc(t->cells, capacity * sizeof(*grown));
            if (grown == NULL) {
                t->failed = true;
                return;
            }
            t->cells = grown;
            t->cell_capacity = capacity;
        }
        struct cell *cell = &t->cells[t->cell_count++];
        memset(cell, 0, sizeof(*cell));
        cell->node = td;
        style_compute(td, &row->style, &cell->style);
        cell->row = (int)index;
        cell->colspan = int_attribute(td, "colspan", 1, 1, MAX_COLUMNS);
        cell->rowspan = int_attribute(td, "rowspan", 1, 1, 100);
        cell->align = (enum align)cell->style.align;
        if (opos_html_attribute(td, "align") == NULL) {
            cell->align = (enum align)align_attribute(tr, cell->align);
        }
        cell->valign = valign_attribute(td, valign_attribute(tr, 1));
        cell->nowrap = opos_html_attribute(td, "nowrap") != NULL;
        const char *width = opos_html_attribute(td, "width");
        int number;
        bool percent;
        cell->spec = 0;
        if (width != NULL && parse_length(width, &number, &percent)
            && number > 0) {
            cell->spec = percent ? -number : number;
        }
        for (int side = 0; side < 4; side++) {
            if (cell->style.padding[side] == 0) {
                cell->style.padding[side] = cell_padding;
            }
        }
    }
}

static void layout_table(struct ctx *c, const struct opos_html_node *node,
                         struct style *st) {
    flush_inline(c);
    if (c->depth >= MAX_DEPTH) {
        return;
    }
    c->depth++;
    int spacing = int_attribute(node, "cellspacing", 2, 0, 50);
    int cell_padding = int_attribute(node, "cellpadding", 1, 0, 50);
    int border = int_attribute(node, "border", 0, 0, 20);
    int cell_border = border > 0 ? 1 : 0;

    struct table_data t = {0};
    /* Captions go above the table. */
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        if (child->type != HTML_NODE_ELEMENT) {
            continue;
        }
        if (child->tag == HTML_TAG_CAPTION) {
            struct style *caption = arena_alloc(c->page, sizeof(*caption));
            if (caption != NULL) {
                style_compute(child, st, caption);
                caption->display = DISPLAY_BLOCK;
                caption->align = ALIGN_CENTER;
                layout_block(c, child, caption);
            }
        } else if (child->tag == HTML_TAG_TR) {
            table_add_row(&t, child, st, cell_padding);
        } else if (child->tag == HTML_TAG_THEAD || child->tag == HTML_TAG_TBODY
                   || child->tag == HTML_TAG_TFOOT) {
            for (const struct opos_html_node *tr = child->first_child;
                 tr != NULL; tr = tr->next_sibling) {
                if (tr->type == HTML_NODE_ELEMENT && tr->tag == HTML_TAG_TR) {
                    table_add_row(&t, tr, st, cell_padding);
                }
            }
        }
    }
    if (t.failed || t.cell_count == 0) {
        free(t.rows);
        free(t.cells);
        c->failed |= t.failed;
        c->depth--;
        return;
    }

    /* Columns: the first free slot in each row, honouring row spans. */
    uint8_t *grid = calloc(t.row_count * MAX_COLUMNS, 1);
    if (grid == NULL) {
        free(t.rows);
        free(t.cells);
        c->failed = true;
        c->depth--;
        return;
    }
    int ncols = 0;
    for (size_t i = 0; i < t.cell_count; i++) {
        struct cell *cell = &t.cells[i];
        int column = 0;
        while (column < MAX_COLUMNS && grid[cell->row * MAX_COLUMNS + column]) {
            column++;
        }
        if (column >= MAX_COLUMNS) {
            cell->colspan = 0;
            continue;
        }
        if (column + cell->colspan > MAX_COLUMNS) {
            cell->colspan = MAX_COLUMNS - column;
        }
        if (cell->row + cell->rowspan > (int)t.row_count) {
            cell->rowspan = (int)t.row_count - cell->row;
        }
        cell->column = column;
        for (int r = 0; r < cell->rowspan; r++) {
            for (int k = 0; k < cell->colspan; k++) {
                grid[(cell->row + r) * MAX_COLUMNS + column + k] = 1;
            }
        }
        if (column + cell->colspan > ncols) {
            ncols = column + cell->colspan;
        }
    }
    free(grid);
    if (ncols == 0) {
        free(t.rows);
        free(t.cells);
        c->depth--;
        return;
    }

    /* Content widths. */
    int colmin[MAX_COLUMNS] = {0};
    int colmax[MAX_COLUMNS] = {0};
    int colpct[MAX_COLUMNS] = {0};
    for (size_t i = 0; i < t.cell_count; i++) {
        struct cell *cell = &t.cells[i];
        if (cell->colspan == 0) {
            continue;
        }
        int extra = cell->style.padding[LEFT] + cell->style.padding[RIGHT]
                  + 2 * cell_border;
        measure_cell(c, cell, extra);
        if (cell->spec > 0) {
            cell->max = max_int(cell->max, cell->spec + extra);
            cell->min = max_int(cell->min, min_int(cell->spec + extra,
                                                   cell->max));
        } else if (cell->spec < 0 && !c->measuring && cell->colspan == 1) {
            colpct[cell->column] = max_int(colpct[cell->column], -cell->spec);
        }
        if (cell->colspan == 1) {
            colmin[cell->column] = max_int(colmin[cell->column], cell->min);
            colmax[cell->column] = max_int(colmax[cell->column], cell->max);
        }
    }
    for (size_t i = 0; i < t.cell_count; i++) {
        struct cell *cell = &t.cells[i];
        if (cell->colspan <= 1) {
            continue;
        }
        /* A spanning cell widens its columns evenly when they are too
           narrow for it. */
        int have_min = 0, have_max = 0;
        for (int k = 0; k < cell->colspan; k++) {
            have_min += colmin[cell->column + k];
            have_max += colmax[cell->column + k];
        }
        int spans = cell->colspan;
        if (cell->min > have_min) {
            int share = (cell->min - have_min + spans - 1) / spans;
            for (int k = 0; k < spans; k++) {
                colmin[cell->column + k] += share;
            }
        }
        if (cell->max > have_max) {
            int share = (cell->max - have_max + spans - 1) / spans;
            for (int k = 0; k < spans; k++) {
                colmax[cell->column + k] += share;
            }
        }
    }
    for (int i = 0; i < ncols; i++) {
        if (colmax[i] < colmin[i]) {
            colmax[i] = colmin[i];
        }
    }

    int available = clamp_min(c->bwidth - st->margin[LEFT] - st->margin[RIGHT],
                              1);
    int spacing_total = (ncols + 1) * spacing + 2 * border;
    int target = -1;
    const char *width_attr = opos_html_attribute(node, "width");
    if (width_attr != NULL) {
        int number;
        bool percent;
        if (parse_length(width_attr, &number, &percent) && number > 0) {
            if (!percent) {
                target = number;
            } else if (!c->measuring) {
                target = available * number / 100;
            }
        }
    }
    if (target < 0 && st->width >= 0) {
        target = st->width;
    } else if (target < 0 && st->width_pct >= 0 && !c->measuring) {
        target = available * st->width_pct / 100;
    }
    int sum_min = 0, sum_max = 0;
    for (int i = 0; i < ncols; i++) {
        sum_min += colmin[i];
        sum_max += colmax[i];
    }
    int inner;
    if (target > 0) {
        inner = max_int(target - spacing_total, sum_min);
    } else {
        inner = clamp_min(min_int(sum_max, max_int(available - spacing_total,
                                                   sum_min)), sum_min);
    }
    for (int i = 0; i < ncols; i++) {
        if (colpct[i] > 0) {
            int wanted = inner * colpct[i] / 100;
            if (wanted > colmax[i]) {
                sum_max += wanted - colmax[i];
                colmax[i] = wanted;
            }
        }
    }
    int widths[MAX_COLUMNS];
    if (inner >= sum_max) {
        int extra = target > 0 ? inner - sum_max : 0;
        for (int i = 0; i < ncols; i++) {
            widths[i] = colmax[i] + (sum_max > 0
                                     ? (int)((long)extra * colmax[i] / sum_max)
                                     : extra / ncols);
        }
    } else if (inner <= sum_min || sum_max == sum_min) {
        for (int i = 0; i < ncols; i++) {
            widths[i] = colmin[i];
        }
    } else {
        for (int i = 0; i < ncols; i++) {
            widths[i] = colmin[i] + (int)((long)(colmax[i] - colmin[i])
                                          * (inner - sum_min)
                                          / (sum_max - sum_min));
        }
    }
    int column_x[MAX_COLUMNS + 1];
    int total = border + spacing;
    for (int i = 0; i < ncols; i++) {
        column_x[i] = total;
        total += widths[i] + spacing;
    }
    column_x[ncols] = total;
    total += border;

    int x0 = c->bx + st->margin[LEFT];
    if (!c->measuring) {
        int room = available - total;
        if (room > 0) {
            enum align place = (enum align)align_attribute(node, c->align);
            if (place == ALIGN_CENTER) {
                x0 += room / 2;
            } else if (place == ALIGN_RIGHT) {
                x0 += room;
            }
        }
    }
    note_right(c, x0 + total);

    c->y += max_int(c->pending_margin, st->margin[TOP]);
    c->pending_margin = 0;
    int table_top = c->y;
    size_t table_fill = (size_t)-1, table_outline = (size_t)-1;
    if (!c->measuring) {
        if (st->has_background
            && add_item(c, ITEM_RECT, x0, table_top, total, 0) != NULL) {
            table_fill = c->page->item_count - 1;
            c->page->items[table_fill].colour = st->background;
        }
        if (border > 0 && add_item(c, ITEM_OUTLINE, x0, table_top, total, 0)
                              != NULL) {
            table_outline = c->page->item_count - 1;
            c->page->items[table_outline].colour = 0x00808080;
        }
    }

    int saved_x = c->bx, saved_width = c->bwidth;
    enum align saved_align = c->align;
    int y = table_top + border + spacing;
    size_t next_cell = 0;
    for (size_t r = 0; r < t.row_count && !c->failed; r++) {
        struct row *row = &t.rows[r];
        row->top = y;
        int row_height = 0;
        size_t first_cell = next_cell;
        while (next_cell < t.cell_count && t.cells[next_cell].row == (int)r) {
            next_cell++;
        }
        for (size_t i = first_cell; i < next_cell; i++) {
            struct cell *cell = &t.cells[i];
            if (cell->colspan == 0) {
                continue;
            }
            int cell_x = x0 + column_x[cell->column];
            int cell_width = column_x[cell->column + cell->colspan]
                           - column_x[cell->column] - spacing;
            struct style *style = &cell->style;
            int pad_l = style->padding[LEFT] + cell_border;
            int pad_r = style->padding[RIGHT] + cell_border;
            int pad_t = style->padding[TOP] + cell_border;
            int pad_b = style->padding[BOTTOM] + cell_border;
            cell->fill_index = (size_t)-1;
            cell->outline_index = (size_t)-1;
            if (!c->measuring) {
                bool has_bg = style->has_background || row->style.has_background;
                if (has_bg && add_item(c, ITEM_RECT, cell_x, y, cell_width, 0)
                                  != NULL) {
                    cell->fill_index = c->page->item_count - 1;
                    c->page->items[cell->fill_index].colour =
                        style->has_background ? style->background
                                              : row->style.background;
                }
                if (cell_border > 0
                    && add_item(c, ITEM_OUTLINE, cell_x, y, cell_width, 0)
                           != NULL) {
                    cell->outline_index = c->page->item_count - 1;
                    c->page->items[cell->outline_index].colour = 0x00808080;
                }
            }
            c->bx = cell_x + pad_l;
            c->bwidth = clamp_min(cell_width - pad_l - pad_r, 1);
            c->y = y + pad_t;
            c->pending_margin = 0;
            c->align = cell->align;
            cell->first_item = c->page->item_count;
            int cell_floats = c->float_count;
            c->float_count = 0;
            process_children(c, cell->node, style);
            flush_inline(c);
            c->float_count = cell_floats;
            cell->last_item = c->page->item_count;
            cell->height = c->y - y + pad_b;
            if (cell->rowspan == 1) {
                row_height = max_int(row_height, cell->height);
            }
        }
        /* A cell spanning rows that ends here may need the row taller. */
        for (size_t i = 0; i < next_cell; i++) {
            struct cell *cell = &t.cells[i];
            if (cell->colspan != 0 && cell->rowspan > 1
                && cell->row + cell->rowspan - 1 == (int)r) {
                int have = y - t.rows[cell->row].top;
                row_height = max_int(row_height, cell->height - have);
            }
        }
        row->height = row_height;
        y += row_height + spacing;
    }
    c->bx = saved_x;
    c->bwidth = saved_width;
    c->align = saved_align;

    int table_bottom = y + border;
    if (!c->measuring) {
        for (size_t i = 0; i < t.cell_count; i++) {
            struct cell *cell = &t.cells[i];
            if (cell->colspan == 0) {
                continue;
            }
            int last_row = cell->row + cell->rowspan - 1;
            int box_top = t.rows[cell->row].top;
            int box_height = t.rows[last_row].top + t.rows[last_row].height
                           - box_top;
            if (cell->fill_index != (size_t)-1) {
                c->page->items[cell->fill_index].height = box_height;
            }
            if (cell->outline_index != (size_t)-1) {
                c->page->items[cell->outline_index].height = box_height;
            }
            int shift = 0;
            if (cell->valign == 1) {
                shift = (box_height - cell->height) / 2;
            } else if (cell->valign == 2) {
                shift = box_height - cell->height;
            }
            if (shift > 0) {
                for (size_t k = cell->first_item; k < cell->last_item; k++) {
                    c->page->items[k].y += shift;
                }
            }
        }
        if (table_fill != (size_t)-1) {
            c->page->items[table_fill].height = table_bottom - table_top;
        }
        if (table_outline != (size_t)-1) {
            c->page->items[table_outline].height = table_bottom - table_top;
        }
    }
    c->y = table_bottom;
    c->pending_margin = st->margin[BOTTOM];
    free(t.rows);
    free(t.cells);
    c->depth--;
}

/* ------------------------------------------------------------ floats, flex */

/* Lays one box out with a fixed content width at x (its margin box starts
   there) and returns the y just below it (including the bottom margin). */
static int layout_boxed(struct ctx *c, const struct opos_html_node *node,
                        const struct style *original, int x, int content_width,
                        int top) {
    struct style copy = *original;
    copy.width = content_width;
    copy.width_pct = -1;
    copy.max_width = copy.max_width_pct = -1;
    copy.margin_auto_left = copy.margin_auto_right = false;
    copy.float_side = 0;
    copy.clear_both = false;
    if (copy.display != DISPLAY_LIST_ITEM && copy.display != DISPLAY_TABLE) {
        copy.display = DISPLAY_BLOCK;
    }
    int frame = 2 * copy.border + copy.padding[LEFT] + copy.padding[RIGHT]
              + copy.margin[LEFT] + copy.margin[RIGHT];
    int saved_x = c->bx, saved_width = c->bwidth, saved_y = c->y;
    int saved_margin = c->pending_margin;
    enum align saved_align = c->align;
    c->bx = x;
    c->bwidth = content_width + frame;
    c->y = top;
    c->pending_margin = 0;
    if (copy.display == DISPLAY_TABLE) {
        copy.width = content_width;
        layout_table(c, node, &copy);
    } else {
        layout_block(c, node, &copy);
    }
    int bottom = c->y + c->pending_margin;
    c->bx = saved_x;
    c->bwidth = saved_width;
    c->y = saved_y;
    c->pending_margin = saved_margin;
    c->align = saved_align;
    return bottom;
}

static void float_note(struct ctx *c, int side, int x, int width, int top,
                       int bottom) {
    if (bottom > c->float_extent) {
        c->float_extent = bottom;
    }
    if (c->float_count == MAX_FLOATS) {
        int kept = 0;
        for (int i = 0; i < c->float_count; i++) {
            if (c->floats[i].bottom > c->y) {
                c->floats[kept++] = c->floats[i];
            }
        }
        c->float_count = kept;
        if (kept == MAX_FLOATS) {
            int oldest = 0;
            for (int i = 1; i < kept; i++) {
                if (c->floats[i].bottom < c->floats[oldest].bottom) {
                    oldest = i;
                }
            }
            c->floats[oldest] = c->floats[--c->float_count];
        }
    }
    struct float_box *f = &c->floats[c->float_count++];
    f->side = side;
    f->x = x;
    f->width = width;
    f->top = top;
    f->bottom = bottom;
}

static void layout_float(struct ctx *c, const struct opos_html_node *node,
                         struct style *st) {
    flush_inline(c);
    int left, right;
    line_bounds(c, c->y, &left, &right);
    int room = clamp_min(right - left, 1);
    int top = c->y;
    int saved_floats = c->float_count;
    if (node->tag == HTML_TAG_IMG) {
        size_t before = c->inl_count;
        add_image(c, node, st);
        if (c->inl_count == before) {
            return;
        }
        int w = c->inl[before].width + st->margin[LEFT] + st->margin[RIGHT];
        while (w > right - left && float_clear(c)) {
            line_bounds(c, c->y, &left, &right);
            top = c->y;
        }
        int x = st->float_side == 1 ? left : right - w;
        if (x < left) {
            x = left;
        }
        int saved_x = c->bx, saved_width = c->bwidth;
        c->bx = x + st->margin[LEFT];
        c->bwidth = w;
        c->float_count = 0;
        flush_inline(c);
        int bottom = c->y;
        c->bx = saved_x;
        c->bwidth = saved_width;
        c->y = top;
        c->float_count = saved_floats;
        float_note(c, st->float_side, x, w, top, bottom + st->margin[BOTTOM]);
        return;
    }
    int frame = 2 * st->border + st->padding[LEFT] + st->padding[RIGHT];
    int margins = st->margin[LEFT] + st->margin[RIGHT];
    {
        int want = st->width >= 0 ? st->width
                 : st->width_pct >= 0 ? 0
                 : measure_content(c, node, st, false);
        want += frame + margins;
        while (want > room && float_clear(c)) {
            line_bounds(c, c->y, &left, &right);
            room = clamp_min(right - left, 1);
            top = c->y;
        }
    }
    int cw;
    if (st->width >= 0) {
        cw = st->width;
    } else if (st->width_pct >= 0) {
        cw = room * st->width_pct / 100 - frame - margins;
    } else {
        cw = measure_content(c, node, st, false);
        if (cw > room - frame - margins) {
            cw = room - frame - margins;
        }
    }
    if (st->max_width >= 0 && cw > st->max_width) {
        cw = st->max_width;
    }
    cw = clamp_min(cw, 1);
    int total = cw + frame + margins;
    int x = st->float_side == 1 ? left : right - total;
    if (x < left) {
        x = left;
    }
    c->float_count = 0;
    int bottom = layout_boxed(c, node, st, x, cw, top);
    c->float_count = saved_floats;
    float_note(c, st->float_side, x, total, top, bottom);
}

#define MAX_FLEX_ITEMS 40

struct flex_item {
    const struct opos_html_node *node;
    struct style *style;
    int min, base, width, frame, grow, margin_auto;
};

static void layout_flex_children(struct ctx *c,
                                 const struct opos_html_node *node,
                                 struct style *st) {
    struct flex_item *items = malloc(MAX_FLEX_ITEMS * sizeof(*items));
    if (items == NULL) {
        c->failed = true;
        return;
    }
    int count = 0;
    bool plain = true;
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        if (child->type == HTML_NODE_TEXT) {
            for (size_t i = 0; i < child->text_length; i++) {
                if (!is_ws(child->text[i])) {
                    plain = false;
                }
            }
            continue;
        }
        struct style *s = arena_alloc(c->page, sizeof(*s));
        if (s == NULL) {
            c->failed = true;
            break;
        }
        style_compute(child, st, s);
        if (s->display == DISPLAY_NONE) {
            continue;
        }
        if (count == MAX_FLEX_ITEMS) {
            plain = false;
            break;
        }
        if (s->display == DISPLAY_INLINE && child->tag != HTML_TAG_IMG
            && child->tag != HTML_TAG_BR) {
            s->display = DISPLAY_BLOCK;
        }
        if (child->tag == HTML_TAG_IMG || child->tag == HTML_TAG_BR
            || s->display == DISPLAY_INLINE
            || s->display == DISPLAY_INLINE_BLOCK
            || s->display == DISPLAY_TABLE) {
            plain = false;
        }
        items[count].node = child;
        items[count].style = s;
        count++;
    }
    if (!plain || count == 0 || c->failed) {
        free(items);
        process_children(c, node, st);
        return;
    }
    int saved_floats = c->float_count;
    int avail = c->bwidth;
    int gap = st->gap;
    for (int i = 0; i < count; i++) {
        struct flex_item *it = &items[i];
        struct style *s = it->style;
        int ml = s->margin_auto_left ? 0 : s->margin[LEFT];
        int mr = s->margin_auto_right ? 0 : s->margin[RIGHT];
        it->margin_auto = s->margin_auto_left || s->margin_auto_right;
        it->frame = 2 * s->border + s->padding[LEFT] + s->padding[RIGHT]
                  + ml + mr;
        it->grow = s->flex_grow;
        int max, min;
        if (s->width >= 0) {
            max = min = s->width;
        } else if (s->width_pct >= 0) {
            max = min = avail * s->width_pct / 100 - it->frame;
        } else {
            struct style probe = *s;
            max = measure_content(c, it->node, &probe, false);
            min = measure_content(c, it->node, &probe, true);
            if (min > max) {
                min = max;
            }
        }
        if (s->max_width >= 0 && max > s->max_width) {
            max = s->max_width;
        }
        if (min > max) {
            min = max;
        }
        it->min = clamp_min(min, 0);
        it->base = s->flex_zero ? it->min : clamp_min(max, 0);
    }
    int line_start = 0;
    while (line_start < count && !c->failed) {
        int line_end = line_start;
        int sum = 0;
        while (line_end < count) {
            int w = items[line_end].base + items[line_end].frame
                  + (line_end > line_start ? gap : 0);
            if (st->flex_wrap && line_end > line_start && sum + w > avail) {
                break;
            }
            sum += w;
            line_end++;
        }
        int n = line_end - line_start;
        struct flex_item *line = &items[line_start];
        int free_space = avail - sum;
        int shift = 0, extra_gap = 0;
        if (free_space > 0) {
            int grow_total = 0, auto_total = 0;
            for (int i = 0; i < n; i++) {
                grow_total += line[i].grow;
                auto_total += line[i].margin_auto;
            }
            for (int i = 0; i < n; i++) {
                line[i].width = line[i].base;
            }
            if (grow_total > 0) {
                int given = 0;
                for (int i = 0; i < n; i++) {
                    if (line[i].grow) {
                        int part = free_space / grow_total;
                        line[i].width += part;
                        given += part;
                    }
                }
                free_space -= given;
            }
            if (auto_total > 0) {
                shift = 0;
            } else if (st->justify == 1) {
                shift = free_space / 2;
            } else if (st->justify == 2) {
                shift = free_space;
            } else if (st->justify == 3 && n > 1) {
                extra_gap = free_space / (n - 1);
            }
        } else {
            int shrinkable = 0;
            for (int i = 0; i < n; i++) {
                shrinkable += line[i].base - line[i].min;
            }
            int need = -free_space;
            for (int i = 0; i < n; i++) {
                int cut = shrinkable <= need ? line[i].base - line[i].min
                        : (int)((long)need * (line[i].base - line[i].min)
                                / shrinkable);
                line[i].width = line[i].base - cut;
            }
        }
        int x = c->bx + shift;
        int top = c->y;
        int bottom = top;
        int auto_extra = 0;
        if (free_space > 0) {
            int auto_total = 0;
            for (int i = 0; i < n; i++) {
                auto_total += line[i].margin_auto;
            }
            if (auto_total > 0) {
                auto_extra = free_space / auto_total;
            }
        }
        for (int i = 0; i < n; i++) {
            struct flex_item *it = &line[i];
            struct style *s = it->style;
            if (s->margin_auto_left && auto_extra > 0) {
                x += auto_extra;
            }
            int ml = s->margin_auto_left ? 0 : s->margin[LEFT];
            int mr = s->margin_auto_right ? 0 : s->margin[RIGHT];
            struct style item_style = *s;
            item_style.margin[LEFT] = ml;
            item_style.margin[RIGHT] = mr;
            c->float_count = 0;
            int item_bottom = layout_boxed(c, it->node, &item_style, x,
                                           clamp_min(it->width, 1), top);
            if (item_bottom > bottom) {
                bottom = item_bottom;
            }
            x += it->width + it->frame + gap + extra_gap;
            if (s->margin_auto_right && auto_extra > 0) {
                x += auto_extra;
            }
        }
        c->y = bottom;
        c->pending_margin = 0;
        line_start = line_end;
    }
    c->float_count = saved_floats;
    free(items);
}

static void layout_grid_children(struct ctx *c,
                                 const struct opos_html_node *node,
                                 struct style *st) {
    int avail = c->bwidth;
    int gap = st->gap;
    int columns = st->grid_count;
    int kinds[12];
    int sizes[12];
    for (int i = 0; i < columns; i++) {
        kinds[i] = st->grid_kind[i];
        sizes[i] = st->grid_size[i];
    }
    if (st->grid_fill_min > 0) {
        columns = (avail + gap) / (st->grid_fill_min + gap);
        if (columns < 1) {
            columns = 1;
        }
        if (columns > 12) {
            columns = 12;
        }
        for (int i = 0; i < columns; i++) {
            kinds[i] = 'f';
            sizes[i] = 100;
        }
    }
    struct flex_item *items = malloc(MAX_FLEX_ITEMS * sizeof(*items));
    if (items == NULL) {
        c->failed = true;
        return;
    }
    int count = 0;
    bool plain = true;
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        if (child->type == HTML_NODE_TEXT) {
            for (size_t i = 0; i < child->text_length; i++) {
                if (!is_ws(child->text[i])) {
                    plain = false;
                }
            }
            continue;
        }
        struct style *s = arena_alloc(c->page, sizeof(*s));
        if (s == NULL) {
            c->failed = true;
            break;
        }
        style_compute(child, st, s);
        if (s->display == DISPLAY_NONE) {
            continue;
        }
        if (count == MAX_FLEX_ITEMS || s->display == DISPLAY_INLINE_BLOCK
            || child->tag == HTML_TAG_IMG || child->tag == HTML_TAG_BR) {
            plain = false;
            break;
        }
        if (s->display == DISPLAY_INLINE) {
            s->display = DISPLAY_BLOCK;
        }
        items[count].node = child;
        items[count].style = s;
        count++;
    }
    if (!plain || count == 0 || c->failed) {
        free(items);
        process_children(c, node, st);
        return;
    }
    /* Column widths. */
    int widths[12];
    int fixed = gap * (columns - 1);
    int weight = 0;
    for (int i = 0; i < columns; i++) {
        widths[i] = 0;
        if (kinds[i] == 'p') {
            widths[i] = sizes[i];
        } else if (kinds[i] == 'c') {
            widths[i] = avail * sizes[i] / 100;
        } else if (kinds[i] == 'f') {
            weight += sizes[i];
        } else {
            /* auto: as wide as the widest item in the column */
            for (int k = i; k < count; k += columns) {
                struct style probe = *items[k].style;
                int w = measure_content(c, items[k].node, &probe, false);
                w += 2 * probe.border + probe.padding[LEFT]
                   + probe.padding[RIGHT] + probe.margin[LEFT]
                   + probe.margin[RIGHT];
                if (w > widths[i]) {
                    widths[i] = w;
                }
            }
        }
        if (kinds[i] != 'f') {
            fixed += widths[i];
        }
    }
    int rest = avail - fixed;
    if (rest < 0) {
        rest = 0;
    }
    for (int i = 0; i < columns; i++) {
        if (kinds[i] == 'f' && weight > 0) {
            widths[i] = rest * sizes[i] / weight;
        }
    }
    int saved_floats = c->float_count;
    for (int row = 0; row * columns < count && !c->failed; row++) {
        int x = c->bx;
        int top = c->y;
        int bottom = top;
        for (int col = 0; col < columns; col++) {
            int index = row * columns + col;
            if (index < count) {
                struct style item_style = *items[index].style;
                int frame = 2 * item_style.border + item_style.padding[LEFT]
                          + item_style.padding[RIGHT]
                          + item_style.margin[LEFT] + item_style.margin[RIGHT];
                int cw = clamp_min(widths[col] - frame, 1);
                c->float_count = 0;
                int item_bottom = layout_boxed(c, items[index].node,
                                               &item_style, x, cw, top);
                if (item_bottom > bottom) {
                    bottom = item_bottom;
                }
            }
            x += widths[col] + gap;
        }
        c->y = bottom + (row * columns + columns < count ? gap : 0);
        c->pending_margin = 0;
    }
    c->float_count = saved_floats;
    free(items);
}

/* ------------------------------------------------------------------ nodes */

static void process_node(struct ctx *c, const struct opos_html_node *node,
                         struct style *parent) {
    if (node->type == HTML_NODE_TEXT) {
        add_text(c, node->text, node->text_length, parent);
        return;
    }
    struct style *style = arena_alloc(c->page, sizeof(*style));
    if (style == NULL) {
        c->failed = true;
        return;
    }
    style_compute(node, parent, style);
    if (node->tag == HTML_TAG_OL || node->tag == HTML_TAG_UL
        || node->tag == HTML_TAG_MENU || node->tag == HTML_TAG_DIR) {
        style->ordered = node->tag == HTML_TAG_OL;
        style->list_index = int_attribute(node, "start", 1, -1000, 100000) - 1;
    } else if (node->tag == HTML_TAG_LI) {
        style->ordered = parent->ordered;
        parent->list_index++;
        style->list_index = int_attribute(node, "value", parent->list_index,
                                          -1000, 100000);
        parent->list_index = style->list_index;
    }
    if (style->display != DISPLAY_NONE && !c->measuring) {
        if (style->clear_both) {
            flush_inline(c);
            for (int i = 0; i < c->float_count; i++) {
                if (c->floats[i].bottom > c->y) {
                    c->y = c->floats[i].bottom;
                    c->pending_margin = 0;
                }
            }
        }
        if (style->float_side != 0 && c->float_count < MAX_FLOATS + 1) {
            layout_float(c, node, style);
            return;
        }
    }
    switch (style->display) {
    case DISPLAY_NONE:
        return;
    case DISPLAY_BLOCK:
        if (node->tag == HTML_TAG_HR) {
            layout_hr(c, style);
        } else {
            layout_block(c, node, style);
        }
        return;
    case DISPLAY_LIST_ITEM:
    case DISPLAY_TABLE_PART:
    case DISPLAY_TABLE_CELL:
        layout_block(c, node, style);
        return;
    case DISPLAY_TABLE:
        layout_table(c, node, style);
        return;
    case DISPLAY_INLINE_BLOCK:
        add_control(c, node, style);
        return;
    case DISPLAY_INLINE:
        if (node->tag == HTML_TAG_BR) {
            add_break(c, style);
        } else if (node->tag == HTML_TAG_IMG) {
            add_image(c, node, style);
        } else if (node->tag != HTML_TAG_WBR) {
            process_children(c, node, style);
        }
        return;
    }
}

/* ------------------------------------------------------------------- page */

static void free_arena(struct page *page) {
    struct arena_chunk *chunk = page->arena;
    while (chunk != NULL) {
        struct arena_chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    page->arena = NULL;
}

void page_free_layout(struct page *page) {
    page->item_count = 0;
    free_arena(page);
}

void layout_page(struct page *page, struct opos_font *fonts[FONT_COUNT],
                 int width) {
    page_free_layout(page);
    style_sheets = page->sheets;
    struct ctx c;
    memset(&c, 0, sizeof(c));
    c.page = page;
    c.fonts = fonts;
    c.bx = 0;
    c.bwidth = clamp_min(width, 40);
    c.align = ALIGN_LEFT;

    struct style root;
    memset(&root, 0, sizeof(root));
    root.display = DISPLAY_BLOCK;
    root.font_size = DEFAULT_FONT_SIZE;
    root.colour = DEFAULT_TEXT;
    root.border_colour = DEFAULT_TEXT;

    page->background = 0x00ffffff;
    struct style html_style, body_style;
    style_compute(page->doc->root, &root, &html_style);
    style_compute(page->doc->body, &html_style, &body_style);
    if (html_style.has_background) {
        page->background = html_style.background;
    }
    if (body_style.has_background) {
        page->background = body_style.background;
    }

    process_node(&c, page->doc->root, &root);
    flush_inline(&c);
    page->content_width = max_int(width, c.max_right);
    page->content_height = max_int(c.y + c.pending_margin, c.float_extent);
    free(c.inl);
    free(c.placed);
    free(c.measures);
}

void page_destroy(struct page *page) {
    form_fields_free(page);
    css_free(page->sheets);
    style_sheets = NULL;
    free_arena(page);
    free(page->items);
    for (size_t i = 0; i < page->image_count; i++) {
        free(page->images[i]->pixels);
        free(page->images[i]->scaled);
        free(page->images[i]);
    }
    free(page->images);
    opos_html_free(page->doc);
    memset(page, 0, sizeof(*page));
}

const struct display_item *page_item_at(const struct page *page, int x, int y) {
    for (size_t i = page->item_count; i > 0; i--) {
        const struct display_item *item = &page->items[i - 1];
        if ((item->kind == ITEM_TEXT || item->kind == ITEM_IMAGE)
            && x >= item->x && x < item->x + item->width && y >= item->y
            && y < item->y + item->height) {
            return item;
        }
    }
    return NULL;
}

const struct opos_html_node *page_control_at(const struct page *page, int x,
                                             int y) {
    for (size_t i = page->item_count; i > 0; i--) {
        const struct display_item *item = &page->items[i - 1];
        if (item->control != NULL && x >= item->x
            && x < item->x + item->width && y >= item->y
            && y < item->y + item->height) {
            return item->control;
        }
    }
    return NULL;
}
