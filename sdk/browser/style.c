/*
 * Browser styles: what each HTML element looks like without a style sheet,
 * plus the legacy presentation attributes and a small inline-style subset.
 * apply_declaration() is the one place that turns "property: value" into a
 * style change, so a style sheet engine can reuse it.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "browser.h"

/* ---------------------------------------------------------------- colours */

struct named_colour {
    const char *name;
    uint32_t value;
};

static const struct named_colour named_colours[] = {
    {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000},
    {"green", 0x008000}, {"blue", 0x0000ff}, {"yellow", 0xffff00},
    {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xc0c0c0},
    {"maroon", 0x800000}, {"purple", 0x800080}, {"fuchsia", 0xff00ff},
    {"magenta", 0xff00ff}, {"lime", 0x00ff00}, {"olive", 0x808000},
    {"navy", 0x000080}, {"teal", 0x008080}, {"aqua", 0x00ffff},
    {"cyan", 0x00ffff}, {"orange", 0xffa500}, {"brown", 0xa52a2a},
    {"pink", 0xffc0cb}, {"gold", 0xffd700}, {"darkgray", 0xa9a9a9},
    {"darkgrey", 0xa9a9a9}, {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3},
    {"darkblue", 0x00008b}, {"lightblue", 0xadd8e6}, {"darkgreen", 0x006400},
    {"darkred", 0x8b0000}, {"whitesmoke", 0xf5f5f5}, {"ivory", 0xfffff0},
    {"beige", 0xf5f5dc}, {"tan", 0xd2b48c}, {"coral", 0xff7f50},
    {"crimson", 0xdc143c}, {"indigo", 0x4b0082}, {"violet", 0xee82ee},
    {"khaki", 0xf0e68c}, {"salmon", 0xfa8072}, {"skyblue", 0x87ceeb},
    {"steelblue", 0x4682b4}, {"dimgray", 0x696969}, {"dimgrey", 0x696969},
    {"gainsboro", 0xdcdcdc}, {"lightyellow", 0xffffe0},
    {"lightgreen", 0x90ee90}, {"orangered", 0xff4500},
    {"royalblue", 0x4169e1}, {"slategray", 0x708090},
    {"slategrey", 0x708090}, {"snow", 0xfffafa}, {"transparent", 0xffffffff},
};

static int hex_value(int c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static const char *skip_space(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text++;
    }
    return text;
}

/* Reads a number ("12", "-.5", "1e2" is not used by CSS colours). Sets
   *percent when a % follows. Returns the position after it, or NULL. */
static const char *read_number(const char *p, double *value, bool *percent) {
    p = skip_space(p);
    bool negative = false;
    if (*p == '-' || *p == '+') {
        negative = *p == '-';
        p++;
    }
    if (!isdigit((unsigned char)*p) && !(*p == '.'
                                         && isdigit((unsigned char)p[1]))) {
        return NULL;
    }
    double v = 0;
    while (isdigit((unsigned char)*p)) {
        v = v * 10 + (*p++ - '0');
    }
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (isdigit((unsigned char)*p)) {
            v += (*p++ - '0') * scale;
            scale /= 10;
        }
    }
    *percent = *p == '%';
    if (*percent) {
        p++;
    }
    while (isalpha((unsigned char)*p)) {
        p++; /* units such as deg */
    }
    *value = negative ? -v : v;
    return p;
}

static int clamp_byte(double v) {
    return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5);
}

/* Reads the arguments of rgb()/hsl(): three numbers and an optional alpha,
   separated by commas, spaces or "/". */
static bool read_channels(const char *p, double v[4], bool pct[4],
                          int *count) {
    *count = 0;
    p = skip_space(p);
    while (*p != ')' && *p != '\0' && *count < 4) {
        p = read_number(p, &v[*count], &pct[*count]);
        if (p == NULL) {
            return false;
        }
        (*count)++;
        p = skip_space(p);
        if (*p == ',' || *p == '/') {
            p = skip_space(p + 1);
        }
    }
    return *count >= 3;
}

static double hue_to_rgb(double m1, double m2, double h) {
    if (h < 0) {
        h += 1;
    }
    if (h > 1) {
        h -= 1;
    }
    if (h * 6 < 1) {
        return m1 + (m2 - m1) * h * 6;
    }
    if (h * 2 < 1) {
        return m2;
    }
    if (h * 3 < 2) {
        return m1 + (m2 - m1) * (2.0 / 3 - h) * 6;
    }
    return m1;
}

/* The end of a colour argument: the next top-level comma or ")". */
static const char *argument_end(const char *p) {
    int depth = 0;
    while (*p != '\0') {
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            if (depth == 0) {
                return p;
            }
            depth--;
        } else if (*p == ',' && depth == 0) {
            return p;
        }
        p++;
    }
    return p;
}

