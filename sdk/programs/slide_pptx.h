/*
 * PowerPoint presentations (.pptx) for Presentation: reading and writing.
 *
 * Reading follows presentation.xml's slide list. Each slide's placeholders
 * take what they leave out (position, text look, bullets) from their
 * layout and master, as PowerPoint does; theme colours and the master's
 * colour map are resolved to RGB. Shapes on layouts and masters (logos,
 * bars) are kept as read-only "decorations" drawn under the slide.
 * Groups are flattened. Tables, charts, SmartArt, effects, rotation and
 * picture fills are not shown yet: loading reports that something was
 * left out.
 *
 * Writing makes a small, complete package: one theme, one master, a
 * layout per decoration set and kind of slide, and slides whose shapes
 * carry all of their look, so they look the same whatever the layout.
 * Parts are stored (not deflated), like Writer's and Spreadsheet's.
 */
#ifndef SLIDE_PPTX_H
#define SLIDE_PPTX_H

/* ============================================================ helpers */

static inline void pptx_unused(void) {
    (void)x_on; /* shared with Writer, not needed here */
}

static bool x_child(struct xml *x, struct xtok *t) {
    int type = xml_next(x, t);
    return type != X_END && type != X_CLOSE;
}

/* Skips an element's contents (after its open tag). */
static void x_skip(struct xml *x, const struct xtok *t) {
    if (t->type != X_OPEN) {
        return;
    }
    int depth = 1;
    struct xtok k;
    while (depth > 0) {
        int type = xml_next(x, &k);
        if (type == X_END) {
            return;
        }
        if (type == X_OPEN) {
            depth++;
        } else if (type == X_CLOSE) {
            depth--;
        }
    }
}

static int64_t num(const char *s) {
    bool negative = false;
    while (*s == ' ') {
        s++;
    }
    if (*s == '-' || *s == '+') {
        negative = *s == '-';
        s++;
    }
    int64_t v = 0;
    while (*s >= '0' && *s <= '9') {
        if (v < ((int64_t)1 << 56)) {
            v = v * 10 + (*s - '0');
        }
        s++;
    }
    return negative ? -v : v;
}

static bool x_num(const struct xtok *t, const char *name, int64_t *out) {
    char v[40];
    if (!x_attr(t, name, v, sizeof(v)) || v[0] == '\0') {
        return false;
    }
    *out = num(v);
    return true;
}

static bool x_true(const struct xtok *t, const char *name) {
    char v[16];
    if (!x_attr(t, name, v, sizeof(v))) {
        return false;
    }
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0;
}

/* The value of a prefixed "id" attribute (r:id), which x_attr cannot tell
   from a plain id. */
static bool x_rid(const struct xtok *t, char *out, size_t capacity) {
    const char *a = t->attrs, *end = t->attrs + t->attrs_length;
    for (const char *c = a; c + 4 < end; c++) {
        if (memcmp(c, ":id=", 4) != 0 || c == a || c[-1] == ' ') {
            continue;
        }
        const char *v = c + 4;
        if (*v != '"' && *v != '\'') {
            continue;
        }
        char quote = *v++;
        size_t n = 0;
        while (v < end && *v != quote && n + 1 < capacity) {
            out[n++] = *v++;
        }
        out[n] = '\0';
        return true;
    }
    return false;
}

/* ============================================================ colours */

enum { C_DK1, C_LT1, C_DK2, C_LT2, C_ACC1, C_ACC2, C_ACC3, C_ACC4, C_ACC5,
       C_ACC6, C_HLINK, C_FOLHLINK, C_COUNT };

static const char *const scheme_names[C_COUNT] = {
    "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3", "accent4",
    "accent5", "accent6", "hlink", "folHlink",
};

static const uint32_t office_scheme[C_COUNT] = {
    0x000000, 0xffffff, 0x44546a, 0xe7e6e6, 0x4472c4, 0xed7d31, 0xa5a5a5,
    0xffc000, 0x5b9bd5, 0x70ad47, 0x0563c1, 0x954f72,
};

/* The theme of the master being read, and its colour map (bg1, tx1, bg2,
   tx2 to scheme slots). */
static uint32_t cur_scheme[C_COUNT];
static int cur_map[4] = {C_LT1, C_DK1, C_LT2, C_DK2};

static int scheme_slot(const char *name) {
    for (int i = 0; i < C_COUNT; i++) {
        if (strcmp(name, scheme_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static uint32_t scheme_colour(const char *name) {
    static const char *const mapped[4] = {"bg1", "tx1", "bg2", "tx2"};
    for (int i = 0; i < 4; i++) {
        if (strcmp(name, mapped[i]) == 0) {
            return cur_scheme[cur_map[i]];
        }
    }
    int slot = scheme_slot(name);
    if (slot >= 0) {
        return cur_scheme[slot];
    }
    return cur_scheme[cur_map[1]]; /* phClr and unknown names */
}

static void to_hsl(uint32_t c, double *h, double *s, double *l) {
    double r = (c >> 16 & 0xff) / 255.0, g = (c >> 8 & 0xff) / 255.0,
           b = (c & 0xff) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *l = (mx + mn) / 2;
    if (mx == mn) {
        *h = *s = 0;
        return;
    }
    double d = mx - mn;
    *s = *l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) {
        *h = (g - b) / d + (g < b ? 6 : 0);
    } else if (mx == g) {
        *h = (b - r) / d + 2;
    } else {
        *h = (r - g) / d + 4;
    }
    *h /= 6;
}

static double hue_part(double p, double q, double t) {
    if (t < 0) {
        t += 1;
    }
    if (t > 1) {
        t -= 1;
    }
    if (t < 1.0 / 6) {
        return p + (q - p) * 6 * t;
    }
    if (t < 0.5) {
        return q;
    }
    if (t < 2.0 / 3) {
        return p + (q - p) * (2.0 / 3 - t) * 6;
    }
    return p;
}

static uint32_t from_hsl(double h, double s, double l) {
    double r, g, b;
    if (s == 0) {
        r = g = b = l;
    } else {
        double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        double p = 2 * l - q;
        r = hue_part(p, q, h + 1.0 / 3);
        g = hue_part(p, q, h);
        b = hue_part(p, q, h - 1.0 / 3);
    }
    int ri = (int)(r * 255 + 0.5), gi = (int)(g * 255 + 0.5),
        bi = (int)(b * 255 + 0.5);
    ri = ri < 0 ? 0 : ri > 255 ? 255 : ri;
    gi = gi < 0 ? 0 : gi > 255 ? 255 : gi;
    bi = bi < 0 ? 0 : bi > 255 ? 255 : bi;
    return (uint32_t)(ri << 16 | gi << 8 | bi);
}

static uint32_t hex_colour(const char *v) {
    uint32_t c = 0;
    for (int i = 0; i < 6 && v[i] != '\0'; i++) {
        int d = isdigit((unsigned char)v[i]) ? v[i] - '0'
            : (tolower((unsigned char)v[i]) - 'a' + 10) & 15;
        c = c << 4 | (uint32_t)d;
    }
    return c;
}

static bool is_colour_element(const struct xtok *t) {
    return x_is(t, "srgbClr") || x_is(t, "schemeClr") || x_is(t, "sysClr")
        || x_is(t, "prstClr") || x_is(t, "scrgbClr") || x_is(t, "hslClr");
}

/* A colour element and its modifiers (lumMod, lumOff, tint, shade). */
static uint32_t read_colour(struct xml *x, const struct xtok *t) {
    char v[32] = "";
    uint32_t c = 0;
    if (x_is(t, "srgbClr")) {
        x_attr(t, "val", v, sizeof(v));
        c = hex_colour(v);
    } else if (x_is(t, "schemeClr")) {
        x_attr(t, "val", v, sizeof(v));
        c = scheme_colour(v);
    } else if (x_is(t, "sysClr")) {
        if (x_attr(t, "lastClr", v, sizeof(v))) {
            c = hex_colour(v);
        } else {
            x_attr(t, "val", v, sizeof(v));
            c = strcmp(v, "window") == 0 ? 0xffffff : 0x000000;
        }
    } else if (x_is(t, "prstClr")) {
        static const struct {
            const char *name;
            uint32_t colour;
        } named[] = {
            {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000},
            {"green", 0x008000}, {"blue", 0x0000ff}, {"yellow", 0xffff00},
            {"gray", 0x808080}, {"grey", 0x808080}, {"orange", 0xffa500},
            {"purple", 0x800080}, {"darkBlue", 0x00008b},
            {"darkRed", 0x8b0000}, {"lightGray", 0xd3d3d3},
        };
        x_attr(t, "val", v, sizeof(v));
        for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
            if (strcmp(v, named[i].name) == 0) {
                c = named[i].colour;
            }
        }
    } else if (x_is(t, "scrgbClr")) {
        int64_t r = 0, g = 0, b = 0;
        x_num(t, "r", &r);
        x_num(t, "g", &g);
        x_num(t, "b", &b);
        c = (uint32_t)((r * 255 / 100000) << 16 | (g * 255 / 100000) << 8
                       | (b * 255 / 100000));
    } else if (x_is(t, "hslClr")) {
        int64_t h = 0, s = 0, l = 0;
        x_num(t, "hue", &h);
        x_num(t, "sat", &s);
        x_num(t, "lum", &l);
        c = from_hsl(h / 21600000.0, s / 100000.0, l / 100000.0);
    }
    if (t->type != X_OPEN) {
        return c & 0xffffff;
    }
    double mod = 1, off = 0;
    bool lum = false;
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        int64_t value;
        if (x_num(&k, "val", &value)) {
            double f = value / 100000.0;
            if (x_is(&k, "lumMod")) {
                mod = f;
                lum = true;
            } else if (x_is(&k, "lumOff")) {
                off = f;
                lum = true;
            } else if (x_is(&k, "tint")) {
                int ch[3] = {(int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff),
                             (int)(c & 0xff)};
                for (int i = 0; i < 3; i++) {
                    ch[i] = (int)(ch[i] * f + 255 * (1 - f) + 0.5);
                }
                c = (uint32_t)(ch[0] << 16 | ch[1] << 8 | ch[2]);
            } else if (x_is(&k, "shade")) {
                int ch[3] = {(int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff),
                             (int)(c & 0xff)};
                for (int i = 0; i < 3; i++) {
                    ch[i] = (int)(ch[i] * f + 0.5);
                }
                c = (uint32_t)(ch[0] << 16 | ch[1] << 8 | ch[2]);
            }
        }
        x_skip(x, &k);
    }
    if (lum) {
        double h, s, l;
        to_hsl(c, &h, &s, &l);
        l = l * mod + off;
        l = l < 0 ? 0 : l > 1 ? 1 : l;
        c = from_hsl(h, s, l);
    }
    return c & 0xffffff;
}

/* The first colour inside an element (solidFill, fillRef, a scheme slot);
   reads to its end. NO_COLOUR when there is none. */
static uint32_t read_colour_in(struct xml *x, const struct xtok *open) {
    if (open->type != X_OPEN) {
        return NO_COLOUR;
    }
    uint32_t c = NO_COLOUR;
    struct xtok t;
    while (x_child(x, &t)) {
        if (t.type == X_TEXT) {
            continue;
        }
        if (is_colour_element(&t)) {
            uint32_t v = read_colour(x, &t);
            if (c == NO_COLOUR) {
                c = v;
            }
        } else {
            x_skip(x, &t);
        }
    }
    return c;
}

static bool load_lossy;

/* A fill element: sets *out (NO_COLOUR for none) and returns true, or
   returns false (and reads nothing) when t is not a fill. Gradients
   become the average of their stops. */
static bool read_fill(struct xml *x, const struct xtok *t, uint32_t *out) {
    if (x_is(t, "noFill")) {
        x_skip(x, t);
        *out = NO_COLOUR;
        return true;
    }
    if (x_is(t, "solidFill")) {
        *out = read_colour_in(x, t);
        return true;
    }
    if (x_is(t, "gradFill")) {
        int r = 0, g = 0, b = 0, n = 0;
        if (t->type == X_OPEN) {
            int depth = 1;
            struct xtok k;
            while (depth > 0) {
                int type = xml_next(x, &k);
                if (type == X_END) {
                    break;
                }
                if (type == X_CLOSE) {
                    depth--;
                    continue;
                }
                if (is_colour_element(&k)) {
                    uint32_t c = read_colour(x, &k);
                    r += (int)(c >> 16 & 0xff);
                    g += (int)(c >> 8 & 0xff);
                    b += (int)(c & 0xff);
                    n++;
                    continue;
                }
                if (type == X_OPEN) {
                    depth++;
                }
            }
        }
        *out = n > 0 ? (uint32_t)((r / n) << 16 | (g / n) << 8 | (b / n))
                     : NO_COLOUR;
        return true;
    }
    if (x_is(t, "pattFill")) {
        uint32_t c = NO_COLOUR;
        if (t->type == X_OPEN) {
            struct xtok k;
            while (x_child(x, &k)) {
                if (x_is(&k, "fgClr")) {
                    c = read_colour_in(x, &k);
                } else {
                    x_skip(x, &k);
                }
            }
        }
        *out = c;
        return true;
    }
    if (x_is(t, "blipFill")) {
        x_skip(x, t);
        load_lossy = true;
        *out = 0xd9d9d9;
        return true;
    }
    return false;
}

/* ============================================================ text styles */

#define PS_ALIGN 1u
#define PS_MAR 2u
#define PS_IND 4u
#define PS_BULLET 8u
#define PS_BEFORE 16u
#define PS_BEFORE_PCT 32u
#define PS_AFTER 64u
#define PS_AFTER_PCT 128u
#define PS_LINE 256u
#define PS_LINE_PTS 512u
#define PS_SIZE 1024u
#define PS_COLOUR 2048u

/* Paragraph and character properties, each only where set. */
struct pstyle {
    uint32_t set;
    uint8_t align, bullet, num_type, fmask, fval;
    uint16_t num_start;
    uint32_t bullet_char;
    int32_t mar, ind, before, after, line;
    uint32_t size, colour;
};

static void ps_merge(struct pstyle *d, const struct pstyle *s) {
    uint32_t set = s->set;
    if (set & PS_ALIGN) {
        d->align = s->align;
    }
    if (set & PS_MAR) {
        d->mar = s->mar;
    }
    if (set & PS_IND) {
        d->ind = s->ind;
    }
    if (set & PS_BULLET) {
        d->bullet = s->bullet;
        d->bullet_char = s->bullet_char;
        d->num_type = s->num_type;
        d->num_start = s->num_start;
    }
    if (set & (PS_BEFORE | PS_BEFORE_PCT)) {
        d->before = s->before;
        d->set &= ~(PS_BEFORE | PS_BEFORE_PCT);
    }
    if (set & (PS_AFTER | PS_AFTER_PCT)) {
        d->after = s->after;
        d->set &= ~(PS_AFTER | PS_AFTER_PCT);
    }
    if (set & (PS_LINE | PS_LINE_PTS)) {
        d->line = s->line;
        d->set &= ~(PS_LINE | PS_LINE_PTS);
    }
    if (set & PS_SIZE) {
        d->size = s->size;
    }
    if (set & PS_COLOUR) {
        d->colour = s->colour;
    }
    d->set |= set;
    d->fval = (uint8_t)((d->fval & ~s->fmask) | (s->fval & s->fmask));
    d->fmask |= s->fmask;
}

static void ps_flag(struct pstyle *ps, uint8_t flag, bool on) {
    ps->fmask |= flag;
    if (on) {
        ps->fval |= flag;
    } else {
        ps->fval &= (uint8_t)~flag;
    }
}

static void read_rpr(struct xml *x, const struct xtok *t, struct pstyle *ps) {
    char v[32];
    int64_t n;
    if (x_num(t, "sz", &n) && n > 0) {
        ps->size = (uint32_t)(n < 100 ? 100 : n > 400000 ? 400000 : n);
        ps->set |= PS_SIZE;
    }
    if (x_attr(t, "b", v, sizeof(v))) {
        ps_flag(ps, S_BOLD, strcmp(v, "1") == 0 || strcmp(v, "true") == 0);
    }
    if (x_attr(t, "i", v, sizeof(v))) {
        ps_flag(ps, S_ITALIC, strcmp(v, "1") == 0 || strcmp(v, "true") == 0);
    }
    if (x_attr(t, "u", v, sizeof(v))) {
        ps_flag(ps, S_UNDER, strcmp(v, "none") != 0);
    }
    if (x_attr(t, "strike", v, sizeof(v))) {
        ps_flag(ps, S_STRIKE, strcmp(v, "noStrike") != 0);
    }
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        uint32_t c;
        if (x_is(&k, "solidFill") || x_is(&k, "gradFill")) {
            read_fill(x, &k, &c);
            if (c != NO_COLOUR) {
                ps->colour = c;
                ps->set |= PS_COLOUR;
            }
        } else {
            x_skip(x, &k);
        }
    }
}

