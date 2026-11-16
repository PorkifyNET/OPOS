/*
 * Writer: the OPOS word processor.
 *
 * Pages of formatted text: paragraph styles (Normal, Title, Heading 1-3),
 * bold, italic, underline, strikethrough, sizes, colours, two fonts,
 * alignment, bulleted and numbered lists. Opens and saves Word documents
 * (.docx, see writer_docx.h) and plain text (.txt). See docs/writer.md.
 *
 * Keys: Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as,
 * Ctrl+Z undo, Ctrl+Y redo, Ctrl+A select all, Ctrl+X/C/V cut, copy,
 * paste; Ctrl+B/I/U bold, italic, underline; Ctrl+L/E/R/J alignment;
 * Ctrl+0 Normal, Ctrl+1/2/3 headings; Ctrl+[ and Ctrl+] text size;
 * Ctrl+Shift+L bullets; Ctrl++ and Ctrl+- zoom; Shift+Enter line break;
 * Tab and Shift+Tab change a list item's level.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_font.h>
#include <opos_gfx.h>
#include <opos_http.h>

OPOS_MANIFEST("Writer", "1.0", OPOS_CAP_CLIPBOARD_READ);

/* ------------------------------------------------------------ constants */

/* The page at 100% zoom, in pixels of 1/96 inch: A4 with 2.54 cm
   margins. */
#define PAGE_W 794
#define PAGE_H 1123
#define MARGIN 96
#define PAGE_GAP 24
#define TAB_STEP 48       /* 1.27 cm */
#define LIST_INDENT 48    /* per list level */
#define LIST_LEVELS 5

#define ROW_H 40
#define TOOLBAR_H (2 * ROW_H)
#define STATUS_H 28
#define SCROLLBAR_W 14

#define PATH_MAX_LENGTH 256
#define UNDO_MAX 200
#define UNDO_BYTES (96u * 1024 * 1024)
#define TEXT_MAX (64u * 1024 * 1024) /* bytes in one document */

#define UI_BACK OPOS_RGB(30, 34, 46)
#define UI_PANEL OPOS_RGB(40, 46, 62)
#define UI_BUTTON OPOS_RGB(56, 64, 86)
#define UI_HOVER OPOS_RGB(72, 82, 110)
#define UI_ACTIVE OPOS_RGB(70, 110, 190)
#define UI_TEXT OPOS_RGB(230, 236, 248)
#define UI_DIM OPOS_RGB(130, 140, 165)
#define UI_WARN OPOS_RGB(245, 205, 110)
#define WORKSPACE OPOS_RGB(62, 66, 78)
#define PAGE_SHADOW OPOS_RGB(44, 47, 56)
#define SELECTION OPOS_RGB(180, 213, 254)
#define HEADING_BLUE 0x1f4e79u

/* ---------------------------------------------------------- the model */

/*
 * A character's look, packed: colour (bits 0-23), flags (24-31) and the
 * size in half points (32-47), the unit .docx files use.
 */
typedef uint64_t cstyle;
#define S_BOLD 1u
#define S_ITALIC 2u
#define S_UNDER 4u
#define S_STRIKE 8u
#define S_MONO 16u
#define ST_COLOR(s) ((uint32_t)((s) & 0xffffff))
#define ST_FLAGS(s) ((uint32_t)((s) >> 24 & 0xff))
#define ST_HALF(s) ((uint32_t)((s) >> 32 & 0xffff))

static cstyle st_make(uint32_t flags, uint32_t half, uint32_t colour) {
    return (cstyle)(colour & 0xffffff) | (cstyle)(flags & 0xff) << 24
        | (cstyle)(half & 0xffff) << 32;
}

static cstyle st_with_flags(cstyle s, uint32_t flags) {
    return st_make(flags, ST_HALF(s), ST_COLOR(s));
}

static cstyle st_with_half(cstyle s, uint32_t half) {
    return st_make(ST_FLAGS(s), half, ST_COLOR(s));
}

static cstyle st_with_colour(cstyle s, uint32_t colour) {
    return st_make(ST_FLAGS(s), ST_HALF(s), colour);
}

enum kind { K_NORMAL, K_TITLE, K_H1, K_H2, K_H3, K_COUNT };
enum list { L_NONE, L_BULLET, L_NUMBER };
enum align { A_LEFT, A_CENTER, A_RIGHT, A_JUSTIFY };

struct kind_info {
    const char *name;
    uint32_t flags;
    uint32_t half;
    uint32_t colour;
    int before, after; /* points */
};

static const struct kind_info kinds[K_COUNT] = {
    {"NORMAL", 0, 24, 0, 0, 8},
    {"TITLE", 0, 52, 0x1f2937, 0, 12},
    {"HEADING 1", S_BOLD, 40, HEADING_BLUE, 18, 6},
    {"HEADING 2", S_BOLD, 32, HEADING_BLUE, 14, 4},
    {"HEADING 3", S_BOLD, 26, HEADING_BLUE, 12, 4},
};

static cstyle kind_style(int kind) {
    return st_make(kinds[kind].flags, kinds[kind].half, kinds[kind].colour);
}

struct line {
    int start, end;   /* bytes of the paragraph */
    int width;        /* 1/64 px, without trailing spaces */
    int ascent, descent, height;
    int spaces;       /* stretchable spaces when justified */
    bool last;        /* ends the paragraph or a line break */
    int page, page_y; /* where pagination put it */
};

struct para {
    char *text;
    cstyle *st;       /* one per byte */
    int len, cap;
    cstyle end_style; /* the look of the paragraph mark (empty paragraph) */
    uint8_t kind, list, align, level;
    /* layout */
    bool dirty;
    struct line *lines;
    int nlines, lines_cap;
    int number;       /* list number, set by pagination */
};

static struct para *paras;
static int npara, para_cap;

/* The caret and the other end of the selection. */
static int cur_p, cur_o, anc_p, anc_o;
static cstyle type_style;    /* the look of typed text */
static int goal_x = -1;      /* kept by Up and Down, document pixels */

static int zoom = 100;
static int scroll_x, scroll_y;
static int view_width, view_height;
static struct opos_canvas canvas;
static struct opos_canvas *screen = &canvas;
static int pages = 1;
static unsigned doc_version;

static char path[PATH_MAX_LENGTH];
static char file_name[64] = "Untitled";
static bool can_write;   /* path came from opos_save_file in this run */
static bool modified;
static char message[112];
static bool message_warn;

enum pending { PENDING_NONE, PENDING_CLOSE, PENDING_NEW, PENDING_OPEN };
static enum pending pending;

static int mouse_x, mouse_y;
static bool shift_down, ctrl_down;
static bool close_removes_window; /* OPOS before 0.6.6 */

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
    const char *parts[3] = {"Writer - ", file_name, modified ? "*" : ""};
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

static int px(int value) {
    return value * zoom / 100;
}

static int pt_px(int points) { /* points to pixels at this zoom */
    return (points * 4 * zoom + 150) / 300;
}

static int next_char(const struct para *p, int o) {
    if (o >= p->len) {
        return p->len;
    }
    o++;
    while (o < p->len && ((unsigned char)p->text[o] & 0xc0) == 0x80) {
        o++;
    }
    return o;
}

static int prev_char(const struct para *p, int o) {
    if (o <= 0) {
        return 0;
    }
    o--;
    while (o > 0 && ((unsigned char)p->text[o] & 0xc0) == 0x80) {
        o--;
    }
    return o;
}

static bool is_word_byte(unsigned char c) {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z') || c == '_' || c == '\'';
}

/* ------------------------------------------------------- paragraphs */

static bool para_reserve(struct para *p, int need) {
    if (need <= p->cap) {
        return true;
    }
    int cap = p->cap < 16 ? 16 : p->cap;
    while (cap < need) {
        cap *= 2;
    }
    char *text = realloc(p->text, (size_t)cap);
    if (text == NULL) {
        return false;
    }
    p->text = text;
    cstyle *st = realloc(p->st, (size_t)cap * sizeof(cstyle));
    if (st == NULL) {
        return false;
    }
    p->st = st;
    p->cap = cap;
    return true;
}

static void para_free(struct para *p) {
    free(p->text);
    free(p->st);
    free(p->lines);
    memset(p, 0, sizeof(*p));
}

static void para_init(struct para *p, int kind) {
    memset(p, 0, sizeof(*p));
    p->kind = (uint8_t)kind;
    p->end_style = kind_style(kind);
    p->dirty = true;
}

/* Inserts text at o; styles has one entry per byte, or is NULL to use
   style for all of it. */
static bool para_insert(struct para *p, int o, const char *text, int length,
                        const cstyle *styles, cstyle style) {
    if (length <= 0) {
        return true;
    }
    if (!para_reserve(p, p->len + length)) {
        return false;
    }
    memmove(p->text + o + length, p->text + o, (size_t)(p->len - o));
    memmove(p->st + o + length, p->st + o,
            (size_t)(p->len - o) * sizeof(cstyle));
    memcpy(p->text + o, text, (size_t)length);
    for (int i = 0; i < length; i++) {
        p->st[o + i] = styles != NULL ? styles[i] : style;
    }
    p->len += length;
    p->dirty = true;
    return true;
}

static void para_delete(struct para *p, int from, int to) {
    if (to <= from) {
        return;
    }
    memmove(p->text + from, p->text + to, (size_t)(p->len - to));
    memmove(p->st + from, p->st + to, (size_t)(p->len - to) * sizeof(cstyle));
    p->len -= to - from;
    p->dirty = true;
}

/* Makes room for a paragraph at index at; returns it, initialised. */
static struct para *doc_insert_para(int at, int kind) {
    if (npara == para_cap) {
        int cap = para_cap < 64 ? 64 : para_cap * 2;
        struct para *grown = realloc(paras, (size_t)cap * sizeof(*paras));
        if (grown == NULL) {
            return NULL;
        }
        paras = grown;
        para_cap = cap;
    }
    memmove(paras + at + 1, paras + at,
            (size_t)(npara - at) * sizeof(*paras));
    npara++;
    para_init(&paras[at], kind);
    return &paras[at];
}

static void doc_remove_paras(int from, int to) { /* [from, to) */
    for (int i = from; i < to; i++) {
        para_free(&paras[i]);
    }
    memmove(paras + from, paras + to, (size_t)(npara - to) * sizeof(*paras));
    npara -= to - from;
}

static void doc_clear(void) {
    for (int i = 0; i < npara; i++) {
        para_free(&paras[i]);
    }
    npara = 0;
}

static size_t doc_bytes(void) {
    size_t total = 0;
    for (int i = 0; i < npara; i++) {
        total += (size_t)paras[i].len + 1;
    }
    return total;
}

/* The look a character typed at (p, o) takes. */
static cstyle style_at(int pi, int o) {
    const struct para *p = &paras[pi];
    if (o > 0 && o <= p->len && p->text[o - 1] != '\n') {
        return p->st[o - 1];
    }
    if (o < p->len) {
        return p->st[o];
    }
    if (o > 0) {
        return p->st[o - 1];
    }
    return p->end_style;
}

/* ------------------------------------------------------------- fonts */

static struct opos_font *fonts[2][4]; /* [mono][bold | italic << 1] */
static bool fonts_tried[2][4];

static struct opos_font *font_for(cstyle s) {
    uint32_t flags = ST_FLAGS(s);
    int mono = (flags & S_MONO) != 0;
    int variant = ((flags & S_BOLD) ? 1 : 0) | ((flags & S_ITALIC) ? 2 : 0);
    if (mono) {
        variant &= 1; /* Noto Sans Mono has no italic */
    }
    if (!fonts_tried[mono][variant]) {
        static const char *const names[2][4] = {
            {"/share/fonts/NotoSans-Regular.ttf",
             "/share/fonts/NotoSans-Bold.ttf",
             "/share/fonts/NotoSans-Italic.ttf",
             "/share/fonts/NotoSans-BoldItalic.ttf"},
            {"/share/fonts/NotoSansMono-Regular.ttf",
             "/share/fonts/NotoSansMono-Bold.ttf", NULL, NULL},
        };
        fonts_tried[mono][variant] = true;
        fonts[mono][variant] = opos_font_load(names[mono][variant]);
    }
    if (fonts[mono][variant] != NULL) {
        return fonts[mono][variant];
    }
    if (variant != 0) {
        return font_for(st_with_flags(s, flags & S_MONO));
    }
    if (mono) {
        return font_for(st_with_flags(s, 0));
    }
    return NULL;
}

static int size_px(cstyle s) { /* pixel height at this zoom */
    int size = (int)((ST_HALF(s) * 4 * (uint32_t)zoom + 300) / 600);
    return size < 3 ? 3 : size;
}

/* Advance of a character in 1/64 px (tabs are handled by the caller). */
static int advance(cstyle s, uint32_t code) {
    struct opos_font *font = font_for(s);
    if (font == NULL) {
        return size_px(s) * 32;
    }
    if (code < 0x20) {
        return 0;
    }
    return opos_font_advance(font, size_px(s), code);
}