bool parse_colour_alpha(const char *text, uint32_t *out, int *alpha) {
    text = skip_space(text);
    *alpha = 255;
    if (*text == '#') {
        text++;
        size_t digits = 0;
        while (hex_value((unsigned char)text[digits]) >= 0) {
            digits++;
        }
        if (digits == 3 || digits == 4) {
            int r = hex_value((unsigned char)text[0]);
            int g = hex_value((unsigned char)text[1]);
            int b = hex_value((unsigned char)text[2]);
            *out = OPOS_RGB(r * 17, g * 17, b * 17);
            if (digits == 4) {
                *alpha = hex_value((unsigned char)text[3]) * 17;
            }
            return true;
        }
        if (digits == 6 || digits == 8) {
            uint32_t value = 0;
            for (size_t i = 0; i < 6; i++) {
                value = value << 4 | (uint32_t)hex_value((unsigned char)text[i]);
            }
            *out = value;
            if (digits == 8) {
                *alpha = hex_value((unsigned char)text[6]) * 16
                       + hex_value((unsigned char)text[7]);
            }
            return true;
        }
        return false;
    }
    bool is_rgb = strncmp(text, "rgb(", 4) == 0 || strncmp(text, "rgba(", 5) == 0;
    bool is_hsl = strncmp(text, "hsl(", 4) == 0 || strncmp(text, "hsla(", 5) == 0;
    if (is_rgb || is_hsl) {
        double v[4];
        bool pct[4];
        int count;
        if (!read_channels(strchr(text, '(') + 1, v, pct, &count)) {
            return false;
        }
        if (count == 4) {
            *alpha = clamp_byte(pct[3] ? v[3] * 2.55 : v[3] * 255);
        }
        if (is_rgb) {
            int c[3];
            for (int i = 0; i < 3; i++) {
                c[i] = clamp_byte(pct[i] ? v[i] * 2.55 : v[i]);
            }
            *out = OPOS_RGB(c[0], c[1], c[2]);
        } else {
            double h = v[0] / 360.0;
            h -= (long)h;
            if (h < 0) {
                h += 1;
            }
            double sat = v[1] / 100, light = v[2] / 100;
            double m2 = light <= 0.5 ? light * (sat + 1)
                                     : light + sat - light * sat;
            double m1 = light * 2 - m2;
            *out = OPOS_RGB(clamp_byte(hue_to_rgb(m1, m2, h + 1.0 / 3) * 255),
                            clamp_byte(hue_to_rgb(m1, m2, h) * 255),
                            clamp_byte(hue_to_rgb(m1, m2, h - 1.0 / 3) * 255));
        }
        return true;
    }
    if (strncmp(text, "color-mix(", 10) == 0) {
        /* color-mix(in <space>, A [p%], B [q%]): mixed in sRGB. */
        const char *p = argument_end(text + 10);
        if (*p != ',') {
            return false;
        }
        uint32_t colour[2];
        int alphas[2];
        double share[2] = {-1, -1};
        for (int i = 0; i < 2; i++) {
            const char *start = skip_space(p + 1);
            p = argument_end(start);
            char piece[160];
            size_t n = (size_t)(p - start);
            if (n >= sizeof(piece)) {
                return false;
            }
            memcpy(piece, start, n);
            piece[n] = '\0';
            /* An optional percentage after the colour. */
            char *last = strrchr(piece, ' ');
            if (last != NULL && strchr(last, '%') != NULL) {
                share[i] = atoi(last + 1) / 100.0;
                *last = '\0';
            }
            if (!parse_colour_alpha(piece, &colour[i], &alphas[i])) {
                return false;
            }
            if (i == 0 && *p != ',') {
                return false;
            }
        }
        if (share[0] < 0 && share[1] < 0) {
            share[0] = share[1] = 0.5;
        } else if (share[0] < 0) {
            share[0] = 1 - share[1];
        } else if (share[1] < 0) {
            share[1] = 1 - share[0];
        }
        double total = share[0] + share[1];
        if (total <= 0) {
            return false;
        }
        double a = share[0] / total;
        int r = clamp_byte(((colour[0] >> 16) & 255) * a
                           + ((colour[1] >> 16) & 255) * (1 - a));
        int g = clamp_byte(((colour[0] >> 8) & 255) * a
                           + ((colour[1] >> 8) & 255) * (1 - a));
        int b = clamp_byte((colour[0] & 255) * a + (colour[1] & 255) * (1 - a));
        *out = OPOS_RGB(r, g, b);
        *alpha = clamp_byte(alphas[0] * a + alphas[1] * (1 - a));
        return true;
    }
    size_t length = 0;
    while (isalpha((unsigned char)text[length])) {
        length++;
    }
    if (length == 0) {
        /* Old pages write bare hex like bgcolor=ffffff. */
        size_t digits = 0;
        while (hex_value((unsigned char)text[digits]) >= 0) {
            digits++;
        }
        if (digits == 6) {
            uint32_t value = 0;
            for (size_t i = 0; i < 6; i++) {
                value = value << 4 | (uint32_t)hex_value((unsigned char)text[i]);
            }
            *out = value;
            return true;
        }
        return false;
    }
    for (size_t i = 0; i < sizeof(named_colours) / sizeof(named_colours[0]);
         i++) {
        const char *name = named_colours[i].name;
        if (strlen(name) != length) {
            continue;
        }
        size_t k = 0;
        while (k < length && tolower((unsigned char)text[k]) == name[k]) {
            k++;
        }
        if (k == length) {
            if (named_colours[i].value == 0xffffffffu) {
                *out = 0;
                *alpha = 0; /* transparent */
                return true;
            }
            *out = named_colours[i].value;
            return true;
        }
    }
    return false;
}

bool parse_colour(const char *text, uint32_t *out) {
    int alpha;
    uint32_t colour;
    if (!parse_colour_alpha(text, &colour, &alpha) || alpha == 0) {
        return false; /* transparent: no colour */
    }
    *out = colour;
    return true;
}

int viewport_width_px = 800;
int viewport_height_px = 600;

/* ---- calc(), min(), max(), clamp(): values are px + % of the container. */

struct calc_value {
    double px, pct;
    bool number; /* unitless */
    double cap;  /* min() of px and %: the px limit, else -1 */
};

static bool calc_sum(const char **p, struct calc_value *out, int depth);

static bool calc_unit(const char **p, double number, struct calc_value *out) {
    const char *t = *p;
    out->px = out->pct = 0;
    out->number = false;
    out->cap = -1;
    double w = viewport_width_px, h = viewport_height_px;
    if (*t == '%') {
        out->pct = number;
        t++;
    } else if (strncmp(t, "px", 2) == 0) {
        out->px = number;
        t += 2;
    } else if (strncmp(t, "rem", 3) == 0) {
        out->px = number * 16;
        t += 3;
    } else if (strncmp(t, "em", 2) == 0) {
        out->px = number * 16;
        t += 2;
    } else if (strncmp(t, "pt", 2) == 0) {
        out->px = number * 4 / 3;
        t += 2;
    } else if (strncmp(t, "ch", 2) == 0 || strncmp(t, "ex", 2) == 0) {
        out->px = number * 8;
        t += 2;
    } else if (strncmp(t, "vmin", 4) == 0) {
        out->px = number * (w < h ? w : h) / 100;
        t += 4;
    } else if (strncmp(t, "vmax", 4) == 0) {
        out->px = number * (w > h ? w : h) / 100;
        t += 4;
    } else if (strncmp(t, "vw", 2) == 0 || strncmp(t, "dvw", 3) == 0
               || strncmp(t, "svw", 3) == 0 || strncmp(t, "lvw", 3) == 0) {
        out->px = number * w / 100;
        t += t[1] == 'w' ? 2 : 3;
    } else if (strncmp(t, "vh", 2) == 0 || strncmp(t, "dvh", 3) == 0
               || strncmp(t, "svh", 3) == 0 || strncmp(t, "lvh", 3) == 0) {
        out->px = number * h / 100;
        t += t[1] == 'h' ? 2 : 3;
    } else if (isalpha((unsigned char)*t)) {
        return false;
    } else {
        out->px = number;
        out->number = true;
    }
    *p = t;
    return true;
}