/* lnSpc, spcBef or spcAft: a percentage (1/1000 %) or points (1/100). */
static void read_spacing(struct xml *x, const struct xtok *t, int32_t *value,
                         bool *points) {
    *points = false;
    *value = -1;
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        int64_t n;
        if (x_is(&k, "spcPct") && x_num(&k, "val", &n)) {
            *value = (int32_t)n;
            *points = false;
        } else if (x_is(&k, "spcPts") && x_num(&k, "val", &n)) {
            *value = (int32_t)n;
            *points = true;
        }
        x_skip(x, &k);
    }
}

static int num_type_code(const char *v) {
    static const char *const names[N_COUNT] = {
        "arabicPeriod", "arabicParenR", "arabicParenBoth", "arabicPlain",
        "alphaLcPeriod", "alphaUcPeriod", "alphaLcParenR", "alphaUcParenR",
        "romanLcPeriod", "romanUcPeriod",
    };
    for (int i = 0; i < N_COUNT; i++) {
        if (strcmp(v, names[i]) == 0) {
            return i;
        }
    }
    return N_ARABIC_PERIOD;
}

static const char *num_type_name(int type) {
    static const char *const names[N_COUNT] = {
        "arabicPeriod", "arabicParenR", "arabicParenBoth", "arabicPlain",
        "alphaLcPeriod", "alphaUcPeriod", "alphaLcParenR", "alphaUcParenR",
        "romanLcPeriod", "romanUcPeriod",
    };
    return names[type < N_COUNT ? type : 0];
}

static void read_ppr(struct xml *x, const struct xtok *t, struct pstyle *ps,
                     int *level) {
    char v[32];
    int64_t n;
    if (x_num(t, "marL", &n)) {
        ps->mar = (int32_t)n;
        ps->set |= PS_MAR;
    }
    if (x_num(t, "indent", &n)) {
        ps->ind = (int32_t)n;
        ps->set |= PS_IND;
    }
    if (x_attr(t, "algn", v, sizeof(v))) {
        ps->align = strcmp(v, "ctr") == 0 ? A_CENTER : strcmp(v, "r") == 0
            ? A_RIGHT : strcmp(v, "just") == 0 || strcmp(v, "dist") == 0
            || strcmp(v, "justLow") == 0 || strcmp(v, "thaiDist") == 0
            ? A_JUSTIFY : A_LEFT;
        ps->set |= PS_ALIGN;
    }
    if (level != NULL && x_num(t, "lvl", &n)) {
        *level = n < 0 ? 0 : n > 8 ? 8 : (int)n;
    }
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        int32_t value;
        bool points;
        if (x_is(&k, "lnSpc")) {
            read_spacing(x, &k, &value, &points);
            if (value >= 0) {
                ps->line = value;
                ps->set &= ~(PS_LINE | PS_LINE_PTS);
                ps->set |= points ? PS_LINE_PTS : PS_LINE;
            }
        } else if (x_is(&k, "spcBef") || x_is(&k, "spcAft")) {
            bool before = x_is(&k, "spcBef");
            read_spacing(x, &k, &value, &points);
            if (value >= 0) {
                if (before) {
                    ps->before = value;
                    ps->set &= ~(PS_BEFORE | PS_BEFORE_PCT);
                    ps->set |= points ? PS_BEFORE : PS_BEFORE_PCT;
                } else {
                    ps->after = value;
                    ps->set &= ~(PS_AFTER | PS_AFTER_PCT);
                    ps->set |= points ? PS_AFTER : PS_AFTER_PCT;
                }
            }
        } else if (x_is(&k, "buNone")) {
            ps->bullet = BU_NONE;
            ps->set |= PS_BULLET;
            x_skip(x, &k);
        } else if (x_is(&k, "buChar")) {
            char c[16] = "";
            x_attr(&k, "char", c, sizeof(c));
            uint32_t code = 0x2022;
            if (c[0] != '\0') {
                opos_utf8_decode(c, strlen(c), &code);
            }
            /* Symbol-font bullets (Wingdings) have no meaning in Unicode
               fonts: a plain bullet instead. */
            if (code >= 0xf000 && code <= 0xf0ff) {
                code = 0x2022;
            }
            ps->bullet = BU_CHAR;
            ps->bullet_char = code;
            ps->set |= PS_BULLET;
            x_skip(x, &k);
        } else if (x_is(&k, "buAutoNum")) {
            x_attr(&k, "type", v, sizeof(v));
            ps->bullet = BU_NUMBER;
            ps->num_type = (uint8_t)num_type_code(v);
            ps->num_start = x_num(&k, "startAt", &n) && n > 0 && n < 30000
                ? (uint16_t)n : 1;
            ps->set |= PS_BULLET;
            x_skip(x, &k);
        } else if (x_is(&k, "buBlip")) {
            ps->bullet = BU_CHAR;
            ps->bullet_char = 0x2022;
            ps->set |= PS_BULLET;
            x_skip(x, &k);
        } else if (x_is(&k, "defRPr")) {
            read_rpr(x, &k, ps);
        } else {
            x_skip(x, &k);
        }
    }
}

/* A list style: defaults and nine levels. */
struct tstyle {
    struct pstyle def, lvl[9];
};

static void read_lst(struct xml *x, const struct xtok *t, struct tstyle *ts) {
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "defPPr")) {
            read_ppr(x, &k, &ts->def, NULL);
        } else if (k.name_length == 7 && memcmp(k.name, "lvl", 3) == 0
                   && k.name[3] >= '1' && k.name[3] <= '9'
                   && memcmp(k.name + 4, "pPr", 3) == 0) {
            read_ppr(x, &k, &ts->lvl[k.name[3] - '1'], NULL);
        } else {
            x_skip(x, &k);
        }
    }
}

/* ============================================================ body */

#define BP_ANCHOR 1u
#define BP_WRAP 2u
#define BP_INS 4u /* four bits from here */
#define BP_FIT 64u

struct bodyp {
    uint32_t set;
    uint8_t anchor, autofit;
    bool wrap;
    int32_t ins[4];
    int32_t font_scale, ln_red;
};

static void read_bodypr(struct xml *x, const struct xtok *t, struct bodyp *b) {
    char v[16];
    if (x_attr(t, "anchor", v, sizeof(v))) {
        b->anchor = strcmp(v, "ctr") == 0 ? AN_MIDDLE
            : strcmp(v, "b") == 0 ? AN_BOTTOM : AN_TOP;
        b->set |= BP_ANCHOR;
    }
    if (x_attr(t, "wrap", v, sizeof(v))) {
        b->wrap = strcmp(v, "none") != 0;
        b->set |= BP_WRAP;
    }
    static const char *const ins[4] = {"lIns", "tIns", "rIns", "bIns"};
    for (int i = 0; i < 4; i++) {
        int64_t n;
        if (x_num(t, ins[i], &n)) {
            b->ins[i] = (int32_t)(n < 0 ? 0 : n);
            b->set |= BP_INS << i;
        }
    }
    char vert[16];
    if (x_attr(t, "vert", vert, sizeof(vert)) && strcmp(vert, "horz") != 0) {
        load_lossy = true;
    }
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "normAutofit")) {
            int64_t n;
            b->autofit = FIT_TEXT;
            b->font_scale = x_num(&k, "fontScale", &n) && n > 1000 ? (int32_t)n
                                                                   : 100000;
            b->ln_red = x_num(&k, "lnSpcReduction", &n) ? (int32_t)n : 0;
            b->set |= BP_FIT;
        } else if (x_is(&k, "spAutoFit")) {
            b->autofit = FIT_SHAPE;
            b->font_scale = 100000;
            b->ln_red = 0;
            b->set |= BP_FIT;
        } else if (x_is(&k, "noAutofit")) {
            b->autofit = FIT_NONE;
            b->font_scale = 100000;
            b->ln_red = 0;
            b->set |= BP_FIT;
        }
        x_skip(x, &k);
    }
}

static void bp_merge(struct bodyp *d, const struct bodyp *s) {
    if (s->set & BP_ANCHOR) {
        d->anchor = s->anchor;
    }
    if (s->set & BP_WRAP) {
        d->wrap = s->wrap;
    }
    for (int i = 0; i < 4; i++) {
        if (s->set & (BP_INS << i)) {
            d->ins[i] = s->ins[i];
        }
    }
    if (s->set & BP_FIT) {
        d->autofit = s->autofit;
        d->font_scale = s->font_scale;
        d->ln_red = s->ln_red;
    }
    d->set |= s->set;
}

/* ============================================================ raw shapes */

enum { RT_NONE, RT_TITLE, RT_CTR_TITLE, RT_SUBTITLE, RT_BODY, RT_DT, RT_FTR,
       RT_SLDNUM, RT_OTHER };

static int ph_code(const char *v) {
    if (v[0] == '\0' || strcmp(v, "body") == 0 || strcmp(v, "obj") == 0) {
        return RT_BODY;
    }
    static const struct {
        const char *name;
        int code;
    } codes[] = {
        {"title", RT_TITLE}, {"ctrTitle", RT_CTR_TITLE},
        {"subTitle", RT_SUBTITLE}, {"dt", RT_DT}, {"ftr", RT_FTR},
        {"sldNum", RT_SLDNUM},
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        if (strcmp(v, codes[i].name) == 0) {
            return codes[i].code;
        }
    }
    return RT_OTHER;
}

