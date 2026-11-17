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
    DISPLAY_INLINE_BLOCK, /* form controls, inline-block/-flex/-grid boxes */
    DISPLAY_CONTENTS      /* no box: children belong to the parent */
};

enum align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

enum position { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED, POS_STICKY };

enum { BG_SIZE_AUTO, BG_SIZE_COVER, BG_SIZE_CONTAIN, BG_SIZE_EXPLICIT };

/* Background layers kept per box (CSS allows any number). */
#define BG_LAYERS 4

/* What the page area keeps clear around the content (px). */
#define PAGE_MARGIN 8

struct image_ref;

/* A CSS background image layer. All zero: none. */
struct background {
    const char *url;           /* as written; copied for the page; a
                                  gradient is "gradient:" + its CSS */
    bool gradient;             /* no size of its own: fills the box */
    uint8_t size_kind;         /* BG_SIZE_* */
    bool image_only;           /* no background colour */
    bool mask;                 /* a mask image: drawn in mask_colour */
    uint32_t mask_colour;
    bool no_repeat_x, no_repeat_y;
    int size[2];               /* explicit size: px or percent, -1 auto */
    bool size_pct[2];
    int position[2];           /* px or percent */
    bool position_pct[2];
    /* In a laid-out page: the layer's image and the layer under it. */
    struct image_ref *image;
    const struct background *below;
};

/* Copies text for the page being laid out (background URLs). */
extern char *(*style_copy_text)(const char *text, size_t length);
/* Zeroed memory that lives as long as the page being laid out. */
extern void *(*style_alloc)(size_t size);

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
    int height;              /* px, -1 = auto */
    int height_pct;          /* percent height (not laid out), 0 = none */
    struct background mask;  /* mask-image (icons drawn in a colour) */
    bool border_box;         /* box-sizing: border-box */
    struct background bg;    /* the top background layer */
    /* Layers under it (BG_LAYERS - 1 of them, page memory), or NULL. */
    struct background *bg_more;
    uint8_t bg_layers;       /* layers in use, counting bg */
    bool margin_auto_left, margin_auto_right;
    bool no_marker;          /* list-style: none */
    bool out_of_flow;        /* position absolute/fixed or hidden: not in
                                the flow (absolute boxes are placed later) */
    uint8_t position;        /* enum position */
    enum display flow_display; /* out-of-flow boxes: display before NONE */
    bool invisible;          /* visibility: hidden or collapse */
    bool clipped;            /* clip, clip-path or scale(0): hidden content */
    int offset[4];           /* top, right, bottom, left: px or percent */
    bool offset_set[4], offset_pct[4];
    int translate[2];        /* transform: translate(); px or percent */
    bool translate_pct[2];
    bool opacity_zero;       /* opacity: 0 */
    /* text-indent of -999px or less: text pushed out of view so that a
       background image stands in for it. */
    bool text_replaced;
    /* align-items (container) and align-self (item): 0 normal/auto,
       1 start, 2 center, 3 end. */
    uint8_t align_items, align_self;
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
    /* The element this style was computed for (text inherits it). */
    const struct opos_html_node *element;
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

#define PSEUDO_BEFORE 1
#define PSEUDO_AFTER 2
/* The style of element's ::before or ::after box (which: PSEUDO_*), with
   element_style as its parent. Returns its generated text (UTF-8, in
   text, NUL-terminated) and true, or false when it shows no text. */
bool style_compute_pseudo(const struct opos_html_node *element, int which,
                          const struct style *element_style,
                          struct style *style, char *text, size_t capacity);

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
    /* Text and images: the element they belong to (for script events). */
    const struct opos_html_node *element;
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
    /* image; on a rectangle: its background image, placed by bg */
    struct image_ref *image;
    const struct background *bg;
    /* the form control a control's background rectangle belongs to */
    const struct opos_html_node *control;
    /* Byte offset in the control value represented by this text item. */
    size_t field_offset;
    bool round;                /* rect/outline: a circle in the square box */
    bool fixed;                /* position: fixed; y is from the view's top */
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
    size_t selection_anchor;   /* byte offsets in value */
    size_t selection_cursor;
    bool checked;              /* checkboxes and radio buttons */
    int option;                /* select: index of the chosen option */
};

static inline size_t browser_utf8_next(const char *text, size_t length,
                                       size_t at) {
    if (at >= length) {
        return length;
    }
    at++;
    while (at < length && ((unsigned char)text[at] & 0xc0) == 0x80) {
        at++;
    }
    return at;
}

