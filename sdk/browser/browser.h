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
    DISPLAY_INLINE_BLOCK /* form controls */
};

enum align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

struct style {
    enum display display;
    int font_size;           /* pixels */
    bool bold, italic, mono;
    bool underline, strike;
    bool pre;                /* keep white space and line breaks */
    uint32_t colour;         /* 0x00RRGGBB */
    bool has_background;
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
    int float_side;          /* 0 none, 1 left, 2 right */
    bool clear_both;
    bool flex, flex_wrap, flex_zero; /* flex_zero: flex: N (basis 0) */
    int flex_grow, justify, gap;     /* justify: 0 start 1 center 2 end 3 between */
    int grid_count;                  /* tracks from grid-template-columns */
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
};

/* Lays the document out for a viewport width. Rebuilds the display list. */
void layout_page(struct page *page, struct opos_font *fonts[FONT_COUNT],
                 int width);

void page_free_layout(struct page *page);

/* Frees everything the page owns: layout, document, images. */
void page_destroy(struct page *page);

/* The item (link or not) under a point in page coordinates, or NULL. */
const struct display_item *page_item_at(const struct page *page, int x, int y);

#endif