struct rrun {
    int start, len;
    struct pstyle rpr;
};

struct rpara {
    struct pstyle ppr, end;
    bool has_end;
    int level;
    char *text;
    int len, cap;
    struct rrun *runs;
    int nruns, rcap;
};

struct rxfrm {
    bool has;
    int64_t x, y, w, h, cx, cy, cw, ch;
    uint8_t flip;
    bool rotated;
};

struct rshape {
    int kind;                 /* 0 sp, 1 pic, 2 cxnSp */
    bool has_ph, txbox, custom;
    int phtype, idx;
    struct rxfrm xf;
    char prst[32];
    bool fill_set, line_set, line_w_set;
    uint32_t fill, line;
    int32_t line_w;
    int fill_ref, ln_ref;
    uint32_t fill_ref_col, ln_ref_col;
    bool font_ref;
    uint32_t font_col;
    struct bodyp body;
    struct tstyle lst;
    struct rpara *paras;
    int npara, pcap;
    bool has_text;
    char embed[64];
    bool cropped;
};

static void rshape_free(struct rshape *r) {
    for (int i = 0; i < r->npara; i++) {
        free(r->paras[i].text);
        free(r->paras[i].runs);
    }
    free(r->paras);
    r->paras = NULL;
    r->npara = r->pcap = 0;
}

static void rp_add(struct rpara *p, const char *text, size_t length,
                   const struct pstyle *rpr) {
    if (length == 0) {
        return;
    }
    if (p->len + (int)length + 1 > p->cap) {
        int cap = p->cap < 32 ? 32 : p->cap;
        while (cap < p->len + (int)length + 1) {
            cap *= 2;
        }
        char *grown = realloc(p->text, (size_t)cap);
        if (grown == NULL) {
            return;
        }
        p->text = grown;
        p->cap = cap;
    }
    if (p->nruns == p->rcap) {
        int cap = p->rcap < 4 ? 4 : p->rcap * 2;
        struct rrun *grown = realloc(p->runs, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        p->runs = grown;
        p->rcap = cap;
    }
    int start = p->len;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\v') {
            c = '\n';
        } else if (c < 0x20 && c != '\t' && c != '\n') {
            continue;
        }
        p->text[p->len++] = (char)c;
    }
    if (p->len > start) {
        struct rrun *r = &p->runs[p->nruns++];
        r->start = start;
        r->len = p->len - start;
        r->rpr = *rpr;
    }
}

/* <a:t>: its text, decoded, appended to b. */
static void read_t(struct xml *x, const struct xtok *t, struct buf *b) {
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            char *out = malloc(k.text_length + 8);
            if (out != NULL) {
                size_t n = xml_decode(k.text, k.text_length, out);
                buf_put(b, out, n);
                free(out);
            }
        } else {
            x_skip(x, &k);
        }
    }
}

/* A run (r, fld) or line break (br). */
static void read_run(struct xml *x, const struct xtok *open, struct rpara *p,
                     bool line_break) {
    struct pstyle rpr;
    memset(&rpr, 0, sizeof(rpr));
    struct buf text = {0};
    if (open->type == X_OPEN) {
        struct xtok k;
        while (x_child(x, &k)) {
            if (k.type == X_TEXT) {
                continue;
            }
            if (x_is(&k, "rPr")) {
                read_rpr(x, &k, &rpr);
            } else if (x_is(&k, "t")) {
                read_t(x, &k, &text);
            } else {
                x_skip(x, &k);
            }
        }
    }
    if (line_break) {
        buf_byte(&text, '\n');
    }
    if (!text.failed) {
        rp_add(p, text.data, text.length, &rpr);
    }
    free(text.data);
}

static void read_para(struct xml *x, const struct xtok *open, struct rshape *r) {
    if (r->npara == r->pcap) {
        int cap = r->pcap < 4 ? 4 : r->pcap * 2;
        struct rpara *grown = realloc(r->paras, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            x_skip(x, open);
            return;
        }
        r->paras = grown;
        r->pcap = cap;
    }
    struct rpara *p = &r->paras[r->npara++];
    memset(p, 0, sizeof(*p));
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "pPr")) {
            read_ppr(x, &k, &p->ppr, &p->level);
        } else if (x_is(&k, "r") || x_is(&k, "fld")) {
            read_run(x, &k, p, false);
        } else if (x_is(&k, "br")) {
            read_run(x, &k, p, true);
        } else if (x_is(&k, "endParaRPr")) {
            read_rpr(x, &k, &p->end);
            p->has_end = true;
        } else {
            if (x_is(&k, "AlternateContent")) {
                load_lossy = true; /* equations */
            }
            x_skip(x, &k);
        }
    }
}

static void read_txbody(struct xml *x, const struct xtok *open,
                        struct rshape *r) {
    if (open->type != X_OPEN) {
        return;
    }
    r->has_text = true;
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "bodyPr")) {
            read_bodypr(x, &k, &r->body);
        } else if (x_is(&k, "lstStyle")) {
            read_lst(x, &k, &r->lst);
        } else if (x_is(&k, "p")) {
            read_para(x, &k, r);
        } else {
            x_skip(x, &k);
        }
    }
}

static void read_xfrm(struct xml *x, const struct xtok *t, struct rxfrm *f) {
    f->has = true;
    if (x_true(t, "flipH")) {
        f->flip |= FLIP_H;
    }
    if (x_true(t, "flipV")) {
        f->flip |= FLIP_V;
    }
    int64_t rot;
    if (x_num(t, "rot", &rot) && rot % 21600000 != 0) {
        f->rotated = true;
        load_lossy = true;
    }
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "off")) {
            x_num(&k, "x", &f->x);
            x_num(&k, "y", &f->y);
        } else if (x_is(&k, "ext")) {
            x_num(&k, "cx", &f->w);
            x_num(&k, "cy", &f->h);
        } else if (x_is(&k, "chOff")) {
            x_num(&k, "x", &f->cx);
            x_num(&k, "y", &f->cy);
        } else if (x_is(&k, "chExt")) {
            x_num(&k, "cx", &f->cw);
            x_num(&k, "cy", &f->ch);
        }
        x_skip(x, &k);
    }
}

static void read_ln(struct xml *x, const struct xtok *t, struct rshape *r) {
    int64_t w;
    if (x_num(t, "w", &w)) {
        r->line_w = (int32_t)w;
        r->line_w_set = true;
    }
    if (t->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        uint32_t c;
        if (read_fill(x, &k, &c)) {
            r->line = c;
            r->line_set = true;
        } else {
            x_skip(x, &k);
        }
    }
}

static void read_sppr(struct xml *x, const struct xtok *open, struct rshape *r) {
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        uint32_t c;
        if (x_is(&k, "xfrm")) {
            read_xfrm(x, &k, &r->xf);
        } else if (x_is(&k, "prstGeom")) {
            x_attr(&k, "prst", r->prst, sizeof(r->prst));
            x_skip(x, &k);
        } else if (x_is(&k, "custGeom")) {
            r->custom = true;
            x_skip(x, &k);
        } else if (x_is(&k, "ln")) {
            read_ln(x, &k, r);
        } else if (read_fill(x, &k, &c)) {
            r->fill = c;
            r->fill_set = true;
        } else {
            x_skip(x, &k);
        }
    }
}

static void read_style(struct xml *x, const struct xtok *open, struct rshape *r) {
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        int64_t idx = 0;
        x_num(&k, "idx", &idx);
        if (x_is(&k, "fillRef")) {
            r->fill_ref = (int)idx;
            r->fill_ref_col = read_colour_in(x, &k);
        } else if (x_is(&k, "lnRef")) {
            r->ln_ref = (int)idx;
            r->ln_ref_col = read_colour_in(x, &k);
        } else if (x_is(&k, "fontRef")) {
            uint32_t c = read_colour_in(x, &k);
            if (c != NO_COLOUR) {
                r->font_ref = true;
                r->font_col = c;
            }
        } else {
            x_skip(x, &k);
        }
    }
}

static void read_nv(struct xml *x, const struct xtok *open, struct rshape *r) {
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "cNvSpPr")) {
            if (x_true(&k, "txBox")) {
                r->txbox = true;
            }
            x_skip(x, &k);
        } else if (x_is(&k, "nvPr") && k.type == X_OPEN) {
            struct xtok q;
            while (x_child(x, &q)) {
                if (x_is(&q, "ph")) {
                    char v[32] = "";
                    int64_t idx = 0;
                    x_attr(&q, "type", v, sizeof(v));
                    x_num(&q, "idx", &idx);
                    r->has_ph = true;
                    r->phtype = ph_code(v);
                    r->idx = (int)idx;
                }
                if (q.type != X_TEXT) {
                    x_skip(x, &q);
                }
            }
        } else {
            x_skip(x, &k);
        }
    }
}

static void read_blipfill(struct xml *x, const struct xtok *open,
                          struct rshape *r) {
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "blip")) {
            x_attr(&k, "embed", r->embed, sizeof(r->embed));
        } else if (x_is(&k, "srcRect")) {
            int64_t n;
            static const char *const sides[4] = {"l", "t", "r", "b"};
            for (int i = 0; i < 4; i++) {
                if (x_num(&k, sides[i], &n) && n != 0) {
                    r->cropped = true;
                }
            }
        }
        x_skip(x, &k);
    }
}

/* Reads a whole sp, pic or cxnSp. */
static void read_shape(struct xml *x, const struct xtok *open, int kind,
                       struct rshape *r) {
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "nvSpPr") || x_is(&k, "nvPicPr") || x_is(&k, "nvCxnSpPr")) {
            read_nv(x, &k, r);
        } else if (x_is(&k, "spPr")) {
            read_sppr(x, &k, r);
        } else if (x_is(&k, "style")) {
            read_style(x, &k, r);
        } else if (x_is(&k, "txBody")) {
            read_txbody(x, &k, r);
        } else if (x_is(&k, "blipFill")) {
            read_blipfill(x, &k, r);
        } else {
            x_skip(x, &k);
        }
    }
}

/* ============================================================ the package */

struct rel {
    char id[40];
    char type[32];
    char target[200];
};

struct rels {
    struct rel *r;
    int n;
};

static const uint8_t *pk_zip;
static size_t pk_size;

static char *part_read(const char *name, size_t *length) {
    size_t n = 0;
    char *data = (char *)zip_read(pk_zip, pk_size, name, &n);
    if (length != NULL) {
        *length = n;
    }
    return data;
}

/* Resolves a relationship target against the part that has it. */
static void path_join(char *out, size_t capacity, const char *part,
                      const char *target) {
    char tmp[512];
    if (target[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", target + 1);
    } else {
        const char *slash = strrchr(part, '/');
        int dir = slash != NULL ? (int)(slash - part + 1) : 0;
        snprintf(tmp, sizeof(tmp), "%.*s%s", dir, part, target);
    }
    char *segs[64];
    int n = 0;
    char *s = tmp;
    while (s != NULL && *s != '\0') {
        char *next = strchr(s, '/');
        if (next != NULL) {
            *next++ = '\0';
        }
        if (strcmp(s, "..") == 0) {
            if (n > 0) {
                n--;
            }
        } else if (strcmp(s, ".") != 0 && s[0] != '\0' && n < 64) {
            segs[n++] = s;
        }
        s = next;
    }
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t length = strlen(segs[i]);
        if (used + length + 2 > capacity) {
            break;
        }
        if (i > 0) {
            out[used++] = '/';
        }
        memcpy(out + used, segs[i], length);
        used += length;
        out[used] = '\0';
    }
}

static void read_rels(const char *part, struct rels *out) {
    out->r = NULL;
    out->n = 0;
    char name[300];
    const char *slash = strrchr(part, '/');
    int dir = slash != NULL ? (int)(slash - part + 1) : 0;
    snprintf(name, sizeof(name), "%.*s_rels/%s.rels", dir, part, part + dir);
    size_t length;
    char *xml = part_read(name, &length);
    if (xml == NULL) {
        return;
    }
    int cap = 0;
    struct xml x = {xml, length, 0};
    struct xtok t;
    while (xml_next(&x, &t) != X_END) {
        if ((t.type != X_OPEN && t.type != X_EMPTY)
            || !x_is(&t, "Relationship")) {
            continue;
        }
        char mode[16] = "";
        if (x_attr(&t, "TargetMode", mode, sizeof(mode))
            && strcmp(mode, "External") == 0) {
            continue;
        }
        if (out->n == cap) {
            cap = cap < 8 ? 8 : cap * 2;
            struct rel *grown = realloc(out->r, (size_t)cap * sizeof(*grown));
            if (grown == NULL) {
                break;
            }
            out->r = grown;
        }
        struct rel *r = &out->r[out->n];
        char type[200] = "", target[200] = "";
        x_attr(&t, "Id", r->id, sizeof(r->id));
        x_attr(&t, "Type", type, sizeof(type));
        x_attr(&t, "Target", target, sizeof(target));
        const char *last = strrchr(type, '/');
        snprintf(r->type, sizeof(r->type), "%s", last != NULL ? last + 1 : type);
        path_join(r->target, sizeof(r->target), part, target);
        out->n++;
    }
    free(xml);
}