static bool calc_factor(const char **p, struct calc_value *out, int depth) {
    const char *t = skip_space(*p);
    if (depth > 8) {
        return false;
    }
    bool negative = false;
    if (*t == '-' && !isdigit((unsigned char)t[1]) && t[1] != '.') {
        negative = true; /* -(...) or -min(...) */
        t++;
    }
    if (isdigit((unsigned char)*t) || *t == '.'
        || ((*t == '-' || *t == '+')
            && (isdigit((unsigned char)t[1]) || t[1] == '.'))) {
        double sign = 1, number = 0;
        if (*t == '-' || *t == '+') {
            sign = *t == '-' ? -1 : 1;
            t++;
        }
        while (isdigit((unsigned char)*t)) {
            number = number * 10 + (*t++ - '0');
        }
        if (*t == '.') {
            t++;
            double scale = 0.1;
            while (isdigit((unsigned char)*t)) {
                number += (*t++ - '0') * scale;
                scale /= 10;
            }
        }
        if (!calc_unit(&t, number * sign, out)) {
            return false;
        }
    } else if (*t == '(' || strncmp(t, "calc(", 5) == 0) {
        t = strchr(t, '(') + 1;
        if (!calc_sum(&t, out, depth + 1)) {
            return false;
        }
        t = skip_space(t);
        if (*t != ')') {
            return false;
        }
        t++;
    } else if (strncmp(t, "min(", 4) == 0 || strncmp(t, "max(", 4) == 0
               || strncmp(t, "clamp(", 6) == 0) {
        char kind = t[1] == 'i' ? 'n' : t[1] == 'a' ? 'x' : 'c';
        t = strchr(t, '(') + 1;
        struct calc_value args[8];
        int count = 0;
        for (;;) {
            if (count == 8 || !calc_sum(&t, &args[count], depth + 1)) {
                return false;
            }
            count++;
            t = skip_space(t);
            if (*t == ',') {
                t++;
                continue;
            }
            if (*t != ')') {
                return false;
            }
            t++;
            break;
        }
        if (kind == 'c') {
            if (count != 3) {
                return false;
            }
            /* clamp(lo, x, hi) = max(lo, min(x, hi)): with mixed units
               use the preferred value, capped by hi when hi is in px. */
            *out = args[1];
            if (args[1].pct == 0 && args[0].pct == 0 && args[2].pct == 0) {
                double v = args[1].px;
                v = v > args[2].px ? args[2].px : v;
                v = v < args[0].px ? args[0].px : v;
                out->px = v;
            } else if (args[2].pct == 0 && args[1].pct != 0) {
                out->cap = args[2].px;
            }
        } else {
            *out = args[0];
            for (int i = 1; i < count; i++) {
                bool pure_px = out->pct == 0 && args[i].pct == 0;
                bool pure_pct = out->px == 0 && args[i].px == 0;
                if (pure_px) {
                    bool smaller = args[i].px < out->px;
                    if (smaller == (kind == 'n')) {
                        out->px = args[i].px;
                    }
                } else if (pure_pct) {
                    bool smaller = args[i].pct < out->pct;
                    if (smaller == (kind == 'n')) {
                        out->pct = args[i].pct;
                    }
                } else if (kind == 'n') {
                    /* min(100%, 80rem): the percentage, at most the px. */
                    const struct calc_value *px = out->pct == 0 ? out : &args[i];
                    const struct calc_value *pc = out->pct == 0 ? &args[i] : out;
                    double cap = px->px;
                    *out = *pc;
                    out->cap = cap;
                } else if (out->pct == 0) {
                    *out = args[i]; /* max(px, %): the percentage */
                }
            }
        }
    } else {
        return false; /* var() left over, env(), keywords */
    }
    if (negative) {
        out->px = -out->px;
        out->pct = -out->pct;
    }
    *p = t;
    return true;
}

static bool calc_product(const char **p, struct calc_value *out, int depth) {
    if (!calc_factor(p, out, depth)) {
        return false;
    }
    for (;;) {
        const char *t = skip_space(*p);
        if (*t != '*' && *t != '/') {
            return true;
        }
        char op = *t++;
        struct calc_value right;
        if (!calc_factor(&t, &right, depth)) {
            return false;
        }
        if (op == '*') {
            if (right.number) {
                out->px *= right.px;
                out->pct *= right.px;
            } else if (out->number) {
                double k = out->px;
                *out = right;
                out->px *= k;
                out->pct *= k;
            } else {
                return false;
            }
        } else {
            if (!right.number || right.px == 0) {
                return false;
            }
            out->px /= right.px;
            out->pct /= right.px;
        }
        out->cap = -1;
        *p = t;
    }
}

static bool calc_sum(const char **p, struct calc_value *out, int depth) {
    if (!calc_product(p, out, depth)) {
        return false;
    }
    for (;;) {
        const char *t = skip_space(*p);
        /* CSS wants spaces round + and -; "-" glued to a number is a sign. */
        if ((*t != '+' && *t != '-') || !(t[1] == ' ' || t[1] == '\t')) {
            return true;
        }
        char op = *t++;
        struct calc_value right;
        if (!calc_product(&t, &right, depth)) {
            return false;
        }
        double k = op == '+' ? 1 : -1;
        out->px += k * right.px;
        out->pct += k * right.pct;
        out->number = out->number && right.number;
        out->cap = -1;
        *p = t;
    }
}

int last_length_cap = -1;

/* calc()/min()/max()/clamp(). The result is pixels, or a percentage when a
   percentage is involved (a px part next to it is dropped, so calc(100% -
   2rem) is 100%). last_length_cap gets the px limit of min(%, px). */
static bool parse_math(const char *text, int *value, bool *percent) {
    const char *p = text;
    struct calc_value v;
    if (!calc_factor(&p, &v, 0)) {
        return false;
    }
    if (v.pct != 0) {
        *percent = true;
        *value = (int)(v.pct + (v.pct < 0 ? -0.5 : 0.5));
        last_length_cap = v.cap >= 0 ? (int)(v.cap + 0.5) : -1;
    } else {
        *percent = false;
        *value = (int)(v.px + (v.px < 0 ? -0.5 : 0.5));
    }
    return true;
}

bool parse_length(const char *text, int *value, bool *percent) {
    text = skip_space(text);
    last_length_cap = -1;
    if (strncmp(text, "calc(", 5) == 0 || strncmp(text, "min(", 4) == 0
        || strncmp(text, "max(", 4) == 0 || strncmp(text, "clamp(", 6) == 0) {
        return parse_math(text, value, percent);
    }
    bool negative = false;
    if (*text == '-' || *text == '+') {
        negative = *text == '-';
        text++;
    }
    if (!isdigit((unsigned char)*text) && *text != '.') {
        return false;
    }
    long whole = 0;
    while (isdigit((unsigned char)*text)) {
        whole = whole * 10 + (*text++ - '0');
        if (whole > 100000) {
            whole = 100000;
        }
    }
    long fraction = 0; /* thousandths */
    if (*text == '.') {
        text++;
        long scale = 100;
        while (isdigit((unsigned char)*text)) {
            fraction += (*text++ - '0') * scale;
            scale /= 10;
        }
    }
    *percent = *text == '%';
    long result = whole;
    if (strncmp(text, "em", 2) == 0 || strncmp(text, "rem", 3) == 0) {
        /* Callers that need em convert; here an em is 16 pixels. */
        result = (whole * 1000 + fraction) * 16 / 1000;
    } else if (strncmp(text, "pt", 2) == 0) {
        result = (whole * 1000 + fraction) * 4 / 3000;
    } else if (text[0] == 'v' && (text[1] == 'w' || text[1] == 'h'
                                  || strncmp(text + 1, "min", 3) == 0
                                  || strncmp(text + 1, "max", 3) == 0)) {
        /* Viewport units: a hundredth of the viewport width or height. */
        long w = viewport_width_px, h = viewport_height_px;
        long side = text[1] == 'w' ? w : text[1] == 'h' ? h
                  : text[2] == 'i' ? (w < h ? w : h) : (w > h ? w : h);
        result = (whole * 1000 + fraction) * side / 100000;
    } else if (fraction >= 500) {
        result = whole + 1;
    }
    *value = (int)(negative ? -result : result);
    return true;
}

/* ------------------------------------------------------------ declarations */

