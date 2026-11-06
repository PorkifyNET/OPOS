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

bool parse_colour(const char *text, uint32_t *out) {
    text = skip_space(text);
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
            return true;
        }
        if (digits == 6 || digits == 8) {
            uint32_t value = 0;
            for (size_t i = 0; i < 6; i++) {
                value = value << 4 | (uint32_t)hex_value((unsigned char)text[i]);
            }
            *out = value;
            return true;
        }
        return false;
    }
    if (strncmp(text, "rgb", 3) == 0 && (text[3] == '(' || text[3] == 'a')) {
        const char *p = strchr(text, '(');
        if (p == NULL) {
            return false;
        }
        p++;
        int channel[3];
        for (int i = 0; i < 3; i++) {
            p = skip_space(p);
            if (!isdigit((unsigned char)*p)) {
                return false;
            }
            int value = 0;
            while (isdigit((unsigned char)*p)) {
                value = value * 10 + (*p++ - '0');
            }
            if (*p == '%') {
                value = value * 255 / 100;
                p++;
            }
            channel[i] = value > 255 ? 255 : value;
            p = skip_space(p);
            if (*p == ',' || *p == '/') {
                p++;
            }
        }
        *out = OPOS_RGB(channel[0], channel[1], channel[2]);
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
                return false; /* transparent: no colour */
            }
            *out = named_colours[i].value;
            return true;
        }
    }
    return false;
}

int viewport_width_px = 800;
int viewport_height_px = 600;

bool parse_length(const char *text, int *value, bool *percent) {
    text = skip_space(text);
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

    if (word_is(name, name_length, "display")) {
        if (word_is(text, strlen(text), "none")) {
            style->display = DISPLAY_NONE;
        } else if (word_is(text, strlen(text), "flex")
                   || word_is(text, strlen(text), "inline-flex")) {
            style->display = DISPLAY_BLOCK;
            style->flex = true;
        } else if (word_is(text, strlen(text), "block")
                   || word_is(text, strlen(text), "flex")
                   || word_is(text, strlen(text), "grid")
                   || word_is(text, strlen(text), "flow-root")) {
            style->display = DISPLAY_BLOCK;
        } else if (word_is(text, strlen(text), "inline")) {
            style->display = DISPLAY_INLINE;
        } else if (word_is(text, strlen(text), "inline-block")) {
            style->display = style->display == DISPLAY_INLINE_BLOCK
                           ? DISPLAY_INLINE_BLOCK : DISPLAY_INLINE;
        } else if (word_is(text, strlen(text), "list-item")) {
            style->display = DISPLAY_LIST_ITEM;
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
            if (strncmp(token, "url(", 4) != 0 && parse_colour(token, &colour)) {
                style->has_background = true;
                style->background = colour;
                break;
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
        style->flex = style->flex && strncmp(text, "column", 6) != 0;
    } else if (word_is(name, name_length, "flex-wrap")) {
        style->flex_wrap = strncmp(text, "wrap", 4) == 0;
    } else if (word_is(name, name_length, "flex-grow")) {
        style->flex_grow = atoi(text) > 0 ? 1 : 0;
    } else if (word_is(name, name_length, "flex")) {
        style->flex_grow = atoi(text) > 0 ? 1 : 0;
        style->flex_zero = style->flex_grow && strchr(text, ' ') == NULL;
    } else if (word_is(name, name_length, "gap")
               || word_is(name, name_length, "column-gap")) {
        int side[4] = {0};
        parse_box(text, style->font_size, side);
        style->gap = side[word_is(name, name_length, "gap") ? 1 : 0];
        if (style->gap == 0) {
            style->gap = side[0];
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