static const struct rel *rel_by_id(const struct rels *r, const char *id) {
    for (int i = 0; i < r->n; i++) {
        if (strcmp(r->r[i].id, id) == 0) {
            return &r->r[i];
        }
    }
    return NULL;
}

static const struct rel *rel_by_type(const struct rels *r, const char *type) {
    for (int i = 0; i < r->n; i++) {
        if (strcmp(r->r[i].type, type) == 0) {
            return &r->r[i];
        }
    }
    return NULL;
}

/* ============================================================ masters, layouts */

struct phdef {
    int type, idx;
    struct rxfrm xf;
    struct bodyp body;
    struct tstyle lst;
};

#define MAX_MASTERS 8
#define MAX_LAYOUTS 96
#define MAX_PH 24

struct lmaster {
    char path[200];
    uint32_t scheme[C_COUNT];
    int map[4];
    struct tstyle title, body, other;
    struct phdef ph[MAX_PH];
    int nph;
    struct slide deco;
    uint32_t bg;
    bool has_bg;
};

struct llayout {
    char path[200];
    int master;
    struct phdef ph[MAX_PH];
    int nph;
    struct slide deco;
    uint32_t bg;
    bool has_bg, show_master;
    int deco_index; /* -2 not made yet */
};

static struct lmaster *lmasters;
static int nlmasters;
static struct llayout *llayouts;
static int nllayouts;
static struct tstyle default_text;

struct mcache {
    char path[200];
    int index;
};
static struct mcache *mcache;
static int nmcache, mcache_cap;

/* What a shape tree is being read for. */
enum { W_SLIDE, W_LAYOUT, W_MASTER };

struct walk {
    int mode;
    const char *part;
    struct rels *rels;
    struct lmaster *m;
    struct llayout *l;
    struct phdef *ph;   /* layout or master placeholders being collected */
    int *nph;
    struct slide *out;  /* where shapes go */
    uint32_t bg;
    bool has_bg;
};

struct xform {
    double sx, sy, ox, oy;
};

static void apply_master(const struct lmaster *m) {
    memcpy(cur_scheme, m->scheme, sizeof(cur_scheme));
    memcpy(cur_map, m->map, sizeof(cur_map));
}

static int media_for(const char *path) {
    for (int i = 0; i < nmcache; i++) {
        if (strcmp(mcache[i].path, path) == 0) {
            return mcache[i].index;
        }
    }
    size_t size;
    char *data = part_read(path, &size);
    if (data == NULL) {
        return -1;
    }
    const char *dot = strrchr(path, '.');
    char ext[8] = "bin";
    if (dot != NULL && strlen(dot + 1) < sizeof(ext)) {
        size_t i = 0;
        for (; dot[1 + i] != '\0'; i++) {
            ext[i] = (char)tolower((unsigned char)dot[1 + i]);
        }
        ext[i] = '\0';
    }
    int index = media_add((uint8_t *)data, size, ext);
    if (index < 0) {
        return -1;
    }
    if (nmcache == mcache_cap) {
        int cap = mcache_cap < 16 ? 16 : mcache_cap * 2;
        struct mcache *grown = realloc(mcache, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return index;
        }
        mcache = grown;
        mcache_cap = cap;
    }
    snprintf(mcache[nmcache].path, sizeof(mcache[nmcache].path), "%s", path);
    mcache[nmcache].index = index;
    nmcache++;
    return index;
}

/* The master or layout placeholder a slide placeholder takes from. */
static const struct phdef *find_ph(const struct phdef *list, int n, int type,
                                   int idx) {
    if (idx > 0) {
        for (int i = 0; i < n; i++) {
            if (list[i].idx == idx) {
                return &list[i];
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (list[i].type == type) {
            return &list[i];
        }
    }
    if (type == RT_CTR_TITLE || type == RT_TITLE) {
        int other = type == RT_TITLE ? RT_CTR_TITLE : RT_TITLE;
        for (int i = 0; i < n; i++) {
            if (list[i].type == other) {
                return &list[i];
            }
        }
    }
    return NULL;
}

static int master_type(int type) {
    switch (type) {
    case RT_TITLE: case RT_CTR_TITLE: return RT_TITLE;
    case RT_DT: case RT_FTR: case RT_SLDNUM: return type;
    default: return RT_BODY;
    }
}

static int64_t xf_x(const struct xform *f, int64_t v) {
    return (int64_t)floor(f->ox + v * f->sx + 0.5);
}

static int64_t xf_y(const struct xform *f, int64_t v) {
    return (int64_t)floor(f->oy + v * f->sy + 0.5);
}

/* Turns a raw shape into a deck shape. */
static void build_shape(struct rshape *r, struct walk *w, const struct xform *tf) {
    const struct phdef *lp = NULL, *mp = NULL;
    struct lmaster *m = w->m;
    if (r->has_ph && w->mode != W_SLIDE) {
        /* Layout and master placeholders only lend their properties. */
        if (*w->nph < MAX_PH) {
            struct phdef *d = &w->ph[(*w->nph)++];
            memset(d, 0, sizeof(*d));
            d->type = r->phtype;
            d->idx = r->idx;
            d->xf = r->xf;
            d->body = r->body;
            d->lst = r->lst;
        }
        return;
    }
    int type = r->phtype;
    if (r->has_ph) {
        if (w->l != NULL) {
            lp = find_ph(w->l->ph, w->l->nph, r->phtype, r->idx);
        }
        if (lp != NULL) {
            type = lp->type;
        }
        mp = find_ph(m->ph, m->nph, master_type(type), 0);
    }
    if (r->kind == 0 && !r->has_text && !r->fill_set && r->fill_ref == 0
        && !r->line_set && r->ln_ref == 0 && !r->has_ph) {
        return; /* invisible */
    }
    struct shape *s = slide_add_shape(w->out, -1);
    if (s == NULL) {
        load_lossy = true;
        return;
    }
    /* Where. */
    const struct rxfrm *xf = r->xf.has ? &r->xf : lp != NULL && lp->xf.has
        ? &lp->xf : mp != NULL && mp->xf.has ? &mp->xf : NULL;
    if (xf != NULL) {
        bool own = xf == &r->xf;
        s->x = own ? xf_x(tf, xf->x) : xf->x;
        s->y = own ? xf_y(tf, xf->y) : xf->y;
        s->w = own ? (int64_t)floor(xf->w * tf->sx + 0.5) : xf->w;
        s->h = own ? (int64_t)floor(xf->h * tf->sy + 0.5) : xf->h;
        s->flip = xf->flip;
    } else {
        s->x = 0;
        s->y = 0;
        s->w = slide_w;
        s->h = slide_h;
    }
    if (s->w < 0) {
        s->w = 0;
    }
    if (s->h < 0) {
        s->h = 0;
    }
    /* What. */
    if (r->kind == 1) {
        s->type = SH_PICTURE;
        const struct rel *rel = r->embed[0] != '\0'
            ? rel_by_id(w->rels, r->embed) : NULL;
        s->media = rel != NULL ? media_for(rel->target) : -1;
        if (s->media < 0) {
            load_lossy = true;
        } else {
            enum image_format f = image_sniff(media[s->media].data,
                                              media[s->media].size);
            if (f == IMAGE_FORMAT_UNKNOWN) {
                load_lossy = true;
            }
        }
        if (r->cropped) {
            load_lossy = true;
        }
    } else if (r->kind == 2 || strcmp(r->prst, "line") == 0
               || strncmp(r->prst, "straightConnector", 17) == 0
               || strncmp(r->prst, "bentConnector", 13) == 0
               || strncmp(r->prst, "curvedConnector", 15) == 0) {
        s->type = SH_LINE;
    } else if (r->has_ph || r->txbox) {
        s->type = SH_TEXT;
    } else if (strcmp(r->prst, "ellipse") == 0) {
        s->type = SH_ELLIPSE;
    } else if (strcmp(r->prst, "roundRect") == 0
               || strcmp(r->prst, "snipRoundRect") == 0
               || strcmp(r->prst, "round2SameRect") == 0
               || strcmp(r->prst, "flowChartAlternateProcess") == 0) {
        s->type = SH_ROUND;
    } else {
        s->type = SH_RECT;
        if (r->custom || (r->prst[0] != '\0' && strcmp(r->prst, "rect") != 0
                          && strcmp(r->prst, "flowChartProcess") != 0)) {
            load_lossy = true;
        }
    }
    if (r->has_ph) {
        s->ph = (uint8_t)(r->phtype == RT_TITLE ? PH_TITLE
                          : r->phtype == RT_CTR_TITLE ? PH_CTR_TITLE
                          : r->phtype == RT_SUBTITLE ? PH_SUBTITLE
                          : r->phtype == RT_BODY ? PH_BODY : PH_OTHER);
        s->ph_idx = r->idx;
    }
    /* Fill and outline. */
    s->fill = r->fill_set ? r->fill : r->fill_ref > 0 ? r->fill_ref_col
                                                      : NO_COLOUR;
    if (s->type == SH_PICTURE && !r->line_set) {
        s->line = NO_COLOUR;
    } else if (r->line_set) {
        s->line = r->line;
    } else if (r->ln_ref > 0 && r->ln_ref_col != NO_COLOUR) {
        s->line = r->ln_ref_col;
    } else if (s->type == SH_LINE) {
        s->line = 0x000000;
    } else {
        s->line = NO_COLOUR;
    }
    if (r->line_w_set) {
        s->line_w = r->line_w;
    } else {
        s->line_w = r->ln_ref >= 3 ? 19050 : r->ln_ref == 2 ? 12700 : 9525;
    }
    if (s->type == SH_PICTURE || s->type == SH_LINE || !r->has_text) {
        return;
    }
    /* The text area. */
    struct bodyp body;
    memset(&body, 0, sizeof(body));
    body.wrap = true;
    body.ins[0] = body.ins[2] = 91440;
    body.ins[1] = body.ins[3] = 45720;
    body.font_scale = 100000;
    if (mp != NULL) {
        bp_merge(&body, &mp->body);
    }
    if (lp != NULL) {
        bp_merge(&body, &lp->body);
    }
    bp_merge(&body, &r->body);
    s->anchor = body.anchor;
    s->wrap = body.wrap;
    s->ins_l = body.ins[0];
    s->ins_t = body.ins[1];
    s->ins_r = body.ins[2];
    s->ins_b = body.ins[3];
    s->autofit = body.autofit;
    s->font_scale = body.font_scale > 0 ? body.font_scale : 100000;
    /* The text, its look taken from the chain of list styles. */
    const struct tstyle *chain[4];
    int n = 0;
    if (r->has_ph) {
        int kind = master_type(type);
        chain[n++] = kind == RT_TITLE ? &m->title
            : kind == RT_BODY ? &m->body : &m->other;
        if (mp != NULL) {
            chain[n++] = &mp->lst;
        }
        if (lp != NULL) {
            chain[n++] = &lp->lst;
        }
    } else {
        chain[n++] = w->mode == W_SLIDE ? &default_text : &m->other;
    }
    struct pstyle font;
    memset(&font, 0, sizeof(font));
    if (r->font_ref) {
        font.colour = r->font_col;
        font.set = PS_COLOUR;
    }
    for (int pi = 0; pi < r->npara; pi++) {
        struct rpara *rp = &r->paras[pi];
        int level = rp->level;
        struct pstyle P;
        memset(&P, 0, sizeof(P));
        P.size = 1800;
        P.colour = scheme_colour("tx1");
        P.line = 100000;
        for (int c = 0; c < n; c++) {
            ps_merge(&P, &chain[c]->def);
            ps_merge(&P, &chain[c]->lvl[level]);
        }
        ps_merge(&P, &font);
        ps_merge(&P, &r->lst.def);
        ps_merge(&P, &r->lst.lvl[level]);
        ps_merge(&P, &rp->ppr);
        cstyle base = st_make(P.fval & P.fmask, P.size, P.colour);
        struct para *p = shape_insert_para(s, s->npara, base);
        if (p == NULL) {
            break;
        }
        p->align = P.align;
        p->level = (uint8_t)level;
        p->bullet = P.bullet;
        p->bullet_char = P.bullet_char != 0 ? P.bullet_char : 0x2022;
        p->num_type = P.num_type;
        p->num_start = P.num_start > 0 ? P.num_start : 1;
        p->mar_l = P.mar;
        p->indent = P.ind;
        p->space_before = (P.set & PS_BEFORE_PCT)
            ? (int32_t)((int64_t)P.size * P.before / 100000) : P.before;
        p->space_after = (P.set & PS_AFTER_PCT)
            ? (int32_t)((int64_t)P.size * P.after / 100000) : P.after;
        if (P.set & PS_LINE_PTS) {
            p->line_pct = (int32_t)((int64_t)P.line * 100000 * 10
                                    / ((int64_t)P.size * 12));
        } else {
            p->line_pct = P.line > 0 ? P.line : 100000;
        }
        if (body.ln_red > 0) {
            p->line_pct -= body.ln_red;
        }
        if (p->line_pct < 30000) {
            p->line_pct = 30000;
        }
        for (int k = 0; k < rp->nruns; k++) {
            struct pstyle R = P;
            ps_merge(&R, &rp->runs[k].rpr);
            cstyle st = st_make(R.fval & R.fmask, R.size, R.colour);
            para_insert(p, p->len, rp->text + rp->runs[k].start,
                        rp->runs[k].len, NULL, st);
        }
        if (rp->has_end && p->len == 0) {
            struct pstyle R = P;
            ps_merge(&R, &rp->end);
            p->end_style = st_make(R.fval & R.fmask, R.size, R.colour);
        } else if (p->len > 0) {
            p->end_style = p->st[p->len - 1];
        }
    }
    if (s->npara == 0) {
        struct pstyle P;
        memset(&P, 0, sizeof(P));
        P.size = 1800;
        P.colour = scheme_colour("tx1");
        for (int c = 0; c < n; c++) {
            ps_merge(&P, &chain[c]->def);
            ps_merge(&P, &chain[c]->lvl[0]);
        }
        ps_merge(&P, &font);
        shape_start_text(s, st_make(P.fval & P.fmask, P.size, P.colour),
                         P.align, P.bullet);
    }
}

static void read_tree(struct xml *x, const struct xtok *open, struct walk *w,
                      const struct xform *parent);

static void read_alternate(struct xml *x, const struct xtok *open,
                           struct walk *w, const struct xform *tf) {
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "Fallback")) {
            read_tree(x, &k, w, tf);
        } else {
            x_skip(x, &k);
        }
    }
}