static bool word_is(const char *text, size_t length, const char *word) {
    if (strlen(word) != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)text[i]) != word[i]) {
            return false;
        }
    }
    return true;
}

/* Reads a font size: px, pt, em, rem, %, or a keyword. */
static bool parse_font_size(const char *value, int parent_size, int *size) {
    static const struct {
        const char *name;
        int size;
    } keywords[] = {
        {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16},
        {"large", 18}, {"x-large", 24}, {"xx-large", 32},
    };
    value = skip_space(value);
    size_t length = 0;
    while (isalpha((unsigned char)value[length]) || value[length] == '-') {
        length++;
    }
    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (word_is(value, length, keywords[i].name)) {
            *size = keywords[i].size;
            return true;
        }
    }
    if (word_is(value, length, "smaller")) {
        *size = parent_size * 5 / 6;
        return true;
    }
    if (word_is(value, length, "larger")) {
        *size = parent_size * 6 / 5;
        return true;
    }
    /* number with unit: em and % are relative to the parent. */
    int number;
    bool percent;
    if (!parse_length(value, &number, &percent)) {
        return false;
    }
    const char *unit = value;
    while (isdigit((unsigned char)*unit) || *unit == '.') {
        unit++;
    }
    if (percent) {
        *size = parent_size * number / 100;
    } else if (strcmp(unit, "em") == 0) {
        *size = parent_size * number / 16;
    } else {
        *size = number;
    }
    if (*size < 6) {
        *size = 6;
    }
    if (*size > 200) {
        *size = 200;
    }
    return true;
}

/* Reads up to four lengths into sides (CSS margin/padding shorthand). */
/* flex-grow as a weight in hundredths ("1" -> 100, "0.5" -> 50). */
static int flex_weight(const char *text) {
    int whole = 0, hundredths = 0;
    while (isdigit((unsigned char)*text)) {
        whole = whole * 10 + (*text++ - '0');
        if (whole > 1000) {
            whole = 1000;
        }
    }
    if (*text == '.') {
        text++;
        if (isdigit((unsigned char)*text)) {
            hundredths = (*text++ - '0') * 10;
            if (isdigit((unsigned char)*text)) {
                hundredths += *text - '0';
            }
        }
    }
    return whole * 100 + hundredths;
}

static void parse_box(const char *value, int font_size, int sides[4]) {
    int values[4];
    int count = 0;
    while (count < 4) {
        value = skip_space(value);
        if (*value == '\0' || *value == '!') {
            break;
        }
        int number = 0;
        bool percent = false;
        const char *start = value;
        if (parse_length(value, &number, &percent)) {
            /* em is relative to the element's font size. */
            const char *unit = value;
            while (isdigit((unsigned char)*unit) || *unit == '.'
                   || *unit == '-' || *unit == '+') {
                unit++;
            }
            if (strncmp(unit, "em", 2) == 0 && !isalpha((unsigned char)unit[2])) {
                number = number * font_size / 16;
            }
            if (percent) {
                number = 0;
            }
        } else if (strncmp(start, "auto", 4) == 0) {
            number = 0;
        } else {
            break;
        }
        values[count++] = number;
        while (*value != '\0' && *value != ' ' && *value != '\t') {
            value++;
        }
    }
    if (count == 0) {
        return;
    }
    sides[TOP] = values[0];
    sides[RIGHT] = count > 1 ? values[1] : values[0];
    sides[BOTTOM] = count > 2 ? values[2] : values[0];
    sides[LEFT] = count > 3 ? values[3] : count > 1 ? values[1] : values[0];
}

/* grid-template-columns: px, %, fr, auto, minmax(), repeat(). */
static void grid_add_track(struct style *style, const char *token,
                           size_t length, int font_size) {
    if (style->grid_count >= 12) {
        return;
    }
    char text[48];
    if (length >= sizeof(text)) {
        length = sizeof(text) - 1;
    }
    memcpy(text, token, length);
    text[length] = '\0';
    if (strncmp(text, "minmax(", 7) == 0) {
        char *comma = strchr(text, ',');
        if (comma != NULL) {
            char *second = comma + 1;
            while (*second == ' ') {
                second++;
            }
            char *close = strchr(second, ')');
            if (close != NULL) {
                *close = '\0';
            }
            memmove(text, second, strlen(second) + 1);
        }
    }
    int number;
    bool percent;
    int index = style->grid_count++;
    size_t n = strlen(text);
    if (n > 2 && strcmp(text + n - 2, "fr") == 0) {
        style->grid_kind[index] = 'f';
        style->grid_size[index] = atoi(text) > 0 ? atoi(text) * 100 : 100;
    } else if (parse_length(text, &number, &percent)) {
        if (strncmp(text + strspn(text, "0123456789.-"), "em", 2) == 0) {
            number = number * font_size / 16;
        }
        style->grid_kind[index] = percent ? 'c' : 'p';
        style->grid_size[index] = number;
    } else {
        style->grid_kind[index] = 'a';
        style->grid_size[index] = 0;
    }
}

static void parse_grid_columns(struct style *style, const char *text) {
    style->grid_count = 0;
    style->grid_fill_min = 0;
    const char *p = text;
    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        const char *start = p;
        int paren = 0;
        while (*p != '\0' && (paren > 0 || *p != ' ')) {
            paren += *p == '(';
            paren -= *p == ')' && paren > 0;
            p++;
        }
        size_t length = (size_t)(p - start);
        if (length == 0) {
            break;
        }
        if (length > 7 && strncmp(start, "repeat(", 7) == 0) {
            const char *comma = memchr(start, ',', length);
            if (comma == NULL) {
                continue;
            }
            const char *inner = comma + 1;
            while (*inner == ' ') {
                inner++;
            }
            size_t inner_length = (size_t)(start + length - 1 - inner);
            if (strncmp(start + 7, "auto-fill", 9) == 0
                || strncmp(start + 7, "auto-fit", 8) == 0) {
                /* minmax(N, ...) or N: the minimum width of a column. */
                const char *arg = inner;
                if (strncmp(arg, "minmax(", 7) == 0) {
                    arg += 7;
                }
                int number;
                bool percent;
                if (parse_length(arg, &number, &percent) && !percent) {
                    if (strncmp(arg + strspn(arg, "0123456789.-"), "em", 2)
                        == 0) {
                        number = number * style->font_size / 16;
                    }
                    style->grid_fill_min = number > 20 ? number : 20;
                }
                style->grid_count = 1;
                style->grid_kind[0] = 'f';
                style->grid_size[0] = 100;
                continue;
            }
            int times = atoi(start + 7);
            if (times < 1 || times > 12) {
                times = 1;
            }
            int first = style->grid_count;
            const char *q = inner;
            const char *end = inner + inner_length;
            while (q < end) {
                while (q < end && *q == ' ') {
                    q++;
                }
                const char *s = q;
                int depth = 0;
                while (q < end && (depth > 0 || *q != ' ')) {
                    depth += *q == '(';
                    depth -= *q == ')' && depth > 0;
                    q++;
                }
                if (q > s) {
                    grid_add_track(style, s, (size_t)(q - s), style->font_size);
                }
            }
            int made = style->grid_count - first;
            for (int t = 1; t < times && made > 0; t++) {
                for (int k = 0; k < made && style->grid_count < 12; k++) {
                    style->grid_kind[style->grid_count] =
                        style->grid_kind[first + k];
                    style->grid_size[style->grid_count++] =
                        style->grid_size[first + k];
                }
            }
        } else if (start[0] != '[') {
            grid_add_track(style, start, length, style->font_size);
        }
    }
}

