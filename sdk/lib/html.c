/*
 * HTML parser: see include/opos_html.h.
 *
 * Needs only malloc/free and string functions, so a host build can test it
 * on its own.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/opos_html.h"

#define MAX_DEPTH 256
#define MAX_NODES 400000
#define ARENA_CHUNK (64 * 1024)

/* ---------------------------------------------------------------- arena */

struct chunk {
    struct chunk *next;
    size_t used;
    size_t size;
    /* data follows */
};

struct arena {
    struct chunk *chunks;
    size_t nodes;
    bool failed;
};

static void *arena_alloc(struct arena *arena, size_t size) {
    size = (size + 7) & ~(size_t)7;
    struct chunk *chunk = arena->chunks;
    if (chunk == NULL || chunk->size - chunk->used < size) {
        size_t capacity = size > ARENA_CHUNK ? size : ARENA_CHUNK;
        struct chunk *fresh = malloc(sizeof(*fresh) + capacity);
        if (fresh == NULL) {
            arena->failed = true;
            return NULL;
        }
        fresh->used = 0;
        fresh->size = capacity;
        /* A big block gets its own chunk behind the current one so the
           current chunk keeps serving small requests. */
        if (size > ARENA_CHUNK && chunk != NULL) {
            fresh->next = chunk->next;
            chunk->next = fresh;
            fresh->used = size;
            return (char *)(fresh + 1);
        }
        fresh->next = arena->chunks;
        arena->chunks = fresh;
        chunk = fresh;
    }
    void *result = (char *)(chunk + 1) + chunk->used;
    chunk->used += size;
    return result;
}