/* The shapes of an spTree or grpSp. */
static void read_tree(struct xml *x, const struct xtok *open, struct walk *w,
                      const struct xform *parent) {
    if (open->type != X_OPEN) {
        return;
    }
    struct xform tf = *parent;
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "sp") || x_is(&k, "pic") || x_is(&k, "cxnSp")) {
            int kind = x_is(&k, "sp") ? 0 : x_is(&k, "pic") ? 1 : 2;
            struct rshape r;
            read_shape(x, &k, kind, &r);
            build_shape(&r, w, &tf);
            rshape_free(&r);
        } else if (x_is(&k, "grpSpPr")) {
            if (k.type != X_OPEN) {
                continue;
            }
            struct xtok q;
            while (x_child(x, &q)) {
                if (x_is(&q, "xfrm")) {
                    struct rxfrm f;
                    memset(&f, 0, sizeof(f));
                    read_xfrm(x, &q, &f);
                    if (f.cw > 0 && f.ch > 0) {
                        double kx = (double)f.w / (double)f.cw;
                        double ky = (double)f.h / (double)f.ch;
                        tf.ox = parent->ox + parent->sx * (f.x - f.cx * kx);
                        tf.oy = parent->oy + parent->sy * (f.y - f.cy * ky);
                        tf.sx = parent->sx * kx;
                        tf.sy = parent->sy * ky;
                    }
                } else if (q.type != X_TEXT) {
                    x_skip(x, &q);
                }
            }
        } else if (x_is(&k, "grpSp")) {
            read_tree(x, &k, w, &tf);
        } else if (x_is(&k, "AlternateContent")) {
            read_alternate(x, &k, w, &tf);
        } else {
            if (x_is(&k, "graphicFrame") || x_is(&k, "contentPart")) {
                load_lossy = true;
            }
            x_skip(x, &k);
        }
    }
}

static void read_bg(struct xml *x, const struct xtok *open, struct walk *w) {
    if (open->type != X_OPEN) {
        return;
    }
    struct xtok k;
    while (x_child(x, &k)) {
        if (k.type == X_TEXT) {
            continue;
        }
        if (x_is(&k, "bgPr") && k.type == X_OPEN) {
            struct xtok q;
            while (x_child(x, &q)) {
                uint32_t c;
                if (q.type == X_TEXT) {
                    continue;
                }
                if (read_fill(x, &q, &c)) {
                    if (c != NO_COLOUR) {
                        w->bg = c;
                        w->has_bg = true;
                    }
                } else {
                    x_skip(x, &q);
                }
            }
        } else if (x_is(&k, "bgRef")) {
            int64_t idx = 0;
            x_num(&k, "idx", &idx);
            uint32_t c = read_colour_in(x, &k);
            if (c != NO_COLOUR) {
                w->bg = c;
                w->has_bg = true;
            }
            if (idx > 1001) {
                load_lossy = true; /* gradient or picture background */
            }
        } else {
            x_skip(x, &k);
        }
    }
}

/* Reads a slide, layout or master part. Returns the root's attributes'
   show and showMasterSp flags through *show and *show_master. */
static bool read_part(const char *part, struct walk *w, bool *show,
                      bool *show_master) {
    size_t length;
    char *xml = part_read(part, &length);
    if (xml == NULL) {
        return false;
    }
    struct xml x = {xml, length, 0};
    struct xtok t;
    static const struct xform identity = {1, 1, 0, 0};
    bool root = false;
    *show = *show_master = true;
    while (xml_next(&x, &t) != X_END) {
        if (t.type != X_OPEN) {
            continue;
        }
        if (!root) {
            root = true;
            char v[16];
            if (x_attr(&t, "show", v, sizeof(v)) && (v[0] == '0' || v[0] == 'f')) {
                *show = false;
            }
            if (x_attr(&t, "showMasterSp", v, sizeof(v))
                && (v[0] == '0' || v[0] == 'f')) {
                *show_master = false;
            }
            continue;
        }
        if (x_is(&t, "cSld")) {
            struct xtok k;
            while (x_child(&x, &k)) {
                if (k.type == X_TEXT) {
                    continue;
                }
                if (x_is(&k, "bg")) {
                    read_bg(&x, &k, w);
                } else if (x_is(&k, "spTree")) {
                    read_tree(&x, &k, w, &identity);
                } else {
                    x_skip(&x, &k);
                }
            }
        } else if (x_is(&t, "txStyles") && w->m != NULL && w->mode == W_MASTER) {
            struct xtok k;
            while (x_child(&x, &k)) {
                if (x_is(&k, "titleStyle")) {
                    read_lst(&x, &k, &w->m->title);
                } else if (x_is(&k, "bodyStyle")) {
                    read_lst(&x, &k, &w->m->body);
                } else if (x_is(&k, "otherStyle")) {
                    read_lst(&x, &k, &w->m->other);
                } else if (k.type != X_TEXT) {
                    x_skip(&x, &k);
                }
            }
        } else {
            x_skip(&x, &t);
        }
    }
    free(xml);
    return true;
}

static void read_theme(const char *part, struct lmaster *m) {
    memcpy(m->scheme, office_scheme, sizeof(m->scheme));
    size_t length;
    char *xml = part_read(part, &length);
    if (xml == NULL) {
        return;
    }
    struct xml x = {xml, length, 0};
    struct xtok t;
    bool in_scheme = false;
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN && x_is(&t, "clrScheme")) {
            in_scheme = true;
            continue;
        }
        if (t.type == X_CLOSE && x_is(&t, "clrScheme")) {
            break;
        }
        if (in_scheme && t.type == X_OPEN) {
            char name[16];
            snprintf(name, sizeof(name), "%.*s", (int)t.name_length, t.name);
            int slot = scheme_slot(name);
            uint32_t c = read_colour_in(&x, &t);
            if (slot >= 0 && c != NO_COLOUR) {
                m->scheme[slot] = c;
            }
        }
    }
    free(xml);
}

static void read_clrmap(const char *part, struct lmaster *m) {
    m->map[0] = C_LT1;
    m->map[1] = C_DK1;
    m->map[2] = C_LT2;
    m->map[3] = C_DK2;
    size_t length;
    char *xml = part_read(part, &length);
    if (xml == NULL) {
        return;
    }
    struct xml x = {xml, length, 0};
    struct xtok t;
    while (xml_next(&x, &t) != X_END) {
        if ((t.type == X_OPEN || t.type == X_EMPTY) && x_is(&t, "clrMap")) {
            static const char *const names[4] = {"bg1", "tx1", "bg2", "tx2"};
            for (int i = 0; i < 4; i++) {
                char v[16];
                if (x_attr(&t, names[i], v, sizeof(v))) {
                    int slot = scheme_slot(v);
                    if (slot >= 0) {
                        m->map[i] = slot;
                    }
                }
            }
            break;
        }
    }
    free(xml);
}

static int load_master(const char *part) {
    for (int i = 0; i < nlmasters; i++) {
        if (strcmp(lmasters[i].path, part) == 0) {
            return i;
        }
    }
    if (nlmasters == MAX_MASTERS) {
        return nlmasters > 0 ? 0 : -1;
    }
    struct lmaster *m = &lmasters[nlmasters];
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", part);
    m->deco.background = 0xffffff;
    struct rels rels;
    read_rels(part, &rels);
    const struct rel *theme = rel_by_type(&rels, "theme");
    if (theme != NULL) {
        read_theme(theme->target, m);
    } else {
        memcpy(m->scheme, office_scheme, sizeof(m->scheme));
    }
    read_clrmap(part, m);
    apply_master(m);
    struct walk w = {W_MASTER, part, &rels, m, NULL, m->ph, &m->nph, &m->deco,
                     0, false};
    bool show, show_master;
    read_part(part, &w, &show, &show_master);
    m->bg = w.bg;
    m->has_bg = w.has_bg;
    free(rels.r);
    return nlmasters++;
}

static int load_layout(const char *part) {
    for (int i = 0; i < nllayouts; i++) {
        if (strcmp(llayouts[i].path, part) == 0) {
            return i;
        }
    }
    if (nllayouts == MAX_LAYOUTS) {
        return -1;
    }
    struct rels rels;
    read_rels(part, &rels);
    const struct rel *mr = rel_by_type(&rels, "slideMaster");
    int mi = mr != NULL ? load_master(mr->target) : -1;
    if (mi < 0) {
        free(rels.r);
        return -1;
    }
    struct llayout *l = &llayouts[nllayouts];
    memset(l, 0, sizeof(*l));
    snprintf(l->path, sizeof(l->path), "%s", part);
    l->master = mi;
    l->deco_index = -2;
    l->deco.background = 0xffffff;
    apply_master(&lmasters[mi]);
    struct walk w = {W_LAYOUT, part, &rels, &lmasters[mi], NULL, l->ph, &l->nph,
                     &l->deco, 0, false};
    bool show;
    read_part(part, &w, &show, &l->show_master);
    l->bg = w.bg;
    l->has_bg = w.has_bg;
    free(rels.r);
    return nllayouts++;
}

/* The deck decoration for a layout: the master's shapes (unless the
   layout hides them) under the layout's own. */
static int layout_deco(struct llayout *l) {
    if (l->deco_index != -2) {
        return l->deco_index;
    }
    l->deco_index = -1;
    struct lmaster *m = &lmasters[l->master];
    int total = (l->show_master ? m->deco.n : 0) + l->deco.n;
    if (total == 0) {
        return -1;
    }
    struct slide *d = deco_add();
    if (d == NULL) {
        return -1;
    }
    for (int pass = 0; pass < 2; pass++) {
        const struct slide *from = pass == 0 ? &m->deco : &l->deco;
        if (pass == 0 && !l->show_master) {
            continue;
        }
        for (int i = 0; i < from->n; i++) {
            struct shape *s = slide_add_shape(d, -1);
            if (s == NULL || !shape_copy(s, &from->shapes[i])) {
                if (s != NULL) {
                    d->n--;
                }
                break;
            }
        }
    }
    l->deco_index = ndecos - 1;
    return l->deco_index;
}

static void read_presentation(const char *part, char ids[][40], int *count) {
    size_t length;
    char *xml = part_read(part, &length);
    *count = 0;
    if (xml == NULL) {
        return;
    }
    struct xml x = {xml, length, 0};
    struct xtok t;
    while (xml_next(&x, &t) != X_END) {
        if (t.type != X_OPEN && t.type != X_EMPTY) {
            continue;
        }
        if (x_is(&t, "sldSz")) {
            int64_t cx = 0, cy = 0;
            if (x_num(&t, "cx", &cx) && x_num(&t, "cy", &cy) && cx >= 914400
                && cy >= 914400 && cx <= 51206400 && cy <= 51206400) {
                slide_w = cx;
                slide_h = cy;
            }
        } else if (x_is(&t, "sldId")) {
            if (*count < MAX_SLIDES && x_rid(&t, ids[*count], 40)) {
                (*count)++;
            }
        } else if (x_is(&t, "defaultTextStyle")) {
            read_lst(&x, &t, &default_text);
        } else if (x_is(&t, "notesMasterIdLst") || x_is(&t, "extLst")
                   || x_is(&t, "handoutMasterIdLst")) {
            x_skip(&x, &t);
        }
    }
    free(xml);
}