void apply_declaration(struct style *style, const char *name,
                       size_t name_length, const char *value,
                       const struct style *parent) {
    value = skip_space(value);
    size_t value_length = strcspn(value, ";!");
    while (value_length > 0 && (value[value_length - 1] == ' ')) {
        value_length--;
    }
    char text[96];
    if (value_length >= sizeof(text)) {
        value_length = sizeof(text) - 1;
    }
    memcpy(text, value, value_length);
    text[value_length] = '\0';

    /* margin-inline, padding-block-start, ...: map to physical sides. */
    for (int box = 0; box < 2; box++) {
        const char *prefix = box == 0 ? "margin-" : "padding-";
        size_t plen = strlen(prefix);
        if (name_length <= plen || strncmp(name, prefix, plen) != 0) {
            continue;
        }
        const char *rest = name + plen;
        size_t rlen = name_length - plen;
        bool inline_axis = rlen >= 6 && strncmp(rest, "inline", 6) == 0;
        bool block_axis = rlen >= 5 && strncmp(rest, "block", 5) == 0;
        if (!inline_axis && !block_axis) {
            break;
        }
        const char *edge = rest + (inline_axis ? 6 : 5);
        size_t elen = rlen - (inline_axis ? 6 : 5);
        const char *first = inline_axis ? "left" : "top";
        const char *second = inline_axis ? "right" : "bottom";
        char physical[24];
        if (elen == 0) {
            /* One or two values: start [end]. */
            char a[96], b[96];
            const char *space = strchr(text, ' ');
            if (space == NULL) {
                snprintf(a, sizeof(a), "%s", text);
                snprintf(b, sizeof(b), "%s", text);
            } else {
                snprintf(a, sizeof(a), "%.*s", (int)(space - text), text);
                snprintf(b, sizeof(b), "%s", skip_space(space));
            }
            snprintf(physical, sizeof(physical), "%s%s", prefix, first);
            apply_declaration(style, physical, strlen(physical), a, parent);
            snprintf(physical, sizeof(physical), "%s%s", prefix, second);
            apply_declaration(style, physical, strlen(physical), b, parent);
            return;
        }
        if (word_is(edge, elen, "-start") || word_is(edge, elen, "-end")) {
            snprintf(physical, sizeof(physical), "%s%s", prefix,
                     edge[1] == 's' ? first : second);
            apply_declaration(style, physical, strlen(physical), text, parent);
            return;
        }
        break;
    }

    if (word_is(name, name_length, "display")) {
        /* A later display value replaces an earlier flex or grid. */
        bool known = word_is(text, strlen(text), "none")
                  || word_is(text, strlen(text), "block")
                  || word_is(text, strlen(text), "inline")
                  || word_is(text, strlen(text), "inline-block")
                  || word_is(text, strlen(text), "list-item")
                  || word_is(text, strlen(text), "flow-root")
                  || word_is(text, strlen(text), "flex")
                  || word_is(text, strlen(text), "inline-flex")
                  || word_is(text, strlen(text), "grid")
                  || word_is(text, strlen(text), "inline-grid")
                  || word_is(text, strlen(text), "contents");
        if (known) {
            style->flex = false;
            style->grid = word_is(text, strlen(text), "grid")
                       || word_is(text, strlen(text), "inline-grid");
        }
        if (word_is(text, strlen(text), "none")) {
            style->display = DISPLAY_NONE;
        } else if (word_is(text, strlen(text), "flex")
                   || word_is(text, strlen(text), "inline-flex")) {
            style->display = DISPLAY_BLOCK;
            style->flex = true;
        } else if (word_is(text, strlen(text), "block")
                   || word_is(text, strlen(text), "flex")
                   || word_is(text, strlen(text), "grid")
                   || word_is(text, strlen(text), "inline-grid")
                   || word_is(text, strlen(text), "flow-root")) {
            style->display = DISPLAY_BLOCK;
        } else if (word_is(text, strlen(text), "inline")) {
            style->display = DISPLAY_INLINE;
        } else if (word_is(text, strlen(text), "inline-block")) {
            style->display = style->display == DISPLAY_INLINE_BLOCK
                           ? DISPLAY_INLINE_BLOCK : DISPLAY_INLINE;
        } else if (word_is(text, strlen(text), "list-item")) {
            style->display = DISPLAY_LIST_ITEM;
        } else if (word_is(text, strlen(text), "contents")) {
            style->display = DISPLAY_CONTENTS;
        }
    } else if (word_is(name, name_length, "color")) {
        uint32_t colour;
        if (parse_colour(text, &colour)) {
            style->colour = colour;
        }
    } else if (word_is(name, name_length, "background-color")
               || word_is(name, name_length, "background")) {
        uint32_t colour;
        /* "background: #fff url(x) no-repeat": use the first colour. */
        char *token = text;
        while (*token != '\0') {
            char *end = token;
            while (*end != '\0' && *end != ' ') {
                end++;
            }
            char saved = *end;
            *end = '\0';
            int alpha;
            if (strncmp(token, "url(", 4) != 0
                && parse_colour_alpha(token, &colour, &alpha)) {
                /* "transparent" (or any fully transparent colour) removes
                   a background set by an earlier rule. */
                style->has_background = alpha > 0;
                style->background = colour;
                style->background_alpha = (uint8_t)alpha;
                break;
            }
            if (word_is(token, strlen(token), "none")) {
                style->has_background = false;
            }
            *end = saved;
            token = end;
            while (*token == ' ') {
                token++;
            }
        }
    } else if (word_is(name, name_length, "font-weight")) {
        int weight = atoi(text);
        style->bold = word_is(text, strlen(text), "bold")
                   || word_is(text, strlen(text), "bolder") || weight >= 600;
    } else if (word_is(name, name_length, "font-style")) {
        style->italic = word_is(text, strlen(text), "italic")
                     || word_is(text, strlen(text), "oblique");
    } else if (word_is(name, name_length, "font-size")) {
        int size;
        if (parse_font_size(text, parent->font_size, &size)) {
            style->font_size = size;
        }
    } else if (word_is(name, name_length, "font-family")) {
        style->mono = strstr(text, "mono") != NULL
                   || strstr(text, "Mono") != NULL
                   || strstr(text, "courier") != NULL
                   || strstr(text, "Courier") != NULL;
    } else if (word_is(name, name_length, "text-align")) {
        if (word_is(text, strlen(text), "center")) {
            style->align = ALIGN_CENTER;
        } else if (word_is(text, strlen(text), "right")) {
            style->align = ALIGN_RIGHT;
        } else if (word_is(text, strlen(text), "left")) {
            style->align = ALIGN_LEFT;
        }
    } else if (word_is(name, name_length, "text-decoration")) {
        style->underline = strstr(text, "underline") != NULL;
        style->strike = strstr(text, "line-through") != NULL;
    } else if (word_is(name, name_length, "white-space")) {
        style->pre = word_is(text, strlen(text), "pre")
                  || word_is(text, strlen(text), "pre-wrap")
                  || word_is(text, strlen(text), "pre-line");
    } else if (word_is(name, name_length, "width")
               || word_is(name, name_length, "max-width")) {
        int number;
        bool percent;
        bool is_max = name_length > 5;
        if (parse_length(text, &number, &percent) && number >= 0) {
            if (is_max) {
                style->max_width = percent ? -1 : number;
                style->max_width_pct = percent ? number : -1;
            } else {
                style->width = percent ? -1 : number;
                style->width_pct = percent ? number : -1;
                if (percent && last_length_cap >= 0) {
                    style->max_width = last_length_cap; /* min(100%, 80rem) */
                }
            }
        } else if (word_is(text, strlen(text), "auto")
                   || word_is(text, strlen(text), "none")) {
            if (is_max) {
                style->max_width = style->max_width_pct = -1;
            } else {
                style->width = style->width_pct = -1;
            }
        }
    } else if (word_is(name, name_length, "float")) {
        style->float_side = word_is(text, strlen(text), "left") ? 1
                          : word_is(text, strlen(text), "right") ? 2 : 0;
    } else if (word_is(name, name_length, "clear")) {
        style->clear_both = !word_is(text, strlen(text), "none");
    } else if (word_is(name, name_length, "flex-direction")) {
        style->flex_column = strncmp(text, "column", 6) == 0;
    } else if (word_is(name, name_length, "flex-wrap")) {
        style->flex_wrap = strncmp(text, "wrap", 4) == 0;
    } else if (word_is(name, name_length, "flex-grow")) {
        style->flex_grow = flex_weight(text);
    } else if (word_is(name, name_length, "flex-shrink")) {
        style->no_shrink = text[0] == '0' && text[1] != '.';
    } else if (word_is(name, name_length, "flex-basis")) {
        style->flex_zero = text[0] == '0' && !isdigit((unsigned char)text[1])
                        && text[1] != '.';
    } else if (word_is(name, name_length, "flex")) {
        /* none | auto | <grow> [<shrink>] [<basis>]; "flex: 1" means a
           zero basis, "auto" a content basis. */
        if (word_is(text, strlen(text), "none")
            || word_is(text, strlen(text), "initial")) {
            style->flex_grow = 0;
            style->flex_zero = false;
            style->no_shrink = word_is(text, strlen(text), "none");
        } else if (word_is(text, strlen(text), "auto")) {
            style->flex_grow = 100;
            style->flex_zero = false;
        } else if (isdigit((unsigned char)text[0]) || text[0] == '.') {
            style->flex_grow = flex_weight(text);
            const char *shrink = strchr(text, ' ');
            style->no_shrink = shrink != NULL && shrink[1] == '0'
                            && shrink[2] != '.' && shrink[2] != '%'
                            && !isalpha((unsigned char)shrink[2]);
            const char *basis = strrchr(text, ' ');
            style->flex_zero = basis == NULL
                            || (basis[1] == '0' && !isdigit((unsigned char)basis[2])
                                && basis[2] != '.');
        }
    } else if (word_is(name, name_length, "gap")
               || word_is(name, name_length, "column-gap")
               || word_is(name, name_length, "grid-column-gap")
               || word_is(name, name_length, "grid-gap")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->gap = side[word_is(name, name_length, "gap")
                          || word_is(name, name_length, "grid-gap") ? 1 : 0];
        if (style->gap == 0) {
            style->gap = side[0];
        }
    } else if (word_is(name, name_length, "grid-column")) {
        /* "span N", "A", "A / B", "A / span N", "1 / -1". */
        style->grid_start = 0;
        style->grid_span = 1;
        char *slash = strchr(text, '/');
        char *second = slash != NULL ? slash + 1 : NULL;
        if (slash != NULL) {
            *slash = '\0';
        }
        const char *first = skip_space(text);
        if (strncmp(first, "span", 4) == 0) {
            style->grid_span = atoi(first + 4);
        } else if (isdigit((unsigned char)*first)) {
            style->grid_start = atoi(first);
        }
        if (second != NULL) {
            second = (char *)skip_space(second);
            if (strncmp(second, "span", 4) == 0) {
                style->grid_span = atoi(second + 4);
            } else if (*second == '-' && second[1] == '1') {
                style->grid_span = -1;
            } else if (isdigit((unsigned char)*second)
                       && style->grid_start > 0) {
                style->grid_span = atoi(second) - style->grid_start;
            }
        }
        if (style->grid_span == 0) {
            style->grid_span = 1;
        }
    } else if (word_is(name, name_length, "grid-template-columns")) {
        parse_grid_columns(style, text);
    } else if (word_is(name, name_length, "justify-content")) {
        style->justify = strstr(text, "center") ? 1
                       : strstr(text, "end") ? 2
                       : strstr(text, "between") ? 3 : 0;
    } else if (word_is(name, name_length, "position")) {
        style->out_of_flow = word_is(text, strlen(text), "absolute")
                          || word_is(text, strlen(text), "fixed");
    } else if (word_is(name, name_length, "opacity")) {
        /* Only exactly zero ("0", "0.0", ".0", "0%") counts. */
        const char *t = text;
        bool zero = *t != '\0';
        for (; *t != '\0' && *t != ' ' && *t != '!'; t++) {
            if (*t != '0' && *t != '.' && *t != '%') {
                zero = false;
            }
        }
        style->opacity_zero = zero;
    } else if (word_is(name, name_length, "grid-area")
               || word_is(name, name_length, "grid-row")) {
        style->grid_area_set = !word_is(text, strlen(text), "auto");
    } else if (word_is(name, name_length, "visibility")) {
        style->out_of_flow = word_is(text, strlen(text), "hidden")
                          || word_is(text, strlen(text), "collapse");
    } else if (word_is(name, name_length, "list-style")
               || word_is(name, name_length, "list-style-type")) {
        style->no_marker = strstr(text, "none") != NULL;
    } else if (word_is(name, name_length, "border")) {
        int number;
        bool percent;
        style->border = 0;
        if (strstr(text, "none") == NULL && strstr(text, "hidden") == NULL) {
            style->border = 1;
            for (char *token = text; *token != '\0';) {
                char *end = token;
                int paren = 0;
                while (*end != '\0' && (paren > 0 || *end != ' ')) {
                    paren += *end == '(';
                    paren -= *end == ')' && paren > 0;
                    end++;
                }
                char saved = *end;
                *end = '\0';
                uint32_t colour;
                if (isdigit((unsigned char)token[0])
                    && parse_length(token, &number, &percent)) {
                    style->border = number > 0 ? number : 0;
                } else if (parse_colour(token, &colour)) {
                    style->border_colour = colour;
                }
                *end = saved;
                token = end;
                while (*token == ' ') {
                    token++;
                }
            }
        }
    } else if (word_is(name, name_length, "border-color")) {
        uint32_t colour;
        if (parse_colour(text, &colour)) {
            style->border_colour = colour;
        }
    } else if (word_is(name, name_length, "border-width")) {
        int number;
        bool percent;
        if (parse_length(text, &number, &percent)) {
            style->border = number;
        }
    } else if (word_is(name, name_length, "margin")) {
        parse_box(text, style->font_size, style->margin);
        {
            char *tok[4];
            int n = 0;
            char copy[96];
            snprintf(copy, sizeof(copy), "%s", text);
            for (char *t = copy; *t != '\0' && n < 4;) {
                while (*t == ' ') {
                    t++;
                }
                if (*t == '\0') {
                    break;
                }
                tok[n++] = t;
                while (*t != '\0' && *t != ' ') {
                    t++;
                }
                if (*t != '\0') {
                    *t++ = '\0';
                }
            }
            bool right = n >= 2 ? strcmp(tok[1], "auto") == 0
                       : n == 1 && strcmp(tok[0], "auto") == 0;
            bool left = n == 4 ? strcmp(tok[3], "auto") == 0 : right;
            style->margin_auto_left = left;
            style->margin_auto_right = right;
        }
    } else if (word_is(name, name_length, "margin-top")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->margin[TOP] = side[0];
    } else if (word_is(name, name_length, "margin-bottom")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->margin[BOTTOM] = side[0];
    } else if (word_is(name, name_length, "margin-left")) {
        style->margin_auto_left = strncmp(text, "auto", 4) == 0;
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->margin[LEFT] = side[0];
    } else if (word_is(name, name_length, "margin-right")) {
        style->margin_auto_right = strncmp(text, "auto", 4) == 0;
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->margin[RIGHT] = side[0];
    } else if (word_is(name, name_length, "padding")) {
        parse_box(text, style->font_size, style->padding);
    } else if (word_is(name, name_length, "padding-top")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->padding[TOP] = side[0];
    } else if (word_is(name, name_length, "padding-bottom")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->padding[BOTTOM] = side[0];
    } else if (word_is(name, name_length, "padding-left")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->padding[LEFT] = side[0];
    } else if (word_is(name, name_length, "padding-right")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->padding[RIGHT] = side[0];
    }
}