static inline size_t browser_utf8_previous(const char *text, size_t at) {
    if (at == 0) {
        return 0;
    }
    do {
        at--;
    } while (at > 0 && ((unsigned char)text[at] & 0xc0) == 0x80);
    return at;
}

static inline size_t browser_utf8_count(const char *text, size_t length) {
    size_t count = 0;
    for (size_t at = 0; at < length;
         at = browser_utf8_next(text, length, at)) {
        count++;
    }
    return count;
}

static inline size_t browser_utf8_offset(const char *text, size_t length,
                                         size_t characters) {
    size_t at = 0;
    while (characters-- > 0 && at < length) {
        at = browser_utf8_next(text, length, at);
    }
    return at;
}

/* An <img>, inline <svg> or CSS background image of the page. Images
   with a URL are loaded after the page is laid out. */
struct image_ref {
    const struct opos_html_node *node; /* <img>/<svg>; NULL: background */
    char url[OPOS_HTTP_URL_MAX];
    char *data_url;            /* a data: URL (malloc'd), instead of url */
    int state;                 /* 0 not asked, 1 loaded, 2 failed */
    int width, height;         /* natural size once loaded */
    uint32_t *pixels;          /* decoded 0xAARRGGBB (not for SVG) */
    const struct opos_html_node *svg;  /* SVG: its <svg> element */
    struct opos_html_doc *svg_doc;     /* SVG file: the document it is in */
    uint32_t colour;           /* SVG currentColor (the text colour) */
    uint32_t *scaled;          /* cached copy at scaled_width/height */
    int scaled_width, scaled_height;
    uint32_t scaled_colour;
};

/* image.c */
bool image_is_svg(const uint8_t *data, size_t length);
/* Decodes fetched bytes (PNG, JPEG, BMP or SVG); sets state 1 on success. */
bool image_ref_decode(struct image_ref *ref, const uint8_t *data,
                      size_t length);
/* Makes ref show an <svg> element of the page. */
void image_ref_inline_svg(struct image_ref *ref,
                          const struct opos_html_node *svg);
/* The bytes of a data: URL (malloc'd, NUL-terminated), or NULL. */
uint8_t *data_url_decode(const char *url, size_t *length);
/* Makes ref a CSS gradient ("linear-gradient(...)" or
   "radial-gradient(...)"); false when it cannot be drawn. */
bool image_ref_gradient(struct image_ref *ref, const char *css);
/* The image at width x height (cached in ref->scaled), or NULL. */
const uint32_t *image_ref_scaled(struct image_ref *ref, int width,
                                 int height);
void image_ref_free(struct image_ref *ref);
/* Draws the image scaled to width x height at x, y onto 0x00RRGGBB
   pixels, inside the clip rectangle. */
void image_blend(uint32_t *target, int stride, int target_width,
                 int target_height, struct image_ref *ref, int x, int y,
                 int width, int height, int clip_left, int clip_top,
                 int clip_right, int clip_bottom);
/* The first tile of a background image in a box. */
void background_tiles(const struct background *bg, struct image_ref *ref,
                      int box_x, int box_y, int box_width, int box_height,
                      int *tile_x, int *tile_y, int *tile_width,
                      int *tile_height);
/* Paints a rectangle item's background image (item->image, item->bg) at
   x, y onto 0x00RRGGBB pixels, inside the clip rectangle. */
void background_paint(uint32_t *target, int stride, int target_width,
                      int target_height, const struct display_item *item,
                      int x, int y, int clip_top, int clip_bottom);

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
/* Applies the rules for element's ::before or ::after (which: PSEUDO_*);
   false when no rule styles it. */
bool css_apply_pseudo(const struct css_sheet *chain,
                      const struct opos_html_node *element, int which,
                      struct style *style, const struct style *parent);
void css_apply_block(struct style *style, const char *text,
                     const struct style *parent);
void apply_declaration(struct style *style, const char *name,
                       size_t name_length, const char *value,
                       const struct style *parent);
/* Drops the matched rules cached for elements (after scripts changed the
   document). */
void css_forget_matches(struct css_sheet *chain);
/* A selector list for querySelector and matches(); NULL when no selector
   in it is supported. Free it with css_free. */
struct css_sheet *css_selector_parse(const char *selectors);
bool css_selector_matches(const struct css_sheet *selectors,
                          const struct opos_html_node *node);
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
    /* The #fragment target; layout_page sets anchor_y to its top (-1 when
       it was not laid out). */
    const struct opos_html_node *anchor;
    int anchor_y;
    struct script_state *script; /* JavaScript (script.c), or NULL */
};