static void load_free(void) {
    for (int i = 0; i < nlmasters; i++) {
        slide_free(&lmasters[i].deco);
    }
    for (int i = 0; i < nllayouts; i++) {
        slide_free(&llayouts[i].deco);
    }
    free(lmasters);
    free(llayouts);
    free(mcache);
    lmasters = NULL;
    llayouts = NULL;
    mcache = NULL;
    nlmasters = nllayouts = nmcache = mcache_cap = 0;
}

/* Loads a .pptx into the (empty) deck. Returns a problem or NULL. */
static const char *pptx_load(const uint8_t *data, size_t size, bool *lossy) {
    pk_zip = data;
    pk_size = size;
    load_lossy = false;
    memset(&default_text, 0, sizeof(default_text));
    memcpy(cur_scheme, office_scheme, sizeof(cur_scheme));
    slide_w = 12192000;
    slide_h = 6858000;
    char main_part[200] = "ppt/presentation.xml";
    struct rels top;
    read_rels("", &top);
    const struct rel *doc = rel_by_type(&top, "officeDocument");
    if (doc != NULL) {
        snprintf(main_part, sizeof(main_part), "%s", doc->target);
    }
    free(top.r);
    struct rels prels;
    read_rels(main_part, &prels);
    char (*ids)[40] = malloc(MAX_SLIDES * sizeof(*ids));
    lmasters = calloc(MAX_MASTERS, sizeof(*lmasters));
    llayouts = calloc(MAX_LAYOUTS, sizeof(*llayouts));
    if (ids == NULL || lmasters == NULL || llayouts == NULL) {
        free(ids);
        free(prels.r);
        load_free();
        return "NOT ENOUGH MEMORY TO OPEN THAT PRESENTATION";
    }
    int count;
    read_presentation(main_part, ids, &count);
    if (prels.n == 0 && count == 0) {
        free(ids);
        free(prels.r);
        load_free();
        return "THAT IS NOT A POWERPOINT PRESENTATION";
    }
    for (int i = 0; i < count; i++) {
        const struct rel *sr = rel_by_id(&prels, ids[i]);
        if (sr == NULL) {
            continue;
        }
        struct rels srels;
        read_rels(sr->target, &srels);
        const struct rel *lr = rel_by_type(&srels, "slideLayout");
        int li = lr != NULL ? load_layout(lr->target) : -1;
        struct llayout *l = li >= 0 ? &llayouts[li] : NULL;
        struct lmaster *m = l != NULL ? &lmasters[l->master] : NULL;
        if (m == NULL) {
            if (nlmasters == 0) {
                /* No master at all: an empty one with Office colours. */
                memset(&lmasters[0], 0, sizeof(lmasters[0]));
                memcpy(lmasters[0].scheme, office_scheme, sizeof(office_scheme));
                lmasters[0].map[0] = C_LT1;
                lmasters[0].map[1] = C_DK1;
                lmasters[0].map[2] = C_LT2;
                lmasters[0].map[3] = C_DK2;
                nlmasters = 1;
            }
            m = &lmasters[0];
        }
        apply_master(m);
        struct slide *sl = deck_insert_slide(nslides);
        if (sl == NULL) {
            free(srels.r);
            load_lossy = true;
            break;
        }
        struct walk w = {W_SLIDE, sr->target, &srels, m, l, NULL, NULL, sl, 0,
                         false};
        bool show = true, show_master = true;
        read_part(sr->target, &w, &show, &show_master);
        sl = &slides[nslides - 1];
        sl->hidden = !show;
        sl->background = w.has_bg ? w.bg : l != NULL && l->has_bg ? l->bg
            : m->has_bg ? m->bg : scheme_colour("bg1");
        sl->deco = l != NULL && show_master ? layout_deco(l) : -1;
        free(srels.r);
    }
    free(ids);
    free(prels.r);
    load_free();
    *lossy = load_lossy;
    return NULL;
}

/* ============================================================ writing */

#define NS_A "http://schemas.openxmlformats.org/drawingml/2006/main"
#define NS_R "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
#define NS_P "http://schemas.openxmlformats.org/presentationml/2006/main"
#define NS_PKG_REL "http://schemas.openxmlformats.org/package/2006/relationships"
#define REL_TYPE "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
#define CT_PML "application/vnd.openxmlformats-officedocument.presentationml."
#define PML_ROOT_NS " xmlns:a=\"" NS_A "\" xmlns:r=\"" NS_R "\" xmlns:p=\"" NS_P "\""

static void put_attr(struct buf *b, const char *name, int64_t value) {
    buf_byte(b, ' ');
    buf_puts(b, name);
    buf_puts(b, "=\"");
    buf_int(b, (long)value);
    buf_byte(b, '"');
}

/* Text for an attribute value: & < > and the quote escaped. */
static void put_attr_text(struct buf *b, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c == '&') {
            buf_puts(b, "&amp;");
        } else if (c == '<') {
            buf_puts(b, "&lt;");
        } else if (c == '>') {
            buf_puts(b, "&gt;");
        } else if (c == '"') {
            buf_puts(b, "&quot;");
        } else {
            buf_byte(b, (uint8_t)c);
        }
    }
}

static void put_srgb(struct buf *b, uint32_t colour) {
    char hex[8];
    snprintf(hex, sizeof(hex), "%06X", colour & 0xffffff);
    buf_puts(b, "<a:srgbClr val=\"");
    buf_puts(b, hex);
    buf_puts(b, "\"/>");
}

static void put_solid(struct buf *b, uint32_t colour) {
    buf_puts(b, "<a:solidFill>");
    put_srgb(b, colour);
    buf_puts(b, "</a:solidFill>");
}

static void put_rpr(struct buf *b, const char *tag, cstyle s) {
    buf_puts(b, "<a:");
    buf_puts(b, tag);
    buf_puts(b, " lang=\"en-US\"");
    put_attr(b, "sz", ST_SIZE(s));
    uint32_t flags = ST_FLAGS(s);
    if (flags & S_BOLD) {
        buf_puts(b, " b=\"1\"");
    }
    if (flags & S_ITALIC) {
        buf_puts(b, " i=\"1\"");
    }
    if (flags & S_UNDER) {
        buf_puts(b, " u=\"sng\"");
    }
    if (flags & S_STRIKE) {
        buf_puts(b, " strike=\"sngStrike\"");
    }
    buf_puts(b, " dirty=\"0\">");
    put_solid(b, ST_COLOR(s));
    buf_puts(b, "</a:");
    buf_puts(b, tag);
    buf_byte(b, '>');
}

static void put_para(struct buf *b, const struct para *p) {
    static const char *const aligns[4] = {"l", "ctr", "r", "just"};
    buf_puts(b, "<a:p><a:pPr");
    put_attr(b, "marL", p->mar_l);
    put_attr(b, "indent", p->indent);
    if (p->level > 0) {
        put_attr(b, "lvl", p->level);
    }
    buf_puts(b, " algn=\"");
    buf_puts(b, aligns[p->align & 3]);
    buf_puts(b, "\"><a:lnSpc><a:spcPct");
    put_attr(b, "val", p->line_pct);
    buf_puts(b, "/></a:lnSpc><a:spcBef><a:spcPts");
    put_attr(b, "val", p->space_before);
    buf_puts(b, "/></a:spcBef><a:spcAft><a:spcPts");
    put_attr(b, "val", p->space_after);
    buf_puts(b, "/></a:spcAft>");
    if (p->bullet == BU_CHAR) {
        char utf[8];
        int n = utf8_put(utf, p->bullet_char != 0 ? p->bullet_char : 0x2022);
        buf_puts(b, "<a:buFont typeface=\"Arial\"/><a:buChar char=\"");
        put_attr_text(b, utf, (size_t)n);
        buf_puts(b, "\"/>");
    } else if (p->bullet == BU_NUMBER) {
        buf_puts(b, "<a:buFont typeface=\"+mj-lt\"/><a:buAutoNum type=\"");
        buf_puts(b, num_type_name(p->num_type));
        buf_byte(b, '"');
        if (p->num_start > 1) {
            put_attr(b, "startAt", p->num_start);
        }
        buf_puts(b, "/>");
    } else {
        buf_puts(b, "<a:buNone/>");
    }
    buf_puts(b, "</a:pPr>");
    int i = 0;
    while (i < p->len) {
        if (p->text[i] == '\n') {
            buf_puts(b, "<a:br>");
            put_rpr(b, "rPr", p->st[i]);
            buf_puts(b, "</a:br>");
            i++;
            continue;
        }
        int j = i;
        while (j < p->len && p->st[j] == p->st[i] && p->text[j] != '\n') {
            j++;
        }
        buf_puts(b, "<a:r>");
        put_rpr(b, "rPr", p->st[i]);
        buf_puts(b, "<a:t>");
        xml_text(b, p->text + i, (size_t)(j - i));
        buf_puts(b, "</a:t></a:r>");
        i = j;
    }
    put_rpr(b, "endParaRPr", p->len > 0 ? p->st[p->len - 1] : p->end_style);
    buf_puts(b, "</a:p>");
}

static void put_xfrm(struct buf *b, const struct shape *s) {
    buf_puts(b, "<a:xfrm");
    if (s->flip & FLIP_H) {
        buf_puts(b, " flipH=\"1\"");
    }
    if (s->flip & FLIP_V) {
        buf_puts(b, " flipV=\"1\"");
    }
    buf_puts(b, "><a:off");
    put_attr(b, "x", s->x);
    put_attr(b, "y", s->y);
    buf_puts(b, "/><a:ext");
    put_attr(b, "cx", s->w > 0 ? s->w : 0);
    put_attr(b, "cy", s->h > 0 ? s->h : 0);
    buf_puts(b, "/></a:xfrm>");
}

static void put_ln(struct buf *b, const struct shape *s) {
    if (s->line == NO_COLOUR) {
        buf_puts(b, "<a:ln><a:noFill/></a:ln>");
        return;
    }
    buf_puts(b, "<a:ln");
    put_attr(b, "w", s->line_w > 0 ? s->line_w : 12700);
    buf_byte(b, '>');
    put_solid(b, s->line);
    buf_puts(b, "</a:ln>");
}

/* Picture relationships of the part being written: media index to rId
   number (rId1 is the layout or master). */
struct part_media {
    int *media;
    int n, cap;
};

static int part_media_rid(struct part_media *pm, int index) {
    for (int i = 0; i < pm->n; i++) {
        if (pm->media[i] == index) {
            return i + 2;
        }
    }
    if (pm->n == pm->cap) {
        int cap = pm->cap < 8 ? 8 : pm->cap * 2;
        int *grown = realloc(pm->media, (size_t)cap * sizeof(int));
        if (grown == NULL) {
            return -1;
        }
        pm->media = grown;
        pm->cap = cap;
    }
    pm->media[pm->n++] = index;
    return pm->n + 1;
}

static const char *media_ext(int index) {
    const char *e = media[index].ext;
    return strcmp(e, "jpg") == 0 ? "jpeg" : e;
}