/* -------------------------------------------------------------- tag styles */

static int em(int font_size, int hundredths) {
    return (font_size * hundredths + 50) / 100;
}

static void block_margins(struct style *style, int top_hundredths,
                          int bottom_hundredths) {
    style->margin[TOP] = em(style->font_size, top_hundredths);
    style->margin[BOTTOM] = em(style->font_size, bottom_hundredths);
}

static int legacy_font_size(const char *value, int parent_size) {
    static const int sizes[] = {10, 13, 16, 18, 24, 32, 48};
    value = skip_space(value);
    int number = 0;
    int sign = 0;
    if (*value == '+' || *value == '-') {
        sign = *value == '-' ? -1 : 1;
        value++;
    }
    while (isdigit((unsigned char)*value)) {
        number = number * 10 + (*value++ - '0');
    }
    if (sign != 0) {
        int index = 2 + sign * number; /* relative to size 3 */
        number = index + 1;
    }
    if (number < 1) {
        number = 1;
    }
    if (number > 7) {
        number = 7;
    }
    (void)parent_size;
    return sizes[number - 1];
}

const struct css_sheet *style_sheets;

void style_compute(const struct opos_html_node *element,
                   const struct style *parent, struct style *style) {
    memset(style, 0, sizeof(*style));
    style->vars = parent->vars;
    style->font_size = parent->font_size;
    style->bold = parent->bold;
    style->italic = parent->italic;
    style->mono = parent->mono;
    style->underline = parent->underline;
    style->strike = parent->strike;
    style->pre = parent->pre;
    style->colour = parent->colour;
    style->align = parent->align;
    style->link = parent->link;
    style->no_marker = parent->no_marker;
    style->border_colour = parent->colour;
    style->width = style->max_width = -1;
    style->width_pct = style->max_width_pct = -1;
    style->display = DISPLAY_INLINE;

    switch (element->tag) {
    case HTML_TAG_HEAD: case HTML_TAG_SCRIPT: case HTML_TAG_STYLE:
    case HTML_TAG_TITLE: case HTML_TAG_META: case HTML_TAG_LINK:
    case HTML_TAG_BASE: case HTML_TAG_TEMPLATE: case HTML_TAG_FRAMESET:
    case HTML_TAG_FRAME: case HTML_TAG_AREA: case HTML_TAG_MAP:
    case HTML_TAG_PARAM: case HTML_TAG_SOURCE: case HTML_TAG_TRACK:
    case HTML_TAG_NOFRAMES: case HTML_TAG_NOEMBED: case HTML_TAG_IFRAME:
    case HTML_TAG_OBJECT: case HTML_TAG_EMBED: case HTML_TAG_VIDEO:
    case HTML_TAG_AUDIO: case HTML_TAG_CANVAS: case HTML_TAG_SVG:
    case HTML_TAG_OPTION: case HTML_TAG_OPTGROUP: case HTML_TAG_COLGROUP:
    case HTML_TAG_COL: case HTML_TAG_DIALOG:
        style->display = DISPLAY_NONE;
        break;
    case HTML_TAG_HTML:
        style->display = DISPLAY_BLOCK;
        break;
    case HTML_TAG_BODY:
        style->display = DISPLAY_BLOCK;
        for (int i = 0; i < 4; i++) {
            style->margin[i] = 8;
        }
        break;
    case HTML_TAG_DIV: case HTML_TAG_NAV: case HTML_TAG_HEADER:
    case HTML_TAG_FOOTER: case HTML_TAG_SECTION: case HTML_TAG_ARTICLE:
    case HTML_TAG_ASIDE: case HTML_TAG_MAIN: case HTML_TAG_HGROUP:
    case HTML_TAG_FORM: case HTML_TAG_LEGEND: case HTML_TAG_FIGCAPTION:
    case HTML_TAG_DETAILS: case HTML_TAG_SUMMARY: case HTML_TAG_DT:
    case HTML_TAG_NOSCRIPT:
        style->display = element->tag == HTML_TAG_NOSCRIPT ? DISPLAY_INLINE
                                                           : DISPLAY_BLOCK;
        break;
    case HTML_TAG_ADDRESS:
        style->display = DISPLAY_BLOCK;
        style->italic = true;
        break;
    case HTML_TAG_P:
        style->display = DISPLAY_BLOCK;
        block_margins(style, 100, 100);
        break;
    case HTML_TAG_H1: case HTML_TAG_H2: case HTML_TAG_H3:
    case HTML_TAG_H4: case HTML_TAG_H5: case HTML_TAG_H6: {
        static const int scale[6] = {200, 150, 117, 100, 83, 67};
        static const int margin[6] = {67, 83, 100, 133, 167, 233};
        int level = element->tag - HTML_TAG_H1;
        style->display = DISPLAY_BLOCK;
        style->font_size = em(parent->font_size, scale[level]);
        style->bold = true;
        block_margins(style, margin[level], margin[level]);
        break;
    }
    case HTML_TAG_BLOCKQUOTE: case HTML_TAG_FIGURE:
        style->display = DISPLAY_BLOCK;
        block_margins(style, 100, 100);
        style->margin[LEFT] = 40;
        style->margin[RIGHT] = 40;
        break;
    case HTML_TAG_UL: case HTML_TAG_OL: case HTML_TAG_MENU:
    case HTML_TAG_DIR:
        style->display = DISPLAY_BLOCK;
        if (parent->display != DISPLAY_LIST_ITEM) {
            block_margins(style, 100, 100);
        }
        style->padding[LEFT] = 40;
        break;
    case HTML_TAG_LI:
        style->display = DISPLAY_LIST_ITEM;
        break;
    case HTML_TAG_DL:
        style->display = DISPLAY_BLOCK;
        block_margins(style, 100, 100);
        break;
    case HTML_TAG_DD:
        style->display = DISPLAY_BLOCK;
        style->margin[LEFT] = 40;
        break;
    case HTML_TAG_PRE: case HTML_TAG_LISTING: case HTML_TAG_XMP:
        style->display = DISPLAY_BLOCK;
        style->pre = true;
        if (!parent->mono) {
            style->font_size = em(parent->font_size, 82);
        }
        style->mono = true;
        block_margins(style, 100, 100);
        break;
    case HTML_TAG_CODE: case HTML_TAG_KBD: case HTML_TAG_SAMP:
    case HTML_TAG_TT:
        if (!parent->mono) {
            style->font_size = em(parent->font_size, 87);
        }
        style->mono = true;
        break;
    case HTML_TAG_FIELDSET:
        style->display = DISPLAY_BLOCK;
        style->border = 1;
        style->border_colour = 0x00a0a0a0;
        block_margins(style, 50, 50);
        for (int i = 0; i < 4; i++) {
            style->padding[i] = 8;
        }
        break;
    case HTML_TAG_CENTER:
        style->display = DISPLAY_BLOCK;
        style->align = ALIGN_CENTER;
        break;
    case HTML_TAG_HR:
        style->display = DISPLAY_BLOCK;
        style->margin[TOP] = 8;
        style->margin[BOTTOM] = 8;
        break;
    case HTML_TAG_TABLE:
        style->display = DISPLAY_TABLE;
        break;
    case HTML_TAG_CAPTION: case HTML_TAG_THEAD: case HTML_TAG_TBODY:
    case HTML_TAG_TFOOT: case HTML_TAG_TR:
        style->display = DISPLAY_TABLE_PART;
        break;
    case HTML_TAG_TD:
        style->display = DISPLAY_TABLE_CELL;
        style->align = ALIGN_LEFT; /* text-align does not pass into cells */
        break;
    case HTML_TAG_TH:
        style->display = DISPLAY_TABLE_CELL;
        style->bold = true;
        style->align = ALIGN_CENTER;
        break;
    case HTML_TAG_B: case HTML_TAG_STRONG:
        style->bold = true;
        break;
    case HTML_TAG_I: case HTML_TAG_EM: case HTML_TAG_CITE:
    case HTML_TAG_VAR: case HTML_TAG_DFN:
        style->italic = true;
        break;
    case HTML_TAG_U: case HTML_TAG_INS:
        style->underline = true;
        break;
    case HTML_TAG_S: case HTML_TAG_STRIKE: case HTML_TAG_DEL:
        style->strike = true;
        break;
    case HTML_TAG_SMALL: case HTML_TAG_SUB: case HTML_TAG_SUP:
        style->font_size = em(parent->font_size, 83);
        break;
    case HTML_TAG_BIG:
        style->font_size = em(parent->font_size, 120);
        break;
    case HTML_TAG_MARK:
        style->has_background = true;
        style->background = 0x00ffff00;
        break;
    case HTML_TAG_A:
        if (opos_html_attribute(element, "href") != NULL) {
            style->link = (struct opos_html_node *)element;
            style->colour = DEFAULT_LINK;
            style->underline = true;
        }
        break;
    case HTML_TAG_INPUT: case HTML_TAG_BUTTON: case HTML_TAG_SELECT:
    case HTML_TAG_TEXTAREA:
        style->display = DISPLAY_INLINE_BLOCK;
        break;
    case HTML_TAG_FONT: {
        const char *size = opos_html_attribute(element, "size");
        if (size != NULL) {
            style->font_size = legacy_font_size(size, parent->font_size);
        }
        break;
    }
    default:
        break;
    }

    /* Presentation attributes of old pages. */
    const char *value;
    uint32_t colour;
    if ((value = opos_html_attribute(element, "bgcolor")) != NULL
        && parse_colour(value, &colour)) {
        style->has_background = true;
        style->background = colour;
    }
    if ((value = opos_html_attribute(element, "color")) != NULL
        && element->tag == HTML_TAG_FONT && parse_colour(value, &colour)) {
        style->colour = colour;
    }
    if ((value = opos_html_attribute(element, "text")) != NULL
        && element->tag == HTML_TAG_BODY && parse_colour(value, &colour)) {
        style->colour = colour;
    }
    if (element->tag == HTML_TAG_IMG
        && (value = opos_html_attribute(element, "align")) != NULL) {
        style->float_side = word_is(value, strlen(value), "left") ? 1
                          : word_is(value, strlen(value), "right") ? 2 : 0;
    }
    if ((value = opos_html_attribute(element, "align")) != NULL
        && element->tag != HTML_TAG_IMG) {
        if (word_is(value, strlen(value), "center")
            || word_is(value, strlen(value), "middle")) {
            style->align = ALIGN_CENTER;
        } else if (word_is(value, strlen(value), "right")) {
            style->align = ALIGN_RIGHT;
        } else if (word_is(value, strlen(value), "left")) {
            style->align = ALIGN_LEFT;
        }
    }
    if (opos_html_attribute(element, "hidden") != NULL) {
        style->display = DISPLAY_NONE;
    }
    css_apply(style_sheets, element, style, parent);
    if ((value = opos_html_attribute(element, "style")) != NULL) {
        css_apply_block(style, value, parent);
    }
    /* An invisible item placed into a shared grid cell (the "stack"
       trick: a hidden copy that only reserves size) would otherwise
       show as a second copy of its sibling's text. The last element
       in the cell stays, because the rules that pick the visible one
       often need :has(), which we do not support yet. */
    if (style->opacity_zero && style->grid_area_set && parent != NULL
        && parent->grid) {
        const struct opos_html_node *next = element->next_sibling;
        while (next != NULL && next->type != HTML_NODE_ELEMENT) {
            next = next->next_sibling;
        }
        if (next != NULL) {
            style->out_of_flow = true;
        }
    }
    if (style->out_of_flow) {
        style->display = DISPLAY_NONE;
    }
    /* A closed <details> shows only its <summary>. */
    const struct opos_html_node *up = element->parent;
    if (up != NULL && up->tag == HTML_TAG_DETAILS
        && element->tag != HTML_TAG_SUMMARY
        && opos_html_attribute(up, "open") == NULL) {
        style->display = DISPLAY_NONE;
    }
    /* A heading or table cell inside a link keeps the link colour only
       when it has no colour of its own: nothing else to do here. */
}