static void metrics(cstyle s, int *ascent, int *descent) {
    struct opos_font *font = font_for(s);
    int size = size_px(s);
    if (font == NULL) {
        *ascent = size * 8 / 10;
        *descent = size / 5;
        return;
    }
    *ascent = opos_font_ascent(font, size);
    *descent = opos_font_descent(font, size);
}

/* ------------------------------------------------------------- layout */

static int para_indent(const struct para *p) { /* px at this zoom */
    return p->list != L_NONE ? px(LIST_INDENT * (p->level + 1)) : 0;
}

static int content_width(const struct para *p) {
    return px(PAGE_W - 2 * MARGIN) - para_indent(p);
}

static bool add_line(struct para *p, int start, int end) {
    if (p->nlines == p->lines_cap) {
        int cap = p->lines_cap < 4 ? 4 : p->lines_cap * 2;
        struct line *grown = realloc(p->lines, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        p->lines = grown;
        p->lines_cap = cap;
    }
    struct line *ln = &p->lines[p->nlines++];
    memset(ln, 0, sizeof(*ln));
    ln->start = start;
    ln->end = end;
    int ascent = 0, descent = 0;
    if (end == start) {
        cstyle s = start > 0 ? p->st[start - 1]
            : p->len > 0 ? p->st[0] : p->end_style;
        metrics(s, &ascent, &descent);
    }
    cstyle last = 0;
    bool have = false;
    for (int i = start; i < end; i++) {
        if (have && p->st[i] == last) {
            continue;
        }
        last = p->st[i];
        have = true;
        int a, d;
        metrics(last, &a, &d);
        if (a > ascent) {
            ascent = a;
        }
        if (d > descent) {
            descent = d;
        }
    }
    ln->ascent = ascent;
    ln->descent = descent;
    ln->height = (ascent + descent) * 115 / 100;
    if (ln->height < 1) {
        ln->height = 1;
    }
    /* Width without trailing spaces and the line break. */
    int stop = end;
    while (stop > start
           && (p->text[stop - 1] == ' ' || p->text[stop - 1] == '\n')) {
        stop--;
    }
    int x = 0, spaces = 0;
    for (int i = start; i < stop;) {
        uint32_t code;
        int used = (int)opos_utf8_decode(p->text + i, (size_t)(stop - i),
                                         &code);
        if (code == '\t') {
            int step = px(TAB_STEP) * 64;
            x = (x / step + 1) * step;
        } else {
            x += advance(p->st[i], code);
            if (code == ' ') {
                spaces++;
            }
        }
        i += used;
    }
    ln->width = x;
    ln->spaces = spaces;
    ln->last = end >= p->len || p->text[end - 1] == '\n';
    return true;
}

static void layout_para(struct para *p) {
    p->nlines = 0;
    p->dirty = false;
    int avail = content_width(p) * 64;
    int start = 0;
    while (start < p->len) {
        int x = 0, last_break = -1, end = p->len;
        for (int i = start; i < p->len;) {
            unsigned char c = (unsigned char)p->text[i];
            if (c == '\n') {
                end = i + 1;
                break;
            }
            uint32_t code;
            int used = (int)opos_utf8_decode(p->text + i,
                                             (size_t)(p->len - i), &code);
            int w;
            if (code == '\t') {
                int step = px(TAB_STEP) * 64;
                w = (x / step + 1) * step - x;
            } else {
                w = advance(p->st[i], code);
            }
            if (code == ' ') {
                last_break = i + 1;
            } else if (x + w > avail && i > start) {
                end = last_break > start ? last_break : i;
                break;
            }
            x += w;
            i += used;
        }
        add_line(p, start, end);
        start = end;
    }
    if (p->len == 0 || p->text[p->len - 1] == '\n') {
        add_line(p, p->len, p->len);
    }
}

static int space_after(int i) {
    const struct para *p = &paras[i];
    if (p->list != L_NONE && i + 1 < npara && paras[i + 1].list != L_NONE) {
        return pt_px(2);
    }
    return pt_px(kinds[p->kind].after);
}

/* Lays out changed paragraphs and places every line on a page. */
static void paginate(void) {
    int content_h = px(PAGE_H - 2 * MARGIN);
    int page = 0, y = 0;
    int counters[LIST_LEVELS] = {0};
    for (int i = 0; i < npara; i++) {
        struct para *p = &paras[i];
        if (p->dirty) {
            layout_para(p);
        }
        if (p->list == L_NONE) {
            memset(counters, 0, sizeof(counters));
        } else {
            for (int l = p->level + 1; l < LIST_LEVELS; l++) {
                counters[l] = 0;
            }
            if (p->list == L_NUMBER) {
                p->number = ++counters[p->level];
            }
        }
        if (y > 0) {
            y += pt_px(kinds[p->kind].before);
        }
        for (int l = 0; l < p->nlines; l++) {
            struct line *ln = &p->lines[l];
            if (y > 0 && y + ln->height > content_h) {
                page++;
                y = 0;
            }
            ln->page = page;
            ln->page_y = y;
            y += ln->height;
        }
        y += space_after(i);
        if (y >= content_h) {
            page++;
            y = 0;
        }
    }
    /* A last page left empty by spacing alone is not a page. */
    if (y == 0 && page > 0) {
        const struct para *last = &paras[npara - 1];
        if (last->lines[last->nlines - 1].page < page) {
            page--;
        }
    }
    pages = page + 1;
}

static void relayout_all(void) {
    for (int i = 0; i < npara; i++) {
        paras[i].dirty = true;
    }
}

/* ----------------------------------------------------------- geometry */

static int page_w(void) { return px(PAGE_W); }
static int page_h(void) { return px(PAGE_H); }

static int doc_view_w(void) { return view_width - SCROLLBAR_W; }
static int doc_view_h(void) { return view_height - TOOLBAR_H - STATUS_H; }

static int doc_height(void) {
    return pages * (page_h() + PAGE_GAP) + PAGE_GAP;
}

static int doc_width(void) {
    return page_w() + 2 * PAGE_GAP;
}

/* Screen position of a page's top left corner. */
static void page_origin(int page, int *x, int *y) {
    int width = doc_width();
    int left = width < doc_view_w() ? (doc_view_w() - width) / 2 : -scroll_x;
    *x = left + PAGE_GAP;
    *y = TOOLBAR_H + PAGE_GAP + page * (page_h() + PAGE_GAP) - scroll_y;
}

static void clamp_scroll(void) {
    int max_y = doc_height() - doc_view_h();
    if (scroll_y > max_y) {
        scroll_y = max_y;
    }
    if (scroll_y < 0) {
        scroll_y = 0;
    }
    int max_x = doc_width() - doc_view_w();
    if (scroll_x > max_x) {
        scroll_x = max_x;
    }
    if (scroll_x < 0) {
        scroll_x = 0;
    }
}

/* Line positions: x[k] for every byte k of the line (1/64 px from the
   text's left edge, after alignment), x[length] after the last one. */
static int *line_x;
static int line_x_cap;

static int line_offset(const struct para *p, const struct line *ln) {
    int avail = content_width(p) * 64;
    int extra = avail - ln->width;
    if (extra <= 0) {
        return 0;
    }
    if (p->align == A_CENTER) {
        return extra / 2;
    }
    if (p->align == A_RIGHT) {
        return extra;
    }
    return 0;
}

static bool line_positions(const struct para *p, const struct line *ln) {
    int length = ln->end - ln->start;
    if (length + 1 > line_x_cap) {
        int cap = length + 64;
        int *grown = realloc(line_x, (size_t)cap * sizeof(int));
        if (grown == NULL) {
            return false;
        }
        line_x = grown;
        line_x_cap = cap;
    }
    int x = line_offset(p, ln);
    int stretch = 0, stretch_left = 0;
    if (p->align == A_JUSTIFY && !ln->last && ln->spaces > 0) {
        stretch = content_width(p) * 64 - ln->width;
        if (stretch < 0) {
            stretch = 0;
        }
        stretch_left = ln->spaces;
    }
    int origin = x;
    for (int i = ln->start; i < ln->end;) {
        uint32_t code;
        int used = (int)opos_utf8_decode(p->text + i, (size_t)(ln->end - i),
                                         &code);
        for (int k = 0; k < used; k++) {
            line_x[i - ln->start + k] = x;
        }
        if (code == '\t') {
            int step = px(TAB_STEP) * 64;
            x = origin + ((x - origin) / step + 1) * step;
        } else if (code != '\n') {
            x += advance(p->st[i], code);
            if (code == ' ' && stretch_left > 0) {
                int add = stretch / stretch_left;
                x += add;
                stretch -= add;
                stretch_left--;
            }
        }
        i += used;
    }
    line_x[length] = x;
    return true;
}

static int find_line(const struct para *p, int o) {
    for (int l = 0; l < p->nlines; l++) {
        if (o < p->lines[l].end || l == p->nlines - 1) {
            /* A line break belongs to its line; the caret after it is on
               the next one. */
            if (o == p->lines[l].end && l + 1 < p->nlines) {
                continue;
            }
            return l;
        }
    }
    return 0;
}

/* Screen position of the caret spot (p, o): x, line top and height. */
static void caret_spot(int pi, int o, int *x, int *top, int *height,
                       int *baseline) {
    struct para *p = &paras[pi];
    int l = find_line(p, o);
    struct line *ln = &p->lines[l];
    int ox, oy;
    page_origin(ln->page, &ox, &oy);
    line_positions(p, ln);
    *x = ox + px(MARGIN) + para_indent(p) + (line_x[o - ln->start] + 32) / 64;
    *top = oy + px(MARGIN) + ln->page_y;
    *height = ln->height;
    *baseline = *top + ln->ascent;
}

/* The text spot nearest to a screen point. */
static void hit(int sx, int sy, int *out_p, int *out_o) {
    int best_p = 0, best_l = 0, best_d = -1;
    for (int i = 0; i < npara; i++) {
        struct para *p = &paras[i];
        for (int l = 0; l < p->nlines; l++) {
            struct line *ln = &p->lines[l];
            int ox, oy;
            page_origin(ln->page, &ox, &oy);
            int top = oy + px(MARGIN) + ln->page_y;
            int d = sy < top ? top - sy
                : sy >= top + ln->height ? sy - (top + ln->height) + 1 : 0;
            if (best_d < 0 || d < best_d) {
                best_d = d;
                best_p = i;
                best_l = l;
            }
            if (d == 0) {
                goto found;
            }
        }
    }
found:;
    struct para *p = &paras[best_p];
    struct line *ln = &p->lines[best_l];
    int ox, oy;
    page_origin(ln->page, &ox, &oy);
    line_positions(p, ln);
    int rx = (sx - ox - px(MARGIN) - para_indent(p)) * 64;
    int end = ln->end;
    /* Not after a line break or the space a wrapped line ends with. */
    if (end > ln->start && (p->text[end - 1] == '\n'
                            || (best_l + 1 < p->nlines
                                && p->text[end - 1] == ' '))) {
        end = prev_char(p, end);
    } else if (best_l + 1 < p->nlines && end > ln->start) {
        end = prev_char(p, end); /* a word broken in the middle */
    }
    int o = ln->start;
    while (o < end) {
        int next = next_char(p, o);
        int mid = (line_x[o - ln->start] + line_x[next - ln->start]) / 2;
        if (rx < mid) {
            break;
        }
        o = next;
    }
    *out_p = best_p;
    *out_o = o;
}

static void scroll_to_caret(void) {
    paginate();
    int x, top, height, baseline;
    caret_spot(cur_p, cur_o, &x, &top, &height, &baseline);
    int view_top = TOOLBAR_H, view_bottom = TOOLBAR_H + doc_view_h();
    if (top < view_top + 8) {
        scroll_y -= view_top + 8 - top;
    } else if (top + height > view_bottom - 8) {
        scroll_y += top + height - (view_bottom - 8);
    }
    if (x < 8) {
        scroll_x -= 8 - x;
    } else if (x > doc_view_w() - 8) {
        scroll_x += x - (doc_view_w() - 8);
    }
    clamp_scroll();
}

/* --------------------------------------------------------- selection */

static bool has_selection(void) {
    return cur_p != anc_p || cur_o != anc_o;
}

/* The selection in document order. */
static void sel_range(int *p1, int *o1, int *p2, int *o2) {
    if (anc_p < cur_p || (anc_p == cur_p && anc_o < cur_o)) {
        *p1 = anc_p, *o1 = anc_o, *p2 = cur_p, *o2 = cur_o;
    } else {
        *p1 = cur_p, *o1 = cur_o, *p2 = anc_p, *o2 = anc_o;
    }
}

static void caret_moved(bool extend) {
    if (!extend) {
        anc_p = cur_p;
        anc_o = cur_o;
    }
    type_style = style_at(cur_p, cur_o);
}

/* -------------------------------------------------------------- undo */

struct snap_para {
    char *text;
    cstyle *st;
    int len;
    cstyle end_style;
    uint8_t kind, list, align, level;
};

struct snap {
    struct snap_para *paras;
    int npara;
    int cur_p, cur_o, anc_p, anc_o;
    size_t bytes;
};

static struct snap undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int undo_count, redo_count;
static size_t undo_total;
enum edit { EDIT_NONE, EDIT_TYPE, EDIT_DELETE, EDIT_OTHER };
static enum edit last_edit = EDIT_NONE;

static void snap_free(struct snap *s) {
    for (int i = 0; i < s->npara; i++) {
        free(s->paras[i].text);
        free(s->paras[i].st);
    }
    free(s->paras);
    memset(s, 0, sizeof(*s));
}

static bool snap_take(struct snap *s) {
    memset(s, 0, sizeof(*s));
    s->paras = calloc((size_t)npara, sizeof(*s->paras));
    if (s->paras == NULL) {
        return false;
    }
    s->npara = npara;
    for (int i = 0; i < npara; i++) {
        struct para *p = &paras[i];
        struct snap_para *q = &s->paras[i];
        q->len = p->len;
        q->end_style = p->end_style;
        q->kind = p->kind;
        q->list = p->list;
        q->align = p->align;
        q->level = p->level;
        if (p->len > 0) {
            q->text = malloc((size_t)p->len);
            q->st = malloc((size_t)p->len * sizeof(cstyle));
            if (q->text == NULL || q->st == NULL) {
                snap_free(s);
                return false;
            }
            memcpy(q->text, p->text, (size_t)p->len);
            memcpy(q->st, p->st, (size_t)p->len * sizeof(cstyle));
        }
        s->bytes += (size_t)p->len * (1 + sizeof(cstyle)) + sizeof(*q);
    }
    s->cur_p = cur_p;
    s->cur_o = cur_o;
    s->anc_p = anc_p;
    s->anc_o = anc_o;
    return true;
}

static bool snap_restore(const struct snap *s) {
    doc_clear();
    for (int i = 0; i < s->npara; i++) {
        const struct snap_para *q = &s->paras[i];
        struct para *p = doc_insert_para(npara, q->kind);
        if (p == NULL) {
            return false;
        }
        p->end_style = q->end_style;
        p->list = q->list;
        p->align = q->align;
        p->level = q->level;
        para_insert(p, 0, q->text, q->len, q->st, 0);
    }
    if (npara == 0) {
        doc_insert_para(0, K_NORMAL);
    }
    cur_p = s->cur_p < npara ? s->cur_p : npara - 1;
    cur_o = s->cur_o <= paras[cur_p].len ? s->cur_o : paras[cur_p].len;
    anc_p = s->anc_p < npara ? s->anc_p : cur_p;
    anc_o = s->anc_o <= paras[anc_p].len ? s->anc_o : cur_o;
    return true;
}

static void stack_drop_oldest(struct snap *stack, int *count) {
    undo_total -= stack[0].bytes;
    snap_free(&stack[0]);
    memmove(stack, stack + 1, (size_t)(*count - 1) * sizeof(*stack));
    (*count)--;
}

static void stack_clear(struct snap *stack, int *count) {
    while (*count > 0) {
        undo_total -= stack[*count - 1].bytes;
        snap_free(&stack[--*count]);
    }
}

/* Remembers the document before a change. Typing and deleting in a row
   make one undo step. */
static void remember(enum edit kind) {
    doc_version++;
    if (kind != EDIT_OTHER && kind == last_edit) {
        return;
    }
    last_edit = kind;
    stack_clear(redo_stack, &redo_count);
    struct snap s;
    if (!snap_take(&s)) {
        say("NOT ENOUGH MEMORY TO UNDO THIS", true);
        return;
    }
    while (undo_count > 0
           && (undo_count == UNDO_MAX || undo_total + s.bytes > UNDO_BYTES)) {
        stack_drop_oldest(undo_stack, &undo_count);
    }
    undo_stack[undo_count++] = s;
    undo_total += s.bytes;
}

static void step(struct snap *from, int *from_count, struct snap *to,
                 int *to_count) {
    if (*from_count == 0) {
        say(from == undo_stack ? "NOTHING TO UNDO" : "NOTHING TO REDO", false);
        return;
    }
    struct snap now;
    if (!snap_take(&now)) {
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    if (*to_count == UNDO_MAX) {
        stack_drop_oldest(to, to_count);
    }
    to[(*to_count)++] = now;
    undo_total += now.bytes;
    struct snap *back = &from[--*from_count];
    snap_restore(back);
    undo_total -= back->bytes;
    snap_free(back);
    last_edit = EDIT_NONE;
    doc_version++;
    type_style = style_at(cur_p, cur_o);
    goal_x = -1;
    set_modified(true);
    scroll_to_caret();
}

static void undo(void) { step(undo_stack, &undo_count, redo_stack, &redo_count); }
static void redo(void) { step(redo_stack, &redo_count, undo_stack, &undo_count); }

/* ----------------------------------------------------------- editing */

static void changed(void) {
    set_modified(true);
    goal_x = -1;
    scroll_to_caret();
}

/* Deletes from (p1, o1) to (p2, o2), in document order. */
static void delete_range(int p1, int o1, int p2, int o2) {
    if (p1 == p2) {
        para_delete(&paras[p1], o1, o2);
    } else {
        struct para *a = &paras[p1], *b = &paras[p2];
        para_delete(a, o1, a->len);
        para_insert(a, a->len, b->text + o2, b->len - o2, b->st + o2, 0);
        if (a->len == 0 && o1 == 0) {
            a->end_style = b->end_style;
        }
        doc_remove_paras(p1 + 1, p2 + 1);
    }
    cur_p = anc_p = p1;
    cur_o = anc_o = o1;
}

static bool delete_selection(void) {
    if (!has_selection()) {
        return false;
    }
    int p1, o1, p2, o2;
    sel_range(&p1, &o1, &p2, &o2);
    delete_range(p1, o1, p2, o2);
    return true;
}

/* Splits the paragraph at the caret (Enter). */
static void split_here(void) {
    struct para *p = &paras[cur_p];
    int kind = p->kind;
    bool at_end = cur_o == p->len;
    int next_kind = kind != K_NORMAL && at_end ? K_NORMAL : kind;
    struct para *q = doc_insert_para(cur_p + 1, next_kind);
    if (q == NULL) {
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    p = &paras[cur_p];
    q->list = p->list;
    q->align = next_kind == kind ? p->align : A_LEFT;
    q->level = p->level;
    if (at_end) {
        q->end_style = next_kind == kind ? style_at(cur_p, cur_o)
            : kind_style(K_NORMAL) | (style_at(cur_p, cur_o) & ((cstyle)S_MONO << 24));
    } else {
        q->end_style = p->st[cur_o];
    }
    para_insert(q, 0, p->text + cur_o, p->len - cur_o, p->st + cur_o, 0);
    para_delete(p, cur_o, p->len);
    if (p->len == 0) {
        p->end_style = type_style;
    }
    cur_p++;
    cur_o = 0;
    anc_p = cur_p;
    anc_o = 0;
    type_style = q->len > 0 ? q->st[0] : q->end_style;
}

static void insert_text(const char *text, int length, cstyle style) {
    struct para *p = &paras[cur_p];
    if (doc_bytes() + (size_t)length > TEXT_MAX) {
        say("THE DOCUMENT IS TOO LARGE", true);
        return;
    }
    if (!para_insert(p, cur_o, text, length, NULL, style)) {
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    cur_o += length;
    anc_o = cur_o;
    anc_p = cur_p;
}

static void type_char(uint32_t code) {
    char bytes[4];
    int length;
    if (code < 0x80) {
        bytes[0] = (char)code;
        length = 1;
    } else {
        return; /* the keyboard sends ASCII only */
    }
    remember(code == ' ' ? EDIT_OTHER : EDIT_TYPE);
    if (code == ' ') {
        last_edit = EDIT_TYPE; /* the next word starts a new undo step */
    }
    if (delete_selection()) {
        type_style = style_at(cur_p, cur_o);
    }
    cstyle style = type_style;
    insert_text(bytes, length, style);
    type_style = style;
    changed();
}

static void key_enter(bool line_break) {
    remember(EDIT_OTHER);
    delete_selection();
    struct para *p = &paras[cur_p];
    if (line_break) {
        cstyle style = type_style;
        insert_text("\n", 1, style);
        type_style = style;
    } else if (p->list != L_NONE && p->len == 0) {
        p->list = L_NONE; /* Enter on an empty item ends the list */
        p->level = 0;
        p->dirty = true;
    } else {
        split_here();
    }
    last_edit = EDIT_NONE;
    changed();
}

static void key_backspace(bool word) {
    if (has_selection()) {
        remember(EDIT_OTHER);
        delete_selection();
        type_style = style_at(cur_p, cur_o);
        changed();
        return;
    }
    struct para *p = &paras[cur_p];
    if (cur_o == 0) {
        if (p->list != L_NONE) {
            remember(EDIT_OTHER);
            p->list = L_NONE;
            p->level = 0;
            p->dirty = true;
            changed();
            return;
        }
        if (cur_p == 0) {
            return;
        }
        remember(EDIT_OTHER);
        struct para *prev = &paras[cur_p - 1];
        if (prev->len == 0 && p->len > 0) {
            doc_remove_paras(cur_p - 1, cur_p);
            cur_p--;
        } else {
            int at = prev->len;
            delete_range(cur_p - 1, at, cur_p, 0);
        }
        anc_p = cur_p;
        anc_o = cur_o;
        type_style = style_at(cur_p, cur_o);
        changed();
        return;
    }
    remember(EDIT_DELETE);
    int from = prev_char(p, cur_o);
    if (word) {
        while (from > 0 && p->text[from - 1] == ' ') {
            from--;
        }
        while (from > 0 && is_word_byte((unsigned char)p->text[from - 1])) {
            from--;
        }
        if (from == cur_o) {
            from = prev_char(p, cur_o);
        }
    }
    para_delete(p, from, cur_o);
    cur_o = anc_o = from;
    type_style = style_at(cur_p, cur_o);
    changed();
}

static void key_delete(void) {
    if (has_selection()) {
        key_backspace(false);
        return;
    }
    struct para *p = &paras[cur_p];
    if (cur_o == p->len) {
        if (cur_p + 1 >= npara) {
            return;
        }
        remember(EDIT_OTHER);
        delete_range(cur_p, cur_o, cur_p + 1, 0);
        changed();
        return;
    }
    remember(EDIT_DELETE);
    para_delete(p, cur_o, next_char(p, cur_o));
    anc_o = cur_o;
    changed();
}

/* ----------------------------------------------------------- formatting */

/* Calls change on the style of every selected character (or the typing
   style when nothing is selected). */
static void restyle(cstyle (*change)(cstyle, uint32_t), uint32_t value) {
    if (!has_selection()) {
        type_style = change(type_style, value);
        if (paras[cur_p].len == 0) {
            paras[cur_p].end_style = type_style;
            paras[cur_p].dirty = true;
        }
        return;
    }
    remember(EDIT_OTHER);
    int p1, o1, p2, o2;
    sel_range(&p1, &o1, &p2, &o2);
    for (int i = p1; i <= p2; i++) {
        struct para *p = &paras[i];
        int from = i == p1 ? o1 : 0;
        int to = i == p2 ? o2 : p->len;
        for (int k = from; k < to; k++) {
            p->st[k] = change(p->st[k], value);
        }
        if (to == p->len) {
            p->end_style = change(p->end_style, value);
        }
        p->dirty = true;
    }
    type_style = style_at(cur_p, cur_o);
    set_modified(true);
}

/* True when every selected character has the flag. */
static bool selection_has(uint32_t flag) {
    if (!has_selection()) {
        return (ST_FLAGS(type_style) & flag) != 0;
    }
    int p1, o1, p2, o2;
    sel_range(&p1, &o1, &p2, &o2);
    for (int i = p1; i <= p2; i++) {
        struct para *p = &paras[i];
        int from = i == p1 ? o1 : 0;
        int to = i == p2 ? o2 : p->len;
        for (int k = from; k < to; k++) {
            if (p->text[k] != '\n' && !(ST_FLAGS(p->st[k]) & flag)) {
                return false;
            }
        }
    }
    return true;
}

static cstyle change_set_flag(cstyle s, uint32_t flag) {
    return st_with_flags(s, ST_FLAGS(s) | flag);
}

static cstyle change_clear_flag(cstyle s, uint32_t flag) {
    return st_with_flags(s, ST_FLAGS(s) & ~flag);
}

static cstyle change_half(cstyle s, uint32_t half) {
    return st_with_half(s, half);
}

static cstyle change_colour(cstyle s, uint32_t colour) {
    return st_with_colour(s, colour);
}

static const uint32_t sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24,
                                 26, 28, 36, 48, 72};
#define SIZE_COUNT ((int)(sizeof(sizes) / sizeof(sizes[0])))

static cstyle change_size_step(cstyle s, uint32_t up) {
    uint32_t half = ST_HALF(s);
    if (up) {
        for (int i = 0; i < SIZE_COUNT; i++) {
            if (sizes[i] * 2 > half) {
                return st_with_half(s, sizes[i] * 2);
            }
        }
        return s;
    }
    for (int i = SIZE_COUNT - 1; i >= 0; i--) {
        if (sizes[i] * 2 < half) {
            return st_with_half(s, sizes[i] * 2);
        }
    }
    return s;
}

static void toggle_flag(uint32_t flag) {
    bool on = selection_has(flag);
    restyle(on ? change_clear_flag : change_set_flag, flag);
}

static void set_font(bool mono) {
    restyle(mono ? change_set_flag : change_clear_flag, S_MONO);
}

/* The paragraphs the caret or selection touches. */
static void para_span(int *first, int *last) {
    int p1, o1, p2, o2;
    sel_range(&p1, &o1, &p2, &o2);
    *first = p1;
    *last = p2;
}

static void set_align(int align) {
    remember(EDIT_OTHER);
    int first, last;
    para_span(&first, &last);
    for (int i = first; i <= last; i++) {
        paras[i].align = (uint8_t)align;
        paras[i].dirty = true;
    }
    set_modified(true);
}

static void set_kind(int kind) {
    remember(EDIT_OTHER);
    int first, last;
    para_span(&first, &last);
    cstyle base = kind_style(kind);
    for (int i = first; i <= last; i++) {
        struct para *p = &paras[i];
        /* The style's size, weight and colour; italic, underline,
           strikethrough and the font stay. */
        uint32_t keep = S_ITALIC | S_UNDER | S_STRIKE | S_MONO;
        for (int k = 0; k < p->len; k++) {
            p->st[k] = st_with_flags(base, (ST_FLAGS(p->st[k]) & keep)
                                     | ST_FLAGS(base));
        }
        p->end_style = st_with_flags(base, (ST_FLAGS(p->end_style) & keep)
                                     | ST_FLAGS(base));
        p->kind = (uint8_t)kind;
        if (kind != K_NORMAL) {
            p->list = L_NONE;
            p->level = 0;
        }
        p->dirty = true;
    }
    type_style = style_at(cur_p, cur_o);
    set_modified(true);
}

static void toggle_list(int list) {
    remember(EDIT_OTHER);
    int first, last;
    para_span(&first, &last);
    bool all = true;
    for (int i = first; i <= last; i++) {
        if (paras[i].list != list) {
            all = false;
        }
    }
    for (int i = first; i <= last; i++) {
        paras[i].list = (uint8_t)(all ? L_NONE : list);
        if (all) {
            paras[i].level = 0;
        }
        paras[i].dirty = true;
    }
    set_modified(true);
}

static void change_level(int delta) {
    int first, last;
    para_span(&first, &last);
    remember(EDIT_OTHER);
    for (int i = first; i <= last; i++) {
        struct para *p = &paras[i];
        if (p->list == L_NONE) {
            if (delta > 0) {
                p->list = L_BULLET;
                p->level = 0;
            }
        } else {
            int level = p->level + delta;
            if (level < 0) {
                p->list = L_NONE;
                level = 0;
            }
            p->level = (uint8_t)(level >= LIST_LEVELS ? LIST_LEVELS - 1
                                                      : level);
        }
        p->dirty = true;
    }
    set_modified(true);
}

/* ---------------------------------------------------------- clipboard */

/* What Writer copied last, with its formatting: pasted when the
   clipboard still holds the same text. */
static struct snap copied;
static char *copied_text;
static size_t copied_length;

static char *selection_text(size_t *length) {
    int p1, o1, p2, o2;
    sel_range(&p1, &o1, &p2, &o2);
    size_t total = 0;
    for (int i = p1; i <= p2; i++) {
        total += (size_t)paras[i].len + 1;
    }
    char *out = malloc(total + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t used = 0;
    for (int i = p1; i <= p2; i++) {
        int from = i == p1 ? o1 : 0;
        int to = i == p2 ? o2 : paras[i].len;
        memcpy(out + used, paras[i].text + from, (size_t)(to - from));
        used += (size_t)(to - from);
        if (i < p2) {
            out[used++] = '\n';
        }
    }
    out[used] = '\0';
    *length = used;
    return out;
}

static void copy_selection(void) {
    if (!has_selection()) {
        return;
    }
    size_t length;
    char *text = selection_text(&length);
    if (text == NULL) {
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    if (length > 65536) {
        say("ONLY THE FIRST 64 KB REACH OTHER PROGRAMS", true);
    }
    opos_clipboard_set(text, length);
    free(copied_text);
    copied_text = text;
    copied_length = length > 65536 ? 65536 : length;
    /* The selected part of the document, with its formatting. */
    snap_free(&copied);
    int p1, o1, p2, o2;
    sel_range(&p1, &o1, &p2, &o2);
    copied.paras = calloc((size_t)(p2 - p1 + 1), sizeof(*copied.paras));
    if (copied.paras == NULL) {
        return;
    }
    copied.npara = p2 - p1 + 1;
    for (int i = p1; i <= p2; i++) {
        struct para *p = &paras[i];
        struct snap_para *q = &copied.paras[i - p1];
        int from = i == p1 ? o1 : 0;
        int to = i == p2 ? o2 : p->len;
        q->len = to - from;
        q->end_style = p->end_style;
        q->kind = p->kind;
        q->list = p->list;
        q->align = p->align;
        q->level = p->level;
        if (q->len > 0) {
            q->text = malloc((size_t)q->len);
            q->st = malloc((size_t)q->len * sizeof(cstyle));
            if (q->text == NULL || q->st == NULL) {
                snap_free(&copied);
                return;
            }
            memcpy(q->text, p->text + from, (size_t)q->len);
            memcpy(q->st, p->st + from, (size_t)q->len * sizeof(cstyle));
        }
    }
}

static void cut_selection(void) {
    if (!has_selection()) {
        return;
    }
    copy_selection();
    remember(EDIT_OTHER);
    delete_selection();
    type_style = style_at(cur_p, cur_o);
    changed();
}

static void paste_rich(void) {
    for (int i = 0; i < copied.npara; i++) {
        const struct snap_para *q = &copied.paras[i];
        if (i > 0) {
            split_here();
            struct para *p = &paras[cur_p];
            p->kind = q->kind;
            p->list = q->list;
            p->align = q->align;
            p->level = q->level;
            p->end_style = q->end_style;
        }
        if (q->len > 0) {
            if (!para_insert(&paras[cur_p], cur_o, q->text, q->len, q->st, 0)) {
                say("NOT ENOUGH MEMORY", true);
                break;
            }
            cur_o += q->len;
        }
    }
    anc_p = cur_p;
    anc_o = cur_o;
}

static void paste(void) {
    static char buffer[65537];
    long length = opos_clipboard_get(buffer, sizeof(buffer) - 1);
    if (length == -OPOS_EACCES) {
        /* What Writer copied itself needs no permission. */
        if (copied.npara > 0) {
            remember(EDIT_OTHER);
            delete_selection();
            paste_rich();
            last_edit = EDIT_NONE;
            changed();
            return;
        }
        say("WRITER MAY NOT READ THE CLIPBOARD (SECURITY CENTER)", true);
        return;
    }
    if (length < 0) {
        say("COULD NOT READ THE CLIPBOARD", true);
        return;
    }
    if (length > (long)sizeof(buffer) - 1) {
        length = sizeof(buffer) - 1;
    }
    if (length == 0) {
        return;
    }
    if (doc_bytes() + (size_t)length > TEXT_MAX) {
        say("THE DOCUMENT IS TOO LARGE", true);
        return;
    }
    remember(EDIT_OTHER);
    delete_selection();
    if (copied.npara > 0 && copied_text != NULL
        && (size_t)length == copied_length
        && memcmp(buffer, copied_text, (size_t)length) == 0) {
        paste_rich();
    } else {
        cstyle style = type_style;
        long start = 0;
        for (long i = 0; i <= length; i++) {
            if (i == length || buffer[i] == '\n') {
                long end = i;
                if (end > start && buffer[end - 1] == '\r') {
                    end--;
                }
                /* Control characters other than tabs are left out. */
                long w = start;
                for (long k = start; k < end; k++) {
                    unsigned char c = (unsigned char)buffer[k];
                    if (c >= 0x20 || c == '\t') {
                        buffer[w++] = (char)c;
                    }
                }
                insert_text(buffer + start, (int)(w - start), style);
                if (i < length) {
                    split_here();
                }
                start = i + 1;
            }
        }
        type_style = style;
    }
    last_edit = EDIT_NONE;
    changed();
}

/* -------------------------------------------------------------- files */

static void new_document(void) {
    doc_clear();
    doc_insert_para(0, K_NORMAL);
    cur_p = cur_o = anc_p = anc_o = 0;
    type_style = kind_style(K_NORMAL);
    scroll_x = scroll_y = 0;
    goal_x = -1;
    stack_clear(undo_stack, &undo_count);
    stack_clear(redo_stack, &redo_count);
    last_edit = EDIT_NONE;
    doc_version++;
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

static void do_new(void) {
    new_document();
    path[0] = '\0';
    strcpy(file_name, "Untitled");
    can_write = false;
    modified = true;
    set_modified(false);
    say("", false);
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
        size_t chunk = capacity - used < 65536 ? capacity - used : 65536;
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

static bool write_whole_file(const char *file, const void *data, size_t size) {
    long fd = opos_open(file, OPOS_O_WRONLY | OPOS_O_CREAT | OPOS_O_TRUNC);
    bool ok = fd >= 0;
    size_t done = 0;
    while (ok && done < size) {
        size_t chunk = size - done > 1048576 ? 1048576 : size - done;
        long wrote = opos_write((int)fd, (const uint8_t *)data + done, chunk);
        if (wrote <= 0) {
            ok = false;
        } else {
            done += (size_t)wrote;
        }
    }
    if (fd >= 0 && opos_close((int)fd) < 0) {
        ok = false;
    }
    return ok;
}

/* A growing byte buffer for building files. */
struct buf {
    char *data;
    size_t length, capacity;
    bool failed;
};

static void buf_put(struct buf *b, const void *data, size_t length) {
    if (b->failed) {
        return;
    }
    if (b->length + length + 1 > b->capacity) {
        size_t capacity = b->capacity < 4096 ? 4096 : b->capacity;
        while (b->length + length + 1 > capacity) {
            capacity *= 2;
        }
        char *grown = realloc(b->data, capacity);
        if (grown == NULL) {
            b->failed = true;
            return;
        }
        b->data = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, data, length);
    b->length += length;
    b->data[b->length] = '\0';
}

static void buf_puts(struct buf *b, const char *text) {
    buf_put(b, text, strlen(text));
}

static void buf_byte(struct buf *b, uint8_t value) {
    buf_put(b, &value, 1);
}

static void buf_int(struct buf *b, long value) {
    char digits[24];
    snprintf(digits, sizeof(digits), "%ld", value);
    buf_puts(b, digits);
}

/* Appends one paragraph from text, decoding UTF-8 (other bytes are taken
   as Windows-1252). */
static const uint16_t cp1252[32] = {
    0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
    0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178,
};

static int utf8_put(char *out, uint32_t code) {
    if (code < 0x80) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800) {
        out[0] = (char)(0xc0 | code >> 6);
        out[1] = (char)(0x80 | (code & 0x3f));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = (char)(0xe0 | code >> 12);
        out[1] = (char)(0x80 | (code >> 6 & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | code >> 18);
    out[1] = (char)(0x80 | (code >> 12 & 0x3f));
    out[2] = (char)(0x80 | (code >> 6 & 0x3f));
    out[3] = (char)(0x80 | (code & 0x3f));
    return 4;
}

static bool valid_utf8(const uint8_t *s, size_t length) {
    for (size_t i = 0; i < length;) {
        uint8_t c = s[i];
        size_t need = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1
            : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 9;
        if (need == 9 || (c >= 0x80 && c < 0xc2)) {
            return false;
        }
        for (size_t k = 1; k <= need; k++) {
            if (i + k >= length || (s[i + k] & 0xc0) != 0x80) {
                return false;
            }
        }
        i += need + 1;
    }
    return true;
}

static bool load_text(const uint8_t *data, size_t size) {
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        data += 3;
        size -= 3;
    }
    bool utf8 = valid_utf8(data, size);
    doc_clear();
    cstyle style = kind_style(K_NORMAL);
    struct para *p = doc_insert_para(0, K_NORMAL);
    char out[4];
    for (size_t i = 0; i < size && p != NULL; i++) {
        uint8_t c = data[i];
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            p = doc_insert_para(npara, K_NORMAL);
            continue;
        }
        if (c < 0x20 && c != '\t') {
            continue;
        }
        int n;
        if (c < 0x80 || utf8) {
            out[0] = (char)c;
            n = 1;
        } else {
            n = utf8_put(out, c < 0xa0 ? cp1252[c - 0x80] : c);
        }
        if (!para_insert(p, p->len, out, n, NULL, style)) {
            return false;
        }
    }
    return p != NULL;
}

static void bullet_text(const struct para *p, char *out, size_t capacity);

static bool save_text(const char *file) {
    struct buf b = {0};
    for (int i = 0; i < npara; i++) {
        const struct para *p = &paras[i];
        if (p->list == L_BULLET) {
            for (int l = 0; l < p->level; l++) {
                buf_puts(&b, "  ");
            }
            buf_puts(&b, "- ");
        } else if (p->list == L_NUMBER) {
            for (int l = 0; l < p->level; l++) {
                buf_puts(&b, "  ");
            }
            char mark[16];
            bullet_text(p, mark, sizeof(mark));
            buf_puts(&b, mark);
            buf_puts(&b, " ");
        }
        buf_put(&b, p->text, (size_t)p->len);
        if (i + 1 < npara) {
            buf_puts(&b, "\n");
        }
    }
    if (b.failed) {
        free(b.data);
        return false;
    }
    bool ok = write_whole_file(file, b.data != NULL ? b.data : "", b.length);
    free(b.data);
    return ok;
}

#include "writer_docx.h"

static bool ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    if (b > a) {
        return false;
    }
    for (size_t i = 0; i < b; i++) {
        if (tolower((unsigned char)text[a - b + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

static void open_path(const char *chosen) {
    size_t size;
    uint8_t *data = read_whole_file(chosen, &size);
    if (data == NULL) {
        say("COULD NOT READ THAT FILE", true);
        return;
    }
    if (size >= 8 && memcmp(data, "\xd0\xcf\x11\xe0", 4) == 0) {
        free(data);
        say("OLD WORD FILES (.DOC) DO NOT OPEN YET; SAVE AS .DOCX ELSEWHERE",
            true);
        return;
    }
    bool zip = size >= 4 && memcmp(data, "PK\3\4", 4) == 0;
    if (zip && !ends_with(chosen, ".docx") && !ends_with(chosen, ".docm")
        && !ends_with(chosen, ".dotx")) {
        free(data);
        say("WRITER OPENS WORD (.DOCX) AND TEXT FILES", true);
        return;
    }
    if (!zip && (ends_with(chosen, ".odt") || ends_with(chosen, ".rtf")
                 || ends_with(chosen, ".pdf"))) {
        free(data);
        say("WRITER OPENS WORD (.DOCX) AND TEXT FILES", true);
        return;
    }
    new_document();
    const char *problem = NULL;
    bool lossy = false;
    if (zip) {
        problem = docx_load(data, size, &lossy);
    } else if (!load_text(data, size)) {
        problem = "NOT ENOUGH MEMORY TO OPEN IT";
    }
    free(data);
    if (npara == 0) {
        doc_insert_para(0, K_NORMAL);
    }
    cur_p = cur_o = anc_p = anc_o = 0;
    type_style = style_at(0, 0);
    if (problem != NULL) {
        new_document();
        say(problem, true);
        return;
    }
    memcpy(path, chosen, strlen(chosen) + 1);
    set_name_from_path();
    can_write = false; /* reading a file does not allow writing it */
    modified = true;
    set_modified(false);
    scroll_x = scroll_y = 0;
    say(lossy ? "OPENED; PICTURES, TABLES AND SOME OTHER PARTS ARE NOT SHOWN YET"
              : "OPENED", lossy);
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
    open_path(chosen);
}

static bool write_document(const char *file) {
    paginate(); /* list numbers */
    bool text = ends_with(file, ".txt") || ends_with(file, ".md");
    bool ok = text ? save_text(file) : docx_save(file);
    if (!ok) {
        say("COULD NOT SAVE THE DOCUMENT", true);
    } else if (text) {
        say("SAVED AS PLAIN TEXT; FORMATTING IS NOT KEPT IN .TXT FILES",
            false);
    } else {
        say("SAVED", false);
    }
    return ok;
}

static bool do_save_as(void) {
    char suggested[PATH_MAX_LENGTH + 8];
    /* A new document starts in the Documents folder. */
    char start[PATH_MAX_LENGTH] = "Document.docx";
    struct opos_user_info user;
    if (path[0] != '/' && opos_user_info(&user) == 0 && user.home[0] == '/'
        && strlen(user.home) + 24 < sizeof(start)) {
        snprintf(start, sizeof(start), "%s/Documents/Document.docx",
                 user.home);
    }
    const char *from = path[0] == '/' ? path : start;
    size_t length = strlen(from);
    memcpy(suggested, from, length + 1);
    /* Documents opened from other formats are saved as .docx. */
    if (!ends_with(suggested, ".docx") && !ends_with(suggested, ".txt")) {
        char *dot = strrchr(suggested, '.');
        char *slash = strrchr(suggested, '/');
        if (dot != NULL && dot != suggested
            && (slash == NULL || dot > slash + 1)) {
            *dot = '\0';
            length = (size_t)(dot - suggested);
        }
        if (length + 6 < sizeof(suggested)) {
            memcpy(suggested + length, ".docx", 6);
        }
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
    if (!write_document(chosen)) {
        return false;
    }
    memcpy(path, chosen, strlen(chosen) + 1);
    set_name_from_path();
    can_write = true;
    modified = true;
    set_modified(false);
    return true;
}

static bool do_save(void) {
    if (!can_write) {
        return do_save_as();
    }
    if (!write_document(path)) {
        return false;
    }
    set_modified(false);
    return true;
}

/* ----------------------------------------------------------- toolbar */

struct box {
    int x, y, w, h;
};

static bool inside(struct box box, int x, int y) {
    return x >= box.x && y >= box.y && x < box.x + box.w
        && y < box.y + box.h;
}

enum button {
    B_NEW, B_OPEN, B_SAVE, B_SAVE_AS, B_UNDO, B_REDO, B_CUT, B_COPY,
    B_PASTE, B_ZOOM_OUT, B_ZOOM, B_ZOOM_IN,
    B_STYLE, B_FONT, B_SIZE_DOWN, B_SIZE, B_SIZE_UP, B_BOLD, B_ITALIC,
    B_UNDER, B_STRIKE, B_COLOUR, B_LEFT, B_CENTER, B_RIGHT, B_JUSTIFY,
    B_BULLETS, B_NUMBERS, B_OUTDENT, B_INDENT,
    B_COUNT
};

static struct box boxes[B_COUNT];

enum menu { MENU_NONE, MENU_STYLE, MENU_FONT, MENU_COLOUR, MENU_SIZE };
static enum menu menu;

static const uint32_t palette[16] = {
    0x000000, 0x404040, 0x7f7f7f, 0xbfbfbf,
    0xc00000, 0xe46c0a, 0xbf9000, 0x00b050,
    0x00b0f0, 0x0070c0, 0x1f4e79, 0x7030a0,
    0xff0000, 0xffc000, 0x92d050, 0xff66cc,
};
static uint32_t last_colour = 0xc00000;

static void layout_toolbar(void) {
    static const char *const row1[] = {"NEW", "OPEN", "SAVE", "SAVE AS",
                                       "UNDO", "REDO", "CUT", "COPY",
                                       "PASTE"};
    int x = 8;
    for (int i = 0; i <= B_PASTE; i++) {
        int w = opos_text_width(row1[i], 2) + 20;
        boxes[i] = (struct box){x, 6, w, ROW_H - 12};
        x += w + 6;
        if (i == B_SAVE_AS || i == B_REDO) {
            x += 14;
        }
    }
    int right = view_width - 8;
    boxes[B_ZOOM_IN] = (struct box){right - 28, 6, 28, ROW_H - 12};
    boxes[B_ZOOM] = (struct box){right - 28 - 4 - 64, 6, 64, ROW_H - 12};
    boxes[B_ZOOM_OUT] = (struct box){right - 28 - 4 - 64 - 4 - 28, 6, 28,
                                     ROW_H - 12};
    int y = ROW_H + 4, h = ROW_H - 10;
    x = 8;
    boxes[B_STYLE] = (struct box){x, y, 150, h};
    x += 150 + 6;
    boxes[B_FONT] = (struct box){x, y, 84, h};
    x += 84 + 14;
    boxes[B_SIZE_DOWN] = (struct box){x, y, 26, h};
    x += 26 + 2;
    boxes[B_SIZE] = (struct box){x, y, 40, h};
    x += 40 + 2;
    boxes[B_SIZE_UP] = (struct box){x, y, 26, h};
    x += 26 + 14;
    for (int i = B_BOLD; i <= B_STRIKE; i++) {
        boxes[i] = (struct box){x, y, 30, h};
        x += 30 + 4;
    }
    boxes[B_COLOUR] = (struct box){x, y, 44, h};
    x += 44 + 14;
    for (int i = B_LEFT; i <= B_JUSTIFY; i++) {
        boxes[i] = (struct box){x, y, 30, h};
        x += 30 + 4;
    }
    x += 10;
    for (int i = B_BULLETS; i <= B_INDENT; i++) {
        boxes[i] = (struct box){x, y, 30, h};
        x += 30 + 4;
    }
}

static void fill(struct box box, uint32_t colour) {
    opos_fill_rect(screen, box.x, box.y, box.w, box.h, colour);
}

static void draw_button_base(struct box box, bool active) {
    bool hover = inside(box, mouse_x, mouse_y) && pending == PENDING_NONE;
    fill(box, active ? UI_ACTIVE : hover ? UI_HOVER : UI_BUTTON);
}

static void draw_label(struct box box, const char *label, uint32_t colour) {
    int width = opos_text_width(label, 2);
    opos_draw_text(screen, box.x + (box.w - width) / 2 + 1,
                   box.y + (box.h - opos_char_height(2)) / 2, label, colour,
                   2);
}

static void draw_button(struct box box, const char *label, bool active,
                        bool enabled) {
    draw_button_base(box, active);
    draw_label(box, label, enabled ? UI_TEXT : UI_DIM);
}

/* A letter in a TrueType face, for B, I, U and S. */
static void draw_face_letter(struct box box, const char *letter, cstyle s,
                             uint32_t colour) {
    struct opos_font *font = font_for(s);
    if (font == NULL) {
        draw_label(box, letter, colour);
        return;
    }
    int size = 18;
    int w = opos_font_text_width(font, size, letter, strlen(letter));
    int x = box.x + (box.w - w) / 2;
    int base = box.y + box.h / 2 + opos_font_ascent(font, size) / 2 - 1;
    opos_font_draw(font, screen, x, base, size, letter, strlen(letter),
                   colour);
    if (ST_FLAGS(s) & S_UNDER) {
        opos_fill_rect(screen, x, base + 2, w, 2, colour);
    }
    if (ST_FLAGS(s) & S_STRIKE) {
        opos_fill_rect(screen, x - 1, base - opos_font_ascent(font, size) / 3,
                       w + 2, 2, colour);
    }
}

static void draw_align_icon(struct box box, int align, uint32_t ink) {
    int widths[4] = {16, 10, 16, 12};
    int x0 = box.x + (box.w - 16) / 2, y0 = box.y + (box.h - 13) / 2;
    for (int i = 0; i < 4; i++) {
        int w = align == A_JUSTIFY ? 16 : widths[i];
        int x = align == A_LEFT || align == A_JUSTIFY ? x0
            : align == A_RIGHT ? x0 + 16 - w : x0 + (16 - w) / 2;
        opos_fill_rect(screen, x, y0 + i * 4, w, 2, ink);
    }
}

static void draw_list_icon(struct box box, int which, uint32_t ink) {
    int x0 = box.x + (box.w - 18) / 2, y0 = box.y + (box.h - 14) / 2;
    for (int i = 0; i < 3; i++) {
        int y = y0 + i * 6;
        if (which == 0) {
            opos_fill_rect(screen, x0, y, 3, 3, ink);
        } else {
            char digit[2] = {(char)('1' + i), '\0'};
            opos_draw_text(screen, x0 - 1, y - 3, digit, ink, 1);
        }
        opos_fill_rect(screen, x0 + 6, y + 1, 12, 2, ink);
    }
}

static void draw_indent_icon(struct box box, bool in, uint32_t ink) {
    int x0 = box.x + (box.w - 18) / 2, y0 = box.y + (box.h - 14) / 2;
    for (int i = 0; i < 4; i++) {
        int w = i == 0 || i == 3 ? 18 : 10;
        opos_fill_rect(screen, x0 + 18 - w, y0 + i * 4, w, 2, ink);
    }
    /* An arrow pointing right (in) or left (out). */
    for (int k = 0; k < 3; k++) {
        int x = in ? x0 + k : x0 + 2 - k;
        opos_fill_rect(screen, x, y0 + 4 + k, 1, 7 - 2 * k, ink);
    }
}

/* The look under the caret, or the first selected character's. */
static cstyle shown_style(void) {
    if (!has_selection()) {
        return type_style;
    }
    int p1, o1, p2, o2;
    sel_range(&p1, &o1, &p2, &o2);
    if (o1 < paras[p1].len) {
        return paras[p1].st[o1];
    }
    return style_at(p2, o2);
}

static void draw_toolbar(void) {
    opos_fill_rect(screen, 0, 0, view_width, TOOLBAR_H, UI_BACK);
    opos_fill_rect(screen, 0, ROW_H, view_width, 1, UI_PANEL);
    static const char *const row1[] = {"NEW", "OPEN", "SAVE", "SAVE AS",
                                       "UNDO", "REDO", "CUT", "COPY",
                                       "PASTE"};
    for (int i = 0; i <= B_PASTE; i++) {
        bool enabled = !((i == B_UNDO && undo_count == 0)
                         || (i == B_REDO && redo_count == 0)
                         || ((i == B_CUT || i == B_COPY) && !has_selection()));
        if (boxes[i].x + boxes[i].w < boxes[B_ZOOM_OUT].x - 8) {
            draw_button(boxes[i], row1[i], false, enabled);
        }
    }
    draw_button(boxes[B_ZOOM_OUT], "-", false, zoom > 50);
    char zoom_text[8];
    snprintf(zoom_text, sizeof(zoom_text), "%d%%", zoom);
    fill(boxes[B_ZOOM], UI_PANEL);
    draw_label(boxes[B_ZOOM], zoom_text, UI_TEXT);
    draw_button(boxes[B_ZOOM_IN], "+", false, zoom < 200);

    cstyle s = shown_style();
    struct para *p = &paras[cur_p];
    draw_button_base(boxes[B_STYLE], menu == MENU_STYLE);
    opos_draw_text(screen, boxes[B_STYLE].x + 8,
                   boxes[B_STYLE].y + (boxes[B_STYLE].h - 18) / 2,
                   kinds[p->kind].name, UI_TEXT, 2);
    opos_draw_text(screen, boxes[B_STYLE].x + boxes[B_STYLE].w - 16,
                   boxes[B_STYLE].y + (boxes[B_STYLE].h - 9) / 2, "V",
                   UI_DIM, 1);
    draw_button_base(boxes[B_FONT], menu == MENU_FONT);
    opos_draw_text(screen, boxes[B_FONT].x + 8,
                   boxes[B_FONT].y + (boxes[B_FONT].h - 18) / 2,
                   (ST_FLAGS(s) & S_MONO) ? "MONO" : "SANS", UI_TEXT, 2);
    opos_draw_text(screen, boxes[B_FONT].x + boxes[B_FONT].w - 16,
                   boxes[B_FONT].y + (boxes[B_FONT].h - 9) / 2, "V", UI_DIM,
                   1);
    draw_button(boxes[B_SIZE_DOWN], "-", false, true);
    char size_text[8];
    uint32_t half = ST_HALF(s);
    if (half % 2 == 0) {
        snprintf(size_text, sizeof(size_text), "%u", half / 2);
    } else {
        snprintf(size_text, sizeof(size_text), "%u.5", half / 2);
    }
    draw_button_base(boxes[B_SIZE], menu == MENU_SIZE);
    draw_label(boxes[B_SIZE], size_text, UI_TEXT);
    draw_button(boxes[B_SIZE_UP], "+", false, true);

    static const char *const letters[4] = {"B", "I", "U", "S"};
    static const uint32_t flags[4] = {S_BOLD, S_ITALIC, S_UNDER, S_STRIKE};
    for (int i = 0; i < 4; i++) {
        bool on = selection_has(flags[i]);
        draw_button_base(boxes[B_BOLD + i], on);
        draw_face_letter(boxes[B_BOLD + i], letters[i],
                         st_make(flags[i], 24, 0), UI_TEXT);
    }
    /* Colour: an "A" over a bar in the last colour used; the right part
       opens the palette. */
    struct box colour_box = boxes[B_COLOUR];
    draw_button_base(colour_box, menu == MENU_COLOUR);
    struct box letter = {colour_box.x, colour_box.y, 30, colour_box.h - 4};
    draw_face_letter(letter, "A", st_make(S_BOLD, 24, 0), UI_TEXT);
    opos_fill_rect(screen, colour_box.x + 6, colour_box.y + colour_box.h - 7,
                   18, 4, last_colour);
    opos_draw_text(screen, colour_box.x + 32,
                   colour_box.y + (colour_box.h - 9) / 2, "V", UI_DIM, 1);
    for (int i = 0; i < 4; i++) {
        draw_button_base(boxes[B_LEFT + i], p->align == i);
        draw_align_icon(boxes[B_LEFT + i], i, UI_TEXT);
    }
    draw_button_base(boxes[B_BULLETS], p->list == L_BULLET);
    draw_list_icon(boxes[B_BULLETS], 0, UI_TEXT);
    draw_button_base(boxes[B_NUMBERS], p->list == L_NUMBER);
    draw_list_icon(boxes[B_NUMBERS], 1, UI_TEXT);
    draw_button_base(boxes[B_OUTDENT], false);
    draw_indent_icon(boxes[B_OUTDENT], false, UI_TEXT);
    draw_button_base(boxes[B_INDENT], false);
    draw_indent_icon(boxes[B_INDENT], true, UI_TEXT);
}

/* -------------------------------------------------------------- menus */

static struct box menu_item(int index) {
    if (menu == MENU_STYLE) {
        static const int heights[K_COUNT] = {32, 46, 40, 36, 32};
        int y = boxes[B_STYLE].y + boxes[B_STYLE].h + 2;
        for (int i = 0; i < index; i++) {
            y += heights[i];
        }
        return (struct box){boxes[B_STYLE].x, y, 220, heights[index]};
    }
    if (menu == MENU_FONT) {
        return (struct box){boxes[B_FONT].x,
                            boxes[B_FONT].y + boxes[B_FONT].h + 2 + index * 32,
                            200, 32};
    }
    if (menu == MENU_SIZE) {
        int column = index / 8, row = index % 8;
        return (struct box){boxes[B_SIZE].x + column * 50,
                            boxes[B_SIZE].y + boxes[B_SIZE].h + 2 + row * 28,
                            50, 28};
    }
    /* Colours: a 4 x 4 grid. */
    return (struct box){boxes[B_COLOUR].x + 6 + (index % 4) * 30,
                        boxes[B_COLOUR].y + boxes[B_COLOUR].h + 8
                            + (index / 4) * 30,
                        26, 26};
}

static int menu_count(void) {
    return menu == MENU_STYLE ? K_COUNT : menu == MENU_FONT ? 2
        : menu == MENU_SIZE ? SIZE_COUNT : menu == MENU_COLOUR ? 16 : 0;
}

static struct box menu_frame(void) {
    int count = menu_count();
    struct box first = menu_item(0), last = menu_item(count - 1);
    if (menu == MENU_COLOUR) {
        return (struct box){first.x - 6, first.y - 6, 4 * 30 + 8,
                            4 * 30 + 8};
    }
    if (menu == MENU_SIZE) {
        return (struct box){first.x, first.y, 100, 8 * 28};
    }
    return (struct box){first.x, first.y, first.w,
                        last.y + last.h - first.y};
}

static void draw_menu(void) {
    if (menu == MENU_NONE) {
        return;
    }
    struct box frame = menu_frame();
    fill((struct box){frame.x - 2, frame.y - 2, frame.w + 4, frame.h + 4},
         UI_ACTIVE);
    fill(frame, UI_PANEL);
    for (int i = 0; i < menu_count(); i++) {
        struct box item = menu_item(i);
        bool hover = inside(item, mouse_x, mouse_y);
        if (menu == MENU_COLOUR) {
            if (hover) {
                fill((struct box){item.x - 2, item.y - 2, item.w + 4,
                                  item.h + 4}, UI_TEXT);
            }
            fill(item, palette[i]);
            continue;
        }
        if (hover) {
            fill(item, UI_HOVER);
        }
        if (menu == MENU_SIZE) {
            char text[8];
            snprintf(text, sizeof(text), "%u", sizes[i]);
            draw_label(item, text, UI_TEXT);
            continue;
        }
        /* Style and font names in their own look. */
        cstyle s = menu == MENU_STYLE ? kind_style(i)
                                      : st_make(i == 1 ? S_MONO : 0, 32, 0);
        uint32_t colour = menu == MENU_STYLE && i != K_NORMAL
            ? OPOS_RGB(150, 190, 240) : UI_TEXT;
        struct opos_font *font = font_for(s);
        const char *name = menu == MENU_STYLE
            ? (const char *[]){"Normal", "Title", "Heading 1", "Heading 2",
                               "Heading 3"}[i]
            : i == 1 ? "Noto Sans Mono" : "Noto Sans";
        int size = (int)ST_HALF(s) * 2 / 3;
        if (size > 30) {
            size = 30;
        }
        if (menu == MENU_FONT) {
            size = 18;
        }
        if (font != NULL) {
            int ascent = opos_font_ascent(font, size);
            int descent = opos_font_descent(font, size);
            opos_font_draw(font, screen, item.x + 10,
                           item.y + (item.h - ascent - descent) / 2 + ascent,
                           size, name, strlen(name), colour);
        }
    }
}

static void menu_pick(int index) {
    enum menu was = menu;
    menu = MENU_NONE;
    if (was == MENU_STYLE) {
        set_kind(index);
    } else if (was == MENU_FONT) {
        set_font(index == 1);
    } else if (was == MENU_SIZE) {
        restyle(change_half, sizes[index] * 2);
    } else if (was == MENU_COLOUR) {
        last_colour = palette[index];
        restyle(change_colour, last_colour);
    }
    last_edit = EDIT_NONE;
}

/* ---------------------------------------------------------- drawing */

static void bullet_text(const struct para *p, char *out, size_t capacity) {
    if (p->list == L_BULLET) {
        static const char *const marks[3] = {"\xe2\x80\xa2", "\xe2\x97\xa6",
                                             "\xe2\x96\xaa"};
        snprintf(out, capacity, "%s", marks[p->level % 3]);
        return;
    }
    int n = p->number;
    switch (p->level % 3) {
    case 1: {
        /* a, b, ..., z, aa, ab, ... */
        char letters[8];
        int used = 0;
        for (int v = n; v > 0 && used < 7; v = (v - 1) / 26) {
            letters[used++] = (char)('a' + (v - 1) % 26);
        }
        int k = 0;
        while (used > 0 && (size_t)k + 2 < capacity) {
            out[k++] = letters[--used];
        }
        out[k++] = '.';
        out[k] = '\0';
        return;
    }
    case 2: {
        static const char *const ones[] = {"", "i", "ii", "iii", "iv", "v",
                                           "vi", "vii", "viii", "ix"};
        static const char *const tens[] = {"", "x", "xx", "xxx", "xl", "l",
                                           "lx", "lxx", "lxxx", "xc"};
        if (n < 100) {
            snprintf(out, capacity, "%s%s.", tens[n / 10], ones[n % 10]);
        } else {
            snprintf(out, capacity, "%d.", n);
        }
        return;
    }
    default:
        snprintf(out, capacity, "%d.", n);
    }
}

static void draw_line(int pi, int l, int ox, int oy, int sel_p1, int sel_o1,
                      int sel_p2, int sel_o2) {
    struct para *p = &paras[pi];
    struct line *ln = &p->lines[l];
    int top = oy + px(MARGIN) + ln->page_y;
    if (top > view_height || top + ln->height < TOOLBAR_H) {
        return;
    }
    int left = ox + px(MARGIN) + para_indent(p);
    int baseline = top + ln->ascent;
    if (!line_positions(p, ln)) {
        return;
    }
    /* The selection behind the text. */
    if (pi >= sel_p1 && pi <= sel_p2) {
        int from = pi == sel_p1 ? sel_o1 : 0;
        int to = pi == sel_p2 ? sel_o2 : p->len + 1;
        int a = from > ln->start ? from : ln->start;
        int b = to < ln->end ? to : ln->end;
        bool past_end = to > ln->end && ln->last;
        if (a < b || (past_end && a <= ln->end)) {
            int x1 = left + (line_x[a - ln->start] + 32) / 64;
            int x2 = left + (line_x[(b < a ? a : b) - ln->start] + 32) / 64;
            if (past_end) {
                x2 += px(6);
            }
            opos_fill_rect(screen, x1, top, x2 - x1, ln->height, SELECTION);
        }
    }
    /* The list mark on the first line. */
    if (l == 0 && p->list != L_NONE) {
        char mark[16];
        bullet_text(p, mark, sizeof(mark));
        cstyle s = p->len > 0 ? p->st[0] : p->end_style;
        s = st_with_flags(s, ST_FLAGS(s) & (S_BOLD | S_MONO));
        struct opos_font *font = font_for(s);
        if (font != NULL) {
            int w = opos_font_text_width(font, size_px(s), mark, strlen(mark));
            int x = left - px(LIST_INDENT / 2) - (p->list == L_NUMBER ? w / 2 : w / 2);
            if (p->list == L_NUMBER && w > px(LIST_INDENT / 2)) {
                x = left - w - px(4);
            }
            opos_font_draw(font, screen, x, baseline, size_px(s), mark,
                           strlen(mark), ST_COLOR(s));
        }
    }
    for (int i = ln->start; i < ln->end;) {
        uint32_t code;
        int used = (int)opos_utf8_decode(p->text + i, (size_t)(ln->end - i),
                                         &code);
        cstyle s = p->st[i];
        int x = left + (line_x[i - ln->start] + 32) / 64;
        int next = left + (line_x[i + used - ln->start] + 32) / 64;
        if (code >= 0x20) {
            struct opos_font *font = font_for(s);
            if (font != NULL && code != ' ') {
                opos_font_draw(font, screen, x, baseline, size_px(s),
                               p->text + i, (size_t)used, ST_COLOR(s));
            }
        }
        uint32_t flags = ST_FLAGS(s);
        if ((flags & (S_UNDER | S_STRIKE)) && code != '\n') {
            int thick = size_px(s) / 14 > 1 ? size_px(s) / 14 : 1;
            if (flags & S_UNDER) {
                opos_fill_rect(screen, x, baseline + size_px(s) / 9 + 1,
                               next - x, thick, ST_COLOR(s));
            }
            if (flags & S_STRIKE) {
                opos_fill_rect(screen, x, baseline - size_px(s) * 3 / 10,
                               next - x, thick, ST_COLOR(s));
            }
        }
        i += used;
    }
}

static void draw_document(void) {
    opos_fill_rect(screen, 0, TOOLBAR_H, view_width, doc_view_h(), WORKSPACE);
    int pw = page_w(), ph = page_h();
    for (int page = 0; page < pages; page++) {
        int ox, oy;
        page_origin(page, &ox, &oy);
        if (oy > view_height || oy + ph < TOOLBAR_H) {
            continue;
        }
        opos_fill_rect(screen, ox + 3, oy + 3, pw, ph, PAGE_SHADOW);
        opos_fill_rect(screen, ox, oy, pw, ph, 0xffffff);
    }
    int sel_p1 = -1, sel_o1 = 0, sel_p2 = -2, sel_o2 = 0;
    if (has_selection()) {
        sel_range(&sel_p1, &sel_o1, &sel_p2, &sel_o2);
    }
    for (int i = 0; i < npara; i++) {
        struct para *p = &paras[i];
        for (int l = 0; l < p->nlines; l++) {
            int ox, oy;
            page_origin(p->lines[l].page, &ox, &oy);
            draw_line(i, l, ox, oy, sel_p1, sel_o1, sel_p2, sel_o2);
        }
    }
    /* The caret. */
    if (pending == PENDING_NONE) {
        int x, top, height, baseline;
        caret_spot(cur_p, cur_o, &x, &top, &height, &baseline);
        int a, d;
        metrics(type_style, &a, &d);
        int caret_top = baseline - a, caret_h = a + d;
        if (caret_top < top) {
            caret_top = top;
        }
        if (caret_h > height) {
            caret_h = height;
        }
        if (caret_top >= TOOLBAR_H && caret_top < view_height - STATUS_H) {
            opos_fill_rect(screen, x, caret_top, px(2) > 1 ? 2 : 1, caret_h,
                           ST_COLOR(type_style));
        }
    }
}

static struct box scroll_track(void) {
    return (struct box){view_width - SCROLLBAR_W, TOOLBAR_H, SCROLLBAR_W,
                        doc_view_h()};
}

static struct box scroll_thumb(void) {
    struct box track = scroll_track();
    int total = doc_height();
    int h = total > 0 ? track.h * doc_view_h() / total : track.h;
    if (h < 24) {
        h = 24;
    }
    if (h > track.h) {
        h = track.h;
    }
    int range = total - doc_view_h();
    int y = range > 0 ? (track.h - h) * scroll_y / range : 0;
    return (struct box){track.x + 2, track.y + y, SCROLLBAR_W - 4, h};
}

static unsigned words_version = ~0u;
static long word_count;

static long count_words(void) {
    if (words_version == doc_version) {
        return word_count;
    }
    long words = 0;
    for (int i = 0; i < npara; i++) {
        bool in_word = false;
        for (int k = 0; k < paras[i].len; k++) {
            unsigned char c = (unsigned char)paras[i].text[k];
            bool space = c == ' ' || c == '\t' || c == '\n';
            if (!space && !in_word) {
                words++;
            }
            in_word = !space;
        }
    }
    words_version = doc_version;
    word_count = words;
    return words;
}

static void draw_status(void) {
    int y = view_height - STATUS_H;
    opos_fill_rect(screen, 0, y, view_width, STATUS_H, UI_BACK);
    char right[64];
    int page = paras[cur_p].lines[find_line(&paras[cur_p], cur_o)].page;
    long words = count_words();
    snprintf(right, sizeof(right), "PAGE %d OF %d   %ld WORD%s", page + 1,
             pages, words, words == 1 ? "" : "S");
    int rw = opos_text_width(right, 2);
    opos_draw_text(screen, view_width - rw - 10, y + 5, right, UI_DIM, 2);
    /* The message, cut to fit. */
    char shown[sizeof(message)];
    strcpy(shown, message);
    int room = (view_width - rw - 30) / opos_char_width(2);
    if (room < 0) {
        room = 0;
    }
    if ((int)strlen(shown) > room) {
        shown[room] = '\0';
    }
    opos_draw_text(screen, 10, y + 5, shown, message_warn ? UI_WARN : UI_TEXT,
                   2);
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
    char upper[41];
    size_t n = 0;
    for (; file_name[n] != '\0' && n < 40; n++) {
        upper[n] = (char)toupper((unsigned char)file_name[n]);
    }
    upper[n] = '\0';
    strcat(question, upper);
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
    paginate();
    clamp_scroll();
    draw_document();
    struct box track = scroll_track();
    fill(track, UI_BACK);
    fill(scroll_thumb(), UI_BUTTON);
    draw_toolbar();
    draw_status();
    draw_menu();
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

static void request(enum pending what) {
    pending = what;
    if (!modified) {
        finish_pending(true);
    }
}

static void set_zoom(int value) {
    if (value < 50) {
        value = 50;
    }
    if (value > 200) {
        value = 200;
    }
    if (value == zoom) {
        return;
    }
    /* Keep the caret's place on screen. */
    paginate();
    int x, top, height, baseline;
    caret_spot(cur_p, cur_o, &x, &top, &height, &baseline);
    int before = top;
    zoom = value;
    relayout_all();
    paginate();
    caret_spot(cur_p, cur_o, &x, &top, &height, &baseline);
    scroll_y += top - before;
    clamp_scroll();
}

static void press_button(int index) {
    menu = MENU_NONE;
    switch (index) {
    case B_NEW: request(PENDING_NEW); break;
    case B_OPEN: request(PENDING_OPEN); break;
    case B_SAVE: do_save(); break;
    case B_SAVE_AS: do_save_as(); break;
    case B_UNDO: undo(); break;
    case B_REDO: redo(); break;
    case B_CUT: cut_selection(); break;
    case B_COPY: copy_selection(); break;
    case B_PASTE: paste(); break;
    case B_ZOOM_OUT: set_zoom(zoom - 10); break;
    case B_ZOOM: set_zoom(100); break;
    case B_ZOOM_IN: set_zoom(zoom + 10); break;
    case B_STYLE: menu = MENU_STYLE; break;
    case B_FONT: menu = MENU_FONT; break;
    case B_SIZE: menu = MENU_SIZE; break;
    case B_SIZE_DOWN: restyle(change_size_step, 0); break;
    case B_SIZE_UP: restyle(change_size_step, 1); break;
    case B_BOLD: toggle_flag(S_BOLD); break;
    case B_ITALIC: toggle_flag(S_ITALIC); break;
    case B_UNDER: toggle_flag(S_UNDER); break;
    case B_STRIKE: toggle_flag(S_STRIKE); break;
    case B_COLOUR:
        if (mouse_x < boxes[B_COLOUR].x + 30) {
            restyle(change_colour, last_colour);
        } else {
            menu = MENU_COLOUR;
        }
        break;
    case B_LEFT: case B_CENTER: case B_RIGHT: case B_JUSTIFY:
        set_align(index - B_LEFT);
        break;
    case B_BULLETS: toggle_list(L_BULLET); break;
    case B_NUMBERS: toggle_list(L_NUMBER); break;
    case B_OUTDENT: change_level(-1); break;
    case B_INDENT: change_level(1); break;
    default: break;
    }
    if (index >= B_STYLE) {
        last_edit = EDIT_NONE;
    }
}

/* Mouse selection: 1 = characters, 2 = words, 3 = paragraphs. */
static int select_unit;
static bool selecting, thumb_drag;
static int thumb_grab;
static long last_click_ms;
static int last_click_x, last_click_y, click_count;
static int word_p, word_from, word_to; /* the word or paragraph clicked */

static long now_ms(void) {
    struct opos_timespec t;
    opos_uptime(&t);
    return t.seconds * 1000 + t.nanoseconds / 1000000;
}

static void word_bounds(int pi, int o, int *from, int *to) {
    const struct para *p = &paras[pi];
    int a = o, b = o;
    bool word = o < p->len && is_word_byte((unsigned char)p->text[o]);
    if (!word && o > 0 && is_word_byte((unsigned char)p->text[o - 1])) {
        word = true;
        a = b = o - 1;
    }
    if (word) {
        while (a > 0 && is_word_byte((unsigned char)p->text[a - 1])) {
            a--;
        }
        while (b < p->len && is_word_byte((unsigned char)p->text[b])) {
            b++;
        }
    } else if (o < p->len) {
        b = next_char(p, o);
    }
    *from = a;
    *to = b;
}

static void extend_selection_to(int pi, int o) {
    if (select_unit == 1) {
        cur_p = pi;
        cur_o = o;
        return;
    }
    int from, to;
    if (select_unit == 2) {
        word_bounds(pi, o, &from, &to);
    } else {
        from = 0;
        to = paras[pi].len;
    }
    bool before = pi < word_p || (pi == word_p && from < word_from);
    if (before) {
        anc_p = word_p;
        anc_o = word_to;
        cur_p = pi;
        cur_o = from;
    } else {
        anc_p = word_p;
        anc_o = word_from;
        cur_p = pi;
        cur_o = to > word_to || pi != word_p ? to : word_to;
    }
}

static void mouse_down(const struct opos_event *event) {
    int x = event->x, y = event->y;
    if (pending != PENDING_NONE) {
        for (int i = 0; i < 3; i++) {
            if (inside(prompt_button(i), x, y)) {
                prompt_answer(i);
            }
        }
        return;
    }
    if (menu != MENU_NONE) {
        for (int i = 0; i < menu_count(); i++) {
            if (inside(menu_item(i), x, y)) {
                menu_pick(i);
                return;
            }
        }
        enum menu was = menu;
        menu = MENU_NONE;
        /* A click on the button that opened it just closes it. */
        if ((was == MENU_STYLE && inside(boxes[B_STYLE], x, y))
            || (was == MENU_FONT && inside(boxes[B_FONT], x, y))
            || (was == MENU_SIZE && inside(boxes[B_SIZE], x, y))
            || (was == MENU_COLOUR && inside(boxes[B_COLOUR], x, y))) {
            return;
        }
    }
    if (y < TOOLBAR_H) {
        if (event->button != OPOS_BUTTON_LEFT) {
            return;
        }
        for (int i = 0; i < B_COUNT; i++) {
            if (inside(boxes[i], x, y)) {
                press_button(i);
                return;
            }
        }
        return;
    }
    if (y >= view_height - STATUS_H) {
        return;
    }
    if (x >= view_width - SCROLLBAR_W) {
        struct box thumb = scroll_thumb();
        if (inside(thumb, x, y)) {
            thumb_drag = true;
            thumb_grab = y - thumb.y;
        } else {
            scroll_y += (y < thumb.y ? -1 : 1) * doc_view_h() * 9 / 10;
            clamp_scroll();
        }
        return;
    }
    if (event->button == OPOS_BUTTON_RIGHT) {
        return;
    }
    long now = now_ms();
    if (now - last_click_ms < 450 && abs(x - last_click_x) < 5
        && abs(y - last_click_y) < 5) {
        click_count = click_count % 3 + 1;
    } else {
        click_count = 1;
    }
    last_click_ms = now;
    last_click_x = x;
    last_click_y = y;
    int pi, o;
    hit(x, y, &pi, &o);
    last_edit = EDIT_NONE;
    goal_x = -1;
    selecting = true;
    select_unit = click_count;
    if (click_count == 1) {
        cur_p = pi;
        cur_o = o;
        caret_moved(shift_down);
        return;
    }
    if (click_count == 2) {
        word_bounds(pi, o, &word_from, &word_to);
    } else {
        word_from = 0;
        word_to = paras[pi].len;
    }
    word_p = pi;
    anc_p = cur_p = pi;
    anc_o = word_from;
    cur_o = word_to;
    type_style = style_at(cur_p, cur_o);
}

static void mouse_move(int x, int y) {
    if (thumb_drag) {
        struct box track = scroll_track();
        struct box thumb = scroll_thumb();
        int range = doc_height() - doc_view_h();
        int room = track.h - thumb.h;
        if (room > 0) {
            scroll_y = (y - thumb_grab - track.y) * range / room;
            clamp_scroll();
        }
        return;
    }
    if (!selecting) {
        return;
    }
    if (y < TOOLBAR_H) {
        scroll_y -= 20;
    } else if (y >= TOOLBAR_H + doc_view_h()) {
        scroll_y += 20;
    }
    clamp_scroll();
    int pi, o;
    hit(x, y, &pi, &o);
    extend_selection_to(pi, o);
    type_style = style_at(cur_p, cur_o);
}

/* Moves the caret one line up (-1) or down (+1), keeping its column. */
static void move_vertical(int direction, bool extend) {
    paginate();
    int x, top, height, baseline;
    caret_spot(cur_p, cur_o, &x, &top, &height, &baseline);
    if (goal_x < 0) {
        goal_x = x + scroll_x;
    }
    struct para *p = &paras[cur_p];
    int l = find_line(p, cur_o);
    int pi = cur_p;
    if (direction < 0) {
        if (l > 0) {
            l--;
        } else if (pi > 0) {
            pi--;
            l = paras[pi].nlines - 1;
        } else {
            cur_o = 0;
            caret_moved(extend);
            return;
        }
    } else {
        if (l + 1 < p->nlines) {
            l++;
        } else if (pi + 1 < npara) {
            pi++;
            l = 0;
        } else {
            cur_o = p->len;
            caret_moved(extend);
            return;
        }
    }
    struct line *ln = &paras[pi].lines[l];
    int ox, oy;
    page_origin(ln->page, &ox, &oy);
    int ty = oy + px(MARGIN) + ln->page_y + ln->height / 2;
    int keep = goal_x;
    hit(goal_x - scroll_x, ty, &cur_p, &cur_o);
    caret_moved(extend);
    goal_x = keep;
}

static void move_page(int direction, bool extend) {
    int keep = goal_x;
    paginate();
    int x, top, height, baseline;
    caret_spot(cur_p, cur_o, &x, &top, &height, &baseline);
    if (keep < 0) {
        keep = x + scroll_x;
    }
    int delta = doc_view_h() * 9 / 10 * direction;
    scroll_y += delta;
    clamp_scroll();
    hit(keep - scroll_x, top + height / 2 + delta, &cur_p, &cur_o);
    caret_moved(extend);
    goal_x = keep;
}

static void move_word(int direction, bool extend) {
    struct para *p = &paras[cur_p];
    if (direction < 0) {
        if (cur_o == 0) {
            if (cur_p > 0) {
                cur_p--;
                cur_o = paras[cur_p].len;
            }
        } else {
            int o = cur_o;
            while (o > 0 && !is_word_byte((unsigned char)p->text[o - 1])) {
                o--;
            }
            while (o > 0 && is_word_byte((unsigned char)p->text[o - 1])) {
                o--;
            }
            cur_o = o;
        }
    } else {
        if (cur_o == p->len) {
            if (cur_p + 1 < npara) {
                cur_p++;
                cur_o = 0;
            }
        } else {
            int o = cur_o;
            while (o < p->len && is_word_byte((unsigned char)p->text[o])) {
                o++;
            }
            while (o < p->len && !is_word_byte((unsigned char)p->text[o])) {
                o++;
            }
            cur_o = o;
        }
    }
    caret_moved(extend);
}

static void key(const struct opos_event *event) {
    uint32_t k = event->key;
    bool ctrl = (event->buttons & OPOS_MOD_CTRL) != 0;
    bool shift = (event->buttons & OPOS_MOD_SHIFT) != 0;
    if (pending != PENDING_NONE) {
        if (k == OPOS_KEY_ESCAPE) {
            finish_pending(false);
        } else if (k == '\n') {
            prompt_answer(0);
        }
        return;
    }
    if (menu != MENU_NONE) {
        if (k == OPOS_KEY_ESCAPE) {
            menu = MENU_NONE;
        }
        return;
    }
    if (ctrl) {
        uint32_t c = k < 0x80 ? (uint32_t)tolower((int)k) : k;
        switch (c) {
        case 'n': request(PENDING_NEW); return;
        case 'o': request(PENDING_OPEN); return;
        case 's': if (shift) { do_save_as(); } else { do_save(); } return;
        case 'z': undo(); return;
        case 'y': redo(); return;
        case 'a':
            anc_p = 0;
            anc_o = 0;
            cur_p = npara - 1;
            cur_o = paras[cur_p].len;
            type_style = style_at(cur_p, cur_o);
            return;
        case 'c': copy_selection(); return;
        case 'x': cut_selection(); return;
        case 'v': paste(); return;
        case 'b': toggle_flag(S_BOLD); last_edit = EDIT_NONE; return;
        case 'i': toggle_flag(S_ITALIC); last_edit = EDIT_NONE; return;
        case 'u': toggle_flag(S_UNDER); last_edit = EDIT_NONE; return;
        case 'l':
            if (shift) {
                toggle_list(L_BULLET);
            } else {
                set_align(A_LEFT);
            }
            last_edit = EDIT_NONE;
            return;
        case 'e': set_align(A_CENTER); last_edit = EDIT_NONE; return;
        case 'r': set_align(A_RIGHT); last_edit = EDIT_NONE; return;
        case 'j': set_align(A_JUSTIFY); last_edit = EDIT_NONE; return;
        case '0': set_kind(K_NORMAL); last_edit = EDIT_NONE; return;
        case '1': set_kind(K_H1); last_edit = EDIT_NONE; return;
        case '2': set_kind(K_H2); last_edit = EDIT_NONE; return;
        case '3': set_kind(K_H3); last_edit = EDIT_NONE; return;
        case '[': restyle(change_size_step, 0); last_edit = EDIT_NONE; return;
        case ']': restyle(change_size_step, 1); last_edit = EDIT_NONE; return;
        case '+': case '=': set_zoom(zoom + 10); return;
        case '-': set_zoom(zoom - 10); return;
        case '\b': key_backspace(true); return;
        case OPOS_KEY_LEFT: move_word(-1, shift); goal_x = -1; scroll_to_caret(); return;
        case OPOS_KEY_RIGHT: move_word(1, shift); goal_x = -1; scroll_to_caret(); return;
        case OPOS_KEY_HOME:
            cur_p = 0;
            cur_o = 0;
            caret_moved(shift);
            goal_x = -1;
            scroll_to_caret();
            return;
        case OPOS_KEY_END:
            cur_p = npara - 1;
            cur_o = paras[cur_p].len;
            caret_moved(shift);
            goal_x = -1;
            scroll_to_caret();
            return;
        default:
            return;
        }
    }
    struct para *p = &paras[cur_p];
    switch (k) {
    case OPOS_KEY_LEFT:
        if (has_selection() && !shift) {
            int p1, o1, p2, o2;
            sel_range(&p1, &o1, &p2, &o2);
            cur_p = p1;
            cur_o = o1;
        } else if (cur_o > 0) {
            cur_o = prev_char(p, cur_o);
        } else if (cur_p > 0) {
            cur_p--;
            cur_o = paras[cur_p].len;
        }
        caret_moved(shift);
        goal_x = -1;
        last_edit = EDIT_NONE;
        scroll_to_caret();
        return;
    case OPOS_KEY_RIGHT:
        if (has_selection() && !shift) {
            int p1, o1, p2, o2;
            sel_range(&p1, &o1, &p2, &o2);
            cur_p = p2;
            cur_o = o2;
        } else if (cur_o < p->len) {
            cur_o = next_char(p, cur_o);
        } else if (cur_p + 1 < npara) {
            cur_p++;
            cur_o = 0;
        }
        caret_moved(shift);
        goal_x = -1;
        last_edit = EDIT_NONE;
        scroll_to_caret();
        return;
    case OPOS_KEY_UP:
    case OPOS_KEY_DOWN:
        move_vertical(k == OPOS_KEY_UP ? -1 : 1, shift);
        last_edit = EDIT_NONE;
        scroll_to_caret();
        return;
    case OPOS_KEY_PAGE_UP:
    case OPOS_KEY_PAGE_DOWN:
        move_page(k == OPOS_KEY_PAGE_UP ? -1 : 1, shift);
        last_edit = EDIT_NONE;
        scroll_to_caret();
        return;
    case OPOS_KEY_HOME:
    case OPOS_KEY_END: {
        paginate();
        struct line *ln = &p->lines[find_line(p, cur_o)];
        if (k == OPOS_KEY_HOME) {
            cur_o = ln->start;
        } else {
            int end = ln->end;
            if (end > ln->start && (p->text[end - 1] == '\n'
                                    || (end < p->len && p->text[end - 1] == ' '))) {
                end--;
            }
            cur_o = end;
        }
        caret_moved(shift);
        goal_x = -1;
        last_edit = EDIT_NONE;
        scroll_to_caret();
        return;
    }
    case OPOS_KEY_ESCAPE:
        anc_p = cur_p;
        anc_o = cur_o;
        return;
    case '\b':
        key_backspace(false);
        return;
    case OPOS_KEY_DELETE:
        key_delete();
        return;
    case '\n':
        key_enter(shift);
        return;
    case '\t':
        /* In a list, at the start of an item: change its level. */
        if (p->list != L_NONE && (cur_o == 0 || has_selection())) {
            change_level(shift ? -1 : 1);
            last_edit = EDIT_NONE;
            return;
        }
        if (!shift) {
            type_char('\t');
        }
        return;
    default:
        if (k >= 0x20 && k < 0x7f) {
            type_char(k);
        }
        return;
    }
}

static void wheel(const struct opos_event *event) {
    int notches = OPOS_WHEEL_NOTCHES(*event);
    if (ctrl_down) {
        set_zoom(zoom + 10 * notches);
        return;
    }
    if (menu != MENU_NONE || pending != PENDING_NONE) {
        return;
    }
    if (shift_down) {
        scroll_x -= notches * 60;
    } else {
        scroll_y -= notches * 60;
    }
    clamp_scroll();
}

static void handle(const struct opos_event *event) {
    switch (event->type) {
    case OPOS_EVENT_RESIZE:
        view_width = (int)event->width;
        view_height = (int)event->height;
        if (canvas.pixels == NULL || canvas.width < view_width
            || canvas.height < view_height) {
            opos_canvas_destroy(&canvas);
            canvas = opos_canvas_create(view_width, view_height);
            if (canvas.pixels == NULL) {
                opos_print("writer: out of memory\n");
                opos_exit(1);
            }
        }
        layout_toolbar();
        clamp_scroll();
        break;
    case OPOS_EVENT_MOUSE_DOWN:
        mouse_x = event->x;
        mouse_y = event->y;
        mouse_down(event);
        break;
    case OPOS_EVENT_MOUSE_MOVE:
        mouse_x = event->x;
        mouse_y = event->y;
        mouse_move(event->x, event->y);
        break;
    case OPOS_EVENT_MOUSE_UP:
        selecting = false;
        thumb_drag = false;
        break;
    case OPOS_EVENT_WHEEL:
        wheel(event);
        break;
    case OPOS_EVENT_KEY:
        key(event);
        break;
    case OPOS_EVENT_KEY_STATE: {
        bool down = event->button != 0;
        if (event->key == 0x2a || event->key == 0x36) {
            shift_down = down;
        } else if (event->key == 0x1d || event->key == 0x9d) {
            ctrl_down = down;
        }
        break;
    }
    case OPOS_EVENT_CLOSE:
        if (close_removes_window) {
            opos_exit(0);
        }
        selecting = false;
        thumb_drag = false;
        menu = MENU_NONE;
        request(PENDING_CLOSE);
        break;
    default:
        break;
    }
}

/* "writer --convert IN OUT": opens IN and saves it as OUT (.docx or
   .txt) without a window, for tests (it also runs on Linux). */
static int convert(const char *in, const char *out) {
    new_document();
    open_path(in);
    opos_print(message);
    opos_print("\n");
    if (message_warn && strncmp(message, "OPENED", 6) != 0) {
        return 1;
    }
    if (!write_document(out)) {
        opos_print(message);
        opos_print("\n");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc == 4 && strcmp(argv[1], "--convert") == 0) {
        return convert(argv[2], argv[3]);
    }
    if (opos_window_create("Writer", 1100, 800) < 0) {
        opos_print("writer: no window; run it from the desktop\n");
        return 1;
    }
    /* Older OPOS versions have no ASK_CLOSE: their close button removes
       the window before CLOSE arrives, so Writer cannot ask then. */
    if (opos_window_options(OPOS_WINDOW_KEY_STATE | OPOS_WINDOW_ASK_CLOSE)
        < 0) {
        opos_window_options(OPOS_WINDOW_KEY_STATE);
        close_removes_window = true;
    }
    new_document();
    if (font_for(kind_style(K_NORMAL)) == NULL) {
        say("COULD NOT LOAD THE FONTS IN /SHARE/FONTS", true);
    }
    update_title();
    if (argc > 1) {
        open_path(argv[1]);
    }
    struct opos_event event;
    for (;;) {
        long timeout = selecting && (mouse_y < TOOLBAR_H
                                     || mouse_y >= TOOLBAR_H + doc_view_h())
            ? 40 : -1;
        long got = opos_window_event(&event, timeout);
        if (got < 0) {
            continue;
        }
        if (got == 0) {
            if (selecting) {
                mouse_move(mouse_x, mouse_y); /* keep scrolling */
                draw();
            }
            continue;
        }
        handle(&event);
        /* Handle what else is waiting before drawing once. */
        int more = 0;
        while (more < 64 && opos_window_event(&event, 0) == 1) {
            handle(&event);
            more++;
        }
        draw();
    }
}
