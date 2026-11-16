/*
 * Presentation: the OPOS slide show program.
 *
 * Slides hold text boxes, title and content placeholders with bulleted or
 * numbered lists, rectangles, rounded rectangles, ellipses, lines and
 * pictures (PNG, JPEG, BMP). Text has bold, italic, underline,
 * strikethrough, sizes, colours and alignment; shapes have fill and
 * outline colours; slides have a background colour. Opens and saves
 * PowerPoint presentations (.pptx, see slide_pptx.h) and shows them full
 * screen. See docs/presentation.md.
 *
 * Mouse: click a shape to select it, drag it to move, drag a handle to
 * resize (Shift keeps the shape); click text to type in it.
 * Keys: Ctrl+M new slide, Ctrl+D duplicate, Delete removes the shape,
 * arrows move it (or change slide when nothing is selected), Esc stops
 * editing, Tab picks the next shape, Ctrl+B/I/U, Ctrl+Z/Y, Ctrl+X/C/V,
 * Ctrl+N/O/S (Ctrl+Shift+S save as), F5 shows the slides from the first,
 * Shift+F5 from this one. In the slide show: Space, Enter, arrows, Page
 * Up/Down and clicks move on, Esc ends it.
 */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_font.h>
#include <opos_gfx.h>
#include <opos_http.h>
#include <opos_image.h>

#include "ooxml.h"

OPOS_MANIFEST("Presentation", "1.0", OPOS_CAP_CLIPBOARD_READ);

#define EMU_PER_PT 12700
#define EMU_PER_INCH 914400
#define EMU_PER_PX 9525 /* at 96 dpi */
#define MAX_SLIDES 2000
#define MAX_SHAPES 1000 /* per slide */
#define TEXT_MAX (4 * 1024 * 1024)

#define PATH_MAX_LENGTH 256
#define UNDO_MAX 100
#define UNDO_BYTES (128u * 1024 * 1024)

/* ============================================================ the model */

/*
 * A character's look, packed: colour (bits 0-23), flags (24-31) and the
 * size in 1/100 point (32-47), the unit .pptx files use.
 */
typedef uint64_t cstyle;
#define S_BOLD 1u
#define S_ITALIC 2u
#define S_UNDER 4u
#define S_STRIKE 8u
#define ST_COLOR(s) ((uint32_t)((s) & 0xffffff))
#define ST_FLAGS(s) ((uint32_t)((s) >> 24 & 0xff))
#define ST_SIZE(s) ((uint32_t)((s) >> 32 & 0xffff))

static cstyle st_make(uint32_t flags, uint32_t size, uint32_t colour) {
    if (size < 100) {
        size = 100;
    }
    if (size > 65535) {
        size = 65535;
    }
    return (cstyle)(colour & 0xffffff) | (cstyle)(flags & 0xff) << 24
        | (cstyle)size << 32;
}

static cstyle st_with_flags(cstyle s, uint32_t flags) {
    return st_make(flags, ST_SIZE(s), ST_COLOR(s));
}

static cstyle st_with_size(cstyle s, uint32_t size) {
    return st_make(ST_FLAGS(s), size, ST_COLOR(s));
}

static cstyle st_with_colour(cstyle s, uint32_t colour) {
    return st_make(ST_FLAGS(s), ST_SIZE(s), colour);
}

enum align { A_LEFT, A_CENTER, A_RIGHT, A_JUSTIFY };
enum bullet { BU_NONE, BU_CHAR, BU_NUMBER };
enum numbering {
    N_ARABIC_PERIOD, N_ARABIC_PAREN_R, N_ARABIC_PARENS, N_ARABIC_PLAIN,
    N_ALPHA_LC_PERIOD, N_ALPHA_UC_PERIOD, N_ALPHA_LC_PAREN_R,
    N_ALPHA_UC_PAREN_R, N_ROMAN_LC_PERIOD, N_ROMAN_UC_PERIOD, N_COUNT,
};

struct line {
    int start, end;     /* bytes of the paragraph */
    int width;          /* 1/64 px, without trailing spaces */
    int x0;             /* px from the text area's left */
    int ascent, descent, height, y;
    int spaces;
    bool last;
};

struct para {
    char *text;
    cstyle *st;         /* one per byte */
    int len, cap;
    cstyle end_style;   /* the look of an empty paragraph */
    uint8_t align, level, bullet, num_type;
    uint16_t num_start;
    uint32_t bullet_char;
    int32_t mar_l, indent;             /* EMU */
    int32_t space_before, space_after; /* 1/100 pt */
    int32_t line_pct;                  /* 100000 = single */
    /* layout, in px for key_scale */
    struct line *lines;
    int nlines, lines_cap;
    double key_scale;
    int key_width, key_fs;
    bool dirty;
    int number;
    int top, height;
};

enum shape_type { SH_TEXT, SH_RECT, SH_ROUND, SH_ELLIPSE, SH_LINE, SH_PICTURE };
enum placeholder { PH_NONE, PH_TITLE, PH_CTR_TITLE, PH_SUBTITLE, PH_BODY,
                   PH_OTHER };
enum anchor { AN_TOP, AN_MIDDLE, AN_BOTTOM };
enum autofit { FIT_NONE, FIT_SHAPE, FIT_TEXT };
#define NO_COLOUR 0xffffffffu
#define FLIP_H 1u
#define FLIP_V 2u

struct shape {
    uint8_t type, ph, anchor, autofit, flip;
    bool wrap;
    int ph_idx;
    int64_t x, y, w, h;          /* EMU */
    uint32_t fill, line;         /* NO_COLOUR = none */
    int32_t line_w;              /* EMU */
    int32_t ins_l, ins_t, ins_r, ins_b;
    int32_t font_scale;          /* 100000 = 100% */
    int media;                   /* SH_PICTURE */
    struct para *paras;
    int npara, para_cap;
    int text_h;                  /* px, from the last layout */
};

struct slide {
    struct shape *shapes;
    int n, cap;
    uint32_t background;
    bool hidden;
    int deco;          /* decoration from the file's layout, or -1 */
    unsigned version;
    struct opos_canvas thumb;
    unsigned thumb_version;
    int thumb_w;
};

struct media {
    uint8_t *data;
    size_t size;
    char ext[8];
    uint32_t *pixels;            /* decoded, 0xAARRGGBB */
    void *scratch;
    int w, h;
    bool tried;
};

static struct slide *slides;
static int nslides, slides_cap;
static int64_t slide_w = 12192000, slide_h = 6858000;
static struct media *media;
static int nmedia, media_cap;
/* Shapes of the file's layouts and masters, drawn under slides. */
static struct slide *decos;
static int ndecos, decos_cap;

/* What is selected and edited. */
static int cur_slide;
static int sel = -1;            /* shape index on the current slide */
static bool editing;            /* typing in the selected shape */
static int cur_p, cur_o, anc_p, anc_o;
static int goal_x = -1;

static void say(const char *text, bool warn);

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
    return c >= 0x80 || isalnum(c) || c == '_' || c == '\'';
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

static void para_init(struct para *p, cstyle style) {
    memset(p, 0, sizeof(*p));
    p->end_style = style;
    p->line_pct = 100000;
    p->bullet_char = 0x2022;
    p->num_start = 1;
    p->dirty = true;
}

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

/* A copy without the layout. */
static bool para_copy(struct para *to, const struct para *from) {
    *to = *from;
    to->text = NULL;
    to->st = NULL;
    to->cap = 0;
    to->len = 0;
    to->lines = NULL;
    to->nlines = to->lines_cap = 0;
    to->dirty = true;
    if (!para_reserve(to, from->len > 0 ? from->len : 1)) {
        return false;
    }
    memcpy(to->text, from->text, (size_t)from->len);
    memcpy(to->st, from->st, (size_t)from->len * sizeof(cstyle));
    to->len = from->len;
    return true;
}

