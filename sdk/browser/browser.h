/*
 * OPOS browser: shared declarations (see docs/browser-plan.md).
 *
 *   style.c   default styles from the HTML tag and legacy attributes
 *   layout.c  block, inline, list and table layout into a display list
 *   main.c    window, address bar, loading, scrolling, painting
 */
#ifndef BROWSER_H
#define BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <opos_font.h>
#include <opos_gfx.h>
#include <opos_http.h>
#include <opos_html.h>
#include <opos_image.h>

#define FONT_COUNT 6 /* sans, bold, italic, bold italic, mono, mono bold */

/* ---------------------------------------------------------------- styles */

enum display {
    DISPLAY_NONE,
    DISPLAY_INLINE,
    DISPLAY_BLOCK,
    DISPLAY_LIST_ITEM,
    DISPLAY_TABLE,
    DISPLAY_TABLE_PART, /* row, row group, caption, column: handled by table */
    DISPLAY_TABLE_CELL,
    DISPLAY_INLINE_BLOCK, /* form controls */
    DISPLAY_CONTENTS      /* no box: children belong to the parent */
};

enum align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

struct var_scope;

struct style {
    const struct var_scope *vars; /* custom properties (css.c) */
    enum display display;
    int font_size;           /* pixels */
    bool bold, italic, mono;
    bool underline, strike;
    bool pre;                /* keep white space and line breaks */
    uint32_t colour;         /* 0x00RRGGBB */
    bool has_background;
    uint8_t background_alpha;  /* 255 opaque; less is blended */
    uint32_t background;
    int margin[4];           /* top, right, bottom, left */
    int padding[4];
    int border;              /* border width in pixels (all sides) */
    uint32_t border_colour;
    enum align align;
    struct opos_html_node *link; /* enclosing <a href> */
    int list_index;          /* <ol>: next number for <li>; <ul>: 0 */
    bool ordered;            /* the list this item belongs to is an <ol> */
    int width, max_width;    /* px, -1 = auto */
    int width_pct, max_width_pct; /* percent of the container, -1 = none */
    bool margin_auto_left, margin_auto_right;
    bool no_marker;          /* list-style: none */
    bool out_of_flow;        /* position absolute/fixed or hidden: not shown */
    bool opacity_zero;       /* opacity: 0 */
    bool grid_area_set;      /* grid-area or grid-row given: may share a cell */
    uint32_t grid_area_name; /* grid-area: <name> hash, 0 = none */
    uint8_t area_count;      /* grid-template-areas: names in the container */
    uint8_t area_start[8], area_span[8]; /* 0-based column, columns */
    uint32_t area_hash[8];
    int float_side;          /* 0 none, 1 left, 2 right */
    bool clear_both;
    bool flex, flex_wrap, flex_zero; /* flex_zero: flex: N (basis 0) */
    bool flex_column, grid;          /* flex-direction: column; display: grid */
    bool no_shrink;                  /* flex-shrink: 0 */
    int flex_grow, justify, gap;     /* justify: 0 start 1 center 2 end 3 between */
    int grid_count;                  /* tracks from grid-template-columns */
    int grid_start, grid_span;       /* grid-column: 1-based line (0 auto), tracks; span -1 = to the end */
    int grid_size[12];               /* px, percent or fr*100 */
    char grid_kind[12];              /* p px, c percent, f fr, a auto */
    int grid_fill_min;               /* repeat(auto-fill, minmax(N, 1fr)) */
};

#define TOP 0
#define RIGHT 1
#define BOTTOM 2
#define LEFT 3

#define DEFAULT_TEXT 0x00202020u
#define DEFAULT_LINK 0x000645ad
#define DEFAULT_FONT_SIZE 16

/* Parses "#rgb", "#rrggbb", "rgb(r,g,b)" and the usual colour names. */
bool parse_colour(const char *text, uint32_t *out);
/* Like parse_colour, and also gives the alpha (0 transparent, 255 opaque);
   understands #rgba, rgb()/rgba(), hsl()/hsla() and color-mix(). */
bool parse_colour_alpha(const char *text, uint32_t *out, int *alpha);

/* Parses an integer length ("12", "12px", "50%"). percent is set for "%".
   Returns false when there is no number. */
bool parse_length(const char *text, int *value, bool *percent);

/* Viewport size in pixels, for the vw, vh, vmin and vmax units. */
extern int viewport_width_px, viewport_height_px;

/* Fills style for element (display, sizes, weight, link colour, legacy
   attributes such as bgcolor, align and width) given its parent's style. */
void style_compute(const struct opos_html_node *element,
                   const struct style *parent, struct style *style);

/* ------------------------------------------------------------- display list */

enum item_kind {
    ITEM_TEXT,
    ITEM_RECT,     /* filled rectangle */
    ITEM_OUTLINE,  /* one-pixel rectangle outline */
    ITEM_IMAGE
};

struct image_ref;