static char *arena_string(struct arena *arena, const char *text,
                          size_t length) {
    char *copy = arena_alloc(arena, length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

/* --------------------------------------------------------------- tables */

#define F_VOID 1
#define F_RAW 2        /* content is raw text up to the end tag */
#define F_RCDATA 4     /* ... with character references decoded */
#define F_CLOSES_P 8   /* a start tag closes an open <p> */
#define F_FORMAT 16    /* formatting element, reopened when misnested */
#define F_HEAD 32      /* belongs in <head> before the body starts */
#define F_PRE 64       /* a newline right after the start tag is dropped */

struct tag_info {
    const char *name;
    enum opos_html_tag tag;
    unsigned flags;
};

static const struct tag_info tags[] = {
    {"html", HTML_TAG_HTML, 0}, {"head", HTML_TAG_HEAD, 0},
    {"body", HTML_TAG_BODY, 0},
    {"title", HTML_TAG_TITLE, F_HEAD | F_RAW | F_RCDATA},
    {"meta", HTML_TAG_META, F_VOID | F_HEAD},
    {"link", HTML_TAG_LINK, F_VOID | F_HEAD},
    {"style", HTML_TAG_STYLE, F_HEAD | F_RAW},
    {"script", HTML_TAG_SCRIPT, F_HEAD | F_RAW},
    {"base", HTML_TAG_BASE, F_VOID | F_HEAD},
    {"noscript", HTML_TAG_NOSCRIPT, 0}, {"template", HTML_TAG_TEMPLATE, 0},
    {"p", HTML_TAG_P, F_CLOSES_P}, {"div", HTML_TAG_DIV, F_CLOSES_P},
    {"span", HTML_TAG_SPAN, 0}, {"a", HTML_TAG_A, F_FORMAT},
    {"img", HTML_TAG_IMG, F_VOID}, {"br", HTML_TAG_BR, F_VOID},
    {"hr", HTML_TAG_HR, F_VOID | F_CLOSES_P}, {"wbr", HTML_TAG_WBR, F_VOID},
    {"h1", HTML_TAG_H1, F_CLOSES_P}, {"h2", HTML_TAG_H2, F_CLOSES_P},
    {"h3", HTML_TAG_H3, F_CLOSES_P}, {"h4", HTML_TAG_H4, F_CLOSES_P},
    {"h5", HTML_TAG_H5, F_CLOSES_P}, {"h6", HTML_TAG_H6, F_CLOSES_P},
    {"ul", HTML_TAG_UL, F_CLOSES_P}, {"ol", HTML_TAG_OL, F_CLOSES_P},
    {"li", HTML_TAG_LI, F_CLOSES_P}, {"dl", HTML_TAG_DL, F_CLOSES_P},
    {"dt", HTML_TAG_DT, F_CLOSES_P}, {"dd", HTML_TAG_DD, F_CLOSES_P},
    {"menu", HTML_TAG_MENU, F_CLOSES_P}, {"dir", HTML_TAG_DIR, F_CLOSES_P},
    {"pre", HTML_TAG_PRE, F_CLOSES_P | F_PRE},
    {"listing", HTML_TAG_LISTING, F_CLOSES_P | F_PRE},
    {"xmp", HTML_TAG_XMP, F_CLOSES_P | F_RAW},
    {"blockquote", HTML_TAG_BLOCKQUOTE, F_CLOSES_P},
    {"table", HTML_TAG_TABLE, F_CLOSES_P}, {"caption", HTML_TAG_CAPTION, 0},
    {"colgroup", HTML_TAG_COLGROUP, 0}, {"col", HTML_TAG_COL, F_VOID},
    {"thead", HTML_TAG_THEAD, 0}, {"tbody", HTML_TAG_TBODY, 0},
    {"tfoot", HTML_TAG_TFOOT, 0}, {"tr", HTML_TAG_TR, 0},
    {"td", HTML_TAG_TD, 0}, {"th", HTML_TAG_TH, 0},
    {"b", HTML_TAG_B, F_FORMAT}, {"strong", HTML_TAG_STRONG, F_FORMAT},
    {"i", HTML_TAG_I, F_FORMAT}, {"em", HTML_TAG_EM, F_FORMAT},
    {"u", HTML_TAG_U, F_FORMAT}, {"s", HTML_TAG_S, F_FORMAT},
    {"strike", HTML_TAG_STRIKE, F_FORMAT}, {"del", HTML_TAG_DEL, 0},
    {"ins", HTML_TAG_INS, 0}, {"code", HTML_TAG_CODE, F_FORMAT},
    {"tt", HTML_TAG_TT, F_FORMAT}, {"kbd", HTML_TAG_KBD, 0},
    {"samp", HTML_TAG_SAMP, 0}, {"var", HTML_TAG_VAR, 0},
    {"small", HTML_TAG_SMALL, F_FORMAT}, {"big", HTML_TAG_BIG, F_FORMAT},
    {"sub", HTML_TAG_SUB, 0}, {"sup", HTML_TAG_SUP, 0},
    {"font", HTML_TAG_FONT, F_FORMAT}, {"nobr", HTML_TAG_NOBR, F_FORMAT},
    {"cite", HTML_TAG_CITE, 0}, {"q", HTML_TAG_Q, 0},
    {"abbr", HTML_TAG_ABBR, 0}, {"mark", HTML_TAG_MARK, 0},
    {"time", HTML_TAG_TIME, 0}, {"dfn", HTML_TAG_DFN, 0},
    {"bdo", HTML_TAG_BDO, 0}, {"bdi", HTML_TAG_BDI, 0},
    {"label", HTML_TAG_LABEL, 0},
    {"form", HTML_TAG_FORM, F_CLOSES_P}, {"input", HTML_TAG_INPUT, F_VOID},
    {"button", HTML_TAG_BUTTON, 0}, {"select", HTML_TAG_SELECT, 0},
    {"option", HTML_TAG_OPTION, 0}, {"optgroup", HTML_TAG_OPTGROUP, 0},
    {"textarea", HTML_TAG_TEXTAREA, F_RAW | F_RCDATA | F_PRE},
    {"fieldset", HTML_TAG_FIELDSET, F_CLOSES_P},
    {"legend", HTML_TAG_LEGEND, 0},
    {"nav", HTML_TAG_NAV, F_CLOSES_P}, {"header", HTML_TAG_HEADER, F_CLOSES_P},
    {"footer", HTML_TAG_FOOTER, F_CLOSES_P},
    {"section", HTML_TAG_SECTION, F_CLOSES_P},
    {"article", HTML_TAG_ARTICLE, F_CLOSES_P},
    {"aside", HTML_TAG_ASIDE, F_CLOSES_P}, {"main", HTML_TAG_MAIN, F_CLOSES_P},
    {"figure", HTML_TAG_FIGURE, F_CLOSES_P},
    {"figcaption", HTML_TAG_FIGCAPTION, F_CLOSES_P},
    {"address", HTML_TAG_ADDRESS, F_CLOSES_P},
    {"details", HTML_TAG_DETAILS, F_CLOSES_P},
    {"summary", HTML_TAG_SUMMARY, F_CLOSES_P},
    {"hgroup", HTML_TAG_HGROUP, F_CLOSES_P},
    {"dialog", HTML_TAG_DIALOG, F_CLOSES_P},
    {"center", HTML_TAG_CENTER, F_CLOSES_P},
    {"iframe", HTML_TAG_IFRAME, F_RAW}, {"object", HTML_TAG_OBJECT, 0},
    {"embed", HTML_TAG_EMBED, F_VOID}, {"param", HTML_TAG_PARAM, F_VOID},
    {"area", HTML_TAG_AREA, F_VOID}, {"map", HTML_TAG_MAP, 0},
    {"source", HTML_TAG_SOURCE, F_VOID}, {"track", HTML_TAG_TRACK, F_VOID},
    {"canvas", HTML_TAG_CANVAS, 0}, {"video", HTML_TAG_VIDEO, 0},
    {"audio", HTML_TAG_AUDIO, 0}, {"svg", HTML_TAG_SVG, 0},
    {"noframes", HTML_TAG_NOFRAMES, F_RAW},
    {"noembed", HTML_TAG_NOEMBED, F_RAW},
    {"frame", HTML_TAG_FRAME, F_VOID}, {"frameset", HTML_TAG_FRAMESET, 0},
};

static const struct tag_info unknown_tag = {NULL, HTML_TAG_UNKNOWN, 0};

static const struct tag_info *tag_lookup(const char *name, size_t length) {
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (tags[i].name[0] == name[0] && strlen(tags[i].name) == length
            && memcmp(tags[i].name, name, length) == 0) {
            return &tags[i];
        }
    }
    return &unknown_tag;
}

struct entity {
    const char *name;
    uint32_t code;
    uint32_t second; /* 0 or a second code point */
};

static const struct entity entities[] = {
    {"amp", '&', 0}, {"lt", '<', 0}, {"gt", '>', 0}, {"quot", '"', 0},
    {"nbsp", 0xa0, 0}, {"copy", 0xa9, 0}, {"reg", 0xae, 0},
    {"apos", '\'', 0},
    {"iexcl", 0xa1, 0}, {"cent", 0xa2, 0}, {"pound", 0xa3, 0},
    {"curren", 0xa4, 0}, {"yen", 0xa5, 0}, {"brvbar", 0xa6, 0},
    {"sect", 0xa7, 0}, {"uml", 0xa8, 0}, {"ordf", 0xaa, 0},
    {"laquo", 0xab, 0}, {"not", 0xac, 0}, {"shy", 0xad, 0},
    {"macr", 0xaf, 0}, {"deg", 0xb0, 0}, {"plusmn", 0xb1, 0},
    {"sup2", 0xb2, 0}, {"sup3", 0xb3, 0}, {"acute", 0xb4, 0},
    {"micro", 0xb5, 0}, {"para", 0xb6, 0}, {"middot", 0xb7, 0},
    {"cedil", 0xb8, 0}, {"sup1", 0xb9, 0}, {"ordm", 0xba, 0},
    {"raquo", 0xbb, 0}, {"frac14", 0xbc, 0}, {"frac12", 0xbd, 0},
    {"frac34", 0xbe, 0}, {"iquest", 0xbf, 0},
    {"Agrave", 0xc0, 0}, {"Aacute", 0xc1, 0}, {"Acirc", 0xc2, 0},
    {"Atilde", 0xc3, 0}, {"Auml", 0xc4, 0}, {"Aring", 0xc5, 0},
    {"AElig", 0xc6, 0}, {"Ccedil", 0xc7, 0}, {"Egrave", 0xc8, 0},
    {"Eacute", 0xc9, 0}, {"Ecirc", 0xca, 0}, {"Euml", 0xcb, 0},
    {"Igrave", 0xcc, 0}, {"Iacute", 0xcd, 0}, {"Icirc", 0xce, 0},
    {"Iuml", 0xcf, 0}, {"ETH", 0xd0, 0}, {"Ntilde", 0xd1, 0},
    {"Ograve", 0xd2, 0}, {"Oacute", 0xd3, 0}, {"Ocirc", 0xd4, 0},
    {"Otilde", 0xd5, 0}, {"Ouml", 0xd6, 0}, {"times", 0xd7, 0},
    {"Oslash", 0xd8, 0}, {"Ugrave", 0xd9, 0}, {"Uacute", 0xda, 0},
    {"Ucirc", 0xdb, 0}, {"Uuml", 0xdc, 0}, {"Yacute", 0xdd, 0},
    {"THORN", 0xde, 0}, {"szlig", 0xdf, 0},
    {"agrave", 0xe0, 0}, {"aacute", 0xe1, 0}, {"acirc", 0xe2, 0},
    {"atilde", 0xe3, 0}, {"auml", 0xe4, 0}, {"aring", 0xe5, 0},
    {"aelig", 0xe6, 0}, {"ccedil", 0xe7, 0}, {"egrave", 0xe8, 0},
    {"eacute", 0xe9, 0}, {"ecirc", 0xea, 0}, {"euml", 0xeb, 0},
    {"igrave", 0xec, 0}, {"iacute", 0xed, 0}, {"icirc", 0xee, 0},
    {"iuml", 0xef, 0}, {"eth", 0xf0, 0}, {"ntilde", 0xf1, 0},
    {"ograve", 0xf2, 0}, {"oacute", 0xf3, 0}, {"ocirc", 0xf4, 0},
    {"otilde", 0xf5, 0}, {"ouml", 0xf6, 0}, {"divide", 0xf7, 0},
    {"oslash", 0xf8, 0}, {"ugrave", 0xf9, 0}, {"uacute", 0xfa, 0},
    {"ucirc", 0xfb, 0}, {"uuml", 0xfc, 0}, {"yacute", 0xfd, 0},
    {"thorn", 0xfe, 0}, {"yuml", 0xff, 0},
    {"OElig", 0x152, 0}, {"oelig", 0x153, 0}, {"Scaron", 0x160, 0},
    {"scaron", 0x161, 0}, {"Yuml", 0x178, 0}, {"fnof", 0x192, 0},
    {"circ", 0x2c6, 0}, {"tilde", 0x2dc, 0},
    {"Alpha", 0x391, 0}, {"Beta", 0x392, 0}, {"Gamma", 0x393, 0},
    {"Delta", 0x394, 0}, {"Omega", 0x3a9, 0}, {"alpha", 0x3b1, 0},
    {"beta", 0x3b2, 0}, {"gamma", 0x3b3, 0}, {"delta", 0x3b4, 0},
    {"epsilon", 0x3b5, 0}, {"lambda", 0x3bb, 0}, {"mu", 0x3bc, 0},
    {"pi", 0x3c0, 0}, {"sigma", 0x3c3, 0}, {"tau", 0x3c4, 0},
    {"phi", 0x3c6, 0}, {"omega", 0x3c9, 0},
    {"ensp", 0x2002, 0}, {"emsp", 0x2003, 0}, {"thinsp", 0x2009, 0},
    {"zwnj", 0x200c, 0}, {"zwj", 0x200d, 0}, {"lrm", 0x200e, 0},
    {"rlm", 0x200f, 0},
    {"ndash", 0x2013, 0}, {"mdash", 0x2014, 0}, {"lsquo", 0x2018, 0},
    {"rsquo", 0x2019, 0}, {"sbquo", 0x201a, 0}, {"ldquo", 0x201c, 0},
    {"rdquo", 0x201d, 0}, {"bdquo", 0x201e, 0}, {"dagger", 0x2020, 0},
    {"Dagger", 0x2021, 0}, {"bull", 0x2022, 0}, {"hellip", 0x2026, 0},
    {"permil", 0x2030, 0}, {"prime", 0x2032, 0}, {"Prime", 0x2033, 0},
    {"lsaquo", 0x2039, 0}, {"rsaquo", 0x203a, 0}, {"oline", 0x203e, 0},
    {"frasl", 0x2044, 0}, {"euro", 0x20ac, 0}, {"trade", 0x2122, 0},
    {"larr", 0x2190, 0}, {"uarr", 0x2191, 0}, {"rarr", 0x2192, 0},
    {"darr", 0x2193, 0}, {"harr", 0x2194, 0}, {"lArr", 0x21d0, 0},
    {"rArr", 0x21d2, 0}, {"hArr", 0x21d4, 0},
    {"forall", 0x2200, 0}, {"part", 0x2202, 0}, {"exist", 0x2203, 0},
    {"empty", 0x2205, 0}, {"nabla", 0x2207, 0}, {"isin", 0x2208, 0},
    {"notin", 0x2209, 0}, {"prod", 0x220f, 0}, {"sum", 0x2211, 0},
    {"minus", 0x2212, 0}, {"radic", 0x221a, 0}, {"infin", 0x221e, 0},
    {"and", 0x2227, 0}, {"or", 0x2228, 0}, {"cap", 0x2229, 0},
    {"cup", 0x222a, 0}, {"int", 0x222b, 0}, {"asymp", 0x2248, 0},
    {"ne", 0x2260, 0}, {"equiv", 0x2261, 0}, {"le", 0x2264, 0},
    {"ge", 0x2265, 0}, {"sub", 0x2282, 0}, {"sup", 0x2283, 0},
    {"loz", 0x25ca, 0}, {"spades", 0x2660, 0}, {"clubs", 0x2663, 0},
    {"hearts", 0x2665, 0}, {"diams", 0x2666, 0}, {"check", 0x2713, 0},
    {"star", 0x2606, 0}, {"hyphen", 0x2010, 0}, {"dash", 0x2010, 0},
    {"lbrace", '{', 0}, {"rbrace", '}', 0}, {"lbrack", '[', 0},
    {"rbrack", ']', 0}, {"lpar", '(', 0}, {"rpar", ')', 0},
    {"comma", ',', 0}, {"period", '.', 0}, {"colon", ':', 0},
    {"semi", ';', 0}, {"excl", '!', 0}, {"quest", '?', 0},
    {"num", '#', 0}, {"dollar", '$', 0}, {"percnt", '%', 0},
    {"ast", '*', 0}, {"plus", '+', 0}, {"equals", '=', 0},
    {"sol", '/', 0}, {"bsol", '\\', 0}, {"lowbar", '_', 0},
    {"grave", '`', 0}, {"verbar", '|', 0}, {"commat", '@', 0},
    {"Tab", '\t', 0}, {"NewLine", '\n', 0},
};

/* The first 7 entries (amp, lt, gt, quot, nbsp, copy, reg) also work
   without the closing semicolon. */
static bool entity_is_legacy(size_t index) {
    return index < 7;
}

/* Windows-1252 0x80-0x9f to Unicode. */
static const uint16_t cp1252[32] = {
    0x20ac, 0x81, 0x201a, 0x192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x2c6, 0x2030, 0x160, 0x2039, 0x152, 0x8d, 0x17d, 0x8f,
    0x90, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x2dc, 0x2122, 0x161, 0x203a, 0x153, 0x9d, 0x17e, 0x178};

/* ------------------------------------------------------------- UTF-8 out */

static size_t utf8_put(char *out, uint32_t code) {
    if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
        code = 0xfffd;
    }
    if (code >= 0x80 && code <= 0x9f) {
        code = cp1252[code - 0x80]; /* as browsers do for numeric refs */
    }
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
        out[1] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[2] = (char)(0x80 | (code & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | code >> 18);
    out[1] = (char)(0x80 | ((code >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((code >> 6) & 0x3f));
    out[3] = (char)(0x80 | (code & 0x3f));
    return 4;
}

/* ---------------------------------------------------- character references */

static bool is_alnum(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z');
}

/* Decodes the reference after '&' at text[i] (i points past the '&').
   Writes at most 8 bytes to out. Returns the number of input bytes used
   after the '&' (0 when it is not a reference) and sets *written. */
static size_t decode_reference(const char *text, size_t i, size_t length,
                               bool in_attribute, char *out,
                               size_t *written) {
    *written = 0;
    if (i >= length) {
        return 0;
    }
    if (text[i] == '#') {
        size_t j = i + 1;
        uint32_t code = 0;
        bool hex = false;
        if (j < length && (text[j] == 'x' || text[j] == 'X')) {
            hex = true;
            j++;
        }
        size_t digits = 0;
        while (j < length) {
            int c = (unsigned char)text[j];
            int value;
            if (c >= '0' && c <= '9') {
                value = c - '0';
            } else if (hex && c >= 'a' && c <= 'f') {
                value = c - 'a' + 10;
            } else if (hex && c >= 'A' && c <= 'F') {
                value = c - 'A' + 10;
            } else {
                break;
            }
            if (code <= 0x10ffff) {
                code = code * (hex ? 16 : 10) + (uint32_t)value;
            }
            digits++;
            j++;
        }
        if (digits == 0) {
            return 0;
        }
        if (j < length && text[j] == ';') {
            j++;
        }
        *written = utf8_put(out, code);
        return j - i;
    }
    if (!is_alnum((unsigned char)text[i])) {
        return 0;
    }
    /* Longest named reference that matches. */
    size_t best = 0;
    size_t best_length = 0;
    bool best_semicolon = false;
    for (size_t k = 0; k < sizeof(entities) / sizeof(entities[0]); k++) {
        size_t name_length = strlen(entities[k].name);
        if (name_length < best_length || i + name_length > length
            || memcmp(text + i, entities[k].name, name_length) != 0) {
            continue;
        }
        bool semicolon = i + name_length < length
                      && text[i + name_length] == ';';
        if (!semicolon) {
            if (!entity_is_legacy(k)) {
                continue;
            }
            /* In attribute values "&ampfoo=" stays literal. */
            if (in_attribute && i + name_length < length
                && (is_alnum((unsigned char)text[i + name_length])
                    || text[i + name_length] == '=')) {
                continue;
            }
        }
        if (name_length > best_length || (semicolon && !best_semicolon)) {
            best = k;
            best_length = name_length;
            best_semicolon = semicolon;
        }
    }
    if (best_length == 0) {
        return 0;
    }
    *written = utf8_put(out, entities[best].code);
    return best_length + (best_semicolon ? 1 : 0);
}

size_t opos_html_decode_entities(const char *text, size_t length, char *out,
                                 size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < length && used + 9 < capacity; i++) {
        if (text[i] == '&') {
            char bytes[8];
            size_t written;
            size_t consumed = decode_reference(text, i + 1, length, false,
                                               bytes, &written);
            if (consumed != 0) {
                memcpy(out + used, bytes, written);
                used += written;
                i += consumed;
                continue;
            }
        }
        out[used++] = text[i];
    }
    if (capacity > 0) {
        out[used] = '\0';
    }
    return used;
}

/* A growing byte buffer for decoded text. */
struct buffer {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
};

static void buffer_put(struct buffer *b, const char *bytes, size_t count) {
    if (b->failed) {
        return;
    }
    if (b->length + count + 1 > b->capacity) {
        size_t capacity = b->capacity == 0 ? 256 : b->capacity;
        while (capacity < b->length + count + 1) {
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
    memcpy(b->data + b->length, bytes, count);
    b->length += count;
}

/* Appends text[from..to) with references decoded. */
static void decode_into(struct buffer *b, const char *text, size_t from,
                        size_t to, bool in_attribute) {
    size_t start = from;
    for (size_t i = from; i < to; i++) {
        if (text[i] != '&') {
            continue;
        }
        char bytes[8];
        size_t written;
        size_t consumed = decode_reference(text, i + 1, to, in_attribute,
                                           bytes, &written);
        if (consumed == 0) {
            continue;
        }
        buffer_put(b, text + start, i - start);
        buffer_put(b, bytes, written);
        i += consumed;
        start = i + 1;
    }
    buffer_put(b, text + start, to - start);
}

/* ----------------------------------------------------------- tree builder */

struct raw_attribute {
    size_t name_from, name_to;
    size_t value_from, value_to;
    bool has_value;
};

#define MAX_ATTRIBUTES 64

struct start_tag {
    const char *name;
    size_t name_length;
    struct raw_attribute attributes[MAX_ATTRIBUTES];
    size_t attribute_count;
    bool self_closing;
};

struct parser {
    struct start_tag tag; /* the start tag being handled */
    struct arena arena;
    struct opos_html_doc *doc;
    struct opos_html_node *stack[MAX_DEPTH];
    size_t depth;                /* entries used in stack */
    enum { BEFORE_HEAD, IN_HEAD, AFTER_HEAD, IN_BODY } mode;
    bool body_created;
    bool head_created;
    bool drop_newline;           /* first newline after <pre> */
    struct buffer text;
};

static struct opos_html_node *current(struct parser *p) {
    return p->stack[p->depth - 1];
}

static struct opos_html_node *node_new(struct parser *p,
                                       enum opos_html_node_type type) {
    if (p->arena.nodes >= MAX_NODES) {
        return NULL;
    }
    struct opos_html_node *node = arena_alloc(&p->arena, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    memset(node, 0, sizeof(*node));
    node->type = type;
    p->arena.nodes++;
    return node;
}

static void node_append(struct opos_html_node *parent,
                        struct opos_html_node *node) {
    node->parent = parent;
    node->previous_sibling = parent->last_child;
    if (parent->last_child != NULL) {
        parent->last_child->next_sibling = node;
    } else {
        parent->first_child = node;
    }
    parent->last_child = node;
}

static struct opos_html_node *element_new(struct parser *p,
                                          const char *name, size_t length,
                                          const struct tag_info *info) {
    struct opos_html_node *node = node_new(p, HTML_NODE_ELEMENT);
    if (node == NULL) {
        return NULL;
    }
    node->tag = info->tag;
    node->name = info->name != NULL ? info->name
                                    : arena_string(&p->arena, name, length);
    return node;
}

static void push(struct parser *p, struct opos_html_node *node) {
    p->stack[p->depth++] = node;
}

/* Pops down to and including stack[level]. */
static void pop_to(struct parser *p, size_t level) {
    if (level >= 1) {
        p->depth = level;
    }
}

static const struct tag_info *info_for(enum opos_html_tag tag) {
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (tags[i].tag == tag) {
            return &tags[i];
        }
    }
    return &unknown_tag;
}

static unsigned flags_of(const struct opos_html_node *node) {
    return node->tag == HTML_TAG_UNKNOWN ? 0 : info_for(node->tag)->flags;
}

/* Adds text to the current node, merging with a preceding text node. */
static void add_text(struct parser *p, const char *text, size_t length) {
    if (length == 0) {
        return;
    }
    struct opos_html_node *parent = current(p);
    struct opos_html_node *last = parent->last_child;
    if (last != NULL && last->type == HTML_NODE_TEXT) {
        /* Rebuild the merged string in the arena (rare: only after skipped
           comments or tags that produced no node). */
        char *merged = arena_alloc(&p->arena, last->text_length + length + 1);
        if (merged == NULL) {
            return;
        }
        memcpy(merged, last->text, last->text_length);
        memcpy(merged + last->text_length, text, length);
        merged[last->text_length + length] = '\0';
        last->text = merged;
        last->text_length += length;
        return;
    }
    struct opos_html_node *node = node_new(p, HTML_NODE_TEXT);
    if (node == NULL) {
        return;
    }
    char *copy = arena_string(&p->arena, text, length);
    if (copy == NULL) {
        return;
    }
    node->text = copy;
    node->text_length = length;
    node_append(parent, node);
}

static bool is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool only_space(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (!is_space((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

/* Makes sure <head> exists and is attached (document start). */
static void ensure_head(struct parser *p) {
    if (!p->head_created) {
        p->head_created = true;
        node_append(p->doc->root, p->doc->head);
    }
}

/* Leaves the head and starts the body. */
static void ensure_body(struct parser *p) {
    if (p->mode == IN_BODY) {
        return;
    }
    ensure_head(p);
    p->depth = 1; /* only <html> open */
    p->body_created = true;
    node_append(p->doc->root, p->doc->body);
    push(p, p->doc->body);
    p->mode = IN_BODY;
}

/* Finds the nearest open element with this tag, stopping at scope
   boundaries; returns its stack index or 0 (stack[0] is <html>). */
static size_t find_open(struct parser *p, enum opos_html_tag tag,
                        bool stop_at_table) {
    for (size_t i = p->depth; i > 1; i--) {
        struct opos_html_node *node = p->stack[i - 1];
        if (node->tag == tag) {
            return i;
        }
        if (stop_at_table
            && (node->tag == HTML_TAG_TABLE || node->tag == HTML_TAG_TD
                || node->tag == HTML_TAG_TH || node->tag == HTML_TAG_CAPTION
                || node->tag == HTML_TAG_BUTTON
                || node->tag == HTML_TAG_OBJECT)) {
            return 0;
        }
    }
    return 0;
}

static bool is_heading(enum opos_html_tag tag) {
    return tag >= HTML_TAG_H1 && tag <= HTML_TAG_H6;
}

/* Pops through the nearest open element whose tag is between low and high
   (enum order), looking down to the enclosing <table>. */
static bool pop_through(struct parser *p, enum opos_html_tag low,
                        enum opos_html_tag high) {
    for (size_t i = p->depth; i > 1; i--) {
        enum opos_html_tag t = p->stack[i - 1]->tag;
        if (t >= low && t <= high) {
            pop_to(p, i - 1);
            return true;
        }
        if (t == HTML_TAG_TABLE) {
            return false;
        }
    }
    return false;
}

/* Implied end tags before a start tag. */
static void close_for_start(struct parser *p, const struct tag_info *info) {
    enum opos_html_tag tag = info->tag;
    if (info->flags & F_CLOSES_P) {
        size_t at = find_open(p, HTML_TAG_P, true);
        if (at != 0) {
            pop_to(p, at - 1);
        }
    }
    if (is_heading(tag) && is_heading(current(p)->tag)) {
        pop_to(p, p->depth - 1);
    }
    switch (tag) {
    case HTML_TAG_LI:
        for (size_t i = p->depth; i > 1; i--) {
            enum opos_html_tag t = p->stack[i - 1]->tag;
            if (t == HTML_TAG_LI) {
                pop_to(p, i - 1);
                break;
            }
            if (t == HTML_TAG_UL || t == HTML_TAG_OL || t == HTML_TAG_TABLE
                || t == HTML_TAG_TD || t == HTML_TAG_TH
                || t == HTML_TAG_BLOCKQUOTE || t == HTML_TAG_MENU) {
                break;
            }
        }
        break;
    case HTML_TAG_DT:
    case HTML_TAG_DD:
        for (size_t i = p->depth; i > 1; i--) {
            enum opos_html_tag t = p->stack[i - 1]->tag;
            if (t == HTML_TAG_DT || t == HTML_TAG_DD) {
                pop_to(p, i - 1);
                break;
            }
            if (t == HTML_TAG_DL || t == HTML_TAG_TABLE
                || t == HTML_TAG_BLOCKQUOTE) {
                break;
            }
        }
        break;
    case HTML_TAG_OPTION:
        if (current(p)->tag == HTML_TAG_OPTION) {
            pop_to(p, p->depth - 1);
        }
        break;
    case HTML_TAG_OPTGROUP:
        if (current(p)->tag == HTML_TAG_OPTION) {
            pop_to(p, p->depth - 1);
        }
        if (current(p)->tag == HTML_TAG_OPTGROUP) {
            pop_to(p, p->depth - 1);
        }
        break;
    case HTML_TAG_TD:
    case HTML_TAG_TH:
        pop_through(p, HTML_TAG_TD, HTML_TAG_TH);
        break;
    case HTML_TAG_TR:
        pop_through(p, HTML_TAG_TR, HTML_TAG_TR);
        break;
    case HTML_TAG_THEAD:
    case HTML_TAG_TBODY:
    case HTML_TAG_TFOOT:
        if (!pop_through(p, HTML_TAG_THEAD, HTML_TAG_TFOOT)) {
            pop_through(p, HTML_TAG_TR, HTML_TAG_TR);
        }
        break;
    case HTML_TAG_CAPTION:
        pop_through(p, HTML_TAG_CAPTION, HTML_TAG_CAPTION);
        break;
    case HTML_TAG_COLGROUP:
        pop_through(p, HTML_TAG_COLGROUP, HTML_TAG_COLGROUP);
        break;
    case HTML_TAG_A: {
        size_t at = find_open(p, HTML_TAG_A, true);
        if (at != 0) {
            pop_to(p, at - 1);
        }
        break;
    }
    case HTML_TAG_BUTTON: {
        size_t at = find_open(p, HTML_TAG_BUTTON, true);
        if (at != 0) {
            pop_to(p, at - 1);
        }
        break;
    }
    default:
        break;
    }
}

static void add_attribute(struct parser *p, struct opos_html_node *node,
                          const char *source,
                          const struct raw_attribute *raw) {
    /* Names are lower-cased; the first of a duplicate wins. */
    size_t name_length = raw->name_to - raw->name_from;
    char *name = arena_alloc(&p->arena, name_length + 1);
    if (name == NULL) {
        return;
    }
    for (size_t i = 0; i < name_length; i++) {
        char c = source[raw->name_from + i];
        name[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    name[name_length] = '\0';
    struct opos_html_attr **tail = &node->attributes;
    for (; *tail != NULL; tail = &(*tail)->next) {
        if (strcmp((*tail)->name, name) == 0) {
            return;
        }
    }
    struct opos_html_attr *attr = arena_alloc(&p->arena, sizeof(*attr));
    if (attr == NULL) {
        return;
    }
    attr->name = name;
    attr->next = NULL;
    if (!raw->has_value) {
        attr->value = "";
    } else {
        p->text.length = 0;
        decode_into(&p->text, source, raw->value_from, raw->value_to, true);
        if (p->text.failed) {
            return;
        }
        attr->value = arena_string(&p->arena, p->text.data ? p->text.data : "",
                                   p->text.length);
        if (attr->value == NULL) {
            return;
        }
    }
    *tail = attr;
}

/* Lower-cased copy of a name for lookup. */
static size_t lower_name(const char *source, size_t length, char *out,
                         size_t capacity) {
    if (length >= capacity) {
        length = capacity - 1;
    }
    for (size_t i = 0; i < length; i++) {
        char c = source[i];
        out[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    out[length] = '\0';
    return length;
}

/* Formatting elements popped by an end tag come back as clones. */
#define MAX_REOPEN 8

static void handle_start(struct parser *p, const char *source,
                         const struct start_tag *tag) {
    char name[64];
    size_t name_length = lower_name(tag->name, tag->name_length, name,
                                    sizeof(name));
    const struct tag_info *info = tag_lookup(name, name_length);

    switch (info->tag) {
    case HTML_TAG_HTML:
        /* Attributes of a repeated <html> are merged into the root. */
        for (size_t i = 0; i < tag->attribute_count; i++) {
            add_attribute(p, p->doc->root, source, &tag->attributes[i]);
        }
        return;
    case HTML_TAG_HEAD:
        if (p->mode == BEFORE_HEAD) {
            ensure_head(p);
            push(p, p->doc->head);
            p->mode = IN_HEAD;
        }
        return;
    case HTML_TAG_BODY:
        if (p->mode != IN_BODY) {
            ensure_body(p);
        }
        for (size_t i = 0; i < tag->attribute_count; i++) {
            add_attribute(p, p->doc->body, source, &tag->attributes[i]);
        }
        return;
    case HTML_TAG_FRAMESET:
    case HTML_TAG_FRAME:
        return;
    default:
        break;
    }

    if (p->mode != IN_BODY) {
        if (info->flags & F_HEAD) {
            if (p->mode == BEFORE_HEAD) {
                ensure_head(p);
                push(p, p->doc->head);
                p->mode = IN_HEAD;
            } else if (p->mode == AFTER_HEAD) {
                /* Late head element: put it in the head, as browsers do. */
                p->depth = 1;
                push(p, p->doc->head);
                p->mode = IN_HEAD;
            }
        } else {
            ensure_body(p);
        }
    }

    close_for_start(p, info);

    struct opos_html_node *node = element_new(p, name, name_length, info);
    if (node == NULL || node->name == NULL) {
        return;
    }
    for (size_t i = 0; i < tag->attribute_count; i++) {
        add_attribute(p, node, source, &tag->attributes[i]);
    }
    node_append(current(p), node);

    /* Void elements never hold content. A "/>" only counts on unknown
       elements and <svg> (known HTML tags ignore the slash). */
    bool empty = (info->flags & F_VOID) != 0
              || (tag->self_closing && (info->tag == HTML_TAG_UNKNOWN
                                        || info->tag == HTML_TAG_SVG));
    if (empty) {
        return;
    }
    if (p->depth < MAX_DEPTH) {
        push(p, node);
    }
    if (info->flags & F_PRE) {
        p->drop_newline = true;
    }
}

static void handle_end(struct parser *p, const char *name_source,
                       size_t name_length_in) {
    char name[64];
    size_t name_length = lower_name(name_source, name_length_in, name,
                                    sizeof(name));
    const struct tag_info *info = tag_lookup(name, name_length);
    switch (info->tag) {
    case HTML_TAG_HTML:
    case HTML_TAG_BODY:
        return; /* the document stays open for trailing content */
    case HTML_TAG_HEAD:
        if (p->mode == IN_HEAD) {
            p->depth = 1;
            p->mode = AFTER_HEAD;
        }
        return;
    case HTML_TAG_BR:
        return;
    case HTML_TAG_P: {
        size_t at = find_open(p, HTML_TAG_P, true);
        if (at != 0) {
            pop_to(p, at - 1);
        }
        return;
    }
    default:
        break;
    }
    /* Find the matching open element. Headings end each other. */
    size_t found = 0;
    for (size_t i = p->depth; i > 1; i--) {
        struct opos_html_node *node = p->stack[i - 1];
        bool same = info->tag != HTML_TAG_UNKNOWN
                  ? (node->tag == info->tag
                     || (is_heading(info->tag) && is_heading(node->tag)))
                  : strcmp(node->name, name) == 0;
        if (same) {
            found = i;
            break;
        }
        if (info->tag >= HTML_TAG_TABLE && info->tag <= HTML_TAG_TH) {
            /* Table tags look through cells and rows, up to the table. */
            if (node->tag == HTML_TAG_TABLE) {
                return;
            }
        } else if (node->tag == HTML_TAG_TD || node->tag == HTML_TAG_TH
                   || node->tag == HTML_TAG_TABLE
                   || node->tag == HTML_TAG_CAPTION
                   || node->tag == HTML_TAG_BUTTON
                   || node->tag == HTML_TAG_OBJECT) {
            /* Other end tags never leave a cell, button or object. */
            return;
        }
    }
    if (found == 0) {
        return;
    }
    /* Formatting elements closed on the way come back after the end. */
    struct opos_html_node *reopen[MAX_REOPEN];
    size_t reopen_count = 0;
    for (size_t i = found; i < p->depth && reopen_count < MAX_REOPEN; i++) {
        struct opos_html_node *node = p->stack[i];
        if (flags_of(node) & F_FORMAT) {
            reopen[reopen_count++] = node;
        }
    }
    pop_to(p, found - 1);
    for (size_t i = 0; i < reopen_count; i++) {
        struct opos_html_node *clone = node_new(p, HTML_NODE_ELEMENT);
        if (clone == NULL) {
            break;
        }
        clone->tag = reopen[i]->tag;
        clone->name = reopen[i]->name;
        clone->attributes = reopen[i]->attributes;
        node_append(current(p), clone);
        if (p->depth < MAX_DEPTH) {
            push(p, clone);
        }
    }
}

/* ---------------------------------------------------------------- scanner */

static bool is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* Finds "</name" (case-insensitive) followed by space, '/' or '>' from pos.
   Returns the index of '<' or length when absent. */
static size_t find_raw_end(const char *s, size_t length, size_t pos,
                           const char *name) {
    size_t name_length = strlen(name);
    for (size_t i = pos; i + 2 + name_length <= length; i++) {
        if (s[i] != '<' || s[i + 1] != '/') {
            continue;
        }
        size_t j = 0;
        while (j < name_length) {
            int c = (unsigned char)s[i + 2 + j];
            if (c >= 'A' && c <= 'Z') {
                c += 32;
            }
            if (c != name[j]) {
                break;
            }
            j++;
        }
        if (j != name_length) {
            continue;
        }
        size_t after = i + 2 + name_length;
        if (after >= length || is_space((unsigned char)s[after])
            || s[after] == '/' || s[after] == '>') {
            return i;
        }
    }
    return length;
}

/* Parses attributes from pos (just after the tag name) up to '>'. Returns
   the index after '>' (or length when the tag is cut off, with *complete
   false). */
static size_t scan_attributes(const char *s, size_t length, size_t pos,
                              struct start_tag *tag, bool *complete) {
    tag->attribute_count = 0;
    tag->self_closing = false;
    *complete = false;
    while (pos < length) {
        while (pos < length && (is_space((unsigned char)s[pos])
                                || s[pos] == '/')) {
            tag->self_closing = s[pos] == '/';
            pos++;
        }
        if (pos >= length) {
            break;
        }
        if (s[pos] == '>') {
            *complete = true;
            return pos + 1;
        }
        tag->self_closing = false;
        struct raw_attribute raw = {pos, pos, 0, 0, false};
        /* The first character may be '=' (it is part of the name). */
        pos++;
        while (pos < length && !is_space((unsigned char)s[pos])
               && s[pos] != '=' && s[pos] != '>' && s[pos] != '/') {
            pos++;
        }
        raw.name_to = pos;
        size_t look = pos;
        while (look < length && is_space((unsigned char)s[look])) {
            look++;
        }
        if (look < length && s[look] == '=') {
            look++;
            while (look < length && is_space((unsigned char)s[look])) {
                look++;
            }
            raw.has_value = true;
            if (look < length && (s[look] == '"' || s[look] == '\'')) {
                char quote = s[look++];
                raw.value_from = look;
                while (look < length && s[look] != quote) {
                    look++;
                }
                raw.value_to = look;
                if (look >= length) {
                    return length; /* cut off inside the value */
                }
                look++;
            } else {
                raw.value_from = look;
                while (look < length && !is_space((unsigned char)s[look])
                       && s[look] != '>') {
                    look++;
                }
                raw.value_to = look;
            }
            pos = look;
        }
        if (tag->attribute_count < MAX_ATTRIBUTES) {
            tag->attributes[tag->attribute_count++] = raw;
        }
    }
    return length;
}

/* Adds the text between pos and end (references decoded) in the right
   place for the current mode. */
static void emit_text(struct parser *p, const char *s, size_t from, size_t to,
                      bool decode) {
    if (from >= to) {
        return;
    }
    if (p->drop_newline) {
        p->drop_newline = false;
        if (s[from] == '\n') {
            from++;
        } else if (s[from] == '\r') {
            from++;
            if (from < to && s[from] == '\n') {
                from++;
            }
        }
        if (from >= to) {
            return;
        }
    }
    p->text.length = 0;
    if (decode) {
        decode_into(&p->text, s, from, to, false);
    } else {
        buffer_put(&p->text, s + from, to - from);
    }
    if (p->text.failed) {
        return;
    }
    /* Normalise CR and CRLF to LF. */
    size_t out = 0;
    for (size_t i = 0; i < p->text.length; i++) {
        char c = p->text.data[i];
        if (c == '\r') {
            if (i + 1 < p->text.length && p->text.data[i + 1] == '\n') {
                continue;
            }
            c = '\n';
        }
        p->text.data[out++] = c;
    }
    p->text.length = out;
    if (p->mode != IN_BODY
        && (current(p) == p->doc->root || current(p) == p->doc->head)) {
        /* Between <html>, <head> and <body>: white space is dropped, and
           anything else starts the body. */
        if (only_space(p->text.data, p->text.length)) {
            return;
        }
        ensure_body(p);
    }
    add_text(p, p->text.data, p->text.length);
}

/* ------------------------------------------------------------ charset */

static bool name_matches(const char *s, size_t length, const char *name) {
    size_t n = strlen(name);
    if (length < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c += 32;
        }
        if (c != name[i]) {
            return false;
        }
    }
    return true;
}

static bool charset_is_latin1(const char *name, size_t length) {
    return name_matches(name, length, "iso-8859-1")
        || name_matches(name, length, "windows-1252")
        || name_matches(name, length, "latin1")
        || name_matches(name, length, "iso8859-1")
        || name_matches(name, length, "cp1252")
        || name_matches(name, length, "us-ascii");
}

/* Looks for charset= in the first 1024 bytes (meta tags). */
static bool sniff_latin1(const char *data, size_t length) {
    size_t limit = length < 1024 ? length : 1024;
    for (size_t i = 0; i + 8 < limit; i++) {
        if (name_matches(data + i, limit - i, "charset")) {
            size_t j = i + 7;
            while (j < limit && (is_space((unsigned char)data[j])
                                 || data[j] == '=' || data[j] == '"'
                                 || data[j] == '\'')) {
                j++;
            }
            return charset_is_latin1(data + j, limit - j);
        }
    }
    return false;
}

/* Converts Latin-1/Windows-1252 bytes to UTF-8. */
static char *latin1_to_utf8(const char *data, size_t length,
                            size_t *out_length) {
    char *out = malloc(length * 3 + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x80) {
            out[used++] = (char)c;
        } else {
            uint32_t code = c >= 0xa0 ? c : cp1252[c - 0x80];
            if (code >= 0x800) {
                out[used++] = (char)(0xe0 | code >> 12);
                out[used++] = (char)(0x80 | ((code >> 6) & 0x3f));
                out[used++] = (char)(0x80 | (code & 0x3f));
            } else {
                out[used++] = (char)(0xc0 | code >> 6);
                out[used++] = (char)(0x80 | (code & 0x3f));
            }
        }
    }
    out[used] = '\0';
    *out_length = used;
    return out;
}

/* Copies the input with every invalid UTF-8 byte replaced by U+FFFD. */
static char *sanitize_utf8(const char *data, size_t length,
                           size_t *out_length) {
    char *out = malloc(length * 3 + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t used = 0;
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)data[i];
        if (c < 0x80) {
            out[used++] = (char)c;
            i++;
            continue;
        }
        size_t need = c >= 0xc2 && c < 0xe0 ? 1 : c >= 0xe0 && c < 0xf0 ? 2
                    : c >= 0xf0 && c < 0xf5 ? 3 : 0;
        bool valid = need != 0 && i + need < length;
        for (size_t k = 1; valid && k <= need; k++) {
            valid = ((unsigned char)data[i + k] & 0xc0) == 0x80;
        }
        if (valid && need >= 2) {
            /* Overlong forms, surrogates and values above U+10FFFF. */
            unsigned char d = (unsigned char)data[i + 1];
            if ((c == 0xe0 && d < 0xa0) || (c == 0xed && d >= 0xa0)
                || (c == 0xf0 && d < 0x90) || (c == 0xf4 && d >= 0x90)) {
                valid = false;
            }
        }
        if (valid) {
            memcpy(out + used, data + i, need + 1);
            used += need + 1;
            i += need + 1;
        } else {
            out[used++] = (char)0xef;
            out[used++] = (char)0xbf;
            out[used++] = (char)0xbd;
            i++;
        }
    }
    out[used] = '\0';
    *out_length = used;
    return out;
}

/* ---------------------------------------------------------------- driver */

static void parse_all(struct parser *p, const char *s, size_t length) {
    size_t pos = 0;
    while (pos < length && !p->arena.failed) {
        if (s[pos] != '<') {
            size_t end = pos;
            while (end < length) {
                if (s[end] == '<' && end + 1 < length
                    && (is_alpha((unsigned char)s[end + 1]) || s[end + 1] == '/'
                        || s[end + 1] == '!' || s[end + 1] == '?')) {
                    break;
                }
                end++;
            }
            emit_text(p, s, pos, end, true);
            pos = end;
            continue;
        }
        if (pos + 1 >= length) {
            emit_text(p, s, pos, length, true);
            break;
        }
        unsigned char next = (unsigned char)s[pos + 1];
        if (next == '!' || next == '?') {
            if (next == '!' && pos + 3 < length && s[pos + 2] == '-'
                && s[pos + 3] == '-') {
                size_t i = pos + 4;
                if (i < length && s[i] == '>') {
                    pos = i + 1;
                    continue;
                }
                if (i + 1 < length && s[i] == '-' && s[i + 1] == '>') {
                    pos = i + 2;
                    continue;
                }
                while (i + 2 < length
                       && !(s[i] == '-' && s[i + 1] == '-'
                            && s[i + 2] == '>')) {
                    i++;
                }
                pos = i + 2 < length ? i + 3 : length;
                continue;
            }
            if (next == '!' && pos + 8 < length
                && memcmp(s + pos + 2, "[CDATA[", 7) == 0) {
                size_t i = pos + 9;
                while (i + 2 < length
                       && !(s[i] == ']' && s[i + 1] == ']'
                            && s[i + 2] == '>')) {
                    i++;
                }
                pos = i + 2 < length ? i + 3 : length;
                continue;
            }
            /* Doctype, processing instruction, bogus comment: up to '>'. */
            while (pos < length && s[pos] != '>') {
                pos++;
            }
            pos = pos < length ? pos + 1 : length;
            continue;
        }
        if (next == '/') {
            if (pos + 2 < length && is_alpha((unsigned char)s[pos + 2])) {
                size_t name_from = pos + 2;
                size_t i = name_from;
                while (i < length && !is_space((unsigned char)s[i])
                       && s[i] != '/' && s[i] != '>') {
                    i++;
                }
                handle_end(p, s + name_from, i - name_from);
                while (i < length && s[i] != '>') {
                    i++;
                }
                pos = i < length ? i + 1 : length;
            } else {
                /* "</>" and "</3": a bogus comment. */
                while (pos < length && s[pos] != '>') {
                    pos++;
                }
                pos = pos < length ? pos + 1 : length;
            }
            continue;
        }
        /* A start tag. */
        struct start_tag *tag = &p->tag;
        size_t name_from = pos + 1;
        size_t i = name_from;
        while (i < length && !is_space((unsigned char)s[i]) && s[i] != '/'
               && s[i] != '>') {
            i++;
        }
        tag->name = s + name_from;
        tag->name_length = i - name_from;
        bool complete;
        size_t after = scan_attributes(s, length, i, tag, &complete);
        if (!complete) {
            break; /* a tag cut off by the end of the file is dropped */
        }
        char name[64];
        size_t name_length = lower_name(tag->name, tag->name_length, name,
                                        sizeof(name));
        const struct tag_info *info = tag_lookup(name, name_length);
        handle_start(p, s, tag);
        pos = after;
        if (info->flags & F_RAW) {
            size_t end = find_raw_end(s, length, pos, name);
            /* <textarea> and <pre>-like: the newline rule applies. */
            emit_text(p, s, pos, end, (info->flags & F_RCDATA) != 0);
            p->drop_newline = false;
            /* Leave the raw element (it is current unless it was a void
               or dropped one). */
            if (current(p) != NULL && current(p)->tag == info->tag
                && p->depth > 1) {
                pop_to(p, p->depth - 1);
            }
            /* Skip the end tag itself. */
            if (end < length) {
                size_t j = end;
                while (j < length && s[j] != '>') {
                    j++;
                }
                pos = j < length ? j + 1 : length;
            } else {
                pos = length;
            }
        }
    }
}

static void trim_title(struct parser *p) {
    struct opos_html_node *title = opos_html_find(p->doc->root, HTML_TAG_TITLE);
    p->doc->title = "";
    if (title == NULL || title->first_child == NULL
        || title->first_child->type != HTML_NODE_TEXT) {
        return;
    }
    const char *text = title->first_child->text;
    size_t length = title->first_child->text_length;
    size_t from = 0;
    while (from < length && is_space((unsigned char)text[from])) {
        from++;
    }
    /* Collapse runs of white space, as a tab title is shown. */
    char *copy = arena_alloc(&p->arena, length - from + 1);
    if (copy == NULL) {
        return;
    }
    size_t used = 0;
    bool pending_space = false;
    for (size_t i = from; i < length; i++) {
        if (is_space((unsigned char)text[i])) {
            pending_space = used > 0;
        } else {
            if (pending_space) {
                copy[used++] = ' ';
                pending_space = false;
            }
            copy[used++] = text[i];
        }
    }
    copy[used] = '\0';
    p->doc->title = copy;
}

struct opos_html_doc *opos_html_parse(const char *data, size_t length,
                                      const char *charset) {
    char *converted = NULL;
    size_t converted_length = 0;
    bool latin1 = charset != NULL
        ? charset_is_latin1(charset, strlen(charset))
        : sniff_latin1(data, length);
    if (latin1) {
        converted = latin1_to_utf8(data, length, &converted_length);
    } else {
        converted = sanitize_utf8(data, length, &converted_length);
    }
    if (converted == NULL) {
        return NULL;
    }
    /* Skip a UTF-8 byte order mark. */
    const char *input = converted;
    size_t input_length = converted_length;
    if (input_length >= 3 && (unsigned char)input[0] == 0xef
        && (unsigned char)input[1] == 0xbb && (unsigned char)input[2] == 0xbf) {
        input += 3;
        input_length -= 3;
    }

    struct parser *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        free(converted);
        return NULL;
    }
    struct opos_html_doc *doc = arena_alloc(&p->arena, sizeof(*doc));
    if (doc == NULL) {
        free(converted);
        free(p);
        return NULL;
    }
    memset(doc, 0, sizeof(*doc));
    p->doc = doc;
    doc->root = element_new(p, "html", 4, tag_lookup("html", 4));
    doc->head = element_new(p, "head", 4, tag_lookup("head", 4));
    doc->body = element_new(p, "body", 4, tag_lookup("body", 4));
    if (doc->root == NULL || doc->head == NULL || doc->body == NULL) {
        goto fail;
    }
    p->mode = BEFORE_HEAD;
    push(p, doc->root);

    parse_all(p, input, input_length);
    if (p->arena.failed || p->text.failed) {
        goto fail;
    }
    if (!p->head_created) {
        ensure_head(p);
    }
    if (!p->body_created) {
        node_append(doc->root, doc->body);
    }
    trim_title(p);
    free(p->text.data);
    free(converted);
    /* Keep the arena for the caller; the parser struct is not needed. */
    struct arena *arena_copy = malloc(sizeof(*arena_copy));
    if (arena_copy == NULL) {
        goto fail_late;
    }
    *arena_copy = p->arena;
    doc->arena = arena_copy;
    free(p);
    return doc;

fail_late:
    for (struct chunk *c = p->arena.chunks; c != NULL;) {
        struct chunk *next = c->next;
        free(c);
        c = next;
    }
    free(p);
    return NULL;
fail:
    free(p->text.data);
    free(converted);
    for (struct chunk *c = p->arena.chunks; c != NULL;) {
        struct chunk *next = c->next;
        free(c);
        c = next;
    }
    free(p);
    return NULL;
}

void opos_html_free(struct opos_html_doc *doc) {
    if (doc == NULL) {
        return;
    }
    struct arena *arena = doc->arena;
    /* doc itself lives in the arena: copy the chunk list head first. */
    struct chunk *c = arena->chunks;
    free(arena);
    while (c != NULL) {
        struct chunk *next = c->next;
        free(c);
        c = next;
    }
}

const char *opos_html_attribute(const struct opos_html_node *node,
                                const char *name) {
    for (const struct opos_html_attr *a = node->attributes; a != NULL;
         a = a->next) {
        if (strcmp(a->name, name) == 0) {
            return a->value;
        }
    }
    return NULL;
}

struct opos_html_node *opos_html_find(struct opos_html_node *node,
                                      enum opos_html_tag tag) {
    if (node == NULL) {
        return NULL;
    }
    if (node->type == HTML_NODE_ELEMENT && node->tag == tag) {
        return node;
    }
    for (struct opos_html_node *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        struct opos_html_node *found = opos_html_find(child, tag);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}
