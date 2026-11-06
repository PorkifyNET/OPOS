/*
 * OPOS browser: window, address bar, page loading, scrolling and painting.
 *
 *   browser [URL]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>

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
    char status[OPOS_HTTP_URL_MAX + 32];
    char history[HISTORY_MAX][OPOS_HTTP_URL_MAX];
    int history_count;
    int history_position;
    bool dirty;
    bool dragging_scrollbar;
};

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
        switch (item->kind) {
        case ITEM_RECT:
            fill_clipped(x, y, item->width, item->height, item->colour, top,
                         bottom);
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
    int status_y = b.height - STATUS_HEIGHT;
    opos_fill_rect(canvas, 0, status_y, b.width, STATUS_HEIGHT, COL_STATUS);
    opos_draw_line(canvas, 0, status_y, b.width, status_y,
                   OPOS_RGB(200, 200, 208));
    opos_font_draw(b.fonts[0], canvas, 6, status_y + 15, 12, b.status,
                   strlen(b.status), OPOS_RGB(70, 70, 80));
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

static __attribute__((noinline)) void fetch_sheet(const char *base,
                                                      const char *href,
                                                      struct css_sheet **chain) {
    char url[OPOS_HTTP_URL_MAX];
    if (!opos_url_resolve(base, href, url, sizeof(url))) {
        return;
    }
    char text[OPOS_HTTP_URL_MAX + 16];
    snprintf(text, sizeof(text), "Style %s", url);
    set_status(text);
    paint();
    struct opos_http_response response;
    if (opos_http_get(url, 1u << 20, &response) == 0) {
        if (response.status == 200) {
            *chain = css_append(*chain, css_parse((const char *)response.body,
                                                  response.length));
        }
        opos_http_free(&response);
    }
}

static void collect_sheets(struct opos_html_node *node, struct css_sheet **chain,
                           const char *base, int *fetched) {
    for (; node != NULL; node = node->next_sibling) {
        if (node->type != HTML_NODE_ELEMENT) {
            continue;
        }
        if (node->tag == HTML_TAG_STYLE) {
            const char *media = opos_html_attribute(node, "media");
            if (media != NULL && strstr(media, "print") != NULL) {
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
                && strstr(rel, "alternate") == NULL && *fetched < 8
                && (media == NULL || strstr(media, "print") == NULL)) {
                (*fetched)++;
                fetch_sheet(base, href, chain);
            }
        }
        collect_sheets(node->first_child, chain, base, fetched);
    }
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
    int fetched = 0;
    collect_sheets(doc->root, &b.page.sheets, url, &fetched);
    snprintf(b.page.url, sizeof(b.page.url), "%s", url);
    b.have_page = true;
    b.scroll = 0;
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
static void load(const char *url) {
    char text[OPOS_HTTP_URL_MAX + 16];
    set_address(url);
    snprintf(text, sizeof(text), "Loading %s", url);
    set_status(text);
    paint();

    struct opos_http_response response;
    opos_http_set_progress(progress, NULL);
    long error = opos_http_get(url, MAX_PAGE_BYTES, &response);
    opos_http_set_progress(NULL, NULL);
    if (error != 0) {
        char detail[160];
        if (error == -OPOS_EFBIG) {
            snprintf(detail, sizeof(detail), "The page is too large.");
        } else {
            snprintf(detail, sizeof(detail),
                     "Could not load the page (error %ld). Check the address"
                     " and the network.", -error);
        }
        show_message(url, "Cannot reach this page", detail);
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

static void navigate(const char *url, bool record) {
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
    load(url);
    if (record && b.history_position >= 0) {
        /* Remember where redirects ended up. */
        snprintf(b.history[b.history_position], sizeof(b.history[0]), "%s",
                 b.url);
    }
    b.dirty = true;
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

static void on_click(int x, int y) {
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
    const struct display_item *item = page_item_at(
        &b.page, x - MARGIN, y - TOOLBAR_HEIGHT - MARGIN + b.scroll);
    char url[OPOS_HTTP_URL_MAX];
    if (link_target(item, url, sizeof(url))) {
        navigate(url, true);
    }
}

static void on_move(int x, int y) {
    char url[OPOS_HTTP_URL_MAX];
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
        b.dirty = true;
        return;
    }
    if (ctrl && (key == 'r' || key == 'R')) {
        on_click(80, 10);
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
            b.dragging_scrollbar = false;
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