struct display_item {
    enum item_kind kind;
    int x, y, width, height;   /* the item's box; text: y is the line top */
    uint32_t colour;
    uint8_t transparency;      /* rectangles: 0 opaque, 255 invisible */
    /* text */
    const char *text;
    size_t length;
    int font;                  /* index into the font table */
    int size;
    int baseline;              /* offset from y to the baseline */
    bool underline, strike;
    struct opos_html_node *link;
    /* image */
    struct image_ref *image;
    /* the form control a control's background rectangle belongs to */
    const struct opos_html_node *control;
    bool round;                /* rect/outline: a circle in the square box */
};

/* For a round item of the given diameter: how many pixels row `row`
   leaves empty at each end. -1 for the row past the end. */
static inline int round_inset(int size, int row) {
    if (row < 0 || row >= size) {
        return -1;
    }
    int dy = 2 * row + 1 - size;
    for (int col = 0; col < size / 2; col++) {
        int dx = 2 * col + 1 - size;
        if (dx * dx + dy * dy <= size * size) {
            return col;
        }
    }
    return size / 2;
}

enum field_kind {
    FIELD_NONE, FIELD_TEXT, FIELD_PASSWORD, FIELD_CHECKBOX, FIELD_RADIO,
    FIELD_SUBMIT, FIELD_BUTTON, FIELD_RESET, FIELD_HIDDEN, FIELD_TEXTAREA,
    FIELD_SELECT
};

/* The current state of one form control. */
struct form_field {
    const struct opos_html_node *node;
    enum field_kind kind;
    char *value;               /* text fields; malloc'd, NUL-terminated */
    size_t length;
    bool checked;              /* checkboxes and radio buttons */
    int option;                /* select: index of the chosen option */
};

/* An <img> of the page, loaded after the page is laid out. */
struct image_ref {
    const struct opos_html_node *node;
    char url[OPOS_HTTP_URL_MAX];
    int state;                 /* 0 not asked, 1 loaded, 2 failed */
    int width, height;         /* natural size once loaded */
    uint32_t *pixels;          /* decoded 0xAARRGGBB */
    uint32_t *scaled;          /* cached resample for scaled_width/height */
    int scaled_width, scaled_height;
};

struct css_sheet;

/* css.c */
struct css_sheet *css_parse(const char *text, size_t length);
struct css_sheet *css_append(struct css_sheet *chain, struct css_sheet *sheet);
void css_free(struct css_sheet *chain);
void css_reset_variables(void);
/* True when a media query ("print", "(max-width: 600px)", ...) applies to
   the browser window. */
bool css_media_matches(const char *query);
void css_apply(const struct css_sheet *chain,
               const struct opos_html_node *element, struct style *style,
               const struct style *parent);
void css_apply_block(struct style *style, const char *text,
                     const struct style *parent);
void apply_declaration(struct style *style, const char *name,
                       size_t name_length, const char *value,
                       const struct style *parent);
/* The style sheets used by style_compute (set by layout_page). */
extern const struct css_sheet *style_sheets;

struct page {
    struct opos_html_doc *doc;
    char url[OPOS_HTTP_URL_MAX];
    struct display_item *items;
    size_t item_count;
    size_t item_capacity;
    int content_width;         /* width laid out for */
    int content_height;        /* total height of the page */
    uint32_t background;
    struct image_ref **images;
    size_t image_count;
    size_t image_capacity;
    void *arena;               /* layout scratch (styles) */
    struct css_sheet *sheets;  /* author style sheets, in order */
    struct form_field *fields; /* form controls touched so far */
    size_t field_count;
    size_t field_capacity;
    const struct opos_html_node *focus; /* the text field being typed in */
};

/* form.c */
enum field_kind form_kind(const struct opos_html_node *node);
/* The control's state, created from its attributes on first use; NULL for
   elements that are not form controls (or without memory). */
struct form_field *form_field(struct page *page,
                              const struct opos_html_node *node);
bool field_set(struct form_field *field, const char *text, size_t length);
void form_fields_free(struct page *page);
const struct opos_html_node *form_option(const struct opos_html_node *select,
                                         int index);
int form_option_count(const struct opos_html_node *select);
const struct opos_html_node *form_of(const struct opos_html_node *node);
void form_check_radio(struct page *page, const struct opos_html_node *radio);
/* The form's data as application/x-www-form-urlencoded (malloc'd). */
char *form_encode(struct page *page, const struct opos_html_node *form,
                  const struct opos_html_node *submitter, size_t *length);

/* Lays the document out for a viewport width. Rebuilds the display list. */
void layout_page(struct page *page, struct opos_font *fonts[FONT_COUNT],
                 int width);

void page_free_layout(struct page *page);

/* Frees everything the page owns: layout, document, images. */
void page_destroy(struct page *page);

/* The item (link or not) under a point in page coordinates, or NULL. */
const struct display_item *page_item_at(const struct page *page, int x, int y);

/* The form control under a point in page coordinates, or NULL. */
const struct opos_html_node *page_control_at(const struct page *page, int x,
                                             int y);

#endif