static void put_shape(struct buf *b, const struct shape *s, int id,
                      struct part_media *pm) {
    char name[48];
    if (s->type == SH_PICTURE) {
        int rid = s->media >= 0 ? part_media_rid(pm, s->media) : -1;
        if (rid < 0) {
            return;
        }
        snprintf(name, sizeof(name), "Picture %d", id - 1);
        buf_puts(b, "<p:pic><p:nvPicPr><p:cNvPr");
        put_attr(b, "id", id);
        buf_puts(b, " name=\"");
        buf_puts(b, name);
        buf_puts(b, "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/>"
                    "</p:cNvPicPr><p:nvPr/></p:nvPicPr><p:blipFill>"
                    "<a:blip r:embed=\"rId");
        buf_int(b, rid);
        buf_puts(b, "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
                    "<p:spPr>");
        put_xfrm(b, s);
        buf_puts(b, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>");
        if (s->line != NO_COLOUR) {
            put_ln(b, s);
        }
        buf_puts(b, "</p:spPr></p:pic>");
        return;
    }
    if (s->type == SH_LINE) {
        snprintf(name, sizeof(name), "Straight Connector %d", id - 1);
        buf_puts(b, "<p:cxnSp><p:nvCxnSpPr><p:cNvPr");
        put_attr(b, "id", id);
        buf_puts(b, " name=\"");
        buf_puts(b, name);
        buf_puts(b, "\"/><p:cNvCxnSpPr/><p:nvPr/></p:nvCxnSpPr><p:spPr>");
        put_xfrm(b, s);
        buf_puts(b, "<a:prstGeom prst=\"line\"><a:avLst/></a:prstGeom>");
        put_ln(b, s);
        buf_puts(b, "</p:spPr></p:cxnSp>");
        return;
    }
    static const char *const ph_names[] = {"", "Title", "Title", "Subtitle",
                                           "Content Placeholder", "Text"};
    bool ph = s->ph != PH_NONE && s->ph != PH_OTHER;
    if (ph) {
        snprintf(name, sizeof(name), "%s %d", ph_names[s->ph], id - 1);
    } else {
        snprintf(name, sizeof(name), "%s %d", s->type == SH_TEXT ? "TextBox"
                 : s->type == SH_ELLIPSE ? "Oval" : "Rectangle", id - 1);
    }
    buf_puts(b, "<p:sp><p:nvSpPr><p:cNvPr");
    put_attr(b, "id", id);
    buf_puts(b, " name=\"");
    buf_puts(b, name);
    buf_puts(b, "\"/>");
    if (ph) {
        buf_puts(b, "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph");
        if (s->ph == PH_TITLE) {
            buf_puts(b, " type=\"title\"");
        } else if (s->ph == PH_CTR_TITLE) {
            buf_puts(b, " type=\"ctrTitle\"");
        } else if (s->ph == PH_SUBTITLE) {
            buf_puts(b, " type=\"subTitle\"");
            put_attr(b, "idx", 1);
        } else {
            put_attr(b, "idx", s->ph_idx > 0 ? s->ph_idx : 1);
        }
        buf_puts(b, "/></p:nvPr></p:nvSpPr>");
    } else if (s->type == SH_TEXT) {
        buf_puts(b, "<p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>");
    } else {
        buf_puts(b, "<p:cNvSpPr/><p:nvPr/></p:nvSpPr>");
    }
    buf_puts(b, "<p:spPr>");
    put_xfrm(b, s);
    buf_puts(b, "<a:prstGeom prst=\"");
    buf_puts(b, s->type == SH_ELLIPSE ? "ellipse" : s->type == SH_ROUND
             ? "roundRect" : "rect");
    buf_puts(b, "\"><a:avLst/></a:prstGeom>");
    if (s->fill == NO_COLOUR) {
        buf_puts(b, "<a:noFill/>");
    } else {
        put_solid(b, s->fill);
    }
    put_ln(b, s);
    buf_puts(b, "</p:spPr>");
    if (s->npara > 0 || s->type == SH_TEXT) {
        static const char *const anchors[3] = {"t", "ctr", "b"};
        buf_puts(b, "<p:txBody><a:bodyPr wrap=\"");
        buf_puts(b, s->wrap ? "square" : "none");
        buf_byte(b, '"');
        put_attr(b, "lIns", s->ins_l);
        put_attr(b, "tIns", s->ins_t);
        put_attr(b, "rIns", s->ins_r);
        put_attr(b, "bIns", s->ins_b);
        buf_puts(b, " rtlCol=\"0\" anchor=\"");
        buf_puts(b, anchors[s->anchor < 3 ? s->anchor : 0]);
        buf_puts(b, "\">");
        if (s->autofit == FIT_TEXT) {
            buf_puts(b, "<a:normAutofit");
            if (s->font_scale > 0 && s->font_scale < 100000) {
                put_attr(b, "fontScale", s->font_scale);
            }
            buf_puts(b, "/>");
        } else if (s->autofit == FIT_SHAPE) {
            buf_puts(b, "<a:spAutoFit/>");
        } else {
            buf_puts(b, "<a:noAutofit/>");
        }
        buf_puts(b, "</a:bodyPr><a:lstStyle/>");
        if (s->npara == 0) {
            buf_puts(b, "<a:p><a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p>");
        }
        for (int i = 0; i < s->npara; i++) {
            put_para(b, &s->paras[i]);
        }
        buf_puts(b, "</p:txBody>");
    }
    buf_puts(b, "</p:sp>");
}

static void put_tree_start(struct buf *b) {
    buf_puts(b, "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/>"
                "<p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm>"
                "<a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
                "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/>"
                "</a:xfrm></p:grpSpPr>");
}

static void put_rels_media(struct buf *b, const char *first_type,
                           const char *first_target,
                           const struct part_media *pm) {
    buf_puts(b, XML_HEAD "<Relationships xmlns=\"" NS_PKG_REL "\">"
                "<Relationship Id=\"rId1\" Type=\"" REL_TYPE);
    buf_puts(b, first_type);
    buf_puts(b, "\" Target=\"");
    buf_puts(b, first_target);
    buf_puts(b, "\"/>");
    for (int i = 0; i < pm->n; i++) {
        char line[160];
        snprintf(line, sizeof(line),
                 "<Relationship Id=\"rId%d\" Type=\"" REL_TYPE "image\" "
                 "Target=\"../media/image%d.%s\"/>",
                 i + 2, pm->media[i] + 1, media_ext(pm->media[i]));
        buf_puts(b, line);
    }
    buf_puts(b, "</Relationships>");
}

/* A placeholder for a master or layout: position and nothing else. */
static void put_ph_def(struct buf *b, int id, const char *type, int idx,
                       int64_t x, int64_t y, int64_t w, int64_t h,
                       bool with_xfrm) {
    buf_puts(b, "<p:sp><p:nvSpPr><p:cNvPr");
    put_attr(b, "id", id);
    buf_puts(b, " name=\"Placeholder ");
    buf_int(b, id - 1);
    buf_puts(b, "\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph");
    if (type != NULL) {
        buf_puts(b, " type=\"");
        buf_puts(b, type);
        buf_byte(b, '"');
    }
    if (idx > 0) {
        put_attr(b, "idx", idx);
    }
    buf_puts(b, "/></p:nvPr></p:nvSpPr><p:spPr>");
    if (with_xfrm) {
        buf_puts(b, "<a:xfrm><a:off");
        put_attr(b, "x", x);
        put_attr(b, "y", y);
        buf_puts(b, "/><a:ext");
        put_attr(b, "cx", w);
        put_attr(b, "cy", h);
        buf_puts(b, "/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>");
    }
    buf_puts(b, "</p:spPr><p:txBody><a:bodyPr");
    if (type != NULL && strcmp(type, "title") == 0) {
        buf_puts(b, " anchor=\"ctr\"");
    }
    buf_puts(b, "><a:normAutofit/></a:bodyPr><a:lstStyle/><a:p>"
                "<a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>");
}

static void put_text_style(struct buf *b, const char *tag, int size,
                           bool bullets) {
    buf_puts(b, "<p:");
    buf_puts(b, tag);
    buf_byte(b, '>');
    for (int level = 0; level < 9; level++) {
        char open[16];
        snprintf(open, sizeof(open), "<a:lvl%dpPr", level + 1);
        buf_puts(b, open);
        if (bullets) {
            put_attr(b, "marL", 228600 + level * 457200);
            put_attr(b, "indent", -228600);
        } else {
            put_attr(b, "marL", level * 457200);
        }
        buf_puts(b, " algn=\"l\"><a:lnSpc><a:spcPct val=\"90000\"/></a:lnSpc>");
        if (bullets) {
            buf_puts(b, "<a:spcBef><a:spcPts val=\"1000\"/></a:spcBef>"
                        "<a:buFont typeface=\"Arial\"/><a:buChar char=\"&#8226;\"/>");
        } else {
            buf_puts(b, "<a:buNone/>");
        }
        int sz = bullets ? body_sizes[level < 5 ? level : 4] : size;
        buf_puts(b, "<a:defRPr");
        put_attr(b, "sz", sz);
        buf_puts(b, " kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/>"
                    "</a:solidFill><a:latin typeface=\"");
        buf_puts(b, strcmp(tag, "titleStyle") == 0 ? "+mj-lt" : "+mn-lt");
        buf_puts(b, "\"/></a:defRPr>");
        snprintf(open, sizeof(open), "</a:lvl%dpPr>", level + 1);
        buf_puts(b, open);
        if (strcmp(tag, "titleStyle") == 0) {
            break;
        }
    }
    buf_puts(b, "</p:");
    buf_puts(b, tag);
    buf_byte(b, '>');
}

static const char theme_xml[] =
    XML_HEAD "<a:theme xmlns:a=\"" NS_A "\" name=\"OPOS\"><a:themeElements>"
    "<a:clrScheme name=\"Office\">"
    "<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1>"
    "<a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
    "<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2>"
    "<a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2>"
    "<a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1>"
    "<a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2>"
    "<a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3>"
    "<a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
    "<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5>"
    "<a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6>"
    "<a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink>"
    "<a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink>"
    "</a:clrScheme><a:fontScheme name=\"OPOS\">"
    "<a:majorFont><a:latin typeface=\"Noto Sans\"/><a:ea typeface=\"\"/>"
    "<a:cs typeface=\"\"/></a:majorFont>"
    "<a:minorFont><a:latin typeface=\"Noto Sans\"/><a:ea typeface=\"\"/>"
    "<a:cs typeface=\"\"/></a:minorFont></a:fontScheme>"
    "<a:fmtScheme name=\"Office\"><a:fillStyleLst>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "</a:fillStyleLst><a:lnStyleLst>"
    "<a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
    "<a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
    "<a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
    "</a:lnStyleLst><a:effectStyleLst>"
    "<a:effectStyle><a:effectLst/></a:effectStyle>"
    "<a:effectStyle><a:effectLst/></a:effectStyle>"
    "<a:effectStyle><a:effectLst/></a:effectStyle>"
    "</a:effectStyleLst><a:bgFillStyleLst>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
    "</a:bgFillStyleLst></a:fmtScheme></a:themeElements>"
    "<a:objectDefaults/><a:extraClrSchemeLst/></a:theme>";

/* One layout per decoration set (or none) and kind of slide (title slide
   or other): index (deco + 1) * 2 + kind. */
static int slide_kind(const struct slide *sl) {
    for (int i = 0; i < sl->n; i++) {
        if (sl->shapes[i].ph == PH_CTR_TITLE || sl->shapes[i].ph == PH_SUBTITLE) {
            return 0;
        }
    }
    return 1;
}

struct zip_out {
    struct buf zip;
    struct zip_part *parts;
    char (*names)[64];
    int n, cap;
};

static void zout_add(struct zip_out *z, const char *name, struct buf *data) {
    if (z->n >= z->cap || data->failed) {
        z->zip.failed = true;
        free(data->data);
        memset(data, 0, sizeof(*data));
        return;
    }
    snprintf(z->names[z->n], 64, "%s", name);
    zip_add(&z->zip, &z->parts[z->n], z->names[z->n], data);
    z->n++;
    free(data->data);
    memset(data, 0, sizeof(*data));
}

static bool pptx_save(const char *file) {
    int nlayout_slots = (ndecos + 1) * 2;
    int *layout_number = calloc((size_t)nlayout_slots, sizeof(int));
    bool *media_used = calloc((size_t)(nmedia > 0 ? nmedia : 1), sizeof(bool));
    struct zip_out z;
    memset(&z, 0, sizeof(z));
    z.cap = 16 + nslides * 2 + nlayout_slots * 2 + nmedia;
    z.parts = calloc((size_t)z.cap, sizeof(*z.parts));
    z.names = calloc((size_t)z.cap, sizeof(*z.names));
    if (layout_number == NULL || media_used == NULL || z.parts == NULL
        || z.names == NULL) {
        free(layout_number);
        free(media_used);
        free(z.parts);
        free(z.names);
        return false;
    }
    /* Which layouts are used, numbered in order. */
    int nlayouts_out = 0;
    for (int i = 0; i < nslides; i++) {
        int slot = (slides[i].deco + 1) * 2 + slide_kind(&slides[i]);
        if (layout_number[slot] == 0) {
            layout_number[slot] = ++nlayouts_out;
        }
    }
    struct buf b = {0};
    char name[96];
    /* Slides. */
    for (int i = 0; i < nslides; i++) {
        struct slide *sl = &slides[i];
        struct part_media pm = {0};
        buf_puts(&b, XML_HEAD "<p:sld" PML_ROOT_NS);
        if (sl->hidden) {
            buf_puts(&b, " show=\"0\"");
        }
        buf_puts(&b, "><p:cSld><p:bg><p:bgPr>");
        put_solid(&b, sl->background);
        buf_puts(&b, "<a:effectLst/></p:bgPr></p:bg>");
        put_tree_start(&b);
        for (int k = 0; k < sl->n; k++) {
            put_shape(&b, &sl->shapes[k], k + 2, &pm);
        }
        buf_puts(&b, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/>"
                     "</p:clrMapOvr></p:sld>");
        snprintf(name, sizeof(name), "ppt/slides/slide%d.xml", i + 1);
        zout_add(&z, name, &b);
        int slot = (sl->deco + 1) * 2 + slide_kind(sl);
        char target[64];
        snprintf(target, sizeof(target), "../slideLayouts/slideLayout%d.xml",
                 layout_number[slot]);
        put_rels_media(&b, "slideLayout", target, &pm);
        snprintf(name, sizeof(name), "ppt/slides/_rels/slide%d.xml.rels", i + 1);
        zout_add(&z, name, &b);
        for (int k = 0; k < pm.n; k++) {
            media_used[pm.media[k]] = true;
        }
        free(pm.media);
    }
    /* Layouts. */
    for (int slot = 0; slot < nlayout_slots; slot++) {
        if (layout_number[slot] == 0) {
            continue;
        }
        int deco = slot / 2 - 1, kind = slot % 2;
        struct part_media pm = {0};
        buf_puts(&b, XML_HEAD "<p:sldLayout" PML_ROOT_NS);
        buf_puts(&b, kind == 0 ? " type=\"title\"" : " type=\"obj\"");
        buf_puts(&b, " preserve=\"1\"><p:cSld name=\"");
        buf_puts(&b, kind == 0 ? "Title Slide" : "Title and Content");
        buf_puts(&b, "\">");
        put_tree_start(&b);
        int id = 2;
        if (kind == 0) {
            put_ph_def(&b, id++, "ctrTitle", 0, sx(1524000), sy(1122363),
                       sx(9144000), sy(2387600), true);
            put_ph_def(&b, id++, "subTitle", 1, sx(1524000), sy(3602038),
                       sx(9144000), sy(1655762), true);
        } else {
            put_ph_def(&b, id++, "title", 0, 0, 0, 0, 0, false);
            put_ph_def(&b, id++, NULL, 1, 0, 0, 0, 0, false);
        }
        if (deco >= 0 && deco < ndecos) {
            for (int k = 0; k < decos[deco].n; k++) {
                put_shape(&b, &decos[deco].shapes[k], id++, &pm);
            }
        }
        buf_puts(&b, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/>"
                     "</p:clrMapOvr></p:sldLayout>");
        snprintf(name, sizeof(name), "ppt/slideLayouts/slideLayout%d.xml",
                 layout_number[slot]);
        zout_add(&z, name, &b);
        put_rels_media(&b, "slideMaster", "../slideMasters/slideMaster1.xml", &pm);
        snprintf(name, sizeof(name),
                 "ppt/slideLayouts/_rels/slideLayout%d.xml.rels",
                 layout_number[slot]);
        zout_add(&z, name, &b);
        for (int k = 0; k < pm.n; k++) {
            media_used[pm.media[k]] = true;
        }
        free(pm.media);
    }
    /* The master. */
    buf_puts(&b, XML_HEAD "<p:sldMaster" PML_ROOT_NS "><p:cSld><p:bg><p:bgRef"
                 " idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg>");
    put_tree_start(&b);
    put_ph_def(&b, 2, "title", 0, sx(838200), sy(365125), sx(10515600),
               sy(1325563), true);
    put_ph_def(&b, 3, "body", 1, sx(838200), sy(1825625), sx(10515600),
               sy(4351338), true);
    buf_puts(&b, "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" "
                 "bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" "
                 "accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" "
                 "accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
                 "<p:sldLayoutIdLst>");
    for (int i = 0; i < nlayouts_out; i++) {
        char line[96];
        snprintf(line, sizeof(line),
                 "<p:sldLayoutId id=\"%u\" r:id=\"rId%d\"/>",
                 2147483649u + (unsigned)i, i + 1);
        buf_puts(&b, line);
    }
    buf_puts(&b, "</p:sldLayoutIdLst><p:txStyles>");
    put_text_style(&b, "titleStyle", 4400, false);
    put_text_style(&b, "bodyStyle", 2800, true);
    put_text_style(&b, "otherStyle", 1800, false);
    buf_puts(&b, "</p:txStyles></p:sldMaster>");
    zout_add(&z, "ppt/slideMasters/slideMaster1.xml", &b);
    buf_puts(&b, XML_HEAD "<Relationships xmlns=\"" NS_PKG_REL "\">");
    for (int i = 0; i < nlayouts_out; i++) {
        char line[200];
        snprintf(line, sizeof(line),
                 "<Relationship Id=\"rId%d\" Type=\"" REL_TYPE "slideLayout\" "
                 "Target=\"../slideLayouts/slideLayout%d.xml\"/>", i + 1, i + 1);
        buf_puts(&b, line);
    }
    {
        char line[200];
        snprintf(line, sizeof(line),
                 "<Relationship Id=\"rId%d\" Type=\"" REL_TYPE "theme\" "
                 "Target=\"../theme/theme1.xml\"/>", nlayouts_out + 1);
        buf_puts(&b, line);
    }
    buf_puts(&b, "</Relationships>");
    zout_add(&z, "ppt/slideMasters/_rels/slideMaster1.xml.rels", &b);
    buf_puts(&b, theme_xml);
    zout_add(&z, "ppt/theme/theme1.xml", &b);
    /* The presentation. */
    buf_puts(&b, XML_HEAD "<p:presentation" PML_ROOT_NS " saveSubsetFonts=\"1\">"
                 "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" "
                 "r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>");
    for (int i = 0; i < nslides; i++) {
        char line[80];
        snprintf(line, sizeof(line), "<p:sldId id=\"%d\" r:id=\"rId%d\"/>",
                 256 + i, i + 2);
        buf_puts(&b, line);
    }
    buf_puts(&b, "</p:sldIdLst><p:sldSz");
    put_attr(&b, "cx", slide_w);
    put_attr(&b, "cy", slide_h);
    buf_puts(&b, "/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
                 "<p:defaultTextStyle><a:lvl1pPr marL=\"0\" algn=\"l\">"
                 "<a:defRPr sz=\"1800\" kern=\"1200\"><a:solidFill>"
                 "<a:schemeClr val=\"tx1\"/></a:solidFill><a:latin "
                 "typeface=\"+mn-lt\"/></a:defRPr></a:lvl1pPr>"
                 "</p:defaultTextStyle></p:presentation>");
    zout_add(&z, "ppt/presentation.xml", &b);
    buf_puts(&b, XML_HEAD "<Relationships xmlns=\"" NS_PKG_REL "\">"
                 "<Relationship Id=\"rId1\" Type=\"" REL_TYPE "slideMaster\" "
                 "Target=\"slideMasters/slideMaster1.xml\"/>");
    for (int i = 0; i < nslides; i++) {
        char line[200];
        snprintf(line, sizeof(line),
                 "<Relationship Id=\"rId%d\" Type=\"" REL_TYPE "slide\" "
                 "Target=\"slides/slide%d.xml\"/>", i + 2, i + 1);
        buf_puts(&b, line);
    }
    {
        static const char *const extra[4] = {"presProps", "viewProps", "theme",
                                             "tableStyles"};
        static const char *const targets[4] = {"presProps.xml", "viewProps.xml",
                                               "theme/theme1.xml",
                                               "tableStyles.xml"};
        for (int i = 0; i < 4; i++) {
            char line[200];
            snprintf(line, sizeof(line),
                     "<Relationship Id=\"rId%d\" Type=\"" REL_TYPE "%s\" "
                     "Target=\"%s\"/>", nslides + 2 + i, extra[i], targets[i]);
            buf_puts(&b, line);
        }
    }
    buf_puts(&b, "</Relationships>");
    zout_add(&z, "ppt/_rels/presentation.xml.rels", &b);
    buf_puts(&b, XML_HEAD "<p:presentationPr" PML_ROOT_NS "/>");
    zout_add(&z, "ppt/presProps.xml", &b);
    buf_puts(&b, XML_HEAD "<p:viewPr" PML_ROOT_NS "><p:normalViewPr>"
                 "<p:restoredLeft sz=\"15620\"/><p:restoredTop sz=\"94660\"/>"
                 "</p:normalViewPr><p:gridSpacing cx=\"76200\" cy=\"76200\"/>"
                 "</p:viewPr>");
    zout_add(&z, "ppt/viewProps.xml", &b);
    buf_puts(&b, XML_HEAD "<a:tblStyleLst xmlns:a=\"" NS_A "\" "
                 "def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>");
    zout_add(&z, "ppt/tableStyles.xml", &b);
    /* Pictures. */
    for (int i = 0; i < nmedia; i++) {
        if (!media_used[i]) {
            continue;
        }
        struct buf mb = {(char *)media[i].data, media[i].size, media[i].size,
                         false};
        snprintf(name, sizeof(name), "ppt/media/image%d.%s", i + 1, media_ext(i));
        if (z.n < z.cap) {
            snprintf(z.names[z.n], 64, "%s", name);
            zip_add(&z.zip, &z.parts[z.n], z.names[z.n], &mb);
            z.n++;
        }
    }
    /* Package parts. */
    buf_puts(&b, XML_HEAD "<cp:coreProperties xmlns:cp=\"http://schemas."
                 "openxmlformats.org/package/2006/metadata/core-properties\" "
                 "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                 "xmlns:dcterms=\"http://purl.org/dc/terms/\" "
                 "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
                 "<dc:creator>OPOS Presentation</dc:creator>"
                 "</cp:coreProperties>");
    zout_add(&z, "docProps/core.xml", &b);
    buf_puts(&b, XML_HEAD "<Properties xmlns=\"http://schemas.openxmlformats.org/"
                 "officeDocument/2006/extended-properties\"><Application>"
                 "OPOS Presentation</Application><Slides>");
    buf_int(&b, nslides);
    buf_puts(&b, "</Slides></Properties>");
    zout_add(&z, "docProps/app.xml", &b);
    buf_puts(&b, XML_HEAD "<Relationships xmlns=\"" NS_PKG_REL "\">"
                 "<Relationship Id=\"rId1\" Type=\"" REL_TYPE "officeDocument\" "
                 "Target=\"ppt/presentation.xml\"/>"
                 "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats"
                 ".org/package/2006/relationships/metadata/core-properties\" "
                 "Target=\"docProps/core.xml\"/>"
                 "<Relationship Id=\"rId3\" Type=\"" REL_TYPE
                 "extended-properties\" Target=\"docProps/app.xml\"/>"
                 "</Relationships>");
    zout_add(&z, "_rels/.rels", &b);
    buf_puts(&b, XML_HEAD "<Types xmlns=\"http://schemas.openxmlformats.org/"
                 "package/2006/content-types\">"
                 "<Default Extension=\"rels\" ContentType=\"application/"
                 "vnd.openxmlformats-package.relationships+xml\"/>"
                 "<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
    {
        /* One Default per picture extension in use. */
        char seen[16][8];
        int nseen = 0;
        for (int i = 0; i < nmedia; i++) {
            if (!media_used[i]) {
                continue;
            }
            const char *ext = media_ext(i);
            bool dup = false;
            for (int k = 0; k < nseen; k++) {
                dup = dup || strcmp(seen[k], ext) == 0;
            }
            if (dup || nseen == 16) {
                continue;
            }
            snprintf(seen[nseen++], 8, "%s", ext);
            const char *type = strcmp(ext, "png") == 0 ? "image/png"
                : strcmp(ext, "jpeg") == 0 ? "image/jpeg"
                : strcmp(ext, "bmp") == 0 ? "image/bmp"
                : strcmp(ext, "gif") == 0 ? "image/gif"
                : strcmp(ext, "emf") == 0 ? "image/x-emf"
                : strcmp(ext, "wmf") == 0 ? "image/x-wmf"
                : strcmp(ext, "svg") == 0 ? "image/svg+xml"
                : strcmp(ext, "tif") == 0 || strcmp(ext, "tiff") == 0
                ? "image/tiff" : "application/octet-stream";
            buf_puts(&b, "<Default Extension=\"");
            buf_puts(&b, ext);
            buf_puts(&b, "\" ContentType=\"");
            buf_puts(&b, type);
            buf_puts(&b, "\"/>");
        }
    }
    buf_puts(&b, "<Override PartName=\"/ppt/presentation.xml\" ContentType=\""
                 CT_PML "presentation.main+xml\"/>"
                 "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" "
                 "ContentType=\"" CT_PML "slideMaster+xml\"/>"
                 "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\""
                 "application/vnd.openxmlformats-officedocument.theme+xml\"/>"
                 "<Override PartName=\"/ppt/presProps.xml\" ContentType=\""
                 CT_PML "presProps+xml\"/>"
                 "<Override PartName=\"/ppt/viewProps.xml\" ContentType=\""
                 CT_PML "viewProps+xml\"/>"
                 "<Override PartName=\"/ppt/tableStyles.xml\" ContentType=\""
                 CT_PML "tableStyles+xml\"/>"
                 "<Override PartName=\"/docProps/core.xml\" ContentType=\""
                 "application/vnd.openxmlformats-package.core-properties+xml\"/>"
                 "<Override PartName=\"/docProps/app.xml\" ContentType=\""
                 "application/vnd.openxmlformats-officedocument."
                 "extended-properties+xml\"/>");
    for (int i = 0; i < nslides; i++) {
        char line[200];
        snprintf(line, sizeof(line),
                 "<Override PartName=\"/ppt/slides/slide%d.xml\" ContentType=\""
                 CT_PML "slide+xml\"/>", i + 1);
        buf_puts(&b, line);
    }
    for (int i = 0; i < nlayouts_out; i++) {
        char line[200];
        snprintf(line, sizeof(line),
                 "<Override PartName=\"/ppt/slideLayouts/slideLayout%d.xml\" "
                 "ContentType=\"" CT_PML "slideLayout+xml\"/>", i + 1);
        buf_puts(&b, line);
    }
    buf_puts(&b, "</Types>");
    zout_add(&z, "[Content_Types].xml", &b);
    zip_finish(&z.zip, z.parts, z.n);
    bool ok = !z.zip.failed && write_whole_file(file, z.zip.data, z.zip.length);
    free(z.zip.data);
    free(z.parts);
    free(z.names);
    free(layout_number);
    free(media_used);
    return ok;
}

#endif