/* True when pages run their scripts (the browser's JavaScript setting):
   <noscript> is hidden and @media (scripting: enabled) matches. */
extern bool scripting_enabled;

/* script.c: JavaScript. script_page_start runs the page's scripts and its
   DOMContentLoaded and load events; page_destroy calls script_page_end. */
void script_page_start(struct page *page);
void script_page_end(struct page *page);
/* Runs the timers that are due; returns milliseconds until the next one,
   or -1 for none. */
long script_run_timers(struct page *page);
/* A mouse event ("click") at target from the user; true when a script
   cancelled its default action. */
bool script_event(struct page *page, const struct opos_html_node *target,
                  const char *type, int x, int y, int button);
/* A key event ("keydown") at target (NULL: the body); modifiers: 1 Shift,
   2 Ctrl, 4 Alt. True when a script cancelled it. */
bool script_key(struct page *page, const struct opos_html_node *target,
                const char *type, const char *key, int code, int modifiers);
/* True once after scripts changed the document (lay it out again). */
bool script_take_dirty(struct page *page);
/* True once after a script set document.title. */
bool script_take_title(struct page *page);
/* A navigation a script asked for (location.href = ...). */
bool script_take_navigation(struct page *page, char *url, size_t capacity,
                            bool *replace);
/* Runs a javascript: URL on the page; false when it is not one. */
bool script_runs_javascript_url(struct page *page, const char *url);
int script_errors(const struct page *page);
/* main.c: shows text in the status bar at once. */
void browser_status(const char *text);

/* library.c: bookmarks, history and downloads. data_dir "" keeps them
   only in memory. */
void library_open(const char *data_dir);
bool bookmark_has(const char *url);
/* Adds or removes the bookmark; true when it is now bookmarked. */
bool bookmark_toggle(const char *url, const char *title);
void history_add(const char *url, const char *title);
void download_record(const char *name, const char *path,
                     unsigned long long size, bool failed);
/* A download running in the background: download_start returns its id
   for the other calls. */
int download_start(const char *name, const char *path);
void download_update(int id, unsigned long long received,
                     unsigned long long total);
void download_end(int id, unsigned long long size, bool failed,
                  bool cancelled);
/* main.c: stops a running download (about:downloads?cancel=ID). */
void download_cancel(int id);
/* The HTML of about:home, about:bookmarks (?remove=N), about:history
   (?clear), about:downloads (?cancel=ID), about:cookies (?remove=N,
   ?clear) or about:blank; malloc'd. */
char *about_page(const char *url, size_t *length);

/* cookies.c: the cookie jar, kept in data_dir/cookies.txt ("": in memory
   only). cookies_attach makes the HTTP library send cookies (and, with
   store, keep the ones responses set) until cookies_detach, which also
   saves them. */
void cookies_open(const char *data_dir);
void cookies_attach(bool store);
void cookies_detach(void);
void cookies_save(void);
/* Calls each for every site with cookies, sorted by name; returns the
   number of sites. */
int cookies_sites(void (*each)(void *context, int index, const char *site,
                               int count),
                  void *context);
/* Deletes the cookies of the site with that index (-1: all sites). */
void cookies_forget(int site);

/* svg.c: SVG images from a tree that opos_html_parse built. */
/* The first <svg> element at or under node, or NULL. */
const struct opos_html_node *svg_root(const struct opos_html_node *node);
/* The element with that id under root, else anywhere in document. */
const struct opos_html_node *svg_find_id(const struct opos_html_node *root,
                                         const struct opos_html_node *document,
                                         const char *id);
/* The natural size: width/height attributes, else the viewBox size, else
   300 x 150. */
bool svg_size(const struct opos_html_node *svg, int *width, int *height);
/* Draws the image at width x height; colour is currentColor. Returns
   malloc'd 0xAARRGGBB pixels, or NULL. */
uint32_t *svg_render(const struct opos_html_node *svg, int width, int height,
                     uint32_t colour);

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

/* The item (link or not) under a point in page coordinates, or NULL.
   scroll: how far the page is scrolled (fixed items do not move). */
const struct display_item *page_item_at(const struct page *page, int x, int y,
                                        int scroll);

/* The form control under a point in page coordinates, or NULL. */
const struct opos_html_node *page_control_at(const struct page *page, int x,
                                             int y, int scroll);

#endif