/* The look a character typed at o takes. */
static cstyle para_style_at(const struct para *p, int o) {
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

/* ----------------------------------------------------------- shapes */

static void shape_free(struct shape *s) {
    for (int i = 0; i < s->npara; i++) {
        para_free(&s->paras[i]);
    }
    free(s->paras);
    s->paras = NULL;
    s->npara = s->para_cap = 0;
}

static struct para *shape_insert_para(struct shape *s, int at, cstyle style) {
    if (s->npara == s->para_cap) {
        int cap = s->para_cap < 4 ? 4 : s->para_cap * 2;
        struct para *grown = realloc(s->paras, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        s->paras = grown;
        s->para_cap = cap;
    }
    memmove(s->paras + at + 1, s->paras + at,
            (size_t)(s->npara - at) * sizeof(*s->paras));
    s->npara++;
    para_init(&s->paras[at], style);
    return &s->paras[at];
}

static void shape_remove_paras(struct shape *s, int from, int to) {
    for (int i = from; i < to; i++) {
        para_free(&s->paras[i]);
    }
    memmove(s->paras + from, s->paras + to,
            (size_t)(s->npara - to) * sizeof(*s->paras));
    s->npara -= to - from;
}

static bool shape_copy(struct shape *to, const struct shape *from) {
    *to = *from;
    to->paras = NULL;
    to->npara = to->para_cap = 0;
    if (from->npara == 0) {
        return true;
    }
    to->paras = calloc((size_t)from->npara, sizeof(*to->paras));
    if (to->paras == NULL) {
        return false;
    }
    to->para_cap = from->npara;
    for (int i = 0; i < from->npara; i++) {
        if (!para_copy(&to->paras[i], &from->paras[i])) {
            to->npara = i;
            shape_free(to);
            return false;
        }
        to->npara = i + 1;
    }
    return true;
}

static bool shape_has_text_area(const struct shape *s) {
    return s->type != SH_LINE && s->type != SH_PICTURE;
}

static bool shape_text_empty(const struct shape *s) {
    for (int i = 0; i < s->npara; i++) {
        if (s->paras[i].len > 0) {
            return false;
        }
    }
    return true;
}

/* A shape's paragraphs all dirty (after a size or scale change). */
static void shape_dirty(struct shape *s) {
    for (int i = 0; i < s->npara; i++) {
        s->paras[i].dirty = true;
    }
}

/* ----------------------------------------------------------- slides */

static void slide_free(struct slide *sl) {
    for (int i = 0; i < sl->n; i++) {
        shape_free(&sl->shapes[i]);
    }
    free(sl->shapes);
    opos_canvas_destroy(&sl->thumb);
    memset(sl, 0, sizeof(*sl));
}

static struct shape *slide_add_shape(struct slide *sl, int at) {
    if (sl->n >= MAX_SHAPES) {
        return NULL;
    }
    if (sl->n == sl->cap) {
        int cap = sl->cap < 8 ? 8 : sl->cap * 2;
        struct shape *grown = realloc(sl->shapes, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        sl->shapes = grown;
        sl->cap = cap;
    }
    if (at < 0 || at > sl->n) {
        at = sl->n;
    }
    memmove(sl->shapes + at + 1, sl->shapes + at,
            (size_t)(sl->n - at) * sizeof(*sl->shapes));
    sl->n++;
    struct shape *s = &sl->shapes[at];
    memset(s, 0, sizeof(*s));
    s->fill = s->line = NO_COLOUR;
    s->ins_l = s->ins_r = 91440;
    s->ins_t = s->ins_b = 45720;
    s->font_scale = 100000;
    s->wrap = true;
    s->media = -1;
    sl->version++;
    return s;
}

static void slide_remove_shape(struct slide *sl, int at) {
    shape_free(&sl->shapes[at]);
    memmove(sl->shapes + at, sl->shapes + at + 1,
            (size_t)(sl->n - at - 1) * sizeof(*sl->shapes));
    sl->n--;
    sl->version++;
}

static bool slide_copy(struct slide *to, const struct slide *from) {
    *to = *from;
    memset(&to->thumb, 0, sizeof(to->thumb));
    to->thumb_version = 0;
    to->thumb_w = 0;
    to->shapes = NULL;
    to->n = to->cap = 0;
    to->version = from->version + 1;
    if (from->n == 0) {
        return true;
    }
    to->shapes = calloc((size_t)from->n, sizeof(*to->shapes));
    if (to->shapes == NULL) {
        return false;
    }
    to->cap = from->n;
    for (int i = 0; i < from->n; i++) {
        if (!shape_copy(&to->shapes[i], &from->shapes[i])) {
            to->n = i;
            slide_free(to);
            return false;
        }
        to->n = i + 1;
    }
    return true;
}

/* Makes room for a slide at index at; returns it, empty. */
static struct slide *deck_insert_slide(int at) {
    if (nslides >= MAX_SLIDES) {
        return NULL;
    }
    if (nslides == slides_cap) {
        int cap = slides_cap < 16 ? 16 : slides_cap * 2;
        struct slide *grown = realloc(slides, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        slides = grown;
        slides_cap = cap;
    }
    memmove(slides + at + 1, slides + at,
            (size_t)(nslides - at) * sizeof(*slides));
    nslides++;
    memset(&slides[at], 0, sizeof(slides[at]));
    slides[at].background = 0xffffff;
    slides[at].deco = -1;
    slides[at].version = 1;
    return &slides[at];
}

static struct slide *deco_add(void) {
    if (ndecos == decos_cap) {
        int cap = decos_cap < 8 ? 8 : decos_cap * 2;
        struct slide *grown = realloc(decos, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        decos = grown;
        decos_cap = cap;
    }
    struct slide *d = &decos[ndecos++];
    memset(d, 0, sizeof(*d));
    d->deco = -1;
    return d;
}

static void deck_remove_slide(int at) {
    slide_free(&slides[at]);
    memmove(slides + at, slides + at + 1,
            (size_t)(nslides - at - 1) * sizeof(*slides));
    nslides--;
}

static void media_free_all(void) {
    for (int i = 0; i < nmedia; i++) {
        free(media[i].data);
        free(media[i].scratch);
    }
    free(media);
    media = NULL;
    nmedia = media_cap = 0;
}

static void image_cache_clear(void);

static void deck_clear(void) {
    for (int i = 0; i < nslides; i++) {
        slide_free(&slides[i]);
    }
    nslides = 0;
    for (int i = 0; i < ndecos; i++) {
        slide_free(&decos[i]);
    }
    ndecos = 0;
    media_free_all();
    image_cache_clear();
    cur_slide = 0;
    sel = -1;
    editing = false;
}

/* Takes the data (malloc'd); returns the media index or -1. */
static int media_add(uint8_t *data, size_t size, const char *ext) {
    if (nmedia == media_cap) {
        int cap = media_cap < 8 ? 8 : media_cap * 2;
        struct media *grown = realloc(media, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            free(data);
            return -1;
        }
        media = grown;
        media_cap = cap;
    }
    struct media *m = &media[nmedia];
    memset(m, 0, sizeof(*m));
    m->data = data;
    m->size = size;
    snprintf(m->ext, sizeof(m->ext), "%s", ext);
    return nmedia++;
}

static bool media_decode(struct media *m) {
    if (m->tried) {
        return m->pixels != NULL;
    }
    m->tried = true;
    struct image_info info;
    if (image_probe(m->data, m->size, &info) != IMAGE_OK) {
        return false;
    }
    m->scratch = malloc(info.scratch_bytes + 16);
    if (m->scratch == NULL) {
        return false;
    }
    void *aligned = (void *)(((uintptr_t)m->scratch + 15) & ~(uintptr_t)15);
    struct image image;
    if (image_decode(m->data, m->size, aligned, info.scratch_bytes, &image)
        != IMAGE_OK) {
        free(m->scratch);
        m->scratch = NULL;
        return false;
    }
    m->pixels = image.pixels;
    m->w = (int)image.width;
    m->h = (int)image.height;
    return true;
}

/* ----------------------------------------------- default text and shapes */

#define TEXT_BLACK 0x000000u
#define ACCENT_FILL 0x4472c4u
#define ACCENT_LINE 0x2f528fu
#define BODY_SPACE_BEFORE 1000 /* 10 pt */

static const int body_sizes[5] = {2800, 2400, 2000, 1800, 1800};

static int64_t sx(int64_t emu) { /* positions of a 16:9 layout, for this size */
    return emu * slide_w / 12192000;
}

static int64_t sy(int64_t emu) {
    return emu * slide_h / 6858000;
}

/* One empty paragraph with this look. */
static void shape_start_text(struct shape *s, cstyle style, int align,
                             int bullet) {
    struct para *p = shape_insert_para(s, 0, style);
    if (p == NULL) {
        return;
    }
    p->align = (uint8_t)align;
    p->bullet = (uint8_t)bullet;
    if (bullet != BU_NONE) {
        p->mar_l = 228600;
        p->indent = -228600;
        p->space_before = BODY_SPACE_BEFORE;
        p->line_pct = 90000;
    }
}

static void level_defaults(struct para *p, int level) {
    p->level = (uint8_t)level;
    if (p->bullet != BU_NONE) {
        p->mar_l = 228600 + level * 457200;
        p->indent = -228600;
    } else {
        p->mar_l = 0;
        p->indent = 0;
    }
}

static struct shape *add_placeholder(struct slide *sl, int ph, int64_t x,
                                     int64_t y, int64_t w, int64_t h) {
    struct shape *s = slide_add_shape(sl, -1);
    if (s == NULL) {
        return NULL;
    }
    s->type = SH_TEXT;
    s->ph = (uint8_t)ph;
    s->x = sx(x);
    s->y = sy(y);
    s->w = sx(w);
    s->h = sy(h);
    s->autofit = FIT_TEXT;
    switch (ph) {
    case PH_CTR_TITLE:
        s->anchor = AN_BOTTOM;
        shape_start_text(s, st_make(0, 6000, TEXT_BLACK), A_CENTER, BU_NONE);
        s->paras[0].line_pct = 90000;
        break;
    case PH_SUBTITLE:
        s->ph_idx = 1;
        shape_start_text(s, st_make(0, 2400, TEXT_BLACK), A_CENTER, BU_NONE);
        s->paras[0].space_before = BODY_SPACE_BEFORE;
        s->paras[0].line_pct = 90000;
        break;
    case PH_TITLE:
        s->anchor = AN_MIDDLE;
        shape_start_text(s, st_make(0, 4400, TEXT_BLACK), A_LEFT, BU_NONE);
        s->paras[0].line_pct = 90000;
        break;
    default:
        s->ph_idx = 1;
        shape_start_text(s, st_make(0, 2800, TEXT_BLACK), A_LEFT, BU_CHAR);
        break;
    }
    return s;
}

enum layout { LAYOUT_TITLE, LAYOUT_CONTENT, LAYOUT_SECTION, LAYOUT_TITLE_ONLY,
              LAYOUT_BLANK, LAYOUT_COUNT };

static void fill_layout(struct slide *sl, int layout) {
    switch (layout) {
    case LAYOUT_TITLE:
        add_placeholder(sl, PH_CTR_TITLE, 1524000, 1122363, 9144000, 2387600);
        add_placeholder(sl, PH_SUBTITLE, 1524000, 3602038, 9144000, 1655762);
        break;
    case LAYOUT_CONTENT:
        add_placeholder(sl, PH_TITLE, 838200, 365125, 10515600, 1325563);
        add_placeholder(sl, PH_BODY, 838200, 1825625, 10515600, 4351338);
        break;
    case LAYOUT_SECTION: {
        struct shape *t = add_placeholder(sl, PH_TITLE, 831850, 1709738,
                                          10515600, 2852737);
        if (t != NULL) {
            t->anchor = AN_BOTTOM;
            t->paras[0].end_style = st_make(0, 6000, TEXT_BLACK);
        }
        struct shape *b = add_placeholder(sl, PH_BODY, 831850, 4589463,
                                          10515600, 1500187);
        if (b != NULL) {
            b->paras[0].bullet = BU_NONE;
            b->paras[0].mar_l = b->paras[0].indent = 0;
            b->paras[0].end_style = st_make(0, 2400, 0x595959);
        }
        break;
    }
    case LAYOUT_TITLE_ONLY:
        add_placeholder(sl, PH_TITLE, 838200, 365125, 10515600, 1325563);
        break;
    default:
        break;
    }
}

/* ============================================================ drawing */

/* Where a slide is drawn: px per EMU. Editor drawing adds placeholder
   prompts and outlines that the show and thumbnails leave out. */
struct view {
    double scale;
    bool editor;
};

static int ez(double scale, int64_t v) {
    return (int)floor((double)v * scale + 0.5);
}

static struct opos_font *fonts[4];
static bool fonts_tried[4];

static struct opos_font *font_for(cstyle s) {
    uint32_t flags = ST_FLAGS(s);
    int variant = ((flags & S_BOLD) ? 1 : 0) | ((flags & S_ITALIC) ? 2 : 0);
    if (!fonts_tried[variant]) {
        static const char *const names[4] = {
            "/share/fonts/NotoSans-Regular.ttf",
            "/share/fonts/NotoSans-Bold.ttf",
            "/share/fonts/NotoSans-Italic.ttf",
            "/share/fonts/NotoSans-BoldItalic.ttf",
        };
        fonts_tried[variant] = true;
        fonts[variant] = opos_font_load(names[variant]);
    }
    if (fonts[variant] == NULL && variant != 0) {
        return font_for(st_with_flags(s, 0));
    }
    return fonts[variant];
}

static int size_px(cstyle s, double scale, int32_t font_scale) {
    double v = (double)ST_SIZE(s) * 127.0 * scale * font_scale / 100000.0;
    int size = (int)(v + 0.5);
    return size < 1 ? 1 : size;
}

static int glyph_advance(cstyle s, int size, uint32_t code) {
    struct opos_font *font = font_for(s);
    if (code < 0x20) {
        return 0;
    }
    if (font == NULL) {
        return size * 32;
    }
    return opos_font_advance(font, size, code);
}

static void font_metrics(cstyle s, int size, int *ascent, int *descent) {
    struct opos_font *font = font_for(s);
    if (font == NULL) {
        *ascent = size * 8 / 10;
        *descent = size / 5;
        return;
    }
    *ascent = opos_font_ascent(font, size);
    *descent = opos_font_descent(font, size);
}

/* --- text layout */

static void number_text(const struct para *p, char *out, size_t capacity) {
    int n = p->number;
    char core[32];
    int type = p->num_type < N_COUNT ? p->num_type : N_ARABIC_PERIOD;
    switch (type) {
    case N_ALPHA_LC_PERIOD: case N_ALPHA_UC_PERIOD:
    case N_ALPHA_LC_PAREN_R: case N_ALPHA_UC_PAREN_R: {
        bool upper = type == N_ALPHA_UC_PERIOD || type == N_ALPHA_UC_PAREN_R;
        int k = 0;
        char rev[16];
        int v = n < 1 ? 1 : n;
        while (v > 0 && k < 15) {
            v--;
            rev[k++] = (char)((upper ? 'A' : 'a') + v % 26);
            v /= 26;
        }
        for (int i = 0; i < k; i++) {
            core[i] = rev[k - 1 - i];
        }
        core[k] = '\0';
        break;
    }
    case N_ROMAN_LC_PERIOD: case N_ROMAN_UC_PERIOD: {
        static const int values[13] = {1000, 900, 500, 400, 100, 90, 50, 40,
                                       10, 9, 5, 4, 1};
        static const char *const upper[13] = {"M", "CM", "D", "CD", "C", "XC",
                                              "L", "XL", "X", "IX", "V", "IV",
                                              "I"};
        size_t used = 0;
        int v = n < 1 ? 1 : n > 3999 ? 3999 : n;
        for (int i = 0; i < 13; i++) {
            while (v >= values[i] && used + 2 < sizeof(core)) {
                for (const char *c = upper[i]; *c != '\0'; c++) {
                    core[used++] = type == N_ROMAN_LC_PERIOD
                        ? (char)tolower((unsigned char)*c) : *c;
                }
                v -= values[i];
            }
        }
        core[used] = '\0';
        break;
    }
    default:
        snprintf(core, sizeof(core), "%d", n);
        break;
    }
    switch (type) {
    case N_ARABIC_PAREN_R: case N_ALPHA_LC_PAREN_R: case N_ALPHA_UC_PAREN_R:
        snprintf(out, capacity, "%s)", core);
        break;
    case N_ARABIC_PARENS:
        snprintf(out, capacity, "(%s)", core);
        break;
    case N_ARABIC_PLAIN:
        snprintf(out, capacity, "%s", core);
        break;
    default:
        snprintf(out, capacity, "%s.", core);
        break;
    }
}

/* The bullet's text (UTF-8), or "" for none. */
static void bullet_text(const struct para *p, char *out, size_t capacity) {
    out[0] = '\0';
    if (p->bullet == BU_CHAR) {
        char utf[8];
        int n = utf8_put(utf, p->bullet_char != 0 ? p->bullet_char : 0x2022);
        utf[n] = '\0';
        snprintf(out, capacity, "%s", utf);
    } else if (p->bullet == BU_NUMBER) {
        number_text(p, out, capacity);
    }
}

static cstyle bullet_style(const struct para *p) {
    return p->len > 0 ? p->st[0] : p->end_style;
}

static int text_width64(cstyle s, int size, const char *text, size_t length) {
    int w = 0;
    for (size_t i = 0; i < length;) {
        uint32_t code;
        size_t used = opos_utf8_decode(text + i, length - i, &code);
        if (used == 0) {
            break;
        }
        w += glyph_advance(s, size, code);
        i += used;
    }
    return w;
}

static bool add_line(struct para *p, int start, int end, int x0, double scale,
                     int32_t fs) {
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
    ln->x0 = x0;
    int ascent = 0, descent = 0, biggest = 0;
    cstyle last = 0;
    bool have = false;
    int from = start, to = end;
    if (end == start) {
        cstyle s = start > 0 ? p->st[start - 1]
            : p->len > 0 ? p->st[0] : p->end_style;
        int size = size_px(s, scale, fs);
        font_metrics(s, size, &ascent, &descent);
        biggest = size;
    }
    for (int i = from; i < to; i++) {
        if (have && p->st[i] == last) {
            continue;
        }
        last = p->st[i];
        have = true;
        int size = size_px(last, scale, fs);
        int a, d;
        font_metrics(last, size, &a, &d);
        if (a > ascent) {
            ascent = a;
        }
        if (d > descent) {
            descent = d;
        }
        if (size > biggest) {
            biggest = size;
        }
    }
    ln->ascent = ascent;
    ln->descent = descent;
    /* PowerPoint's single spacing is 1.2 times the font size. */
    ln->height = (int)((double)biggest * 1.2 * p->line_pct / 100000.0 + 0.5);
    if (ln->height < 1) {
        ln->height = 1;
    }
    int stop = end;
    while (stop > start
           && (p->text[stop - 1] == ' ' || p->text[stop - 1] == '\n')) {
        stop--;
    }
    int x = 0, spaces = 0;
    for (int i = start; i < stop;) {
        uint32_t code;
        int used = (int)opos_utf8_decode(p->text + i, (size_t)(stop - i), &code);
        if (used <= 0) {
            used = 1;
            code = '?';
        }
        if (code == '\t') {
            int step = ez(scale, 914400) * 64;
            if (step < 64) {
                step = 64;
            }
            x = (x / step + 1) * step;
        } else {
            x += glyph_advance(p->st[i], size_px(p->st[i], scale, fs), code);
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

/* The left edges (px) of the first and the other lines, and where the
   bullet goes. */
static void para_edges(const struct para *p, double scale, int32_t fs,
                       int *first, int *rest, int *bullet_x) {
    int mar = ez(scale, p->mar_l), ind = ez(scale, p->indent);
    *rest = mar;
    *bullet_x = mar + ind;
    if (p->bullet == BU_NONE) {
        *first = mar + ind;
        return;
    }
    char text[32];
    bullet_text(p, text, sizeof(text));
    cstyle s = bullet_style(p);
    int size = size_px(s, scale, fs);
    int bw = text_width64(s, size, text, strlen(text)) / 64;
    int gap = size / 4;
    *first = mar;
    if (*bullet_x + bw + gap > mar) {
        *first = *bullet_x + bw + gap;
    }
}

static void layout_para(struct para *p, double scale, int32_t fs, int width) {
    p->nlines = 0;
    p->dirty = false;
    p->key_scale = scale;
    p->key_width = width;
    p->key_fs = fs;
    int first, rest, bx;
    para_edges(p, scale, fs, &first, &rest, &bx);
    int start = 0;
    bool first_line = true;
    while (start < p->len) {
        int x0 = first_line ? first : rest;
        int avail = (width - x0) * 64;
        if (avail < 64) {
            avail = 64;
        }
        int x = 0, last_break = -1, end = p->len;
        for (int i = start; i < p->len;) {
            unsigned char c = (unsigned char)p->text[i];
            if (c == '\n') {
                end = i + 1;
                break;
            }
            uint32_t code;
            int used = (int)opos_utf8_decode(p->text + i, (size_t)(p->len - i),
                                             &code);
            if (used <= 0) {
                used = 1;
                code = '?';
            }
            int w;
            if (code == '\t') {
                int step = ez(scale, 914400) * 64;
                if (step < 64) {
                    step = 64;
                }
                w = (x / step + 1) * step - x;
            } else {
                w = glyph_advance(p->st[i], size_px(p->st[i], scale, fs), code);
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
        add_line(p, start, end, x0, scale, fs);
        start = end;
        first_line = false;
    }
    if (p->len == 0 || p->text[p->len - 1] == '\n') {
        add_line(p, p->len, p->len, first_line ? first : rest, scale, fs);
    }
}

/* Lays out a shape's text for this scale: lines, paragraph tops, list
   numbers and the text height (px). */
static int text_area_width(const struct shape *s, double scale) {
    if (!s->wrap) {
        return 1 << 20;
    }
    int w = ez(scale, s->w - s->ins_l - s->ins_r);
    return w < 1 ? 1 : w;
}

static void layout_shape(struct shape *s, double scale) {
    int width = text_area_width(s, scale);
    int counters[9] = {0};
    int y = 0;
    for (int i = 0; i < s->npara; i++) {
        struct para *p = &s->paras[i];
        if (p->bullet == BU_NUMBER) {
            int level = p->level < 9 ? p->level : 8;
            for (int l = level + 1; l < 9; l++) {
                counters[l] = 0;
            }
            if (counters[level] == 0) {
                counters[level] = p->num_start > 0 ? p->num_start : 1;
            } else {
                counters[level]++;
            }
            p->number = counters[level];
        } else if (p->bullet == BU_NONE) {
            memset(counters, 0, sizeof(counters));
        }
        if (p->dirty || p->key_scale != scale || p->key_width != width
            || p->key_fs != s->font_scale) {
            layout_para(p, scale, s->font_scale, width);
        }
        if (i > 0) {
            y += ez(scale, (int64_t)p->space_before * 127)
                * s->font_scale / 100000;
        }
        p->top = y;
        for (int l = 0; l < p->nlines; l++) {
            p->lines[l].y = y;
            y += p->lines[l].height;
        }
        p->height = y - p->top;
        y += ez(scale, (int64_t)p->space_after * 127) * s->font_scale / 100000;
    }
    s->text_h = y;
}

/* Where the text area starts (px, relative to the slide). */
static void text_origin(const struct shape *s, double scale, int *x, int *y) {
    *x = ez(scale, s->x + s->ins_l);
    int top = ez(scale, s->y + s->ins_t);
    int area = ez(scale, s->h - s->ins_t - s->ins_b);
    if (s->anchor == AN_MIDDLE) {
        top += (area - s->text_h) / 2;
    } else if (s->anchor == AN_BOTTOM) {
        top += area - s->text_h;
    }
    *y = top;
}

/* x (px from the text area's left) of byte o in a line. */
static int line_x(const struct shape *s, const struct para *p,
                  const struct line *ln, int o, double scale) {
    int width = text_area_width(s, scale);
    int x = ln->x0;
    int free_px = width - ln->x0 - ln->width / 64;
    if (s->wrap || p->align != A_LEFT) {
        if (!s->wrap) {
            free_px = 0;
        }
        if (p->align == A_CENTER) {
            x += free_px / 2;
        } else if (p->align == A_RIGHT) {
            x += free_px;
        }
    }
    int acc = 0;
    bool justify = p->align == A_JUSTIFY && !ln->last && ln->spaces > 0
        && s->wrap;
    int extra = justify ? free_px * 64 / ln->spaces : 0;
    for (int i = ln->start; i < o && i < ln->end;) {
        uint32_t code;
        int used = (int)opos_utf8_decode(p->text + i, (size_t)(ln->end - i),
                                         &code);
        if (used <= 0) {
            used = 1;
            code = '?';
        }
        if (code == '\n') {
            break;
        }
        if (code == '\t') {
            int step = ez(scale, 914400) * 64;
            if (step < 64) {
                step = 64;
            }
            acc = (acc / step + 1) * step;
        } else {
            acc += glyph_advance(p->st[i], size_px(p->st[i], scale, s->font_scale),
                                 code);
            if (code == ' ' && justify) {
                acc += extra;
            }
        }
        i += used;
    }
    return x + acc / 64;
}

/* --- pictures, scaled once and kept */

struct scaled {
    int media, w, h;
    uint32_t *pixels;
    unsigned used;
};
#define IMAGE_CACHE 24
static struct scaled image_cache[IMAGE_CACHE];
static unsigned image_clock;

static void image_cache_clear(void) {
    for (int i = 0; i < IMAGE_CACHE; i++) {
        free(image_cache[i].pixels);
        memset(&image_cache[i], 0, sizeof(image_cache[i]));
    }
}

static const uint32_t *scaled_image(int index, int w, int h) {
    if (index < 0 || index >= nmedia || w <= 0 || h <= 0
        || (int64_t)w * h > 40 * 1024 * 1024) {
        return NULL;
    }
    image_clock++;
    int oldest = 0;
    for (int i = 0; i < IMAGE_CACHE; i++) {
        struct scaled *c = &image_cache[i];
        if (c->pixels != NULL && c->media == index && c->w == w && c->h == h) {
            c->used = image_clock;
            return c->pixels;
        }
        if (c->used < image_cache[oldest].used) {
            oldest = i;
        }
    }
    struct media *m = &media[index];
    if (!media_decode(m)) {
        return NULL;
    }
    uint32_t *pixels = malloc((size_t)w * (size_t)h * 4);
    if (pixels == NULL) {
        return NULL;
    }
    image_resample(m->pixels, (uint32_t)m->w, 0, 0, (uint32_t)m->w,
                   (uint32_t)m->h, pixels, (uint32_t)w, (uint32_t)w,
                   (uint32_t)h);
    struct scaled *c = &image_cache[oldest];
    free(c->pixels);
    c->media = index;
    c->w = w;
    c->h = h;
    c->pixels = pixels;
    c->used = image_clock;
    return pixels;
}

/* --- primitive drawing on a canvas (clipped by the canvas) */

static void put_span(struct opos_canvas *cv, int x1, int x2, int y,
                     uint32_t colour) {
    if (x2 > x1) {
        opos_fill_rect(cv, x1, y, x2 - x1, 1, colour);
    }
}

/* Left and right edge (exclusive) of a shape's row (0..h-1), inset. */
static bool shape_span(int type, int w, int h, int row, int inset, int *l,
                       int *r) {
    int iw = w - 2 * inset, ih = h - 2 * inset, ry = row - inset;
    if (iw <= 0 || ih <= 0 || ry < 0 || ry >= ih) {
        return false;
    }
    if (type == SH_ELLIPSE) {
        double a = iw / 2.0, b = ih / 2.0;
        double dy = (ry + 0.5 - b) / b;
        double half = a * sqrt(1 - dy * dy > 0 ? 1 - dy * dy : 0);
        *l = inset + (int)floor(a - half + 0.5);
        *r = inset + (int)floor(a + half + 0.5);
        return *r > *l;
    }
    if (type == SH_ROUND) {
        int radius = (iw < ih ? iw : ih) / 6;
        int dy = 0;
        if (ry < radius) {
            dy = radius - ry;
        } else if (ry >= ih - radius) {
            dy = ry - (ih - radius) + 1;
        }
        int cut = 0;
        if (dy > 0) {
            double q = (double)radius * radius - (double)(dy - 0.5) * (dy - 0.5);
            cut = radius - (int)floor(sqrt(q > 0 ? q : 0) + 0.5);
        }
        *l = inset + cut;
        *r = inset + iw - cut;
        return *r > *l;
    }
    *l = inset;
    *r = inset + iw;
    return true;
}

static void draw_geometry(struct opos_canvas *cv, int type, int x, int y,
                          int w, int h, uint32_t fill, uint32_t line,
                          int line_px) {
    for (int row = 0; row < h; row++) {
        int ol, or_, il, ir;
        bool outer = shape_span(type, w, h, row, 0, &ol, &or_);
        if (!outer) {
            continue;
        }
        if (line == NO_COLOUR || line_px <= 0) {
            if (fill != NO_COLOUR) {
                put_span(cv, x + ol, x + or_, y + row, fill);
            }
            continue;
        }
        bool inner = shape_span(type, w, h, row, line_px, &il, &ir);
        if (!inner) {
            put_span(cv, x + ol, x + or_, y + row, line);
            continue;
        }
        put_span(cv, x + ol, x + il, y + row, line);
        put_span(cv, x + ir, x + or_, y + row, line);
        if (fill != NO_COLOUR) {
            put_span(cv, x + il, x + ir, y + row, fill);
        }
    }
}

static void draw_thick_line(struct opos_canvas *cv, int x0, int y0, int x1,
                            int y1, int width, uint32_t colour) {
    if (width < 1) {
        width = 1;
    }
    int half = width / 2;
    bool steep = abs(y1 - y0) > abs(x1 - x0);
    for (int k = -half; k < width - half; k++) {
        if (steep) {
            opos_draw_line(cv, x0 + k, y0, x1 + k, y1, colour);
        } else {
            opos_draw_line(cv, x0, y0 + k, x1, y1 + k, colour);
        }
    }
}

static void draw_dashed_rect(struct opos_canvas *cv, int x, int y, int w,
                             int h, uint32_t colour) {
    for (int k = 0; k < w; k += 6) {
        int n = w - k < 3 ? w - k : 3;
        opos_fill_rect(cv, x + k, y, n, 1, colour);
        opos_fill_rect(cv, x + k, y + h - 1, n, 1, colour);
    }
    for (int k = 0; k < h; k += 6) {
        int n = h - k < 3 ? h - k : 3;
        opos_fill_rect(cv, x, y + k, 1, n, colour);
        opos_fill_rect(cv, x + w - 1, y + k, 1, n, colour);
    }
}

static uint32_t blend(uint32_t under, uint32_t over, int alpha) {
    int r = (int)(under >> 16 & 0xff), g = (int)(under >> 8 & 0xff),
        b = (int)(under & 0xff);
    r += ((int)(over >> 16 & 0xff) - r) * alpha / 255;
    g += ((int)(over >> 8 & 0xff) - g) * alpha / 255;
    b += ((int)(over & 0xff) - b) * alpha / 255;
    return (uint32_t)(r << 16 | g << 8 | b);
}

static void draw_picture(struct opos_canvas *cv, const struct shape *s,
                         int x, int y, int w, int h) {
    const uint32_t *pixels = scaled_image(s->media, w, h);
    if (pixels == NULL) {
        draw_geometry(cv, SH_RECT, x, y, w, h, 0xd9d9d9, 0x7f7f7f, 1);
        if (w > 80 && h > 20) {
            opos_draw_text(cv, x + (w - opos_text_width("PICTURE", 1)) / 2,
                           y + h / 2 - 4, "PICTURE", 0x595959, 1);
        }
        return;
    }
    for (int row = 0; row < h; row++) {
        int ty = y + row;
        if (ty < 0 || ty >= cv->height) {
            continue;
        }
        int src_row = (s->flip & FLIP_V) ? h - 1 - row : row;
        uint32_t *out = cv->pixels + (size_t)ty * (size_t)cv->stride;
        for (int col = 0; col < w; col++) {
            int tx = x + col;
            if (tx < 0 || tx >= cv->width) {
                continue;
            }
            int src_col = (s->flip & FLIP_H) ? w - 1 - col : col;
            uint32_t p = pixels[(size_t)src_row * (size_t)w + (size_t)src_col];
            uint32_t alpha = p >> 24;
            if (alpha == 255) {
                out[tx] = p & 0xffffff;
            } else if (alpha > 0) {
                out[tx] = blend(out[tx], p, (int)alpha);
            }
        }
    }
}

/* --- text drawing */

static bool presenting;

static void draw_run(struct opos_canvas *cv, int x, int baseline, cstyle s,
                     int size, const char *text, size_t length, int width_px) {
    uint32_t colour = ST_COLOR(s);
    struct opos_font *font = font_for(s);
    if (size < 4 || font == NULL) {
        /* Too small to read (thumbnails): a grey bar. */
        if (width_px > 0) {
            opos_fill_rect(cv, x, baseline - (size + 1) / 2, width_px,
                           size > 2 ? size / 2 : 1, blend(colour, 0xffffff, 90));
        }
        return;
    }
    opos_font_draw(font, cv, x, baseline, size, text, length, colour);
    uint32_t flags = ST_FLAGS(s);
    int thick = size / 14 < 1 ? 1 : size / 14;
    if (flags & S_UNDER) {
        opos_fill_rect(cv, x, baseline + size / 8, width_px, thick, colour);
    }
    if (flags & S_STRIKE) {
        opos_fill_rect(cv, x, baseline - size * 3 / 10, width_px, thick, colour);
    }
}

#define SELECTION_COLOUR 0xb4d5feu

static bool sel_range_ordered(int *p1, int *o1, int *p2, int *o2);

static void draw_text(struct opos_canvas *cv, struct shape *s, double scale,
                      bool edited, bool prompt) {
    layout_shape(s, scale);
    int ox, oy;
    text_origin(s, scale, &ox, &oy);
    int p1 = 0, o1 = 0, p2 = 0, o2 = 0;
    bool selection = edited && sel_range_ordered(&p1, &o1, &p2, &o2);
    if (prompt) {
        /* An empty placeholder: what it is for, in grey. */
        const char *hint = s->ph == PH_TITLE || s->ph == PH_CTR_TITLE
            ? "Click to add title" : s->ph == PH_SUBTITLE
            ? "Click to add subtitle" : "Click to add text";
        struct para *p = &s->paras[0];
        cstyle st = st_with_colour(p->end_style, 0x8c8c8c);
        int size = size_px(st, scale, s->font_scale);
        int w = text_width64(st, size, hint, strlen(hint)) / 64;
        struct line *ln = &p->lines[0];
        int x = ox + ln->x0;
        int width = text_area_width(s, scale);
        if (p->align == A_CENTER) {
            x = ox + (width - w) / 2;
        } else if (p->align == A_RIGHT) {
            x = ox + width - w;
        }
        int base = oy + ln->y + (ln->height - ln->ascent - ln->descent) / 2
            + ln->ascent;
        if (p->bullet != BU_NONE) {
            char b[32];
            bullet_text(p, b, sizeof(b));
            int bf, br, bx;
            para_edges(p, scale, s->font_scale, &bf, &br, &bx);
            draw_run(cv, ox + bx, base, st, size, b, strlen(b), size / 2);
        }
        draw_run(cv, x, base, st, size, hint, strlen(hint), w);
        return;
    }
    for (int pi = 0; pi < s->npara; pi++) {
        struct para *p = &s->paras[pi];
        for (int l = 0; l < p->nlines; l++) {
            struct line *ln = &p->lines[l];
            int top = oy + ln->y;
            if (top > cv->height || top + ln->height < 0) {
                continue;
            }
            int base = top + (ln->height - ln->ascent - ln->descent) / 2
                + ln->ascent;
            /* Selection behind the text. */
            if (selection && pi >= p1 && pi <= p2) {
                int from = pi == p1 ? o1 : 0, to = pi == p2 ? o2 : p->len;
                int a = from > ln->start ? from : ln->start;
                int b = to < ln->end ? to : ln->end;
                bool through = pi < p2 && ln->last && to >= ln->end;
                if (a < b || (through && a <= b) || (a == b && from < to
                                                      && ln->start == ln->end)) {
                    int xa = ox + line_x(s, p, ln, a, scale);
                    int xb = ox + line_x(s, p, ln, b, scale);
                    if (through) {
                        xb += size_px(p->end_style, scale, s->font_scale) / 3;
                    }
                    opos_fill_rect(cv, xa, top, xb - xa, ln->height,
                                   SELECTION_COLOUR);
                }
            }
            if (l == 0 && p->bullet != BU_NONE && p->len > 0) {
                char b[32];
                bullet_text(p, b, sizeof(b));
                int bf, br, bx;
                para_edges(p, scale, s->font_scale, &bf, &br, &bx);
                cstyle bs = bullet_style(p);
                int size = size_px(bs, scale, s->font_scale);
                int shift = line_x(s, p, ln, ln->start, scale) - ln->x0;
                draw_run(cv, ox + bx + shift, base, bs, size, b, strlen(b),
                         text_width64(bs, size, b, strlen(b)) / 64);
            }
            /* Runs of one look. */
            int i = ln->start;
            while (i < ln->end) {
                if (p->text[i] == '\n') {
                    break;
                }
                int j = i;
                while (j < ln->end && p->st[j] == p->st[i] && p->text[j] != '\n'
                       && p->text[j] != '\t') {
                    j++;
                }
                if (p->text[i] == '\t') {
                    i++;
                    continue;
                }
                int stop = j;
                int xa = ox + line_x(s, p, ln, i, scale);
                int xb = ox + line_x(s, p, ln, stop, scale);
                int size = size_px(p->st[i], scale, s->font_scale);
                if (p->align == A_JUSTIFY && !ln->last && ln->spaces > 0) {
                    /* Word by word, so the spaces stretch. */
                    int k = i;
                    while (k < stop) {
                        int e = k;
                        while (e < stop && p->text[e] != ' ') {
                            e++;
                        }
                        int wx = ox + line_x(s, p, ln, k, scale);
                        int we = ox + line_x(s, p, ln, e, scale);
                        if (e > k) {
                            draw_run(cv, wx, base, p->st[i], size, p->text + k,
                                     (size_t)(e - k), we - wx);
                        }
                        k = e < stop ? e + 1 : e;
                    }
                } else {
                    draw_run(cv, xa, base, p->st[i], size, p->text + i,
                             (size_t)(stop - i), xb - xa);
                }
                i = j;
            }
        }
    }
}

/* Draws one shape at a scale onto a canvas whose (0,0) is the slide's
   top-left corner. */
static void draw_shape(struct opos_canvas *cv, struct shape *s,
                       const struct view *v, bool edited) {
    double scale = v->scale;
    int x = ez(scale, s->x), y = ez(scale, s->y);
    int w = ez(scale, s->x + s->w) - x, h = ez(scale, s->y + s->h) - y;
    int lw = s->line_w > 0 ? ez(scale, s->line_w) : 0;
    if (s->line != NO_COLOUR && lw < 1) {
        lw = 1;
    }
    switch (s->type) {
    case SH_PICTURE:
        draw_picture(cv, s, x, y, w, h);
        if (s->line != NO_COLOUR) {
            draw_geometry(cv, SH_RECT, x, y, w, h, NO_COLOUR, s->line, lw);
        }
        return;
    case SH_LINE: {
        int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        if (s->flip & FLIP_H) {
            x0 = x + w;
            x1 = x;
        }
        if (s->flip & FLIP_V) {
            y0 = y + h;
            y1 = y;
        }
        draw_thick_line(cv, x0, y0, x1, y1, lw,
                        s->line != NO_COLOUR ? s->line : 0x000000);
        return;
    }
    default:
        break;
    }
    if (w > 0 && h > 0) {
        draw_geometry(cv, s->type == SH_TEXT ? SH_RECT : s->type, x, y, w, h,
                      s->fill, s->line, lw);
    }
    bool empty = shape_text_empty(s);
    if (v->editor && s->type == SH_TEXT && s->line == NO_COLOUR
        && s->fill == NO_COLOUR && (s->ph != PH_NONE || empty) && w > 2
        && h > 2) {
        draw_dashed_rect(cv, x, y, w, h, 0xa6a6a6);
    }
    if (s->npara == 0) {
        return;
    }
    bool prompt = v->editor && empty && s->ph != PH_NONE && !edited;
    draw_text(cv, s, scale, edited, prompt);
}

static void draw_slide(struct opos_canvas *cv, struct slide *sl,
                       const struct view *v, int edited_shape) {
    opos_fill_rect(cv, 0, 0, cv->width, cv->height, sl->background);
    if (sl->deco >= 0 && sl->deco < ndecos) {
        struct view under = {v->scale, false};
        struct slide *d = &decos[sl->deco];
        for (int i = 0; i < d->n; i++) {
            draw_shape(cv, &d->shapes[i], &under, false);
        }
    }
    for (int i = 0; i < sl->n; i++) {
        draw_shape(cv, &sl->shapes[i], v, i == edited_shape);
    }
}

/* =========================================================== the editor */

#define ROW_H 40
#define TOOLBAR_H (2 * ROW_H)
#define STATUS_H 28
#define PANEL_W 196
#define THUMB_W 150

#define UI_BACK OPOS_RGB(30, 34, 46)
#define UI_PANEL OPOS_RGB(40, 46, 62)
#define UI_BUTTON OPOS_RGB(56, 64, 86)
#define UI_HOVER OPOS_RGB(72, 82, 110)
#define UI_ACTIVE OPOS_RGB(70, 110, 190)
#define UI_TEXT OPOS_RGB(230, 236, 248)
#define UI_DIM OPOS_RGB(130, 140, 165)
#define UI_WARN OPOS_RGB(245, 205, 110)
#define WORKSPACE OPOS_RGB(62, 66, 78)
#define HANDLE_COLOUR OPOS_RGB(26, 115, 232)

static struct opos_canvas canvas;
static int view_width, view_height;
static int mouse_x, mouse_y;
static bool shift_down, ctrl_down;
static int panel_scroll;

static char path[PATH_MAX_LENGTH];
static char file_name[64] = "Untitled";
static bool can_write;
static bool modified;
static char message[112];
static bool message_warn;
static bool close_removes_window;
static bool headless;

enum pending { PENDING_NONE, PENDING_CLOSE, PENDING_NEW, PENDING_OPEN };
static enum pending pending;

static void say(const char *text, bool warn) {
    snprintf(message, sizeof(message), "%s", text);
    message_warn = warn;
}

static void update_title(void) {
    if (headless) {
        return;
    }
    /* Titles show at most 31 characters: long names go without the
       program's name, and the longest are cut short. */
    char title[96];
    const char *star = modified ? "*" : "";
    snprintf(title, sizeof(title), "Presentation - %s%s", file_name, star);
    if (strlen(title) > 31) {
        snprintf(title, sizeof(title), "%s%s", file_name, star);
    }
    if (strlen(title) > 31) {
        snprintf(title, sizeof(title), "%.27s...%s", file_name, star);
    }
    opos_window_title(title);
}

static void set_modified(bool value) {
    if (modified != value) {
        modified = value;
        update_title();
    }
}

static struct slide *cur(void) {
    return &slides[cur_slide];
}

static struct shape *selected(void) {
    return sel >= 0 && sel < cur()->n ? &cur()->shapes[sel] : NULL;
}

/* --- where the slide is on screen */

static struct view main_view;
static int slide_x0, slide_y0, slide_px_w, slide_px_h;

static void place_slide(void) {
    int area_x = PANEL_W, area_y = TOOLBAR_H;
    int area_w = view_width - PANEL_W, area_h = view_height - TOOLBAR_H - STATUS_H;
    int margin = 28;
    double sw = (double)(area_w - 2 * margin) / (double)slide_w;
    double sh = (double)(area_h - 2 * margin) / (double)slide_h;
    double scale = sw < sh ? sw : sh;
    if (scale <= 0) {
        scale = 1e-5;
    }
    main_view.scale = scale;
    main_view.editor = true;
    slide_px_w = ez(scale, slide_w);
    slide_px_h = ez(scale, slide_h);
    slide_x0 = area_x + (area_w - slide_px_w) / 2;
    slide_y0 = area_y + (area_h - slide_px_h) / 2;
}

/* Screen point to slide EMU. */
static int64_t to_emu_x(int x) {
    return (int64_t)floor((x - slide_x0) / main_view.scale + 0.5);
}

static int64_t to_emu_y(int y) {
    return (int64_t)floor((y - slide_y0) / main_view.scale + 0.5);
}

static struct opos_canvas sub_canvas(struct opos_canvas *base, int x, int y,
                                     int w, int h) {
    struct opos_canvas v = {0};
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > base->width) {
        w = base->width - x;
    }
    if (y + h > base->height) {
        h = base->height - y;
    }
    if (w <= 0 || h <= 0) {
        return v;
    }
    v.pixels = base->pixels + (size_t)y * (size_t)base->stride + (size_t)x;
    v.width = w;
    v.height = h;
    v.stride = base->stride;
    return v;
}

/* --- undo: whole-deck snapshots */

struct snap {
    struct slide *slides;
    int nslides;
    int cur_slide, sel;
    bool editing;
    int cur_p, cur_o, anc_p, anc_o;
    int64_t slide_w, slide_h;
    size_t bytes;
};

enum edit { EDIT_NONE, EDIT_TYPE, EDIT_DELETE, EDIT_MOVE, EDIT_OTHER };
static struct snap undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int undo_count, redo_count;
static size_t undo_bytes;
static enum edit last_edit;
static int last_edit_shape = -1, last_edit_slide = -1;

static void snap_free(struct snap *s) {
    for (int i = 0; i < s->nslides; i++) {
        slide_free(&s->slides[i]);
    }
    free(s->slides);
    memset(s, 0, sizeof(*s));
}

static size_t slide_bytes(const struct slide *sl) {
    size_t total = sizeof(*sl) + (size_t)sl->n * sizeof(struct shape);
    for (int i = 0; i < sl->n; i++) {
        for (int k = 0; k < sl->shapes[i].npara; k++) {
            total += sizeof(struct para)
                + (size_t)sl->shapes[i].paras[k].len * (1 + sizeof(cstyle));
        }
    }
    return total;
}

static bool snap_take(struct snap *s) {
    memset(s, 0, sizeof(*s));
    s->slides = calloc((size_t)(nslides > 0 ? nslides : 1), sizeof(*s->slides));
    if (s->slides == NULL) {
        return false;
    }
    for (int i = 0; i < nslides; i++) {
        if (!slide_copy(&s->slides[i], &slides[i])) {
            snap_free(s);
            return false;
        }
        s->nslides = i + 1;
        s->bytes += slide_bytes(&slides[i]);
    }
    s->cur_slide = cur_slide;
    s->sel = sel;
    s->editing = editing;
    s->cur_p = cur_p;
    s->cur_o = cur_o;
    s->anc_p = anc_p;
    s->anc_o = anc_o;
    s->slide_w = slide_w;
    s->slide_h = slide_h;
    return true;
}

/* Puts a snapshot back (it is used up). */
static void snap_restore(struct snap *s) {
    for (int i = 0; i < nslides; i++) {
        slide_free(&slides[i]);
    }
    free(slides);
    slides = s->slides;
    nslides = slides_cap = s->nslides;
    for (int i = 0; i < nslides; i++) {
        slides[i].version += 1000;
    }
    cur_slide = s->cur_slide < nslides ? s->cur_slide : nslides - 1;
    sel = s->sel;
    editing = s->editing;
    cur_p = s->cur_p;
    cur_o = s->cur_o;
    anc_p = s->anc_p;
    anc_o = s->anc_o;
    slide_w = s->slide_w;
    slide_h = s->slide_h;
    if (selected() == NULL) {
        sel = -1;
        editing = false;
    }
    memset(s, 0, sizeof(*s));
}

static void stack_push(struct snap *stack, int *count, struct snap *s) {
    if (*count == UNDO_MAX) {
        undo_bytes -= stack[0].bytes;
        snap_free(&stack[0]);
        memmove(stack, stack + 1, (UNDO_MAX - 1) * sizeof(*stack));
        (*count)--;
    }
    stack[(*count)++] = *s;
    undo_bytes += s->bytes;
    while (undo_bytes > UNDO_BYTES && undo_count > 1) {
        undo_bytes -= undo_stack[0].bytes;
        snap_free(&undo_stack[0]);
        memmove(undo_stack, undo_stack + 1,
                (size_t)(undo_count - 1) * sizeof(*undo_stack));
        undo_count--;
    }
}

static void stack_clear(struct snap *stack, int *count) {
    for (int i = 0; i < *count; i++) {
        undo_bytes -= stack[i].bytes;
        snap_free(&stack[i]);
    }
    *count = 0;
}

/* Before a change: keeps the deck as it is for Undo. Typing and deleting
   in one shape are one step until something else happens. */
static void remember(enum edit kind) {
    if (kind != EDIT_NONE && kind == last_edit
        && (kind == EDIT_TYPE || kind == EDIT_DELETE)
        && last_edit_shape == sel && last_edit_slide == cur_slide) {
        return;
    }
    struct snap s;
    if (!snap_take(&s)) {
        say("NOT ENOUGH MEMORY TO KEEP THIS FOR UNDO", true);
        return;
    }
    stack_push(undo_stack, &undo_count, &s);
    stack_clear(redo_stack, &redo_count);
    last_edit = kind;
    last_edit_shape = sel;
    last_edit_slide = cur_slide;
}

static void forget_last(void) { /* a remembered step that changed nothing */
    if (undo_count > 0) {
        undo_count--;
        undo_bytes -= undo_stack[undo_count].bytes;
        snap_free(&undo_stack[undo_count]);
    }
    last_edit = EDIT_NONE;
}

static void changed(void) {
    cur()->version++;
    set_modified(true);
}

static void step(struct snap *from, int *from_count, struct snap *to,
                 int *to_count) {
    if (*from_count == 0) {
        return;
    }
    struct snap now;
    if (!snap_take(&now)) {
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    struct snap s = from[--(*from_count)];
    undo_bytes -= s.bytes;
    snap_restore(&s);
    stack_push(to, to_count, &now);
    last_edit = EDIT_NONE;
    set_modified(true);
}

static void undo(void) { step(undo_stack, &undo_count, redo_stack, &redo_count); }
static void redo(void) { step(redo_stack, &redo_count, undo_stack, &undo_count); }

static void history_clear(void) {
    stack_clear(undo_stack, &undo_count);
    stack_clear(redo_stack, &redo_count);
    undo_bytes = 0;
    last_edit = EDIT_NONE;
}

/* --- the selection inside text */

static bool has_text_selection(void) {
    return editing && (cur_p != anc_p || cur_o != anc_o);
}

static bool sel_range_ordered(int *p1, int *o1, int *p2, int *o2) {
    if (!has_text_selection()) {
        return false;
    }
    if (anc_p < cur_p || (anc_p == cur_p && anc_o < cur_o)) {
        *p1 = anc_p;
        *o1 = anc_o;
        *p2 = cur_p;
        *o2 = cur_o;
    } else {
        *p1 = cur_p;
        *o1 = cur_o;
        *p2 = anc_p;
        *o2 = anc_o;
    }
    return true;
}

static void caret_moved(bool extend) {
    if (!extend) {
        anc_p = cur_p;
        anc_o = cur_o;
    }
    last_edit = EDIT_NONE;
}

static void stop_editing(void) {
    editing = false;
    goal_x = -1;
    last_edit = EDIT_NONE;
}

static void select_shape(int index) {
    if (index != sel) {
        stop_editing();
    }
    sel = index;
}

static void start_editing(int p, int o) {
    struct shape *s = selected();
    if (s == NULL || !shape_has_text_area(s)) {
        return;
    }
    if (s->npara == 0) {
        cstyle style = st_make(0, 1800, s->fill != NO_COLOUR ? 0xffffff
                                                             : TEXT_BLACK);
        shape_start_text(s, style, s->type == SH_TEXT ? A_LEFT : A_CENTER,
                         BU_NONE);
        if (s->type != SH_TEXT) {
            s->anchor = AN_MIDDLE;
        }
        if (s->npara == 0) {
            return;
        }
    }
    editing = true;
    cur_p = anc_p = p < s->npara ? p : s->npara - 1;
    cur_o = anc_o = o <= s->paras[cur_p].len ? o : s->paras[cur_p].len;
    goal_x = -1;
}

/* Text boxes that grow with their text; placeholders whose text shrinks
   to fit. */
static void fit_height(struct shape *s) {
    if (s == NULL) {
        return;
    }
    double scale = main_view.scale > 0 ? main_view.scale : 0.0001;
    if (s->autofit == FIT_TEXT && shape_has_text_area(s)) {
        int area = ez(scale, s->h - s->ins_t - s->ins_b);
        int fs = 100000;
        for (;;) {
            s->font_scale = fs;
            layout_shape(s, scale);
            if (s->text_h <= area || fs <= 25000) {
                break;
            }
            fs -= 7500;
        }
        return;
    }
    if (s->autofit != FIT_SHAPE || s->type != SH_TEXT) {
        return;
    }
    layout_shape(s, scale);
    int64_t need = (int64_t)floor(s->text_h / scale + 0.5) + s->ins_t + s->ins_b;
    if (need != s->h && need > 0) {
        s->h = need;
    }
}

/* --- text edits */

static cstyle type_style;
static bool type_style_set;

static cstyle style_for_typing(void) {
    struct shape *s = selected();
    if (type_style_set) {
        return type_style;
    }
    return para_style_at(&s->paras[cur_p], cur_o);
}

static void delete_range(struct shape *s, int p1, int o1, int p2, int o2) {
    if (p1 == p2) {
        para_delete(&s->paras[p1], o1, o2);
    } else {
        struct para *a = &s->paras[p1], *b = &s->paras[p2];
        para_delete(a, o1, a->len);
        para_insert(a, a->len, b->text + o2, b->len - o2, b->st + o2, 0);
        shape_remove_paras(s, p1 + 1, p2 + 1);
    }
    cur_p = anc_p = p1;
    cur_o = anc_o = o1;
}

static bool delete_selection(void) {
    int p1, o1, p2, o2;
    if (!sel_range_ordered(&p1, &o1, &p2, &o2)) {
        return false;
    }
    struct shape *s = selected();
    cstyle keep = para_style_at(&s->paras[p1], o1 < s->paras[p1].len ? o1 + 1 : o1);
    delete_range(s, p1, o1, p2, o2);
    if (s->paras[p1].len == 0) {
        s->paras[p1].end_style = keep;
    }
    return true;
}

static size_t shape_text_bytes(const struct shape *s) {
    size_t total = 0;
    for (int i = 0; i < s->npara; i++) {
        total += (size_t)s->paras[i].len + 1;
    }
    return total;
}

static void insert_text(const char *text, int length, cstyle style) {
    struct shape *s = selected();
    if (s == NULL || !editing) {
        return;
    }
    if (shape_text_bytes(s) + (size_t)length > TEXT_MAX) {
        say("THAT IS TOO MUCH TEXT FOR ONE SHAPE", true);
        return;
    }
    if (para_insert(&s->paras[cur_p], cur_o, text, length, NULL, style)) {
        cur_o += length;
        anc_p = cur_p;
        anc_o = cur_o;
    }
}

static void type_char(uint32_t code) {
    char utf[8];
    int n = utf8_put(utf, code);
    remember(EDIT_TYPE);
    cstyle style = style_for_typing();
    delete_selection();
    insert_text(utf, n, style);
    type_style_set = false;
    fit_height(selected());
    changed();
}

/* Enter: a new paragraph with the same list and look. */
static void split_paragraph(void) {
    struct shape *s = selected();
    remember(EDIT_OTHER);
    delete_selection();
    struct para *p = &s->paras[cur_p];
    cstyle style = para_style_at(p, cur_o);
    struct para *next = shape_insert_para(s, cur_p + 1, style);
    if (next == NULL) {
        return;
    }
    p = &s->paras[cur_p];
    struct para copy = *p;
    next->align = copy.align;
    next->level = copy.level;
    next->bullet = copy.bullet;
    next->num_type = copy.num_type;
    next->bullet_char = copy.bullet_char;
    next->mar_l = copy.mar_l;
    next->indent = copy.indent;
    next->space_before = copy.space_before;
    next->space_after = copy.space_after;
    next->line_pct = copy.line_pct;
    para_insert(next, 0, p->text + cur_o, p->len - cur_o, p->st + cur_o, 0);
    para_delete(p, cur_o, p->len);
    if (p->len == 0) {
        p->end_style = style;
    }
    cur_p++;
    cur_o = 0;
    caret_moved(false);
    fit_height(s);
    changed();
}

static void key_backspace(bool word) {
    struct shape *s = selected();
    remember(EDIT_DELETE);
    if (delete_selection()) {
        fit_height(s);
        changed();
        return;
    }
    struct para *p = &s->paras[cur_p];
    if (cur_o == 0) {
        if (p->bullet != BU_NONE || p->level > 0) {
            /* First a list item stops being one, like other programs. */
            if (p->level > 0) {
                level_defaults(p, p->level - 1);
            } else {
                p->bullet = BU_NONE;
                p->mar_l = p->indent = 0;
            }
            p->dirty = true;
            changed();
            return;
        }
        if (cur_p == 0) {
            return;
        }
        int at = s->paras[cur_p - 1].len;
        delete_range(s, cur_p - 1, at, cur_p, 0);
        fit_height(s);
        changed();
        return;
    }
    int from = prev_char(p, cur_o);
    if (word) {
        while (from > 0 && p->text[from - 1] == ' ') {
            from--;
        }
        while (from > 0 && is_word_byte((unsigned char)p->text[from - 1])) {
            from--;
        }
    }
    para_delete(p, from, cur_o);
    cur_o = anc_o = from;
    fit_height(s);
    changed();
}

static void key_delete(void) {
    struct shape *s = selected();
    remember(EDIT_DELETE);
    if (delete_selection()) {
        fit_height(s);
        changed();
        return;
    }
    struct para *p = &s->paras[cur_p];
    if (cur_o >= p->len) {
        if (cur_p + 1 < s->npara) {
            delete_range(s, cur_p, cur_o, cur_p + 1, 0);
        }
    } else {
        para_delete(p, cur_o, next_char(p, cur_o));
    }
    fit_height(s);
    changed();
}

/* Changes the look of the selected text, or of what is typed next, or
   (when not editing) of all the text in the selected shape. */
static void restyle(cstyle (*change)(cstyle, uint32_t), uint32_t value) {
    struct shape *s = selected();
    if (s == NULL || s->npara == 0) {
        return;
    }
    if (editing && !has_text_selection()) {
        type_style = change(style_for_typing(), value);
        type_style_set = true;
        if (s->paras[cur_p].len == 0) {
            s->paras[cur_p].end_style = type_style;
            s->paras[cur_p].dirty = true;
        }
        return;
    }
    remember(EDIT_OTHER);
    int p1 = 0, o1 = 0, p2 = s->npara - 1, o2 = s->paras[s->npara - 1].len;
    if (editing) {
        sel_range_ordered(&p1, &o1, &p2, &o2);
    }
    for (int pi = p1; pi <= p2; pi++) {
        struct para *p = &s->paras[pi];
        int from = pi == p1 ? o1 : 0, to = pi == p2 ? o2 : p->len;
        for (int i = from; i < to; i++) {
            p->st[i] = change(p->st[i], value);
        }
        if (!editing || p->len == 0 || (from == 0 && to == p->len)) {
            p->end_style = change(p->end_style, value);
        }
        p->dirty = true;
    }
    fit_height(s);
    changed();
}

static bool text_has(uint32_t flag) {
    struct shape *s = selected();
    if (s == NULL || s->npara == 0) {
        return false;
    }
    if (editing && !has_text_selection()) {
        return (ST_FLAGS(style_for_typing()) & flag) != 0;
    }
    int p1 = 0, o1 = 0, p2 = s->npara - 1, o2 = s->paras[s->npara - 1].len;
    if (editing) {
        sel_range_ordered(&p1, &o1, &p2, &o2);
    }
    bool any = false;
    for (int pi = p1; pi <= p2; pi++) {
        const struct para *p = &s->paras[pi];
        int from = pi == p1 ? o1 : 0, to = pi == p2 ? o2 : p->len;
        for (int i = from; i < to; i++) {
            if (p->text[i] == '\n' || p->text[i] == ' ') {
                continue;
            }
            any = true;
            if (!(ST_FLAGS(p->st[i]) & flag)) {
                return false;
            }
        }
    }
    if (!any) {
        return (ST_FLAGS(s->paras[p1].end_style) & flag) != 0;
    }
    return true;
}

static cstyle change_set_flag(cstyle s, uint32_t flag) {
    return st_with_flags(s, ST_FLAGS(s) | flag);
}

static cstyle change_clear_flag(cstyle s, uint32_t flag) {
    return st_with_flags(s, ST_FLAGS(s) & ~flag);
}

static cstyle change_size(cstyle s, uint32_t size) {
    return st_with_size(s, size);
}

static cstyle change_colour(cstyle s, uint32_t colour) {
    return st_with_colour(s, colour);
}

static const int font_sizes[] = {8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28,
                                 32, 36, 40, 44, 48, 54, 60, 66, 72, 80, 88,
                                 96};
#define FONT_SIZE_COUNT ((int)(sizeof(font_sizes) / sizeof(font_sizes[0])))

static cstyle change_size_step(cstyle s, uint32_t up) {
    int now = (int)ST_SIZE(s);
    if (up) {
        for (int i = 0; i < FONT_SIZE_COUNT; i++) {
            if (font_sizes[i] * 100 > now) {
                return st_with_size(s, (uint32_t)font_sizes[i] * 100);
            }
        }
        return st_with_size(s, (uint32_t)(now + 1200 > 65500 ? 65500 : now + 1200));
    }
    for (int i = FONT_SIZE_COUNT - 1; i >= 0; i--) {
        if (font_sizes[i] * 100 < now) {
            return st_with_size(s, (uint32_t)font_sizes[i] * 100);
        }
    }
    return s;
}

static void toggle_flag(uint32_t flag) {
    restyle(text_has(flag) ? change_clear_flag : change_set_flag, flag);
}

/* The paragraphs the change applies to. */
static void para_span(int *first, int *last) {
    struct shape *s = selected();
    if (!editing) {
        *first = 0;
        *last = s->npara - 1;
        return;
    }
    *first = cur_p < anc_p ? cur_p : anc_p;
    *last = cur_p < anc_p ? anc_p : cur_p;
}

static void set_align(int align) {
    struct shape *s = selected();
    if (s == NULL || s->npara == 0) {
        return;
    }
    remember(EDIT_OTHER);
    int first, last;
    para_span(&first, &last);
    for (int i = first; i <= last; i++) {
        s->paras[i].align = (uint8_t)align;
        s->paras[i].dirty = true;
    }
    changed();
}

static void toggle_list(int kind) {
    struct shape *s = selected();
    if (s == NULL || s->npara == 0) {
        return;
    }
    remember(EDIT_OTHER);
    int first, last;
    para_span(&first, &last);
    bool all = true;
    for (int i = first; i <= last; i++) {
        all = all && s->paras[i].bullet == kind;
    }
    for (int i = first; i <= last; i++) {
        struct para *p = &s->paras[i];
        p->bullet = (uint8_t)(all ? BU_NONE : kind);
        if (kind == BU_CHAR && p->bullet_char == 0) {
            p->bullet_char = 0x2022;
        }
        level_defaults(p, p->level);
        p->dirty = true;
    }
    fit_height(s);
    changed();
}

static void change_level(int delta) {
    struct shape *s = selected();
    remember(EDIT_OTHER);
    int first, last;
    para_span(&first, &last);
    for (int i = first; i <= last; i++) {
        struct para *p = &s->paras[i];
        int level = p->level + delta;
        if (level < 0 || level > 8) {
            continue;
        }
        level_defaults(p, level);
        /* Body text gets smaller one level down, like other programs. */
        if (s->ph == PH_BODY && level < 5) {
            uint32_t size = (uint32_t)body_sizes[level];
            for (int k = 0; k < p->len; k++) {
                p->st[k] = st_with_size(p->st[k], size);
            }
            p->end_style = st_with_size(p->end_style, size);
        }
        p->dirty = true;
    }
    fit_height(s);
    changed();
}

/* --- the caret in the text */

/* Finds the line holding byte o (the next line at a soft break). */
static int find_line(const struct para *p, int o) {
    for (int l = 0; l < p->nlines; l++) {
        const struct line *ln = &p->lines[l];
        if (o < ln->end || (o == ln->end && (l == p->nlines - 1 || ln->last))) {
            if (o == ln->end && ln->end > ln->start
                && p->text[ln->end - 1] == '\n' && l + 1 < p->nlines) {
                return l + 1;
            }
            return l;
        }
    }
    return p->nlines - 1;
}

/* Screen position of the caret: x, top and height. */
static void caret_spot(int *x, int *top, int *height) {
    struct shape *s = selected();
    double scale = main_view.scale;
    layout_shape(s, scale);
    int ox, oy;
    text_origin(s, scale, &ox, &oy);
    struct para *p = &s->paras[cur_p];
    int l = find_line(p, cur_o);
    struct line *ln = &p->lines[l];
    *x = slide_x0 + ox + line_x(s, p, ln, cur_o, scale);
    *top = slide_y0 + oy + ln->y;
    *height = ln->height;
}

/* Text position at a screen point inside the edited shape. */
static void text_hit(int px_, int py, int *out_p, int *out_o) {
    struct shape *s = selected();
    double scale = main_view.scale;
    layout_shape(s, scale);
    int ox, oy;
    text_origin(s, scale, &ox, &oy);
    int x = px_ - slide_x0 - ox, y = py - slide_y0 - oy;
    int pi = 0, li = 0;
    bool found = false;
    for (int i = 0; i < s->npara && !found; i++) {
        struct para *p = &s->paras[i];
        for (int l = 0; l < p->nlines; l++) {
            pi = i;
            li = l;
            if (y < p->lines[l].y + p->lines[l].height) {
                found = true;
                break;
            }
        }
    }
    struct para *p = &s->paras[pi];
    struct line *ln = &p->lines[li];
    int best = ln->start, best_d = 1 << 30;
    int end = ln->end;
    if (end > ln->start && p->text[end - 1] == '\n') {
        end--;
    }
    if (!ln->last) {
        while (end > ln->start && p->text[end - 1] == ' ') {
            end--;
        }
        end = end < ln->end ? end : ln->end;
    }
    for (int o = ln->start;;) {
        int d = abs(line_x(s, p, ln, o, scale) - x);
        if (d < best_d) {
            best_d = d;
            best = o;
        }
        if (o >= end) {
            break;
        }
        o = next_char(p, o);
    }
    if (!ln->last && best == ln->end) {
        best = prev_char(p, best);
    }
    *out_p = pi;
    *out_o = best;
}

static void caret_vertical(int dir, bool extend) {
    struct shape *s = selected();
    int x, top, height;
    caret_spot(&x, &top, &height);
    if (goal_x < 0) {
        goal_x = x;
    }
    struct para *p = &s->paras[cur_p];
    int l = find_line(p, cur_o);
    int target_y;
    if (dir < 0) {
        if (l == 0 && cur_p == 0) {
            cur_o = 0;
            caret_moved(extend);
            return;
        }
        target_y = top - 2;
    } else {
        if (l == p->nlines - 1 && cur_p == s->npara - 1) {
            cur_o = p->len;
            caret_moved(extend);
            return;
        }
        target_y = top + height + 2;
    }
    int keep = goal_x;
    text_hit(goal_x, target_y, &cur_p, &cur_o);
    caret_moved(extend);
    goal_x = keep;
}

/* --- clipboard */

static struct shape clip_shape;
static bool have_clip_shape;
static char *clip_text; /* what was put on the system clipboard */

static char *selection_text(size_t *length) {
    struct shape *s = selected();
    int p1 = 0, o1 = 0, p2 = s->npara - 1, o2 = s->paras[s->npara - 1].len;
    if (editing && !sel_range_ordered(&p1, &o1, &p2, &o2)) {
        *length = 0;
        return NULL;
    }
    struct buf b = {0};
    for (int pi = p1; pi <= p2; pi++) {
        const struct para *p = &s->paras[pi];
        int from = pi == p1 ? o1 : 0, to = pi == p2 ? o2 : p->len;
        buf_put(&b, p->text + from, (size_t)(to - from));
        if (pi < p2) {
            buf_byte(&b, '\n');
        }
    }
    if (b.failed) {
        free(b.data);
        return NULL;
    }
    *length = b.length;
    return b.data;
}

static void put_on_clipboard(char *text, size_t length) {
    free(clip_text);
    clip_text = NULL;
    if (text == NULL) {
        return;
    }
    opos_clipboard_set(text, length > 65536 ? 65536 : length);
    clip_text = text;
}

static void copy_selection(bool cut) {
    struct shape *s = selected();
    if (s == NULL) {
        return;
    }
    if (editing) {
        size_t length;
        char *text = selection_text(&length);
        if (text == NULL) {
            return;
        }
        put_on_clipboard(text, length);
        if (have_clip_shape) {
            shape_free(&clip_shape);
            have_clip_shape = false;
        }
        if (cut) {
            remember(EDIT_OTHER);
            delete_selection();
            fit_height(s);
            changed();
        }
        return;
    }
    if (have_clip_shape) {
        shape_free(&clip_shape);
    }
    have_clip_shape = shape_copy(&clip_shape, s);
    size_t length = 0;
    char *text = s->npara > 0 ? selection_text(&length) : NULL;
    if (text == NULL || length == 0) {
        free(text);
        text = strdup(s->type == SH_PICTURE ? "[picture]" : "[shape]");
        length = text != NULL ? strlen(text) : 0;
    }
    put_on_clipboard(text, length);
    if (cut) {
        remember(EDIT_OTHER);
        slide_remove_shape(cur(), sel);
        sel = -1;
        changed();
    }
}

static void paste_shape(void) {
    struct shape copy;
    if (!shape_copy(&copy, &clip_shape)) {
        return;
    }
    remember(EDIT_OTHER);
    struct shape *s = slide_add_shape(cur(), -1);
    if (s == NULL) {
        shape_free(&copy);
        say("THIS SLIDE HAS AS MANY SHAPES AS IT CAN", true);
        return;
    }
    *s = copy;
    /* Pasting onto the same place moves it a little. */
    for (int i = 0; i < cur()->n - 1; i++) {
        struct shape *o = &cur()->shapes[i];
        if (o->x == s->x && o->y == s->y) {
            s->x += 182880;
            s->y += 182880;
            i = -1;
        }
    }
    sel = cur()->n - 1;
    editing = false;
    changed();
}

static struct shape *new_text_box(int64_t x, int64_t y, int64_t w);

static void paste_plain(const char *text, size_t length) {
    struct shape *s = selected();
    if (s == NULL || !editing) {
        /* Not in text: a new text box. */
        remember(EDIT_OTHER);
        int64_t w = slide_w / 2;
        s = new_text_box((slide_w - w) / 2, slide_h / 3, w);
        if (s == NULL) {
            return;
        }
        sel = cur()->n - 1;
        start_editing(0, 0);
        last_edit = EDIT_OTHER;
    } else {
        remember(EDIT_OTHER);
        delete_selection();
    }
    cstyle style = style_for_typing();
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i < length && text[i] != '\n') {
            continue;
        }
        size_t n = i - start;
        if (n > 0 && text[start + n - 1] == '\r') {
            n--;
        }
        /* Only printable text and tabs. */
        char *clean = malloc(n + 1);
        if (clean == NULL) {
            break;
        }
        size_t used = 0;
        for (size_t k = 0; k < n; k++) {
            unsigned char c = (unsigned char)text[start + k];
            if (c >= 0x20 || c == '\t') {
                clean[used++] = (char)c;
            }
        }
        insert_text(clean, (int)used, style);
        free(clean);
        if (i < length) {
            struct shape *sh = selected();
            struct para *p = &sh->paras[cur_p];
            struct para *next = shape_insert_para(sh, cur_p + 1, style);
            if (next == NULL) {
                break;
            }
            p = &sh->paras[cur_p];
            next->align = p->align;
            next->bullet = p->bullet;
            next->level = p->level;
            next->mar_l = p->mar_l;
            next->indent = p->indent;
            next->bullet_char = p->bullet_char;
            next->space_before = p->space_before;
            next->line_pct = p->line_pct;
            para_insert(next, 0, p->text + cur_o, p->len - cur_o, p->st + cur_o, 0);
            para_delete(p, cur_o, p->len);
            cur_p++;
            cur_o = 0;
            caret_moved(false);
        }
        start = i + 1;
    }
    fit_height(selected());
    changed();
}

static void paste(void) {
    static char buffer[65537];
    long length = opos_clipboard_get(buffer, sizeof(buffer) - 1);
    bool ours = false;
    if (length >= 0 && clip_text != NULL) {
        size_t n = strlen(clip_text);
        if (n > 65536) {
            n = 65536;
        }
        ours = (size_t)length == n && memcmp(buffer, clip_text, n) == 0;
    }
    if (length == -OPOS_EACCES || length < 0) {
        if (have_clip_shape && !editing) {
            paste_shape();
            return;
        }
        if (clip_text != NULL) {
            paste_plain(clip_text, strlen(clip_text));
            return;
        }
        say(length == -OPOS_EACCES
                ? "PRESENTATION MAY NOT READ THE CLIPBOARD (SECURITY CENTER)"
                : "COULD NOT READ THE CLIPBOARD", true);
        return;
    }
    if (ours && have_clip_shape && !editing) {
        paste_shape();
        return;
    }
    if (length == 0) {
        return;
    }
    buffer[length] = '\0';
    paste_plain(buffer, (size_t)length);
}

/* --- shapes and slides */

static struct shape *new_text_box(int64_t x, int64_t y, int64_t w) {
    struct shape *s = slide_add_shape(cur(), -1);
    if (s == NULL) {
        say("THIS SLIDE HAS AS MANY SHAPES AS IT CAN", true);
        return NULL;
    }
    s->type = SH_TEXT;
    s->x = x;
    s->y = y;
    s->w = w;
    s->autofit = FIT_SHAPE;
    shape_start_text(s, st_make(0, 1800, TEXT_BLACK), A_LEFT, BU_NONE);
    fit_height(s);
    return s;
}

static void delete_shape(void) {
    if (selected() == NULL) {
        return;
    }
    remember(EDIT_OTHER);
    slide_remove_shape(cur(), sel);
    sel = -1;
    editing = false;
    changed();
}

static void duplicate_shape(void) {
    struct shape *s = selected();
    if (s == NULL) {
        return;
    }
    struct shape copy;
    if (!shape_copy(&copy, s)) {
        return;
    }
    remember(EDIT_OTHER);
    struct shape *t = slide_add_shape(cur(), -1);
    if (t == NULL) {
        shape_free(&copy);
        return;
    }
    *t = copy;
    t->x += 182880;
    t->y += 182880;
    t->ph = PH_NONE; /* only one title per slide */
    sel = cur()->n - 1;
    editing = false;
    changed();
}

static void order_shape(bool front) {
    struct shape *s = selected();
    if (s == NULL) {
        return;
    }
    remember(EDIT_OTHER);
    struct shape keep = *s;
    struct slide *sl = cur();
    memmove(sl->shapes + sel, sl->shapes + sel + 1,
            (size_t)(sl->n - sel - 1) * sizeof(*sl->shapes));
    if (front) {
        sl->shapes[sl->n - 1] = keep;
        sel = sl->n - 1;
    } else {
        memmove(sl->shapes + 1, sl->shapes, (size_t)(sl->n - 1) * sizeof(*sl->shapes));
        sl->shapes[0] = keep;
        sel = 0;
    }
    stop_editing();
    changed();
}

static void go_slide(int index) {
    if (index < 0 || index >= nslides) {
        return;
    }
    stop_editing();
    cur_slide = index;
    sel = -1;
    type_style_set = false;
}

static void new_slide(int layout) {
    remember(EDIT_OTHER);
    struct slide *sl = deck_insert_slide(cur_slide + 1);
    if (sl == NULL) {
        say("A PRESENTATION HAS AT MOST 2000 SLIDES HERE", true);
        return;
    }
    sl->background = slides[cur_slide].background;
    sl->deco = slides[cur_slide].deco;
    fill_layout(sl, layout);
    /* New placeholders take the text colour of this slide's title. */
    for (int i = 0; i < slides[cur_slide].n; i++) {
        struct shape *o = &slides[cur_slide].shapes[i];
        if ((o->ph == PH_TITLE || o->ph == PH_CTR_TITLE) && o->npara > 0) {
            uint32_t c = ST_COLOR(o->paras[0].len > 0 ? o->paras[0].st[0]
                                                      : o->paras[0].end_style);
            for (int k = 0; k < sl->n; k++) {
                for (int q = 0; q < sl->shapes[k].npara; q++) {
                    struct para *p = &sl->shapes[k].paras[q];
                    p->end_style = st_with_colour(p->end_style, c);
                }
            }
            break;
        }
    }
    go_slide(cur_slide + 1);
    set_modified(true);
}

static void duplicate_slide(void) {
    struct slide copy;
    if (!slide_copy(&copy, cur())) {
        return;
    }
    remember(EDIT_OTHER);
    struct slide *sl = deck_insert_slide(cur_slide + 1);
    if (sl == NULL) {
        slide_free(&copy);
        return;
    }
    *sl = copy;
    go_slide(cur_slide + 1);
    set_modified(true);
}

static void delete_slide(void) {
    if (nslides <= 1) {
        /* The last slide: emptied instead. */
        remember(EDIT_OTHER);
        while (cur()->n > 0) {
            slide_remove_shape(cur(), cur()->n - 1);
        }
        sel = -1;
        editing = false;
        changed();
        return;
    }
    remember(EDIT_OTHER);
    deck_remove_slide(cur_slide);
    if (cur_slide >= nslides) {
        cur_slide = nslides - 1;
    }
    sel = -1;
    editing = false;
    set_modified(true);
}

static void move_slide(int delta) {
    int to = cur_slide + delta;
    if (to < 0 || to >= nslides) {
        return;
    }
    remember(EDIT_OTHER);
    struct slide keep = slides[cur_slide];
    slides[cur_slide] = slides[to];
    slides[to] = keep;
    cur_slide = to;
    set_modified(true);
}

static void set_background(uint32_t colour) {
    remember(EDIT_OTHER);
    cur()->background = colour;
    changed();
}

static void set_fill(uint32_t colour) {
    struct shape *s = selected();
    if (s == NULL || s->type == SH_LINE) {
        return;
    }
    remember(EDIT_OTHER);
    s->fill = colour;
    changed();
}

static void set_outline(uint32_t colour) {
    struct shape *s = selected();
    if (s == NULL) {
        return;
    }
    remember(EDIT_OTHER);
    s->line = colour;
    if (colour != NO_COLOUR && s->line_w <= 0) {
        s->line_w = 12700;
    }
    if (s->type == SH_LINE && colour == NO_COLOUR) {
        s->line = 0x000000;
    }
    changed();
}

/* =========================================================== files */

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

#include "slide_pptx.h"

static void new_deck(void) {
    deck_clear();
    slide_w = 12192000;
    slide_h = 6858000;
    struct slide *sl = deck_insert_slide(0);
    if (sl != NULL) {
        fill_layout(sl, LAYOUT_TITLE);
    }
    history_clear();
}

static void set_name_from_path(void) {
    const char *slash = strrchr(path, '/');
    snprintf(file_name, sizeof(file_name), "%s", slash != NULL ? slash + 1 : path);
}

static void do_new(void) {
    new_deck();
    path[0] = '\0';
    snprintf(file_name, sizeof(file_name), "Untitled");
    can_write = false;
    modified = true;
    set_modified(false);
    say("", false);
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
        say("OLD POWERPOINT FILES (.PPT) DO NOT OPEN YET; SAVE AS .PPTX ELSEWHERE",
            true);
        return;
    }
    if (size < 4 || memcmp(data, "PK\3\4", 4) != 0
        || ends_with(chosen, ".odp") || ends_with(chosen, ".docx")
        || ends_with(chosen, ".xlsx")) {
        free(data);
        say("PRESENTATION OPENS POWERPOINT FILES (.PPTX)", true);
        return;
    }
    deck_clear();
    bool lossy = false;
    const char *problem = pptx_load(data, size, &lossy);
    free(data);
    history_clear();
    if (problem != NULL || nslides == 0) {
        new_deck();
        say(problem != NULL ? problem : "THAT PRESENTATION HAS NO SLIDES", true);
        return;
    }
    snprintf(path, sizeof(path), "%s", chosen);
    set_name_from_path();
    can_write = false;
    modified = true;
    set_modified(false);
    cur_slide = 0;
    sel = -1;
    editing = false;
    say(lossy ? "OPENED; CHARTS, TABLES, SOME SHAPES AND EFFECTS ARE NOT SHOWN YET"
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

static bool write_deck(const char *file) {
    if (!pptx_save(file)) {
        say("COULD NOT SAVE THE PRESENTATION", true);
        return false;
    }
    say("SAVED", false);
    return true;
}

static void draw(void);

static bool do_save_as(void) {
    char suggested[PATH_MAX_LENGTH + 8];
    char start[PATH_MAX_LENGTH] = "Presentation.pptx";
    struct opos_user_info user;
    user.size = sizeof(user);
    if (path[0] != '/' && opos_user_info(&user) == 0 && user.home[0] == '/'
        && strlen(user.home) + 30 < sizeof(start)) {
        snprintf(start, sizeof(start), "%s/Documents/Presentation.pptx",
                 user.home);
    }
    snprintf(suggested, sizeof(suggested), "%s", path[0] == '/' ? path : start);
    if (!ends_with(suggested, ".pptx")) {
        char *dot = strrchr(suggested, '.');
        char *slash = strrchr(suggested, '/');
        if (dot != NULL && (slash == NULL || dot > slash + 1)) {
            *dot = '\0';
        }
        size_t length = strlen(suggested);
        if (length + 6 < sizeof(suggested)) {
            memcpy(suggested + length, ".pptx", 6);
        }
    }
    if (pending != PENDING_NONE) {
        enum pending keep = pending;
        pending = PENDING_NONE;
        draw();
        pending = keep;
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
    if (!write_deck(chosen)) {
        return false;
    }
    snprintf(path, sizeof(path), "%s", chosen);
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
    if (!write_deck(path)) {
        return false;
    }
    set_modified(false);
    return true;
}

/* Puts a picture file on the slide. */
static void insert_picture(void) {
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
    enum image_format format = image_sniff(data, size);
    struct image_info info;
    if (format == IMAGE_FORMAT_UNKNOWN || image_probe(data, size, &info) != IMAGE_OK) {
        free(data);
        say("PRESENTATION SHOWS PNG, JPEG AND BMP PICTURES", true);
        return;
    }
    const char *ext = format == IMAGE_FORMAT_PNG ? "png"
        : format == IMAGE_FORMAT_JPEG ? "jpeg" : "bmp";
    int index = media_add(data, size, ext);
    if (index < 0) {
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    remember(EDIT_OTHER);
    struct shape *s = slide_add_shape(cur(), -1);
    if (s == NULL) {
        forget_last();
        say("THIS SLIDE HAS AS MANY SHAPES AS IT CAN", true);
        return;
    }
    s->type = SH_PICTURE;
    s->media = index;
    int64_t w = (int64_t)info.width * EMU_PER_PX, h = (int64_t)info.height * EMU_PER_PX;
    int64_t max_w = slide_w * 8 / 10, max_h = slide_h * 8 / 10;
    if (w > max_w) {
        h = h * max_w / w;
        w = max_w;
    }
    if (h > max_h) {
        w = w * max_h / h;
        h = max_h;
    }
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    s->w = w;
    s->h = h;
    s->x = (slide_w - w) / 2;
    s->y = (slide_h - h) / 2;
    sel = cur()->n - 1;
    editing = false;
    changed();
}

/* =========================================================== the slide show */

static int show_index;
static bool show_end;      /* the black "end of slide show" screen */

static int next_shown(int from, int dir) {
    for (int i = from; i >= 0 && i < nslides; i += dir) {
        if (!slides[i].hidden) {
            return i;
        }
    }
    return -1;
}

static void start_show(int from) {
    int first = next_shown(from, 1);
    if (first < 0) {
        first = next_shown(from, -1);
    }
    if (first < 0) {
        say("EVERY SLIDE IS HIDDEN", true);
        return;
    }
    stop_editing();
    presenting = true;
    show_index = first;
    show_end = false;
    if (opos_window_options(OPOS_WINDOW_KEY_STATE | OPOS_WINDOW_FULLSCREEN
                            | OPOS_WINDOW_QUIET
                            | (close_removes_window ? 0 : OPOS_WINDOW_ASK_CLOSE))
        < 0) {
        /* Older OPOS: the show fills the window. */
    }
}

static void end_show(void) {
    presenting = false;
    opos_window_options(OPOS_WINDOW_KEY_STATE
                        | (close_removes_window ? 0 : OPOS_WINDOW_ASK_CLOSE));
    if (show_index >= 0 && show_index < nslides) {
        go_slide(show_index);
    }
}

static void show_step(int dir) {
    if (show_end) {
        if (dir < 0) {
            show_end = false;
        } else {
            end_show();
        }
        return;
    }
    int next = next_shown(show_index + dir, dir);
    if (next < 0) {
        if (dir > 0) {
            show_end = true;
        }
        return;
    }
    show_index = next;
}

static void draw_show(void) {
    opos_fill_rect(&canvas, 0, 0, view_width, view_height, 0x000000);
    if (show_end) {
        const char *text = "END OF SLIDE SHOW, CLICK TO EXIT";
        opos_draw_text(&canvas, (view_width - opos_text_width(text, 2)) / 2, 24,
                       text, 0xd0d0d0, 2);
        return;
    }
    double sw = (double)view_width / (double)slide_w;
    double sh = (double)view_height / (double)slide_h;
    struct view v = {sw < sh ? sw : sh, false};
    int w = ez(v.scale, slide_w), h = ez(v.scale, slide_h);
    struct opos_canvas c = sub_canvas(&canvas, (view_width - w) / 2,
                                      (view_height - h) / 2, w, h);
    if (c.pixels != NULL) {
        draw_slide(&c, &slides[show_index], &v, -1);
    }
}

/* =========================================================== the UI */

struct box {
    int x, y, w, h;
};

static bool inside(struct box b, int x, int y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static void fill_box(struct box b, uint32_t colour) {
    opos_fill_rect(&canvas, b.x, b.y, b.w, b.h, colour);
}

static void draw_label(struct box b, const char *label, uint32_t colour) {
    int width = opos_text_width(label, 2);
    opos_draw_text(&canvas, b.x + (b.w - width) / 2 + 1,
                   b.y + (b.h - opos_char_height(2)) / 2, label, colour, 2);
}

enum button {
    B_NEW, B_OPEN, B_SAVE, B_SAVE_AS, B_UNDO, B_REDO, B_CUT, B_COPY, B_PASTE,
    B_NEW_SLIDE, B_BACKGROUND, B_PRESENT,
    B_TEXT, B_PICTURE, B_SHAPE, B_BOLD, B_ITALIC, B_UNDER, B_STRIKE,
    B_SMALLER, B_SIZE, B_BIGGER, B_COLOUR, B_LEFT, B_CENTER, B_RIGHT,
    B_BULLETS, B_NUMBERS, B_FILL, B_OUTLINE, B_COUNT,
};

static struct box boxes[B_COUNT];

enum menu {
    MENU_NONE, MENU_LAYOUT, MENU_SHAPE, MENU_SIZE, MENU_COLOUR, MENU_FILL,
    MENU_OUTLINE, MENU_BACKGROUND, MENU_SHAPE_CONTEXT, MENU_SLIDE_CONTEXT,
};
static enum menu menu;
static int menu_x, menu_y;

/* What the next drag on the slide makes. */
enum tool { TOOL_NONE, TOOL_TEXT, TOOL_RECT, TOOL_ROUND, TOOL_ELLIPSE,
            TOOL_LINE };
static enum tool tool;

/* A text button's label, which sets its width. */
static const char *button_label(int button) {
    switch (button) {
    case B_NEW: return "NEW";
    case B_OPEN: return "OPEN";
    case B_SAVE: return "SAVE";
    case B_SAVE_AS: return "SAVE AS";
    case B_UNDO: return "UNDO";
    case B_REDO: return "REDO";
    case B_CUT: return "CUT";
    case B_COPY: return "COPY";
    case B_PASTE: return "PASTE";
    case B_NEW_SLIDE: return "NEW SLIDE v";
    case B_BACKGROUND: return "BACKGROUND v";
    case B_PRESENT: return "PRESENT";
    case B_TEXT: return "TEXT";
    case B_PICTURE: return "PICTURE";
    case B_SHAPE: return "SHAPE v";
    case B_FILL: return "FILL v";
    case B_OUTLINE: return "OUTLINE v";
    default: return NULL;
    }
}

static int button_width(int button, int fixed) {
    const char *label = button_label(button);
    int w = label != NULL ? opos_text_width(label, 2) + 16 : 0;
    return w > fixed ? w : fixed;
}

static void layout_toolbar(void) {
    static const struct {
        int button, width;
    } row1[] = {
        {B_NEW, 0}, {B_OPEN, 0}, {B_SAVE, 0}, {B_SAVE_AS, 0}, {-1, 10},
        {B_UNDO, 0}, {B_REDO, 0}, {-1, 10}, {B_CUT, 0}, {B_COPY, 0},
        {B_PASTE, 0}, {-1, 10}, {B_NEW_SLIDE, 0}, {B_BACKGROUND, 0},
    };
    int x = 8, y = 6, h = ROW_H - 12;
    for (size_t i = 0; i < sizeof(row1) / sizeof(row1[0]); i++) {
        if (row1[i].button < 0) {
            x += row1[i].width;
            continue;
        }
        int w = button_width(row1[i].button, row1[i].width);
        boxes[row1[i].button] = (struct box){x, y, w, h};
        x += w + 4;
    }
    int pw = button_width(B_PRESENT, 0);
    int px_ = view_width - 8 - pw;
    if (px_ < x + 8) {
        px_ = x + 8;
    }
    boxes[B_PRESENT] = (struct box){px_, y, pw, h};
    static const struct {
        int button, width;
    } row2[] = {
        {B_TEXT, 0}, {B_PICTURE, 0}, {B_SHAPE, 0}, {-1, 10},
        {B_BOLD, 30}, {B_ITALIC, 30}, {B_UNDER, 30}, {B_STRIKE, 30}, {-1, 10},
        {B_SMALLER, 28}, {B_SIZE, 56}, {B_BIGGER, 28}, {-1, 6}, {B_COLOUR, 50},
        {-1, 10}, {B_LEFT, 30}, {B_CENTER, 30}, {B_RIGHT, 30}, {-1, 10},
        {B_BULLETS, 34}, {B_NUMBERS, 34}, {-1, 10}, {B_FILL, 0},
        {B_OUTLINE, 0},
    };
    x = 8;
    y = ROW_H + 6;
    for (size_t i = 0; i < sizeof(row2) / sizeof(row2[0]); i++) {
        if (row2[i].button < 0) {
            x += row2[i].width;
            continue;
        }
        int w = button_width(row2[i].button, row2[i].width);
        boxes[row2[i].button] = (struct box){x, y, w, h};
        x += w + 4;
    }
}

static void draw_button_base(struct box b, bool on) {
    bool hover = inside(b, mouse_x, mouse_y) && pending == PENDING_NONE;
    fill_box(b, on ? UI_ACTIVE : hover ? UI_HOVER : UI_BUTTON);
}

static void draw_button(int index, const char *label, bool on, bool enabled) {
    draw_button_base(boxes[index], on);
    draw_label(boxes[index], label, enabled ? UI_TEXT : UI_DIM);
}

static void draw_tt(struct box b, const char *text, uint32_t flags, int size,
                    uint32_t colour) {
    cstyle s = st_make(flags, 1100, colour);
    struct opos_font *font = font_for(s);
    if (font == NULL) {
        draw_label(b, text, colour);
        return;
    }
    int w = opos_font_text_width(font, size, text, strlen(text));
    int x = b.x + (b.w - w) / 2;
    int base = b.y + b.h / 2 + opos_font_ascent(font, size) / 2 - 1;
    struct opos_canvas v = sub_canvas(&canvas, b.x, b.y, b.w, b.h);
    if (v.pixels == NULL) {
        return;
    }
    opos_font_draw(font, &v, x - b.x, base - b.y, size, text, strlen(text),
                   colour);
    if (flags & S_UNDER) {
        opos_fill_rect(&v, x - b.x, base - b.y + 2, w, 1, colour);
    }
    if (flags & S_STRIKE) {
        opos_fill_rect(&v, x - b.x, base - b.y - size / 3, w, 1, colour);
    }
}

static void draw_align_icon(int index, int align, bool on) {
    struct box b = boxes[index];
    draw_button_base(b, on);
    int widths[4] = {16, 10, 16, 12};
    int x0 = b.x + (b.w - 16) / 2, y0 = b.y + (b.h - 13) / 2;
    for (int i = 0; i < 4; i++) {
        int w = widths[i];
        int x = align == A_LEFT ? x0 : align == A_RIGHT ? x0 + 16 - w
                                                        : x0 + (16 - w) / 2;
        opos_fill_rect(&canvas, x, y0 + i * 4, w, 2, UI_TEXT);
    }
}

static void draw_list_icon(int index, bool numbers, bool on) {
    struct box b = boxes[index];
    draw_button_base(b, on);
    int x0 = b.x + (b.w - 20) / 2, y0 = b.y + (b.h - 14) / 2;
    for (int i = 0; i < 3; i++) {
        int y = y0 + i * 6;
        if (numbers) {
            char digit[2] = {(char)('1' + i), '\0'};
            opos_draw_text(&canvas, x0, y - 2, digit, UI_TEXT, 1);
        } else {
            opos_fill_rect(&canvas, x0 + 1, y, 3, 3, UI_TEXT);
        }
        opos_fill_rect(&canvas, x0 + 8, y + 1, 12, 2, UI_TEXT);
    }
}

static uint32_t last_colour = 0xc00000, last_fill = 0xffc000;

static const uint32_t palette[20] = {
    0x000000, 0x595959, 0x808080, 0xbfbfbf, 0xffffff,
    0xc00000, 0xff0000, 0xffc000, 0xffff00, 0x92d050,
    0x00b050, 0x00b0f0, 0x0070c0, 0x002060, 0x7030a0,
    0x4472c4, 0xed7d31, 0xa5a5a5, 0x70ad47, 0x44546a,
};

static int current_size(void) {
    struct shape *s = selected();
    if (s == NULL || s->npara == 0) {
        return 0;
    }
    cstyle st = editing ? style_for_typing()
        : s->paras[0].len > 0 ? s->paras[0].st[0] : s->paras[0].end_style;
    return (int)(ST_SIZE(st) + 50) / 100;
}

static void draw_toolbar(void) {
    fill_box((struct box){0, 0, view_width, TOOLBAR_H}, UI_BACK);
    opos_fill_rect(&canvas, 0, ROW_H - 1, view_width, 1, UI_PANEL);
    draw_button(B_NEW, "NEW", false, true);
    draw_button(B_OPEN, "OPEN", false, true);
    draw_button(B_SAVE, "SAVE", false, true);
    draw_button(B_SAVE_AS, "SAVE AS", false, true);
    draw_button(B_UNDO, "UNDO", false, undo_count > 0);
    draw_button(B_REDO, "REDO", false, redo_count > 0);
    struct shape *s = selected();
    draw_button(B_CUT, "CUT", false, s != NULL);
    draw_button(B_COPY, "COPY", false, s != NULL);
    draw_button(B_PASTE, "PASTE", false, true);
    draw_button(B_NEW_SLIDE, "NEW SLIDE v", menu == MENU_LAYOUT, true);
    draw_button(B_BACKGROUND, "BACKGROUND v", menu == MENU_BACKGROUND, true);
    draw_button(B_PRESENT, "PRESENT", false, true);
    draw_button(B_TEXT, "TEXT", tool == TOOL_TEXT, true);
    draw_button(B_PICTURE, "PICTURE", false, true);
    draw_button(B_SHAPE, "SHAPE v", menu == MENU_SHAPE
                || (tool != TOOL_NONE && tool != TOOL_TEXT), true);
    bool text = s != NULL && shape_has_text_area(s);
    struct box b;
    b = boxes[B_BOLD];
    draw_button_base(b, text && text_has(S_BOLD));
    draw_tt(b, "B", S_BOLD, 17, text ? UI_TEXT : UI_DIM);
    b = boxes[B_ITALIC];
    draw_button_base(b, text && text_has(S_ITALIC));
    draw_tt(b, "I", S_ITALIC, 17, text ? UI_TEXT : UI_DIM);
    b = boxes[B_UNDER];
    draw_button_base(b, text && text_has(S_UNDER));
    draw_tt(b, "U", S_UNDER, 17, text ? UI_TEXT : UI_DIM);
    b = boxes[B_STRIKE];
    draw_button_base(b, text && text_has(S_STRIKE));
    draw_tt(b, "S", S_STRIKE, 17, text ? UI_TEXT : UI_DIM);
    draw_button(B_SMALLER, "-", false, text);
    char size[16] = "";
    if (text && s->npara > 0) {
        snprintf(size, sizeof(size), "%d", current_size());
    }
    draw_button(B_SIZE, size, menu == MENU_SIZE, text);
    draw_button(B_BIGGER, "+", false, text);
    b = boxes[B_COLOUR];
    draw_button_base(b, menu == MENU_COLOUR);
    draw_tt((struct box){b.x, b.y - 3, 28, b.h}, "A", S_BOLD, 16, UI_TEXT);
    opos_fill_rect(&canvas, b.x + 6, b.y + b.h - 7, 16, 4, last_colour);
    opos_draw_text(&canvas, b.x + 34, b.y + (b.h - 9) / 2, "v", UI_TEXT, 1);
    opos_fill_rect(&canvas, b.x + 28, b.y + 4, 1, b.h - 8, UI_PANEL);
    int align = text && s->npara > 0
        ? s->paras[editing ? cur_p : 0].align : -1;
    draw_align_icon(B_LEFT, A_LEFT, align == A_LEFT);
    draw_align_icon(B_CENTER, A_CENTER, align == A_CENTER);
    draw_align_icon(B_RIGHT, A_RIGHT, align == A_RIGHT);
    int bullet = text && s->npara > 0
        ? s->paras[editing ? cur_p : 0].bullet : -1;
    draw_list_icon(B_BULLETS, false, bullet == BU_CHAR);
    draw_list_icon(B_NUMBERS, true, bullet == BU_NUMBER);
    draw_button(B_FILL, "FILL v", menu == MENU_FILL, s != NULL);
    draw_button(B_OUTLINE, "OUTLINE v", menu == MENU_OUTLINE, s != NULL);
}

/* --- the slide panel */

static int thumb_h(void) {
    return (int)((int64_t)THUMB_W * slide_h / slide_w);
}

static int thumb_step(void) {
    return thumb_h() + 16;
}

static struct box thumb_box(int index) {
    return (struct box){PANEL_W - THUMB_W - 14,
                        TOOLBAR_H + 10 + index * thumb_step() - panel_scroll,
                        THUMB_W, thumb_h()};
}

static void clamp_panel(void) {
    int total = nslides * thumb_step() + 20;
    int shown = view_height - TOOLBAR_H - STATUS_H;
    if (panel_scroll > total - shown) {
        panel_scroll = total - shown;
    }
    if (panel_scroll < 0) {
        panel_scroll = 0;
    }
}

static void panel_show_current(void) {
    struct box b = thumb_box(cur_slide);
    int top = TOOLBAR_H, bottom = view_height - STATUS_H;
    if (b.y < top + 6) {
        panel_scroll -= top + 6 - b.y;
    } else if (b.y + b.h > bottom - 6) {
        panel_scroll += b.y + b.h - (bottom - 6);
    }
    clamp_panel();
}

static void draw_panel(void) {
    int top = TOOLBAR_H, bottom = view_height - STATUS_H;
    fill_box((struct box){0, top, PANEL_W, bottom - top}, UI_BACK);
    struct opos_canvas area = sub_canvas(&canvas, 0, top, PANEL_W, bottom - top);
    if (area.pixels == NULL) {
        return;
    }
    for (int i = 0; i < nslides; i++) {
        struct box b = thumb_box(i);
        if (b.y + b.h < top || b.y > bottom) {
            continue;
        }
        struct slide *sl = &slides[i];
        int tw = THUMB_W, th = thumb_h();
        if (sl->thumb.pixels == NULL || sl->thumb_version != sl->version
            || sl->thumb_w != tw) {
            if (sl->thumb.pixels == NULL || sl->thumb.width != tw
                || sl->thumb.height != th) {
                opos_canvas_destroy(&sl->thumb);
                sl->thumb = opos_canvas_create(tw, th);
            }
            if (sl->thumb.pixels != NULL) {
                struct view v = {(double)tw / (double)slide_w, false};
                draw_slide(&sl->thumb, sl, &v, -1);
                sl->thumb_version = sl->version;
                sl->thumb_w = tw;
            }
        }
        int ry = b.y - top;
        if (i == cur_slide) {
            opos_fill_rect(&area, b.x - 3, ry - 3, b.w + 6, b.h + 6, UI_ACTIVE);
        }
        if (sl->thumb.pixels != NULL) {
            for (int row = 0; row < th; row++) {
                int y = ry + row;
                if (y < 0 || y >= area.height) {
                    continue;
                }
                memcpy(area.pixels + (size_t)y * (size_t)area.stride + (size_t)b.x,
                       sl->thumb.pixels + (size_t)row * (size_t)sl->thumb.stride,
                       (size_t)tw * 4);
            }
        }
        if (sl->hidden) {
            for (int row = 0; row < th; row += 2) {
                int y = ry + row;
                if (y >= 0 && y < area.height) {
                    opos_fill_rect(&area, b.x, y, tw, 1, UI_BACK);
                }
            }
        }
        char number[12];
        snprintf(number, sizeof(number), "%d", i + 1);
        opos_draw_text(&area, b.x - 8 - opos_text_width(number, 1) - 2, ry + 2,
                       number, i == cur_slide ? UI_TEXT : UI_DIM, 1);
    }
}

/* --- the slide and its handles */

static void shape_rect(const struct shape *s, int *x, int *y, int *w, int *h) {
    *x = slide_x0 + ez(main_view.scale, s->x);
    *y = slide_y0 + ez(main_view.scale, s->y);
    *w = slide_x0 + ez(main_view.scale, s->x + s->w) - *x;
    *h = slide_y0 + ez(main_view.scale, s->y + s->h) - *y;
}

/* Handle k (0..7: corners and sides, clockwise from top-left) of the
   selected shape. */
static void handle_spot(int k, int *hx, int *hy) {
    int x, y, w, h;
    shape_rect(selected(), &x, &y, &w, &h);
    static const int fx[8] = {0, 1, 2, 2, 2, 1, 0, 0};
    static const int fy[8] = {0, 0, 0, 1, 2, 2, 2, 1};
    *hx = x + w * fx[k] / 2;
    *hy = y + h * fy[k] / 2;
}

static void draw_work_area(void) {
    int top = TOOLBAR_H, bottom = view_height - STATUS_H;
    fill_box((struct box){PANEL_W, top, view_width - PANEL_W, bottom - top},
             WORKSPACE);
    opos_fill_rect(&canvas, slide_x0 + 4, slide_y0 + 4, slide_px_w, slide_px_h,
                   OPOS_RGB(44, 47, 56));
    struct opos_canvas c = sub_canvas(&canvas, slide_x0, slide_y0, slide_px_w,
                                      slide_px_h);
    if (c.pixels == NULL) {
        return;
    }
    draw_slide(&c, cur(), &main_view, editing ? sel : -1);
    struct shape *s = selected();
    if (s == NULL) {
        return;
    }
    int x, y, w, h;
    shape_rect(s, &x, &y, &w, &h);
    if (editing) {
        draw_dashed_rect(&canvas, x - 1, y - 1, w + 2, h + 2, HANDLE_COLOUR);
        int cx, ctop, ch;
        caret_spot(&cx, &ctop, &ch);
        opos_fill_rect(&canvas, cx, ctop, 2, ch, 0x000000);
        return;
    }
    opos_draw_rect(&canvas, x - 1, y - 1, w + 2, h + 2, HANDLE_COLOUR);
    for (int k = 0; k < 8; k++) {
        int hx, hy;
        handle_spot(k, &hx, &hy);
        opos_fill_rect(&canvas, hx - 5, hy - 5, 10, 10, 0xffffff);
        opos_draw_rect(&canvas, hx - 5, hy - 5, 10, 10, HANDLE_COLOUR);
    }
}

static void draw_status(void) {
    int y = view_height - STATUS_H;
    fill_box((struct box){0, y, view_width, STATUS_H}, UI_PANEL);
    char where[48];
    snprintf(where, sizeof(where), "SLIDE %d OF %d", cur_slide + 1, nslides);
    opos_draw_text(&canvas, 10, y + 5, where, UI_TEXT, 2);
    if (message[0] != '\0') {
        opos_draw_text(&canvas, 10 + opos_text_width(where, 2) + 30, y + 5,
                       message, message_warn ? UI_WARN : UI_TEXT, 2);
    } else if (tool != TOOL_NONE) {
        opos_draw_text(&canvas, 10 + opos_text_width(where, 2) + 30, y + 5,
                       tool == TOOL_TEXT ? "CLICK OR DRAG ON THE SLIDE FOR A TEXT BOX"
                                         : "DRAG ON THE SLIDE TO DRAW THE SHAPE",
                       UI_DIM, 2);
    }
}

/* --- menus */

struct mitem {
    const char *label;
    int action;
};

enum action {
    A_LAYOUT_TITLE = 1, A_LAYOUT_CONTENT, A_LAYOUT_SECTION, A_LAYOUT_TITLE_ONLY,
    A_LAYOUT_BLANK, A_RECT, A_ROUND, A_ELLIPSE, A_LINE, A_CUT, A_COPY,
    A_PASTE, A_DELETE, A_FRONT, A_BACK, A_DUPLICATE, A_SLIDE_NEW,
    A_SLIDE_DUPLICATE, A_SLIDE_DELETE, A_SLIDE_UP, A_SLIDE_DOWN, A_SLIDE_HIDE,
};

static const struct mitem layout_items[] = {
    {"TITLE SLIDE", A_LAYOUT_TITLE}, {"TITLE AND CONTENT", A_LAYOUT_CONTENT},
    {"SECTION HEADER", A_LAYOUT_SECTION}, {"TITLE ONLY", A_LAYOUT_TITLE_ONLY},
    {"BLANK", A_LAYOUT_BLANK},
};
static const struct mitem shape_items[] = {
    {"RECTANGLE", A_RECT}, {"ROUNDED RECTANGLE", A_ROUND},
    {"ELLIPSE", A_ELLIPSE}, {"LINE", A_LINE},
};
static const struct mitem shape_context_items[] = {
    {"CUT", A_CUT}, {"COPY", A_COPY}, {"PASTE", A_PASTE},
    {"DUPLICATE", A_DUPLICATE}, {"DELETE", A_DELETE},
    {"BRING TO FRONT", A_FRONT}, {"SEND TO BACK", A_BACK},
};
static const struct mitem slide_context_items[] = {
    {"NEW SLIDE", A_SLIDE_NEW}, {"DUPLICATE SLIDE", A_SLIDE_DUPLICATE},
    {"DELETE SLIDE", A_SLIDE_DELETE}, {"MOVE UP", A_SLIDE_UP},
    {"MOVE DOWN", A_SLIDE_DOWN}, {"HIDE OR SHOW IN THE SHOW", A_SLIDE_HIDE},
};

#define MENU_ITEM_H 30
#define SWATCH 28

static bool palette_menu(void) {
    return menu == MENU_COLOUR || menu == MENU_FILL || menu == MENU_OUTLINE
        || menu == MENU_BACKGROUND;
}

static const struct mitem *menu_list(int *count) {
    switch (menu) {
    case MENU_LAYOUT: *count = 5; return layout_items;
    case MENU_SHAPE: *count = 4; return shape_items;
    case MENU_SHAPE_CONTEXT: *count = 7; return shape_context_items;
    case MENU_SLIDE_CONTEXT: *count = 6; return slide_context_items;
    default: *count = 0; return NULL;
    }
}

static int menu_count(void) {
    if (palette_menu()) {
        return 21;
    }
    if (menu == MENU_SIZE) {
        return FONT_SIZE_COUNT;
    }
    int count;
    menu_list(&count);
    return count;
}

static struct box menu_frame(void) {
    struct box b;
    if (palette_menu()) {
        b = (struct box){menu_x, menu_y, 5 * (SWATCH + 6) + 10,
                         4 * (SWATCH + 6) + 10 + MENU_ITEM_H};
    } else if (menu == MENU_SIZE) {
        int rows = (FONT_SIZE_COUNT + 2) / 3;
        b = (struct box){menu_x, menu_y, 3 * 54 + 8, rows * 28 + 8};
    } else {
        int count;
        const struct mitem *items = menu_list(&count);
        int w = 120;
        for (int i = 0; i < count; i++) {
            int lw = opos_text_width(items[i].label, 2) + 24;
            if (lw > w) {
                w = lw;
            }
        }
        b = (struct box){menu_x, menu_y, w, count * MENU_ITEM_H + 8};
    }
    if (b.x + b.w > view_width) {
        b.x = view_width - b.w;
    }
    if (b.y + b.h > view_height) {
        b.y = view_height - b.h;
    }
    return b;
}

static struct box menu_item_box(int i) {
    struct box f = menu_frame();
    if (palette_menu()) {
        if (i == 20) {
            return (struct box){f.x + 5, f.y + 5, f.w - 10, MENU_ITEM_H - 4};
        }
        return (struct box){f.x + 5 + (i % 5) * (SWATCH + 6),
                            f.y + 5 + MENU_ITEM_H + (i / 5) * (SWATCH + 6),
                            SWATCH, SWATCH};
    }
    if (menu == MENU_SIZE) {
        return (struct box){f.x + 4 + (i % 3) * 54, f.y + 4 + (i / 3) * 28, 52, 26};
    }
    return (struct box){f.x + 4, f.y + 4 + i * MENU_ITEM_H, f.w - 8, MENU_ITEM_H};
}

static void draw_menu(void) {
    if (menu == MENU_NONE) {
        return;
    }
    struct box f = menu_frame();
    fill_box((struct box){f.x - 1, f.y - 1, f.w + 2, f.h + 2}, UI_ACTIVE);
    fill_box(f, UI_PANEL);
    if (palette_menu()) {
        struct box first = menu_item_box(20);
        if (inside(first, mouse_x, mouse_y)) {
            fill_box(first, UI_HOVER);
        }
        draw_label(first, menu == MENU_COLOUR ? "BLACK" : menu == MENU_FILL
                   ? "NO FILL" : menu == MENU_OUTLINE ? "NO OUTLINE" : "WHITE",
                   UI_TEXT);
        for (int i = 0; i < 20; i++) {
            struct box b = menu_item_box(i);
            if (inside(b, mouse_x, mouse_y)) {
                fill_box((struct box){b.x - 2, b.y - 2, b.w + 4, b.h + 4}, UI_TEXT);
            }
            fill_box(b, palette[i]);
        }
        return;
    }
    if (menu == MENU_SIZE) {
        for (int i = 0; i < FONT_SIZE_COUNT; i++) {
            struct box b = menu_item_box(i);
            fill_box(b, inside(b, mouse_x, mouse_y) ? UI_HOVER : UI_BUTTON);
            char label[8];
            snprintf(label, sizeof(label), "%d", font_sizes[i]);
            draw_label(b, label, UI_TEXT);
        }
        return;
    }
    int count;
    const struct mitem *items = menu_list(&count);
    for (int i = 0; i < count; i++) {
        struct box b = menu_item_box(i);
        if (inside(b, mouse_x, mouse_y)) {
            fill_box(b, UI_HOVER);
        }
        opos_draw_text(&canvas, b.x + 8, b.y + (b.h - 18) / 2, items[i].label,
                       UI_TEXT, 2);
    }
}

/* --- the prompt */

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
    struct box panel = {(view_width - width) / 2, view_height / 2 - 50, width, 100};
    fill_box((struct box){panel.x - 2, panel.y - 2, panel.w + 4, panel.h + 4},
             UI_ACTIVE);
    fill_box(panel, UI_PANEL);
    char upper[41];
    size_t n = 0;
    for (; file_name[n] != '\0' && n < 40; n++) {
        upper[n] = (char)toupper((unsigned char)file_name[n]);
    }
    upper[n] = '\0';
    char question[80];
    snprintf(question, sizeof(question), "SAVE CHANGES TO %s?", upper);
    opos_draw_text(&canvas, panel.x + 20, panel.y + 16, question, UI_TEXT, 2);
    static const char *const labels[3] = {"SAVE", "DON'T SAVE", "CANCEL"};
    for (int i = 0; i < 3; i++) {
        struct box b = prompt_button(i);
        fill_box(b, i == 0 ? UI_ACTIVE : inside(b, mouse_x, mouse_y) ? UI_HOVER
                                                                     : UI_BUTTON);
        draw_label(b, labels[i], UI_TEXT);
    }
}

static void draw(void) {
    if (canvas.pixels == NULL) {
        return;
    }
    if (presenting) {
        draw_show();
        opos_canvas_present_area(&canvas, view_width, view_height);
        return;
    }
    place_slide();
    draw_work_area();
    draw_panel();
    draw_toolbar();
    draw_status();
    draw_menu();
    draw_prompt();
    opos_canvas_present_area(&canvas, view_width, view_height);
}

/* =========================================================== input */

static void finish_pending(bool go) {
    enum pending was = pending;
    pending = PENDING_NONE;
    if (!go) {
        return;
    }
    if (was == PENDING_CLOSE) {
        opos_exit(0);
    } else if (was == PENDING_NEW) {
        do_new();
    } else if (was == PENDING_OPEN) {
        draw();
        do_open();
    }
}

static void prompt_answer(int answer) {
    if (answer == 0) {
        finish_pending(do_save());
    } else {
        finish_pending(answer == 1);
    }
}

static void request(enum pending what) {
    stop_editing();
    pending = what;
    if (!modified) {
        finish_pending(true);
    }
}

static void do_action(int action) {
    menu = MENU_NONE;
    switch (action) {
    case A_LAYOUT_TITLE: new_slide(LAYOUT_TITLE); break;
    case A_LAYOUT_CONTENT: new_slide(LAYOUT_CONTENT); break;
    case A_LAYOUT_SECTION: new_slide(LAYOUT_SECTION); break;
    case A_LAYOUT_TITLE_ONLY: new_slide(LAYOUT_TITLE_ONLY); break;
    case A_LAYOUT_BLANK: new_slide(LAYOUT_BLANK); break;
    case A_RECT: tool = TOOL_RECT; break;
    case A_ROUND: tool = TOOL_ROUND; break;
    case A_ELLIPSE: tool = TOOL_ELLIPSE; break;
    case A_LINE: tool = TOOL_LINE; break;
    case A_CUT: copy_selection(true); break;
    case A_COPY: copy_selection(false); break;
    case A_PASTE: paste(); break;
    case A_DELETE: delete_shape(); break;
    case A_FRONT: order_shape(true); break;
    case A_BACK: order_shape(false); break;
    case A_DUPLICATE: duplicate_shape(); break;
    case A_SLIDE_NEW: new_slide(LAYOUT_CONTENT); break;
    case A_SLIDE_DUPLICATE: duplicate_slide(); break;
    case A_SLIDE_DELETE: delete_slide(); break;
    case A_SLIDE_UP: move_slide(-1); break;
    case A_SLIDE_DOWN: move_slide(1); break;
    case A_SLIDE_HIDE:
        remember(EDIT_OTHER);
        cur()->hidden = !cur()->hidden;
        changed();
        break;
    default:
        break;
    }
}

static void open_menu(enum menu which, struct box b) {
    menu = which;
    menu_x = b.x;
    menu_y = b.y + b.h + 2;
}

static void press_button(int index) {
    enum menu was = menu;
    menu = MENU_NONE;
    struct box b = boxes[index];
    struct shape *s = selected();
    bool text = s != NULL && shape_has_text_area(s);
    if (index != B_SHAPE && index != B_TEXT) {
        tool = TOOL_NONE;
    }
    switch (index) {
    case B_NEW: request(PENDING_NEW); break;
    case B_OPEN: request(PENDING_OPEN); break;
    case B_SAVE: stop_editing(); do_save(); break;
    case B_SAVE_AS: stop_editing(); do_save_as(); break;
    case B_UNDO: undo(); break;
    case B_REDO: redo(); break;
    case B_CUT: copy_selection(true); break;
    case B_COPY: copy_selection(false); break;
    case B_PASTE: paste(); break;
    case B_NEW_SLIDE:
        if (was != MENU_LAYOUT) {
            open_menu(MENU_LAYOUT, b);
        }
        break;
    case B_BACKGROUND:
        if (was != MENU_BACKGROUND) {
            open_menu(MENU_BACKGROUND, b);
        }
        break;
    case B_PRESENT: start_show(shift_down ? cur_slide : 0); break;
    case B_TEXT:
        tool = tool == TOOL_TEXT ? TOOL_NONE : TOOL_TEXT;
        stop_editing();
        break;
    case B_PICTURE: stop_editing(); draw(); insert_picture(); break;
    case B_SHAPE:
        if (was != MENU_SHAPE) {
            open_menu(MENU_SHAPE, b);
        }
        break;
    case B_BOLD: if (text) toggle_flag(S_BOLD); break;
    case B_ITALIC: if (text) toggle_flag(S_ITALIC); break;
    case B_UNDER: if (text) toggle_flag(S_UNDER); break;
    case B_STRIKE: if (text) toggle_flag(S_STRIKE); break;
    case B_SMALLER: if (text) restyle(change_size_step, 0); break;
    case B_BIGGER: if (text) restyle(change_size_step, 1); break;
    case B_SIZE:
        if (text && was != MENU_SIZE) {
            open_menu(MENU_SIZE, b);
        }
        break;
    case B_COLOUR:
        if (!text) {
            break;
        }
        if (mouse_x < b.x + 28) {
            restyle(change_colour, last_colour);
        } else if (was != MENU_COLOUR) {
            open_menu(MENU_COLOUR, b);
        }
        break;
    case B_LEFT: if (text) set_align(A_LEFT); break;
    case B_CENTER: if (text) set_align(A_CENTER); break;
    case B_RIGHT: if (text) set_align(A_RIGHT); break;
    case B_BULLETS: if (text) toggle_list(BU_CHAR); break;
    case B_NUMBERS: if (text) toggle_list(BU_NUMBER); break;
    case B_FILL:
        if (s != NULL && was != MENU_FILL) {
            open_menu(MENU_FILL, b);
        }
        break;
    case B_OUTLINE:
        if (s != NULL && was != MENU_OUTLINE) {
            open_menu(MENU_OUTLINE, b);
        }
        break;
    default:
        break;
    }
}

/* Mouse state. */
enum drag { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_TEXT, DRAG_CREATE,
            DRAG_PRESS };
static enum drag drag;
static int drag_handle;
static int drag_start_x, drag_start_y;
static int64_t drag_x, drag_y, drag_w, drag_h; /* the shape when it began */
static bool drag_moved;
static long last_click_ms;
static int last_click_x, last_click_y, click_count;

static long now_ms(void) {
    struct opos_timespec t;
    opos_uptime(&t);
    return t.seconds * 1000 + t.nanoseconds / 1000000;
}

/* The topmost shape at a screen point, or -1. */
static int shape_at(int x, int y) {
    struct slide *sl = cur();
    for (int i = sl->n - 1; i >= 0; i--) {
        struct shape *s = &sl->shapes[i];
        int sx_, sy_, w, h;
        shape_rect(s, &sx_, &sy_, &w, &h);
        if (s->type == SH_LINE) {
            int x0 = sx_, y0 = sy_, x1 = sx_ + w, y1 = sy_ + h;
            if (s->flip & FLIP_H) {
                x0 = sx_ + w;
                x1 = sx_;
            }
            if (s->flip & FLIP_V) {
                y0 = sy_ + h;
                y1 = sy_;
            }
            double dx = x1 - x0, dy = y1 - y0;
            double len2 = dx * dx + dy * dy;
            double t = len2 > 0 ? ((x - x0) * dx + (y - y0) * dy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            double ex = x0 + t * dx - x, ey = y0 + t * dy - y;
            if (ex * ex + ey * ey <= 36) {
                return i;
            }
            continue;
        }
        if (x >= sx_ - 2 && x < sx_ + w + 2 && y >= sy_ - 2 && y < sy_ + h + 2) {
            return i;
        }
    }
    return -1;
}

static int handle_at(int x, int y) {
    if (selected() == NULL || editing) {
        return -1;
    }
    for (int k = 0; k < 8; k++) {
        int hx, hy;
        handle_spot(k, &hx, &hy);
        if (abs(x - hx) <= 6 && abs(y - hy) <= 6) {
            return k;
        }
    }
    return -1;
}

/* Is the point on the edited shape's border (where it can be dragged)? */
static bool on_border(int x, int y) {
    struct shape *s = selected();
    if (s == NULL) {
        return false;
    }
    int sx_, sy_, w, h;
    shape_rect(s, &sx_, &sy_, &w, &h);
    bool in_outer = x >= sx_ - 5 && x < sx_ + w + 5 && y >= sy_ - 5
        && y < sy_ + h + 5;
    bool in_inner = x >= sx_ + 4 && x < sx_ + w - 4 && y >= sy_ + 4
        && y < sy_ + h - 4;
    return in_outer && !in_inner;
}

static void create_shape(int x0, int y0, int x1, int y1) {
    int64_t ex0 = to_emu_x(x0), ey0 = to_emu_y(y0);
    int64_t ex1 = to_emu_x(x1), ey1 = to_emu_y(y1);
    bool click = abs(x1 - x0) < 4 && abs(y1 - y0) < 4;
    enum tool t = tool;
    tool = TOOL_NONE;
    remember(EDIT_OTHER);
    if (t == TOOL_TEXT) {
        int64_t w = click ? slide_w / 3 : (ex1 > ex0 ? ex1 - ex0 : ex0 - ex1);
        int64_t x = click ? ex0 : (ex0 < ex1 ? ex0 : ex1);
        int64_t y = ey0 < ey1 || click ? ey0 : ey1;
        if (w < 182880) {
            w = 182880;
        }
        if (new_text_box(x, y, w) == NULL) {
            forget_last();
            return;
        }
        sel = cur()->n - 1;
        start_editing(0, 0);
        last_edit = EDIT_OTHER;
        changed();
        return;
    }
    struct shape *s = slide_add_shape(cur(), -1);
    if (s == NULL) {
        forget_last();
        say("THIS SLIDE HAS AS MANY SHAPES AS IT CAN", true);
        return;
    }
    s->type = (uint8_t)(t == TOOL_RECT ? SH_RECT : t == TOOL_ROUND ? SH_ROUND
                        : t == TOOL_ELLIPSE ? SH_ELLIPSE : SH_LINE);
    if (click) {
        ex1 = ex0 + (t == TOOL_LINE ? 1828800 : 1828800);
        ey1 = ey0 + (t == TOOL_LINE ? 0 : 1371600);
    }
    if (s->type == SH_LINE) {
        if (ex1 < ex0) {
            s->flip |= FLIP_H;
        }
        if (ey1 < ey0) {
            s->flip |= FLIP_V;
        }
        s->line = 0x4472c4;
        s->line_w = 28575;
    } else {
        s->fill = ACCENT_FILL;
        s->line = ACCENT_LINE;
        s->line_w = 12700;
        s->anchor = AN_MIDDLE;
    }
    s->x = ex0 < ex1 ? ex0 : ex1;
    s->y = ey0 < ey1 ? ey0 : ey1;
    s->w = ex0 < ex1 ? ex1 - ex0 : ex0 - ex1;
    s->h = ey0 < ey1 ? ey1 - ey0 : ey0 - ey1;
    if (s->type != SH_LINE) {
        if (s->w < 91440) {
            s->w = 91440;
        }
        if (s->h < 91440) {
            s->h = 91440;
        }
    }
    sel = cur()->n - 1;
    editing = false;
    changed();
}

static void mouse_down_slide(int x, int y, bool right) {
    long now = now_ms();
    if (now - last_click_ms < 450 && abs(x - last_click_x) < 5
        && abs(y - last_click_y) < 5) {
        click_count++;
    } else {
        click_count = 1;
    }
    last_click_ms = now;
    last_click_x = x;
    last_click_y = y;
    drag_start_x = x;
    drag_start_y = y;
    drag_moved = false;
    if (tool != TOOL_NONE && !right) {
        stop_editing();
        drag = DRAG_CREATE;
        return;
    }
    tool = TOOL_NONE;
    struct shape *s = selected();
    if (!right && editing && s != NULL) {
        int sx_, sy_, w, h;
        shape_rect(s, &sx_, &sy_, &w, &h);
        if (x >= sx_ + 4 && x < sx_ + w - 4 && y >= sy_ + 4 && y < sy_ + h - 4) {
            text_hit(x, y, &cur_p, &cur_o);
            if (click_count >= 2) {
                /* A double-click picks the word. */
                struct para *p = &s->paras[cur_p];
                int a = cur_o, b = cur_o;
                while (a > 0 && is_word_byte((unsigned char)p->text[a - 1])) {
                    a--;
                }
                while (b < p->len && is_word_byte((unsigned char)p->text[b])) {
                    b++;
                }
                anc_p = cur_p;
                anc_o = a;
                cur_o = b;
                last_edit = EDIT_NONE;
                return;
            }
            caret_moved(shift_down);
            goal_x = -1;
            type_style_set = false;
            drag = DRAG_TEXT;
            return;
        }
        if (!on_border(x, y)) {
            stop_editing();
        }
    }
    int handle = right ? -1 : handle_at(x, y);
    if (handle >= 0) {
        drag = DRAG_RESIZE;
        drag_handle = handle;
        s = selected();
        drag_x = s->x;
        drag_y = s->y;
        drag_w = s->w;
        drag_h = s->h;
        remember(EDIT_MOVE);
        last_edit = EDIT_MOVE;
        return;
    }
    int hit = editing && on_border(x, y) ? sel : shape_at(x, y);
    if (hit < 0) {
        select_shape(-1);
        if (right) {
            menu = MENU_SHAPE_CONTEXT;
            menu_x = x;
            menu_y = y;
        }
        return;
    }
    bool again = hit == sel;
    select_shape(hit);
    if (right) {
        stop_editing();
        menu = MENU_SHAPE_CONTEXT;
        menu_x = x;
        menu_y = y;
        return;
    }
    s = selected();
    drag_x = s->x;
    drag_y = s->y;
    drag_w = s->w;
    drag_h = s->h;
    drag = DRAG_PRESS;
    (void)again;
}

static void mouse_down(const struct opos_event *event) {
    int x = event->x, y = event->y;
    bool right = (event->button & OPOS_BUTTON_RIGHT) != 0
        && !(event->button & OPOS_BUTTON_LEFT);
    if (presenting) {
        show_step(right ? -1 : 1);
        return;
    }
    if (pending != PENDING_NONE) {
        for (int i = 0; i < 3; i++) {
            if (inside(prompt_button(i), x, y)) {
                prompt_answer(i);
            }
        }
        return;
    }
    if (menu != MENU_NONE) {
        struct box f = menu_frame();
        if (inside(f, x, y)) {
            for (int i = 0; i < menu_count(); i++) {
                if (!inside(menu_item_box(i), x, y)) {
                    continue;
                }
                enum menu which = menu;
                menu = MENU_NONE;
                if (which == MENU_SIZE) {
                    restyle(change_size, (uint32_t)font_sizes[i] * 100);
                } else if (which == MENU_COLOUR) {
                    uint32_t c = i == 20 ? 0x000000 : palette[i];
                    last_colour = c;
                    restyle(change_colour, c);
                } else if (which == MENU_FILL) {
                    uint32_t c = i == 20 ? NO_COLOUR : palette[i];
                    if (i < 20) {
                        last_fill = c;
                    }
                    set_fill(c);
                } else if (which == MENU_OUTLINE) {
                    set_outline(i == 20 ? NO_COLOUR : palette[i]);
                } else if (which == MENU_BACKGROUND) {
                    set_background(i == 20 ? 0xffffff : palette[i]);
                } else {
                    int count;
                    menu = which;
                    const struct mitem *items = menu_list(&count);
                    menu = MENU_NONE;
                    do_action(items[i].action);
                }
                return;
            }
            return;
        }
        enum menu was = menu;
        menu = MENU_NONE;
        for (int b = 0; b < B_COUNT; b++) {
            if (inside(boxes[b], x, y)
                && ((was == MENU_LAYOUT && b == B_NEW_SLIDE)
                    || (was == MENU_BACKGROUND && b == B_BACKGROUND)
                    || (was == MENU_SHAPE && b == B_SHAPE)
                    || (was == MENU_SIZE && b == B_SIZE)
                    || (was == MENU_FILL && b == B_FILL)
                    || (was == MENU_OUTLINE && b == B_OUTLINE)
                    || (was == MENU_COLOUR && b == B_COLOUR
                        && x >= boxes[b].x + 28))) {
                return;
            }
        }
    }
    if (y < TOOLBAR_H) {
        for (int b = 0; b < B_COUNT; b++) {
            if (inside(boxes[b], x, y)) {
                press_button(b);
                return;
            }
        }
        return;
    }
    if (y >= view_height - STATUS_H) {
        return;
    }
    if (x < PANEL_W) {
        for (int i = 0; i < nslides; i++) {
            struct box b = thumb_box(i);
            if (inside((struct box){0, b.y - 6, PANEL_W, b.h + 12}, x, y)) {
                go_slide(i);
                if (right) {
                    menu = MENU_SLIDE_CONTEXT;
                    menu_x = x;
                    menu_y = y;
                }
                return;
            }
        }
        if (right) {
            menu = MENU_SLIDE_CONTEXT;
            menu_x = x;
            menu_y = y;
        }
        return;
    }
    mouse_down_slide(x, y, right);
}

static void mouse_move(int x, int y) {
    if (drag == DRAG_NONE) {
        return;
    }
    if (abs(x - drag_start_x) > 3 || abs(y - drag_start_y) > 3) {
        drag_moved = true;
    }
    struct shape *s = selected();
    if (drag == DRAG_TEXT && s != NULL) {
        text_hit(x, y, &cur_p, &cur_o);
        last_edit = EDIT_NONE;
        return;
    }
    if (drag == DRAG_PRESS && drag_moved && s != NULL) {
        remember(EDIT_MOVE);
        last_edit = EDIT_MOVE;
        drag = DRAG_MOVE;
        stop_editing();
    }
    if (drag == DRAG_MOVE && s != NULL) {
        int64_t dx = (int64_t)floor((x - drag_start_x) / main_view.scale);
        int64_t dy = (int64_t)floor((y - drag_start_y) / main_view.scale);
        s->x = drag_x + dx;
        s->y = drag_y + dy;
        changed();
        return;
    }
    if (drag == DRAG_RESIZE && s != NULL) {
        int64_t dx = (int64_t)floor((x - drag_start_x) / main_view.scale);
        int64_t dy = (int64_t)floor((y - drag_start_y) / main_view.scale);
        static const int fx[8] = {-1, 0, 1, 1, 1, 0, -1, -1};
        static const int fy[8] = {-1, -1, -1, 0, 1, 1, 1, 0};
        int k = drag_handle;
        int64_t x0 = drag_x, y0 = drag_y, x1 = drag_x + drag_w,
                y1 = drag_y + drag_h;
        if (fx[k] < 0) x0 += dx;
        if (fx[k] > 0) x1 += dx;
        if (fy[k] < 0) y0 += dy;
        if (fy[k] > 0) y1 += dy;
        bool keep = (shift_down || s->type == SH_PICTURE) && fx[k] != 0
            && fy[k] != 0 && drag_w > 0 && drag_h > 0;
        if (keep) {
            /* Keep the shape's proportions, following the larger move. */
            int64_t w = x1 - x0, h = y1 - y0;
            double ratio = (double)drag_w / (double)drag_h;
            if (labs(w) * drag_h > labs(h) * drag_w) {
                int64_t nh = (int64_t)((double)labs(w) / ratio) * (h < 0 ? -1 : 1);
                if (fy[k] < 0) y0 = y1 - nh; else y1 = y0 + nh;
            } else {
                int64_t nw = (int64_t)((double)labs(h) * ratio) * (w < 0 ? -1 : 1);
                if (fx[k] < 0) x0 = x1 - nw; else x1 = x0 + nw;
            }
        }
        uint8_t flip = s->flip;
        if (s->type == SH_LINE) {
            /* Dragging past the other end flips the line. */
            bool fh = (x1 < x0) != ((drag_x + drag_w < drag_x));
            bool fv = (y1 < y0);
            flip = (uint8_t)((s->flip & ~(FLIP_H | FLIP_V)));
            if ((x1 < x0) ^ ((s->flip & FLIP_H) != 0)) {
                flip |= FLIP_H;
            } else if (fh) {
                flip |= 0;
            }
            if ((y1 < y0) ^ ((s->flip & FLIP_V) != 0)) {
                flip |= FLIP_V;
            }
            (void)fv;
        }
        s->x = x0 < x1 ? x0 : x1;
        s->y = y0 < y1 ? y0 : y1;
        s->w = x0 < x1 ? x1 - x0 : x0 - x1;
        s->h = y0 < y1 ? y1 - y0 : y0 - y1;
        if (s->type != SH_LINE) {
            if (s->w < 45720) s->w = 45720;
            if (s->h < 45720) s->h = 45720;
        } else {
            s->flip = flip;
        }
        shape_dirty(s);
        changed();
    }
}

static void mouse_up(int x, int y) {
    enum drag was = drag;
    drag = DRAG_NONE;
    if (was == DRAG_CREATE) {
        create_shape(drag_start_x, drag_start_y, x, y);
        return;
    }
    if (was == DRAG_MOVE || was == DRAG_RESIZE) {
        struct shape *s = selected();
        if (s != NULL && s->x == drag_x && s->y == drag_y && s->w == drag_w
            && s->h == drag_h) {
            forget_last();
        } else if (s != NULL) {
            fit_height(s);
        }
        last_edit = EDIT_NONE;
        return;
    }
    if (was == DRAG_PRESS && !drag_moved) {
        /* A click on a shape with text puts the caret there. */
        struct shape *s = selected();
        if (s != NULL && shape_has_text_area(s)
            && (s->type == SH_TEXT || s->npara > 0 || click_count >= 2)) {
            start_editing(0, 0);
            if (editing) {
                text_hit(x, y, &cur_p, &cur_o);
                caret_moved(false);
            }
        }
    }
}

/* Keys while typing in a shape. */
static void edit_key(uint32_t k, bool ctrl, bool shift) {
    struct shape *s = selected();
    struct para *p = &s->paras[cur_p];
    switch (k) {
    case OPOS_KEY_ESCAPE:
        stop_editing();
        return;
    case '\n':
        if (shift) {
            remember(EDIT_TYPE);
            cstyle style = style_for_typing();
            delete_selection();
            insert_text("\n", 1, style);
            fit_height(s);
            changed();
        } else {
            split_paragraph();
        }
        return;
    case '\t':
        if (p->bullet != BU_NONE || p->level > 0 || cur_p != anc_p) {
            change_level(shift ? -1 : 1);
        } else if (!shift) {
            type_char('\t');
        }
        return;
    case '\b':
        key_backspace(ctrl);
        return;
    case OPOS_KEY_DELETE:
        key_delete();
        return;
    case OPOS_KEY_LEFT:
        if (!shift && has_text_selection()) {
            int p1, o1, p2, o2;
            sel_range_ordered(&p1, &o1, &p2, &o2);
            cur_p = p1;
            cur_o = o1;
        } else if (cur_o > 0) {
            cur_o = prev_char(p, cur_o);
            if (ctrl) {
                while (cur_o > 0 && p->text[cur_o - 1] == ' ') {
                    cur_o--;
                }
                while (cur_o > 0
                       && is_word_byte((unsigned char)p->text[cur_o - 1])) {
                    cur_o--;
                }
            }
        } else if (cur_p > 0) {
            cur_p--;
            cur_o = s->paras[cur_p].len;
        }
        caret_moved(shift);
        goal_x = -1;
        type_style_set = false;
        return;
    case OPOS_KEY_RIGHT:
        if (!shift && has_text_selection()) {
            int p1, o1, p2, o2;
            sel_range_ordered(&p1, &o1, &p2, &o2);
            cur_p = p2;
            cur_o = o2;
        } else if (cur_o < p->len) {
            cur_o = next_char(p, cur_o);
            if (ctrl) {
                while (cur_o < p->len
                       && is_word_byte((unsigned char)p->text[cur_o])) {
                    cur_o++;
                }
                while (cur_o < p->len && p->text[cur_o] == ' ') {
                    cur_o++;
                }
            }
        } else if (cur_p + 1 < s->npara) {
            cur_p++;
            cur_o = 0;
        }
        caret_moved(shift);
        goal_x = -1;
        type_style_set = false;
        return;
    case OPOS_KEY_UP:
        caret_vertical(-1, shift);
        type_style_set = false;
        return;
    case OPOS_KEY_DOWN:
        caret_vertical(1, shift);
        type_style_set = false;
        return;
    case OPOS_KEY_HOME:
        if (ctrl) {
            cur_p = 0;
            cur_o = 0;
        } else {
            layout_shape(s, main_view.scale);
            cur_o = p->lines[find_line(p, cur_o)].start;
        }
        caret_moved(shift);
        goal_x = -1;
        return;
    case OPOS_KEY_END:
        if (ctrl) {
            cur_p = s->npara - 1;
            cur_o = s->paras[cur_p].len;
        } else {
            layout_shape(s, main_view.scale);
            struct line *ln = &p->lines[find_line(p, cur_o)];
            cur_o = ln->end;
            if (!ln->last || (cur_o > ln->start && p->text[cur_o - 1] == '\n')) {
                cur_o = prev_char(p, cur_o);
                if (!ln->last && cur_o < ln->end && p->text[cur_o] != ' ') {
                    cur_o = ln->end;
                }
            }
        }
        caret_moved(shift);
        goal_x = -1;
        return;
    default:
        if (k >= 0x20 && k != 0x7f && k < 0x110000 && !(k >= 0x11 && k <= 0x1c)) {
            type_char(k);
        }
        return;
    }
}

static void nudge(int dx, int dy) {
    struct shape *s = selected();
    int64_t step = shift_down ? 91440 * 4 : 91440 / 2;
    if (last_edit != EDIT_MOVE) {
        remember(EDIT_MOVE);
    }
    last_edit = EDIT_MOVE;
    s->x += dx * step;
    s->y += dy * step;
    changed();
}

static void key(const struct opos_event *event) {
    uint32_t k = event->key;
    bool ctrl = (event->buttons & OPOS_MOD_CTRL) != 0;
    bool shift = (event->buttons & OPOS_MOD_SHIFT) != 0;
    if (presenting) {
        switch (k) {
        case OPOS_KEY_ESCAPE: end_show(); break;
        case ' ': case '\n': case OPOS_KEY_RIGHT: case OPOS_KEY_DOWN:
        case OPOS_KEY_PAGE_DOWN: case 'n': case 'N':
            show_step(1);
            break;
        case '\b': case OPOS_KEY_LEFT: case OPOS_KEY_UP: case OPOS_KEY_PAGE_UP:
        case 'p': case 'P':
            show_step(-1);
            break;
        case OPOS_KEY_HOME: {
            int first = next_shown(0, 1);
            if (first >= 0) {
                show_index = first;
                show_end = false;
            }
            break;
        }
        case OPOS_KEY_END: {
            int last = next_shown(nslides - 1, -1);
            if (last >= 0) {
                show_index = last;
                show_end = false;
            }
            break;
        }
        default:
            break;
        }
        return;
    }
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
        case 's':
            stop_editing();
            if (shift) {
                do_save_as();
            } else {
                do_save();
            }
            return;
        case 'z': undo(); return;
        case 'y': redo(); return;
        case 'x': copy_selection(true); return;
        case 'c': copy_selection(false); return;
        case 'v': paste(); return;
        case 'm': new_slide(LAYOUT_CONTENT); return;
        case 'b': if (selected() != NULL) toggle_flag(S_BOLD); return;
        case 'i': if (selected() != NULL) toggle_flag(S_ITALIC); return;
        case 'u': if (selected() != NULL) toggle_flag(S_UNDER); return;
        case 'l': if (selected() != NULL) set_align(A_LEFT); return;
        case 'e': if (selected() != NULL) set_align(A_CENTER); return;
        case 'r': if (selected() != NULL) set_align(A_RIGHT); return;
        case 'j': if (selected() != NULL) set_align(A_JUSTIFY); return;
        case 'd':
            if (selected() != NULL && !editing) {
                duplicate_shape();
            } else if (selected() == NULL) {
                duplicate_slide();
            }
            return;
        case 'a':
            if (editing) {
                struct shape *s = selected();
                anc_p = 0;
                anc_o = 0;
                cur_p = s->npara - 1;
                cur_o = s->paras[cur_p].len;
            }
            return;
        case ']': case '>':
            if (selected() != NULL) restyle(change_size_step, 1);
            return;
        case '[': case '<':
            if (selected() != NULL) restyle(change_size_step, 0);
            return;
        default:
            if (!editing) {
                return;
            }
            break;
        }
        if (!editing) {
            return;
        }
        struct shape *s = selected();
        (void)s;
        if (k == '\b' || k == OPOS_KEY_LEFT || k == OPOS_KEY_RIGHT
            || k == OPOS_KEY_HOME || k == OPOS_KEY_END) {
            edit_key(k, true, shift);
        }
        return;
    }
    if (editing && selected() != NULL) {
        edit_key(k, false, shift);
        return;
    }
    struct shape *s = selected();
    switch (k) {
    case OPOS_KEY_ESCAPE:
        tool = TOOL_NONE;
        select_shape(-1);
        say("", false);
        return;
    case OPOS_KEY_DELETE:
    case '\b':
        if (s != NULL) {
            delete_shape();
        }
        return;
    case '\t': {
        int n = cur()->n;
        if (n > 0) {
            select_shape(sel < 0 ? (shift ? n - 1 : 0)
                         : (sel + (shift ? n - 1 : 1)) % n);
        }
        return;
    }
    case '\n':
        if (s != NULL && shape_has_text_area(s)) {
            start_editing(0, 0);
            if (editing) {
                cur_p = anc_p = selected()->npara - 1;
                cur_o = anc_o = selected()->paras[cur_p].len;
            }
        }
        return;
    case OPOS_KEY_LEFT: if (s != NULL) nudge(-1, 0); else go_slide(cur_slide - 1); return;
    case OPOS_KEY_RIGHT: if (s != NULL) nudge(1, 0); else go_slide(cur_slide + 1); return;
    case OPOS_KEY_UP: if (s != NULL) nudge(0, -1); else go_slide(cur_slide - 1); return;
    case OPOS_KEY_DOWN: if (s != NULL) nudge(0, 1); else go_slide(cur_slide + 1); return;
    case OPOS_KEY_PAGE_UP: go_slide(cur_slide - 1); return;
    case OPOS_KEY_PAGE_DOWN: go_slide(cur_slide + 1); return;
    case OPOS_KEY_HOME: go_slide(0); return;
    case OPOS_KEY_END: go_slide(nslides - 1); return;
    default:
        /* Typing on a selected shape adds to its text. */
        if (s != NULL && shape_has_text_area(s) && k >= 0x20 && k != 0x7f
            && k < 0x110000) {
            start_editing(0, 0);
            if (editing) {
                struct shape *t = selected();
                if (t->npara == 1 && t->paras[0].len == 0) {
                    cur_p = anc_p = 0;
                    cur_o = anc_o = 0;
                } else {
                    cur_p = anc_p = t->npara - 1;
                    cur_o = anc_o = t->paras[cur_p].len;
                }
                type_char(k);
            }
        }
        return;
    }
}

static void wheel(const struct opos_event *event) {
    int notches = OPOS_WHEEL_NOTCHES(*event);
    if (presenting) {
        show_step(notches > 0 ? -1 : 1);
        return;
    }
    if (menu != MENU_NONE || pending != PENDING_NONE) {
        return;
    }
    if (mouse_x < PANEL_W) {
        panel_scroll -= notches * thumb_step();
        clamp_panel();
        return;
    }
    go_slide(cur_slide - notches);
    panel_show_current();
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
                opos_print("presentation: out of memory\n");
                opos_exit(1);
            }
        }
        layout_toolbar();
        clamp_panel();
        break;
    case OPOS_EVENT_MOUSE_DOWN:
        mouse_x = event->x;
        mouse_y = event->y;
        if (!presenting && message[0] != '\0') {
            say("", false);
        }
        mouse_down(event);
        break;
    case OPOS_EVENT_MOUSE_MOVE:
        mouse_x = event->x;
        mouse_y = event->y;
        if (!presenting) {
            mouse_move(event->x, event->y);
        }
        break;
    case OPOS_EVENT_MOUSE_UP:
        if (!presenting) {
            mouse_up(event->x, event->y);
        }
        break;
    case OPOS_EVENT_WHEEL:
        wheel(event);
        break;
    case OPOS_EVENT_KEY:
        if (!presenting && editing && message_warn) {
            say("", false);
        }
        key(event);
        if (!presenting) {
            panel_show_current();
        }
        break;
    case OPOS_EVENT_KEY_STATE: {
        bool down = event->button != 0;
        if (event->key == 0x2a || event->key == 0x36) {
            shift_down = down;
        } else if (event->key == 0x1d || event->key == 0x9d) {
            ctrl_down = down;
        } else if (event->key == 0x3f && down && !presenting
                   && pending == PENDING_NONE) {
            menu = MENU_NONE;
            start_show(shift_down ? cur_slide : 0); /* F5 */
        }
        break;
    }
    case OPOS_EVENT_CLOSE:
        if (close_removes_window) {
            opos_exit(0);
        }
        if (presenting) {
            end_show();
        }
        drag = DRAG_NONE;
        menu = MENU_NONE;
        request(PENDING_CLOSE);
        break;
    default:
        break;
    }
}

/* ======================================================= headless */

/* "presentation --convert IN OUT": opens IN and saves OUT. "presentation
   --print IN": every slide's shapes and text. For tests; both run on
   Linux too. */
static int print_deck(const char *in) {
    open_path(in);
    if (message_warn && strncmp(message, "OPENED", 6) != 0) {
        printf("%s\n", message);
        return 1;
    }
    printf("size %lld x %lld\n", (long long)slide_w, (long long)slide_h);
    static const char *const types[] = {"text", "rect", "round", "ellipse",
                                        "line", "picture"};
    static const char *const phs[] = {"", " title", " ctrTitle", " subTitle",
                                      " body", " other"};
    for (int i = 0; i < nslides; i++) {
        struct slide *sl = &slides[i];
        printf("[slide %d] background %06x%s\n", i + 1, sl->background,
               sl->hidden ? " hidden" : "");
        for (int k = 0; k < sl->n; k++) {
            struct shape *s = &sl->shapes[k];
            printf("  %s%s at %lld,%lld size %lld x %lld", types[s->type],
                   phs[s->ph], (long long)s->x, (long long)s->y,
                   (long long)s->w, (long long)s->h);
            if (s->fill != NO_COLOUR) {
                printf(" fill %06x", s->fill);
            }
            if (s->line != NO_COLOUR) {
                printf(" line %06x/%d", s->line, s->line_w);
            }
            if (s->type == SH_PICTURE) {
                printf(" media %d (%s, %zu bytes)", s->media,
                       s->media >= 0 ? media[s->media].ext : "-",
                       s->media >= 0 ? media[s->media].size : (size_t)0);
            }
            if (s->font_scale != 100000) {
                printf(" fontScale %d", s->font_scale);
            }
            printf(" anchor %d\n", s->anchor);
            for (int pi = 0; pi < s->npara; pi++) {
                struct para *p = &s->paras[pi];
                char bullet[32] = "";
                if (p->bullet != BU_NONE) {
                    p->number = pi + 1;
                    bullet_text(p, bullet, sizeof(bullet));
                }
                printf("    p lvl%d al%d %s [", p->level, p->align, bullet);
                cstyle last = 0;
                bool have = false;
                for (int o = 0; o < p->len; o++) {
                    if (!have || p->st[o] != last) {
                        last = p->st[o];
                        have = true;
                        printf("{%u%s%s%s%s #%06x}", ST_SIZE(last) / 100,
                               ST_FLAGS(last) & S_BOLD ? "b" : "",
                               ST_FLAGS(last) & S_ITALIC ? "i" : "",
                               ST_FLAGS(last) & S_UNDER ? "u" : "",
                               ST_FLAGS(last) & S_STRIKE ? "s" : "",
                               ST_COLOR(last));
                    }
                    putchar(p->text[o] == '\n' ? '|' : p->text[o]);
                }
                if (p->len == 0) {
                    printf("{%u #%06x}", ST_SIZE(p->end_style) / 100,
                           ST_COLOR(p->end_style));
                }
                printf("]\n");
            }
        }
    }
    printf("%s\n", message);
    return 0;
}

/* Draws each slide into a PNG-less raw dump: for tests of the drawing
   code without a window (writes WxH 0xRRGGBB words). */
static int render_slides(const char *in, const char *out_prefix, int width) {
    open_path(in);
    for (int i = 0; i < nslides; i++) {
        int height = (int)((int64_t)width * slide_h / slide_w);
        struct opos_canvas c = opos_canvas_create(width, height);
        if (c.pixels == NULL) {
            return 1;
        }
        struct view v = {(double)width / (double)slide_w, false};
        draw_slide(&c, &slides[i], &v, -1);
        char name[300];
        snprintf(name, sizeof(name), "%s%d.ppm", out_prefix, i + 1);
        struct buf b = {0};
        char head[64];
        snprintf(head, sizeof(head), "P6\n%d %d\n255\n", width, height);
        buf_puts(&b, head);
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint32_t p = c.pixels[(size_t)y * (size_t)c.stride + (size_t)x];
                buf_byte(&b, (uint8_t)(p >> 16));
                buf_byte(&b, (uint8_t)(p >> 8));
                buf_byte(&b, (uint8_t)p);
            }
        }
        write_whole_file(name, b.data, b.length);
        free(b.data);
        opos_canvas_destroy(&c);
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    new_deck();
    if (argc == 4 && strcmp(argv[1], "--convert") == 0) {
        headless = true;
        open_path(argv[2]);
        printf("%s\n", message);
        if (message_warn && strncmp(message, "OPENED", 6) != 0) {
            return 1;
        }
        return write_deck(argv[3]) ? 0 : 1;
    }
    if (argc == 3 && strcmp(argv[1], "--print") == 0) {
        headless = true;
        return print_deck(argv[2]);
    }
    if (argc == 5 && strcmp(argv[1], "--render") == 0) {
        headless = true;
        return render_slides(argv[2], argv[3], atoi(argv[4]));
    }
    if (opos_window_create("Presentation", 1200, 800) < 0) {
        opos_print("presentation: no window; run it from the desktop\n");
        return 1;
    }
    if (opos_window_options(OPOS_WINDOW_KEY_STATE | OPOS_WINDOW_ASK_CLOSE) < 0) {
        opos_window_options(OPOS_WINDOW_KEY_STATE);
        close_removes_window = true;
    }
    if (font_for(0) == NULL) {
        say("COULD NOT LOAD THE FONTS IN /SHARE/FONTS", true);
    }
    update_title();
    if (argc > 1) {
        open_path(argv[1]);
    }
    struct opos_event event;
    for (;;) {
        if (opos_window_event(&event, -1) <= 0) {
            continue;
        }
        handle(&event);
        int more = 0;
        while (more < 64 && opos_window_event(&event, 0) == 1) {
            handle(&event);
            more++;
        }
        draw();
    }
}
