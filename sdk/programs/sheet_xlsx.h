/*
 * Spreadsheet: Excel workbooks (.xlsx) and CSV files.
 *
 * Reading .xlsx: the workbook's sheet list, shared strings, styles (number
 * formats, bold, italic, underline, strikethrough, colours, solid fills,
 * horizontal alignment), column widths, cell values and formulas (shared
 * formulas are spread to their cells). Charts, pictures, merged cells,
 * conditional formats and the like are skipped, and the caller is told.
 * Formulas with functions Spreadsheet does not have keep the value saved
 * in the file.
 *
 * Writing .xlsx: one part per sheet, text as inline strings, formulas
 * with their values, the styles in use. CSV: one sheet's values.
 */
#ifndef SHEET_XLSX_H
#define SHEET_XLSX_H

#define SS_NS "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
#define REL_NS "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

static const struct {
    int id;
    const char *code;
} builtin_formats[] = {
    {0, ""}, {1, "0"}, {2, "0.00"}, {3, "#,##0"}, {4, "#,##0.00"},
    {9, "0%"}, {10, "0.00%"}, {11, "0.00E+00"}, {12, "# ?/?"},
    {13, "# ?" "?/??"}, {14, "yyyy-mm-dd"}, {15, "d-mmm-yy"}, {16, "d-mmm"},
    {17, "mmm-yy"}, {18, "h:mm AM/PM"}, {19, "h:mm:ss AM/PM"},
    {20, "h:mm"}, {21, "h:mm:ss"}, {22, "yyyy-mm-dd h:mm"},
    {37, "#,##0 ;(#,##0)"}, {38, "#,##0 ;[Red](#,##0)"},
    {39, "#,##0.00;(#,##0.00)"}, {40, "#,##0.00;[Red](#,##0.00)"},
    {45, "mm:ss"}, {46, "[h]:mm:ss"}, {47, "mm:ss.0"}, {48, "##0.0E+0"},
    {49, "@"},
};
#define BUILTIN_COUNT ((int)(sizeof(builtin_formats) / sizeof(builtin_formats[0])))

/* Attribute text: like xml_text, and quotes too. */
static void xml_attr(struct buf *b, const char *text) {
    for (const char *c = text; *c != '\0'; c++) {
        switch (*c) {
        case '&': buf_puts(b, "&amp;"); break;
        case '<': buf_puts(b, "&lt;"); break;
        case '>': buf_puts(b, "&gt;"); break;
        case '"': buf_puts(b, "&quot;"); break;
        case '\n': buf_puts(b, "&#10;"); break;
        default:
            if ((unsigned char)*c >= 0x20 || *c == '\t') {
                buf_byte(b, (uint8_t)*c);
            }
            break;
        }
    }
}

/* Element text without control characters XML does not allow. */
static void xml_cell_text(struct buf *b, const char *text) {
    size_t n = strlen(text), start = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            xml_text(b, text + start, i - start);
            start = i + 1;
        }
    }
    xml_text(b, text + start, n - start);
}

/* ------------------------------------------------------------ reading */

/* The text of an element: everything up to its end tag, decoded
   (malloc'd). */
static char *x_element_text(struct xml *x, const char *name) {
    struct buf text = {0};
    struct xtok t;
    int depth = 0;
    while (xml_next(x, &t) != X_END) {
        if (t.type == X_TEXT) {
            char *decoded = malloc(t.text_length + 1);
            if (decoded != NULL) {
                size_t n = xml_decode(t.text, t.text_length, decoded);
                buf_put(&text, decoded, n);
                free(decoded);
            }
        } else if (t.type == X_OPEN) {
            depth++;
        } else if (t.type == X_CLOSE) {
            if (depth == 0 && x_is(&t, name)) {
                break;
            }
            depth--;
        }
    }
    if (text.data == NULL) {
        return strdup("");
    }
    return text.data;
}

static int attr_int(const struct xtok *t, const char *name, int otherwise) {
    char value[32];
    if (!x_attr(t, name, value, sizeof(value)) || value[0] == '\0') {
        return otherwise;
    }
    return (int)strtol(value, NULL, 10);
}

static double attr_double(const struct xtok *t, const char *name,
                          double otherwise) {
    char value[48];
    double v;
    if (!x_attr(t, name, value, sizeof(value))
        || num_scan(value, strlen(value), &v) == 0) {
        return otherwise;
    }
    return v;
}

static const uint32_t theme_colours[10] = {
    0xffffff, 0x000000, 0xe7e6e6, 0x44546a, 0x4472c4,
    0xed7d31, 0xa5a5a5, 0xffc000, 0x5b9bd5, 0x70ad47,
};

static const uint32_t indexed_colours[64] = {
    0x000000, 0xffffff, 0xff0000, 0x00ff00, 0x0000ff, 0xffff00, 0xff00ff,
    0x00ffff, 0x000000, 0xffffff, 0xff0000, 0x00ff00, 0x0000ff, 0xffff00,
    0xff00ff, 0x00ffff, 0x800000, 0x008000, 0x000080, 0x808000, 0x800080,
    0x008080, 0xc0c0c0, 0x808080, 0x9999ff, 0x993366, 0xffffcc, 0xccffff,
    0x660066, 0xff8080, 0x0066cc, 0xccccff, 0x000080, 0xff00ff, 0xffff00,
    0x00ffff, 0x800080, 0x800000, 0x008080, 0x0000ff, 0x00ccff, 0xccffff,
    0xccffcc, 0xffff99, 0x99ccff, 0xff99cc, 0xcc99ff, 0xffcc99, 0x3366ff,
    0x33cccc, 0x99cc00, 0xffcc00, 0xff9900, 0xff6600, 0x666699, 0x969696,
    0x003366, 0x339966, 0x003300, 0x333300, 0x993300, 0x993366, 0x333399,
    0x333333,
};

/* A <color>, <fgColor> ...: 0xffffffff when it says nothing usable. */
static uint32_t read_colour(const struct xtok *t) {
    char value[32];
    uint32_t colour = 0xffffffffu;
    if (x_attr(t, "rgb", value, sizeof(value)) && strlen(value) >= 6) {
        colour = (uint32_t)strtoul(value + strlen(value) - 6, NULL, 16);
    } else if (x_attr(t, "theme", value, sizeof(value))) {
        int index = atoi(value);
        if (index >= 0 && index < 10) {
            colour = theme_colours[index];
        }
    } else if (x_attr(t, "indexed", value, sizeof(value))) {
        int index = atoi(value);
        if (index >= 0 && index < 64) {
            colour = indexed_colours[index];
        }
    }
    if (colour == 0xffffffffu) {
        return colour;
    }
    double tint = attr_double(t, "tint", 0);
    if (tint != 0) {
        int parts[3] = {(int)(colour >> 16 & 0xff), (int)(colour >> 8 & 0xff),
                        (int)(colour & 0xff)};
        for (int i = 0; i < 3; i++) {
            double c = parts[i];
            c = tint > 0 ? c + (255 - c) * tint : c * (1 + tint);
            parts[i] = c < 0 ? 0 : c > 255 ? 255 : (int)(c + 0.5);
        }
        colour = OPOS_RGB(parts[0], parts[1], parts[2]);
    }
    return colour;
}

struct xfont {
    uint8_t flags;
    uint32_t colour;
};

struct xlsx_styles {
    int *xf;             /* cellXfs index -> our style */
    int nxf;
    struct {
        int id;
        char code[FORMAT_MAX];
    } *formats;
    int nformats;
};

static const char *format_code(const struct xlsx_styles *x, int id) {
    for (int i = 0; i < x->nformats; i++) {
        if (x->formats[i].id == id) {
            return x->formats[i].code;
        }
    }
    for (int i = 0; i < BUILTIN_COUNT; i++) {
        if (builtin_formats[i].id == id) {
            return builtin_formats[i].code;
        }
    }
    /* Other built-in ids are dates or numbers by locale. */
    if (id >= 27 && id <= 36) {
        return "yyyy-mm-dd";
    }
    if (id >= 50 && id <= 58) {
        return "yyyy-mm-dd";
    }
    return "";
}

static void read_styles_part(const char *text, size_t size,
                             struct xlsx_styles *out) {
    struct xml x = {text, size, 0};
    struct xtok t;
    struct xfont *fonts = NULL;
    uint32_t *fills = NULL;
    int nfonts = 0, nfills = 0, cap_fonts = 0, cap_fills = 0, cap_xf = 0;
    int cap_formats = 0;
    char section[24] = "";
    struct xfont font = {0, 0};
    uint32_t fill = NO_FILL;
    bool solid = false;
    struct style pending_style;
    bool in_xf = false;
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN || t.type == X_EMPTY) {
            if (x_is(&t, "numFmts") || x_is(&t, "fonts") || x_is(&t, "fills")
                || x_is(&t, "cellXfs") || x_is(&t, "cellStyleXfs")
                || x_is(&t, "dxfs") || x_is(&t, "borders")) {
                if (t.type == X_OPEN) {
                    snprintf(section, sizeof(section), "%.*s",
                             (int)t.name_length, t.name);
                }
                continue;
            }
            if (strcmp(section, "numFmts") == 0 && x_is(&t, "numFmt")) {
                if (out->nformats == cap_formats) {
                    cap_formats = cap_formats == 0 ? 16 : cap_formats * 2;
                    void *grown = realloc(out->formats, (size_t)cap_formats
                                          * sizeof(*out->formats));
                    if (grown == NULL) {
                        continue;
                    }
                    out->formats = grown;
                }
                out->formats[out->nformats].id = attr_int(&t, "numFmtId", -1);
                x_attr(&t, "formatCode", out->formats[out->nformats].code,
                       FORMAT_MAX);
                if (strcmp(out->formats[out->nformats].code, "General") == 0) {
                    out->formats[out->nformats].code[0] = '\0';
                }
                out->nformats++;
            } else if (strcmp(section, "fonts") == 0) {
                if (x_is(&t, "font")) {
                    font.flags = 0;
                    font.colour = 0;
                    if (t.type == X_EMPTY) {
                        goto add_font;
                    }
                } else if (x_is(&t, "b") && x_on(&t)) {
                    font.flags |= F_BOLD;
                } else if (x_is(&t, "i") && x_on(&t)) {
                    font.flags |= F_ITALIC;
                } else if (x_is(&t, "u") && x_on(&t)) {
                    font.flags |= F_UNDER;
                } else if (x_is(&t, "strike") && x_on(&t)) {
                    font.flags |= F_STRIKE;
                } else if (x_is(&t, "color")) {
                    uint32_t colour = read_colour(&t);
                    if (colour != 0xffffffffu) {
                        font.colour = colour;
                    }
                }
            } else if (strcmp(section, "fills") == 0) {
                if (x_is(&t, "fill")) {
                    fill = NO_FILL;
                    solid = false;
                    if (t.type == X_EMPTY) {
                        goto add_fill;
                    }
                } else if (x_is(&t, "patternFill")) {
                    char type[24] = "";
                    x_attr(&t, "patternType", type, sizeof(type));
                    solid = type[0] != '\0' && strcmp(type, "none") != 0
                        && strcmp(type, "gray125") != 0;
                } else if (x_is(&t, "fgColor") && solid) {
                    fill = read_colour(&t);
                }
            } else if (strcmp(section, "cellXfs") == 0) {
                if (x_is(&t, "xf")) {
                    memset(&pending_style, 0, sizeof(pending_style));
                    pending_style.fill = NO_FILL;
                    int font_id = attr_int(&t, "fontId", 0);
                    int fill_id = attr_int(&t, "fillId", 0);
                    int format_id = attr_int(&t, "numFmtId", 0);
                    if (font_id >= 0 && font_id < nfonts) {
                        pending_style.flags = fonts[font_id].flags;
                        pending_style.colour = fonts[font_id].colour;
                    }
                    if (fill_id >= 0 && fill_id < nfills) {
                        pending_style.fill = fills[fill_id];
                    }
                    snprintf(pending_style.format, FORMAT_MAX, "%s",
                             format_code(out, format_id));
                    in_xf = true;
                    if (t.type == X_EMPTY) {
                        goto add_xf;
                    }
                } else if (x_is(&t, "alignment") && in_xf) {
                    char h[24] = "";
                    x_attr(&t, "horizontal", h, sizeof(h));
                    pending_style.align = strcmp(h, "left") == 0 ? AL_LEFT
                        : strcmp(h, "center") == 0
                            || strcmp(h, "centerContinuous") == 0 ? AL_CENTER
                        : strcmp(h, "right") == 0 ? AL_RIGHT : AL_AUTO;
                }
            }
            continue;
        }
        if (t.type != X_CLOSE) {
            continue;
        }
        if (x_is(&t, "numFmts") || x_is(&t, "fonts") || x_is(&t, "fills")
            || x_is(&t, "cellXfs") || x_is(&t, "cellStyleXfs")
            || x_is(&t, "dxfs") || x_is(&t, "borders")) {
            section[0] = '\0';
            continue;
        }
        if (strcmp(section, "fonts") == 0 && x_is(&t, "font")) {
        add_font:
            if (nfonts == cap_fonts) {
                cap_fonts = cap_fonts == 0 ? 16 : cap_fonts * 2;
                void *grown = realloc(fonts, (size_t)cap_fonts * sizeof(*fonts));
                if (grown == NULL) {
                    continue;
                }
                fonts = grown;
            }
            fonts[nfonts++] = font;
        } else if (strcmp(section, "fills") == 0 && x_is(&t, "fill")) {
        add_fill:
            if (nfills == cap_fills) {
                cap_fills = cap_fills == 0 ? 16 : cap_fills * 2;
                void *grown = realloc(fills, (size_t)cap_fills * sizeof(*fills));
                if (grown == NULL) {
                    continue;
                }
                fills = grown;
            }
            fills[nfills++] = fill;
        } else if (strcmp(section, "cellXfs") == 0 && x_is(&t, "xf")) {
        add_xf:
            in_xf = false;
            if (out->nxf == cap_xf) {
                cap_xf = cap_xf == 0 ? 32 : cap_xf * 2;
                int *grown = realloc(out->xf, (size_t)cap_xf * sizeof(int));
                if (grown == NULL) {
                    continue;
                }
                out->xf = grown;
            }
            out->xf[out->nxf++] = style_intern(&pending_style);
        }
    }
    free(fonts);
    free(fills);
}

struct shared_strings {
    char **items;
    int count;
};

static void read_shared_strings(const char *text, size_t size,
                                struct shared_strings *out) {
    struct xml x = {text, size, 0};
    struct xtok t;
    int cap = 0;
    struct buf current = {0};
    bool in_si = false, in_t = false;
    int skip = 0; /* inside <rPh> (phonetic guides) */
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN || t.type == X_EMPTY) {
            if (x_is(&t, "si")) {
                in_si = true;
                free(current.data);
                memset(&current, 0, sizeof(current));
                if (t.type == X_EMPTY) {
                    goto add;
                }
            } else if (x_is(&t, "rPh") && t.type == X_OPEN) {
                skip++;
            } else if (x_is(&t, "t") && t.type == X_OPEN) {
                in_t = true;
            }
            continue;
        }
        if (t.type == X_TEXT) {
            if (in_si && in_t && skip == 0) {
                char *decoded = malloc(t.text_length + 1);
                if (decoded != NULL) {
                    size_t n = xml_decode(t.text, t.text_length, decoded);
                    buf_put(&current, decoded, n);
                    free(decoded);
                }
            }
            continue;
        }
        if (t.type != X_CLOSE) {
            continue;
        }
        if (x_is(&t, "t")) {
            in_t = false;
        } else if (x_is(&t, "rPh")) {
            skip--;
        } else if (x_is(&t, "si")) {
        add:
            in_si = false;
            if (out->count == cap) {
                cap = cap == 0 ? 256 : cap * 2;
                char **grown = realloc(out->items, (size_t)cap * sizeof(char *));
                if (grown == NULL) {
                    break;
                }
                out->items = grown;
            }
            out->items[out->count++] = current.data != NULL ? current.data
                                                            : strdup("");
            memset(&current, 0, sizeof(current));
        }
    }
    free(current.data);
}

/* "B12" -> row 11, col 1. */
static bool parse_cell_ref(const char *s, int *row, int *col) {
    struct refpart p;
    size_t n = strlen(s);
    if (scan_refpart(s, n, 0, 0, &p) != n) {
        return false;
    }
    *row = p.row;
    *col = p.col;
    return true;
}

struct shared_formula {
    char *text;
    int row, col;
};

/* Text that would be read as something else gets a leading '. */
static char *text_input(const char *text, const char *format) {
    double number;
    char guess[FORMAT_MAX];
    size_t n = strlen(text);
    bool quote = !format_is_text(format)
        && (text[0] == '=' || text[0] == '\''
            || parse_typed_number(text, &number, guess, sizeof(guess))
            || (n == 4 && fmt_word(text, n, 0, "true"))
            || (n == 5 && fmt_word(text, n, 0, "false")));
    for (int e = 1; e <= 7 && !quote; e++) {
        quote = strcmp(text, error_names[e]) == 0;
    }
    if (!quote) {
        return strdup(text);
    }
    char *out = malloc(n + 2);
    if (out != NULL) {
        out[0] = '\'';
        memcpy(out + 1, text, n + 1);
    }
    return out;
}

static void read_sheet_part(int si, const char *text, size_t size,
                            const struct xlsx_styles *st,
                            const struct shared_strings *ss, bool *lossy) {
    struct xml x = {text, size, 0};
    struct xtok t;
    struct sheet *s = sheets[si];
    struct shared_formula *shared = NULL;
    int nshared = 0;
    int row = -1, col = -1;
    while (xml_next(&x, &t) != X_END) {
        if (t.type != X_OPEN && t.type != X_EMPTY) {
            continue;
        }
        if (x_is(&t, "col")) {
            int min = attr_int(&t, "min", 1), max = attr_int(&t, "max", min);
            double width = attr_double(&t, "width", -1);
            if (width >= 0 && min >= 1) {
                int pixels = (int)(width * 7 + 0.5);
                if (pixels < 2) {
                    pixels = 2;
                }
                if (pixels > 2000) {
                    pixels = 2000;
                }
                for (int c = min - 1; c < max && c < MAX_COLS; c++) {
                    s->widths[c] = (uint16_t)(pixels == DEFAULT_WIDTH ? 0 : pixels);
                }
            }
            continue;
        }
        if (x_is(&t, "row")) {
            int r = attr_int(&t, "r", row + 2);
            row = r - 1;
            col = -1;
            continue;
        }
        if (x_is(&t, "mergeCell") || x_is(&t, "drawing")
            || x_is(&t, "legacyDrawing") || x_is(&t, "conditionalFormatting")
            || x_is(&t, "picture") || x_is(&t, "tablePart")) {
            *lossy = true;
            continue;
        }
        if (!x_is(&t, "c")) {
            continue;
        }
        /* A cell. */
        char ref[24] = "", type[16] = "n";
        int r = row < 0 ? 0 : row, c = col + 1;
        if (x_attr(&t, "r", ref, sizeof(ref))) {
            parse_cell_ref(ref, &r, &c);
        }
        x_attr(&t, "t", type, sizeof(type));
        int xf = attr_int(&t, "s", 0);
        int style = xf >= 0 && xf < st->nxf ? st->xf[xf] : 0;
        row = r;
        col = c;
        char *value = NULL, *formula = NULL, *inline_text = NULL;
        char ftype[16] = "";
        int fsi = -1;
        bool has_formula = false;
        if (t.type == X_OPEN) {
            struct xtok k;
            while (xml_next(&x, &k) != X_END) {
                if (k.type == X_CLOSE && x_is(&k, "c")) {
                    break;
                }
                if (k.type == X_EMPTY && x_is(&k, "f")) {
                    has_formula = true;
                    x_attr(&k, "t", ftype, sizeof(ftype));
                    fsi = attr_int(&k, "si", -1);
                } else if (k.type == X_OPEN && x_is(&k, "f")) {
                    has_formula = true;
                    x_attr(&k, "t", ftype, sizeof(ftype));
                    fsi = attr_int(&k, "si", -1);
                    free(formula);
                    formula = x_element_text(&x, "f");
                } else if (k.type == X_OPEN && x_is(&k, "v")) {
                    free(value);
                    value = x_element_text(&x, "v");
                } else if (k.type == X_OPEN && x_is(&k, "is")) {
                    struct buf all = {0};
                    struct xtok q;
                    bool in_t = false;
                    int skip = 0;
                    while (xml_next(&x, &q) != X_END) {
                        if (q.type == X_CLOSE && x_is(&q, "is")) {
                            break;
                        }
                        if (q.type == X_OPEN && x_is(&q, "rPh")) {
                            skip++;
                        } else if (q.type == X_CLOSE && x_is(&q, "rPh")) {
                            skip--;
                        } else if (q.type == X_OPEN && x_is(&q, "t")) {
                            in_t = true;
                        } else if (q.type == X_CLOSE && x_is(&q, "t")) {
                            in_t = false;
                        } else if (q.type == X_TEXT && in_t && skip == 0) {
                            char *decoded = malloc(q.text_length + 1);
                            if (decoded != NULL) {
                                size_t n = xml_decode(q.text, q.text_length,
                                                      decoded);
                                buf_put(&all, decoded, n);
                                free(decoded);
                            }
                        }
                    }
                    free(inline_text);
                    inline_text = all.data != NULL ? all.data : strdup("");
                }
            }
        }
        if (r < 0 || r >= MAX_ROWS || c < 0 || c >= MAX_COLS) {
            free(value);
            free(formula);
            free(inline_text);
            continue;
        }
        /* Shared formulas: the first cell has the text, the others
           only the group number. */
        if (has_formula && strcmp(ftype, "shared") == 0 && fsi >= 0) {
            if (formula != NULL && formula[0] != '\0') {
                if (fsi >= nshared) {
                    int count = fsi + 1;
                    struct shared_formula *grown = realloc(
                        shared, (size_t)count * sizeof(*shared));
                    if (grown != NULL) {
                        memset(grown + nshared, 0,
                               (size_t)(count - nshared) * sizeof(*grown));
                        shared = grown;
                        nshared = count;
                    }
                }
                if (fsi < nshared) {
                    free(shared[fsi].text);
                    shared[fsi].text = strdup(formula);
                    shared[fsi].row = r;
                    shared[fsi].col = c;
                }
            } else if (fsi < nshared && shared[fsi].text != NULL) {
                struct rewrite rw = {RW_SHIFT, r - shared[fsi].row,
                                     c - shared[fsi].col, -1, 0, 0, NULL, si};
                free(formula);
                formula = formula_rewrite(shared[fsi].text, &rw);
            }
        }
        const char *format = styles[style].format;
        char *input = NULL;
        if (has_formula && formula != NULL && formula[0] != '\0') {
            struct rewrite rw = {RW_NORMAL, 0, 0, -1, 0, 0, NULL, si};
            char *normal = formula_rewrite(formula, &rw);
            if (normal != NULL) {
                input = malloc(strlen(normal) + 2);
                if (input != NULL) {
                    input[0] = '=';
                    strcpy(input + 1, normal);
                }
                free(normal);
            }
        } else if (strcmp(type, "s") == 0 && value != NULL) {
            int index = atoi(value);
            if (index >= 0 && index < ss->count) {
                input = text_input(ss->items[index], format);
            }
        } else if (strcmp(type, "inlineStr") == 0 && inline_text != NULL) {
            input = text_input(inline_text, format);
        } else if (strcmp(type, "str") == 0 && value != NULL) {
            input = text_input(value, format);
        } else if (strcmp(type, "b") == 0 && value != NULL) {
            input = strdup(value[0] == '1' ? "TRUE" : "FALSE");
        } else if (strcmp(type, "e") == 0 && value != NULL) {
            input = strdup(value);
        } else if (value != NULL && value[0] != '\0') {
            double number;
            if (num_scan(value, strlen(value), &number) > 0) {
                char shown[64];
                input_for_number(number, format, shown, sizeof(shown));
                input = strdup(shown);
            }
        }
        if (input != NULL || style != 0) {
            cell_put(si, r, c, input, style);
            struct cell *cell = cell_find(s, r, c);
            if (cell != NULL && cell->formula && value != NULL) {
                /* The value in the file, kept when the formula has
                   functions Spreadsheet does not have. */
                free(cell->text);
                cell->text = NULL;
                cell->err = 0;
                double number;
                if (strcmp(type, "str") == 0) {
                    cell->kind = V_STR;
                    cell->text = strdup(value);
                } else if (strcmp(type, "b") == 0) {
                    cell->kind = V_BOOL;
                    cell->num = value[0] == '1';
                } else if (strcmp(type, "e") == 0) {
                    cell->kind = V_ERR;
                    cell->err = E_VALUE;
                    for (int e = 1; e <= 7; e++) {
                        if (strcmp(value, error_names[e]) == 0) {
                            cell->err = (uint8_t)e;
                        }
                    }
                } else if (num_scan(value, strlen(value), &number) > 0) {
                    cell->kind = V_NUM;
                    cell->num = number;
                }
                cell->cached = 1;
            }
        }
        free(input);
        free(value);
        free(formula);
        free(inline_text);
    }
    for (int i = 0; i < nshared; i++) {
        free(shared[i].text);
    }
    free(shared);
}

static char *zip_text(const uint8_t *data, size_t size, const char *name,
                      size_t *length) {
    return (char *)zip_read(data, size, name, length);
}

static const char *xlsx_load(const uint8_t *data, size_t size, bool *lossy) {
    size_t length;
    char *workbook = zip_text(data, size, "xl/workbook.xml", &length);
    if (workbook == NULL) {
        return "THIS IS NOT AN EXCEL WORKBOOK OR IT IS DAMAGED";
    }
    size_t rels_length = 0;
    char *rels = zip_text(data, size, "xl/_rels/workbook.xml.rels",
                          &rels_length);
    /* Sheets: names and parts. */
    struct {
        char name[SHEET_NAME_MAX];
        char part[160];
    } *list = calloc(MAX_SHEETS, sizeof(*list));
    int count = 0;
    if (list == NULL) {
        free(workbook);
        free(rels);
        return "NOT ENOUGH MEMORY TO OPEN IT";
    }
    struct xml x = {workbook, length, 0};
    struct xtok t;
    while (xml_next(&x, &t) != X_END) {
        if ((t.type == X_EMPTY || t.type == X_OPEN) && x_is(&t, "sheet")) {
            if (count == MAX_SHEETS) {
                *lossy = true;
                break;
            }
            char id[64] = "";
            x_attr(&t, "name", list[count].name, SHEET_NAME_MAX);
            x_attr(&t, "id", id, sizeof(id));
            /* Find the part in the relationships. */
            snprintf(list[count].part, sizeof(list[count].part),
                     "xl/worksheets/sheet%d.xml", count + 1);
            if (rels != NULL) {
                struct xml rx = {rels, rels_length, 0};
                struct xtok rt;
                while (xml_next(&rx, &rt) != X_END) {
                    char rid[64] = "", target[160] = "";
                    if (!x_is(&rt, "Relationship")) {
                        continue;
                    }
                    x_attr(&rt, "Id", rid, sizeof(rid));
                    if (strcmp(rid, id) != 0) {
                        continue;
                    }
                    x_attr(&rt, "Target", target, sizeof(target));
                    if (target[0] == '/') {
                        snprintf(list[count].part, sizeof(list[count].part),
                                 "%s", target + 1);
                    } else {
                        snprintf(list[count].part, sizeof(list[count].part),
                                 "xl/%s", target);
                    }
                    break;
                }
            }
            if (list[count].name[0] == '\0' || sheet_find(list[count].name) >= 0) {
                snprintf(list[count].name, SHEET_NAME_MAX, "Sheet%d", count + 1);
            }
            int si = sheet_add(list[count].name);
            if (si < 0) {
                break;
            }
            count++;
        } else if (t.type == X_EMPTY && x_is(&t, "workbookPr")) {
            /* date1904 files are rare; their dates would be 4 years off */
        }
    }
    free(workbook);
    free(rels);
    if (count == 0) {
        free(list);
        return "THIS WORKBOOK HAS NO SHEETS";
    }
    struct xlsx_styles st = {0};
    char *styles_text = zip_text(data, size, "xl/styles.xml", &length);
    if (styles_text != NULL) {
        read_styles_part(styles_text, length, &st);
        free(styles_text);
    }
    struct shared_strings ss = {0};
    char *strings = zip_text(data, size, "xl/sharedStrings.xml", &length);
    if (strings != NULL) {
        read_shared_strings(strings, length, &ss);
        free(strings);
    }
    char *chart = zip_text(data, size, "xl/charts/chart1.xml", &length);
    if (chart != NULL) {
        *lossy = true;
        free(chart);
    }
    for (int i = 0; i < count; i++) {
        char *part = zip_text(data, size, list[i].part, &length);
        if (part == NULL) {
            /* Chart sheets and missing parts open empty. */
            *lossy = true;
            continue;
        }
        read_sheet_part(i, part, length, &st, &ss, lossy);
        free(part);
    }
    for (int i = 0; i < ss.count; i++) {
        free(ss.items[i]);
    }
    free(ss.items);
    free(st.xf);
    free(st.formats);
    free(list);
    sheets_version++;
    return NULL;
}

/* ------------------------------------------------------------ writing */

struct xfont_out {
    uint8_t flags;
    uint32_t colour;
};

static int compare_keys(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void put_hex6(struct buf *b, uint32_t colour) {
    char hex[16];
    snprintf(hex, sizeof(hex), "FF%06X", colour & 0xffffff);
    buf_puts(b, hex);
}

static void write_styles(struct buf *b, int *format_ids) {
    struct xfont_out fonts[STYLE_MAX];
    uint32_t fills[STYLE_MAX];
    int font_of[STYLE_MAX], fill_of[STYLE_MAX];
    int nfonts = 0, nfills = 0;
    /* Custom formats: ids from 164. */
    const char *customs[STYLE_MAX];
    int ncustom = 0;
    for (int i = 0; i < nstyles; i++) {
        const struct style *s = &styles[i];
        format_ids[i] = -1;
        for (int k = 0; k < BUILTIN_COUNT; k++) {
            if (strcmp(builtin_formats[k].code, s->format) == 0
                && builtin_formats[k].id != 14 && builtin_formats[k].id != 22) {
                format_ids[i] = builtin_formats[k].id;
                break;
            }
        }
        if (format_ids[i] < 0) {
            for (int k = 0; k < ncustom; k++) {
                if (strcmp(customs[k], s->format) == 0) {
                    format_ids[i] = 164 + k;
                }
            }
            if (format_ids[i] < 0) {
                customs[ncustom] = s->format;
                format_ids[i] = 164 + ncustom++;
            }
        }
        int f = -1;
        for (int k = 0; k < nfonts; k++) {
            if (fonts[k].flags == s->flags && fonts[k].colour == s->colour) {
                f = k;
            }
        }
        if (f < 0) {
            fonts[nfonts].flags = s->flags;
            fonts[nfonts].colour = s->colour;
            f = nfonts++;
        }
        font_of[i] = f;
        int fl = -1;
        if (s->fill == NO_FILL) {
            fl = 0;
        } else {
            for (int k = 0; k < nfills; k++) {
                if (fills[k] == s->fill) {
                    fl = k + 2;
                }
            }
            if (fl < 0) {
                fills[nfills] = s->fill;
                fl = 2 + nfills++;
            }
        }
        fill_of[i] = fl;
    }
    buf_puts(b, XML_HEAD "<styleSheet xmlns=\"" SS_NS "\">");
    if (ncustom > 0) {
        buf_puts(b, "<numFmts count=\"");
        buf_int(b, ncustom);
        buf_puts(b, "\">");
        for (int k = 0; k < ncustom; k++) {
            buf_puts(b, "<numFmt numFmtId=\"");
            buf_int(b, 164 + k);
            buf_puts(b, "\" formatCode=\"");
            xml_attr(b, customs[k]);
            buf_puts(b, "\"/>");
        }
        buf_puts(b, "</numFmts>");
    }
    buf_puts(b, "<fonts count=\"");
    buf_int(b, nfonts);
    buf_puts(b, "\">");
    for (int k = 0; k < nfonts; k++) {
        buf_puts(b, "<font>");
        if (fonts[k].flags & F_BOLD) {
            buf_puts(b, "<b/>");
        }
        if (fonts[k].flags & F_ITALIC) {
            buf_puts(b, "<i/>");
        }
        if (fonts[k].flags & F_STRIKE) {
            buf_puts(b, "<strike/>");
        }
        if (fonts[k].flags & F_UNDER) {
            buf_puts(b, "<u/>");
        }
        buf_puts(b, "<sz val=\"11\"/><color rgb=\"");
        put_hex6(b, fonts[k].colour);
        buf_puts(b, "\"/><name val=\"Calibri\"/><family val=\"2\"/></font>");
    }
    buf_puts(b, "</fonts><fills count=\"");
    buf_int(b, nfills + 2);
    buf_puts(b, "\"><fill><patternFill patternType=\"none\"/></fill>"
                "<fill><patternFill patternType=\"gray125\"/></fill>");
    for (int k = 0; k < nfills; k++) {
        buf_puts(b, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"");
        put_hex6(b, fills[k]);
        buf_puts(b, "\"/><bgColor indexed=\"64\"/></patternFill></fill>");
    }
    buf_puts(b, "</fills><borders count=\"1\"><border><left/><right/><top/>"
                "<bottom/><diagonal/></border></borders>"
                "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" "
                "fillId=\"0\" borderId=\"0\"/></cellStyleXfs><cellXfs count=\"");
    buf_int(b, nstyles);
    buf_puts(b, "\">");
    static const char *const aligns[4] = {NULL, "left", "center", "right"};
    for (int i = 0; i < nstyles; i++) {
        buf_puts(b, "<xf numFmtId=\"");
        buf_int(b, format_ids[i]);
        buf_puts(b, "\" fontId=\"");
        buf_int(b, font_of[i]);
        buf_puts(b, "\" fillId=\"");
        buf_int(b, fill_of[i]);
        buf_puts(b, "\" borderId=\"0\" xfId=\"0\"");
        if (i > 0) {
            buf_puts(b, " applyNumberFormat=\"1\" applyFont=\"1\" applyFill=\"1\"");
        }
        if (styles[i].align != AL_AUTO) {
            buf_puts(b, " applyAlignment=\"1\"><alignment horizontal=\"");
            buf_puts(b, aligns[styles[i].align]);
            buf_puts(b, "\"/></xf>");
        } else {
            buf_puts(b, "/>");
        }
    }
    buf_puts(b, "</cellXfs><cellStyles count=\"1\"><cellStyle name=\"Normal\" "
                "xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>");
}

static void write_sheet(struct buf *b, int si) {
    struct sheet *s = sheets[si];
    buf_puts(b, XML_HEAD "<worksheet xmlns=\"" SS_NS "\" xmlns:r=\"" REL_NS
                "\"><sheetViews><sheetView workbookViewId=\"0\"");
    if (si == active) {
        buf_puts(b, " tabSelected=\"1\"");
    }
    buf_puts(b, "><selection activeCell=\"");
    char name[24];
    cell_name(s->cur_r, s->cur_c, name);
    buf_puts(b, name);
    buf_puts(b, "\" sqref=\"");
    buf_puts(b, name);
    buf_puts(b, "\"/></sheetView></sheetViews>"
                "<sheetFormatPr defaultRowHeight=\"15\"/>");
    /* Column widths, in runs of equal widths. */
    bool any = false;
    for (int c = 0; c < MAX_COLS;) {
        if (s->widths[c] == 0) {
            c++;
            continue;
        }
        int end = c;
        while (end + 1 < MAX_COLS && s->widths[end + 1] == s->widths[c]) {
            end++;
        }
        if (!any) {
            buf_puts(b, "<cols>");
            any = true;
        }
        char width[48];
        num_fixed(s->widths[c] / 7.0, 6, width, sizeof(width));
        num_trim_zeros(width);
        buf_puts(b, "<col min=\"");
        buf_int(b, c + 1);
        buf_puts(b, "\" max=\"");
        buf_int(b, end + 1);
        buf_puts(b, "\" width=\"");
        buf_puts(b, width);
        buf_puts(b, "\" customWidth=\"1\"/>");
        c = end + 1;
    }
    if (any) {
        buf_puts(b, "</cols>");
    }
    buf_puts(b, "<sheetData>");
    uint64_t *keys = malloc((size_t)(s->count + 1) * sizeof(uint64_t));
    if (keys == NULL) {
        b->failed = true;
        return;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->cap; i++) {
        if (s->cells[i].key != 0) {
            keys[n++] = s->cells[i].key;
        }
    }
    qsort(keys, n, sizeof(uint64_t), compare_keys);
    int open_row = -1;
    for (uint32_t i = 0; i < n && !b->failed; i++) {
        int r = (int)((keys[i] - 1) / MAX_COLS), c = (int)((keys[i] - 1) % MAX_COLS);
        struct cell *cell = cell_find(s, r, c);
        if (r != open_row) {
            if (open_row >= 0) {
                buf_puts(b, "</row>");
            }
            buf_puts(b, "<row r=\"");
            buf_int(b, r + 1);
            buf_puts(b, "\">");
            open_row = r;
        }
        cell_name(r, c, name);
        buf_puts(b, "<c r=\"");
        buf_puts(b, name);
        buf_puts(b, "\"");
        if (cell->style != 0) {
            buf_puts(b, " s=\"");
            buf_int(b, cell->style);
            buf_puts(b, "\"");
        }
        if (cell->input == NULL) {
            buf_puts(b, "/>");
            continue;
        }
        char number[64];
        if (cell->formula) {
            if (cell->gen != calc_gen && !cell->cached) {
                cell_compute(si, cell);
            }
            const char *t = cell->kind == V_STR ? "str" : cell->kind == V_BOOL
                ? "b" : cell->kind == V_ERR ? "e" : NULL;
            if (t != NULL) {
                buf_puts(b, " t=\"");
                buf_puts(b, t);
                buf_puts(b, "\"");
            }
            buf_puts(b, "><f>");
            struct rewrite rw = {RW_SAVE, 0, 0, -1, 0, 0, NULL, si};
            char *saved = formula_rewrite(cell->input + 1, &rw);
            xml_cell_text(b, saved != NULL ? saved : cell->input + 1);
            free(saved);
            buf_puts(b, "</f>");
            bool loop = cell->kind == V_ERR && cell->err >= E_CYCLE;
            if (!loop && cell->kind != V_EMPTY) {
                buf_puts(b, "<v>");
                if (cell->kind == V_NUM) {
                    num_full(cell->num, number, sizeof(number));
                    buf_puts(b, number);
                } else if (cell->kind == V_STR) {
                    xml_cell_text(b, cell->text != NULL ? cell->text : "");
                } else if (cell->kind == V_BOOL) {
                    buf_puts(b, cell->num != 0 ? "1" : "0");
                } else {
                    buf_puts(b, error_names[cell->err]);
                }
                buf_puts(b, "</v>");
            }
            buf_puts(b, "</c>");
            continue;
        }
        switch (cell->kind) {
        case V_NUM:
            num_full(cell->num, number, sizeof(number));
            buf_puts(b, "><v>");
            buf_puts(b, number);
            buf_puts(b, "</v></c>");
            break;
        case V_BOOL:
            buf_puts(b, " t=\"b\"><v>");
            buf_puts(b, cell->num != 0 ? "1" : "0");
            buf_puts(b, "</v></c>");
            break;
        case V_ERR:
            buf_puts(b, " t=\"e\"><v>");
            buf_puts(b, error_names[cell->err]);
            buf_puts(b, "</v></c>");
            break;
        default:
            buf_puts(b, " t=\"inlineStr\"><is><t xml:space=\"preserve\">");
            xml_cell_text(b, cell->text != NULL ? cell->text : "");
            buf_puts(b, "</t></is></c>");
            break;
        }
    }
    if (open_row >= 0) {
        buf_puts(b, "</row>");
    }
    free(keys);
    buf_puts(b, "</sheetData><pageMargins left=\"0.7\" right=\"0.7\" "
                "top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
                "</worksheet>");
}

static bool xlsx_save(const char *file) {
    struct buf zip = {0};
    struct zip_part parts[ZIP_MAX_PARTS];
    static char names[MAX_SHEETS][40];
    int nparts = 0;
    struct buf part = {0};

    buf_puts(&part, XML_HEAD "<Types xmlns=\"http://schemas.openxmlformats.org/"
                    "package/2006/content-types\"><Default Extension=\"rels\" "
                    "ContentType=\"application/vnd.openxmlformats-package."
                    "relationships+xml\"/><Default Extension=\"xml\" "
                    "ContentType=\"application/xml\"/><Override PartName=\""
                    "/xl/workbook.xml\" ContentType=\"application/"
                    "vnd.openxmlformats-officedocument.spreadsheetml.sheet."
                    "main+xml\"/><Override PartName=\"/xl/styles.xml\" "
                    "ContentType=\"application/vnd.openxmlformats-"
                    "officedocument.spreadsheetml.styles+xml\"/>");
    for (int i = 0; i < nsheets; i++) {
        buf_puts(&part, "<Override PartName=\"/xl/worksheets/sheet");
        buf_int(&part, i + 1);
        buf_puts(&part, ".xml\" ContentType=\"application/vnd.openxmlformats-"
                        "officedocument.spreadsheetml.worksheet+xml\"/>");
    }
    buf_puts(&part, "</Types>");
    zip_add(&zip, &parts[nparts++], "[Content_Types].xml", &part);
    free(part.data);
    memset(&part, 0, sizeof(part));

    buf_puts(&part, XML_HEAD "<Relationships xmlns=\"http://schemas."
                    "openxmlformats.org/package/2006/relationships\">"
                    "<Relationship Id=\"rId1\" Type=\"" REL_NS
                    "/officeDocument\" Target=\"xl/workbook.xml\"/>"
                    "</Relationships>");
    zip_add(&zip, &parts[nparts++], "_rels/.rels", &part);
    free(part.data);
    memset(&part, 0, sizeof(part));

    buf_puts(&part, XML_HEAD "<workbook xmlns=\"" SS_NS "\" xmlns:r=\"" REL_NS
                    "\"><bookViews><workbookView activeTab=\"");
    buf_int(&part, active);
    buf_puts(&part, "\"/></bookViews><sheets>");
    for (int i = 0; i < nsheets; i++) {
        buf_puts(&part, "<sheet name=\"");
        xml_attr(&part, sheets[i]->name);
        buf_puts(&part, "\" sheetId=\"");
        buf_int(&part, i + 1);
        buf_puts(&part, "\" r:id=\"rId");
        buf_int(&part, i + 1);
        buf_puts(&part, "\"/>");
    }
    buf_puts(&part, "</sheets><calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/>"
                    "</workbook>");
    zip_add(&zip, &parts[nparts++], "xl/workbook.xml", &part);
    free(part.data);
    memset(&part, 0, sizeof(part));

    buf_puts(&part, XML_HEAD "<Relationships xmlns=\"http://schemas."
                    "openxmlformats.org/package/2006/relationships\">");
    for (int i = 0; i < nsheets; i++) {
        buf_puts(&part, "<Relationship Id=\"rId");
        buf_int(&part, i + 1);
        buf_puts(&part, "\" Type=\"" REL_NS "/worksheet\" Target=\"worksheets/"
                        "sheet");
        buf_int(&part, i + 1);
        buf_puts(&part, ".xml\"/>");
    }
    buf_puts(&part, "<Relationship Id=\"rId");
    buf_int(&part, nsheets + 1);
    buf_puts(&part, "\" Type=\"" REL_NS "/styles\" Target=\"styles.xml\"/>"
                    "</Relationships>");
    zip_add(&zip, &parts[nparts++], "xl/_rels/workbook.xml.rels", &part);
    free(part.data);
    memset(&part, 0, sizeof(part));

    int *format_ids = malloc((size_t)(nstyles + 1) * sizeof(int));
    if (format_ids == NULL) {
        free(zip.data);
        return false;
    }
    write_styles(&part, format_ids);
    free(format_ids);
    zip_add(&zip, &parts[nparts++], "xl/styles.xml", &part);
    free(part.data);
    memset(&part, 0, sizeof(part));

    for (int i = 0; i < nsheets && nparts < ZIP_MAX_PARTS; i++) {
        snprintf(names[i], sizeof(names[i]), "xl/worksheets/sheet%d.xml", i + 1);
        write_sheet(&part, i);
        if (part.failed) {
            free(part.data);
            free(zip.data);
            return false;
        }
        zip_add(&zip, &parts[nparts++], names[i], &part);
        free(part.data);
        memset(&part, 0, sizeof(part));
    }
    zip_finish(&zip, parts, nparts);
    bool ok = !zip.failed && write_whole_file(file, zip.data, zip.length);
    free(zip.data);
    return ok;
}

/* ---------------------------------------------------------------- CSV */

static bool csv_valid_utf8(const uint8_t *s, size_t length) {
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

static const char *csv_load(const uint8_t *data, size_t size) {
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        data += 3;
        size -= 3;
    }
    /* Windows-1252 files become UTF-8. */
    uint8_t *converted = NULL;
    if (!csv_valid_utf8(data, size)) {
        static const uint16_t cp1252[32] = {
            0x20ac, 0xfffd, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
            0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0xfffd, 0x017d, 0xfffd,
            0xfffd, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
            0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0xfffd, 0x017e, 0x0178,
        };
        converted = malloc(size * 3 + 1);
        if (converted == NULL) {
            return "NOT ENOUGH MEMORY TO OPEN IT";
        }
        size_t used = 0;
        for (size_t i = 0; i < size; i++) {
            uint8_t c = data[i];
            if (c < 0x80) {
                converted[used++] = c;
            } else {
                used += (size_t)utf8_put((char *)converted + used,
                                         c < 0xa0 ? cp1252[c - 0x80] : c);
            }
        }
        data = converted;
        size = used;
    }
    /* The separator: whichever of , ; and tab the first line has most. */
    int counts[3] = {0, 0, 0};
    bool quote = false;
    for (size_t i = 0; i < size && (quote || data[i] != '\n'); i++) {
        if (data[i] == '"') {
            quote = !quote;
        } else if (!quote) {
            counts[0] += data[i] == ',';
            counts[1] += data[i] == ';';
            counts[2] += data[i] == '\t';
        }
    }
    char sep = counts[1] > counts[0] && counts[1] >= counts[2] ? ';'
        : counts[2] > counts[0] ? '\t' : ',';
    int si = nsheets > 0 ? 0 : sheet_add("Sheet1");
    if (si < 0) {
        free(converted);
        return "NOT ENOUGH MEMORY TO OPEN IT";
    }
    char *field = malloc(INPUT_MAX + 1);
    if (field == NULL) {
        free(converted);
        return "NOT ENOUGH MEMORY TO OPEN IT";
    }
    int r = 0, c = 0;
    size_t i = 0;
    while (i <= size) {
        size_t used = 0;
        bool quoted = false;
        if (i < size && data[i] == '"') {
            quoted = true;
            i++;
            while (i < size) {
                if (data[i] == '"') {
                    if (i + 1 < size && data[i + 1] == '"') {
                        if (used < INPUT_MAX) {
                            field[used++] = '"';
                        }
                        i += 2;
                        continue;
                    }
                    i++;
                    break;
                }
                if (data[i] != '\r' && used < INPUT_MAX) {
                    field[used++] = (char)data[i];
                }
                i++;
            }
        }
        while (i < size && data[i] != (uint8_t)sep && data[i] != '\n') {
            if (data[i] != '\r' && used < INPUT_MAX) {
                field[used++] = (char)data[i];
            }
            i++;
        }
        field[used] = '\0';
        if (used > 0 && r < MAX_ROWS && c < MAX_COLS) {
            if (quoted) {
                /* Quoted text stays text unless it reads as a number. */
                double number;
                char format[FORMAT_MAX];
                if (field[0] == '=' || !parse_typed_number(field, &number, format,
                                                           sizeof(format))) {
                    char *input = text_input(field, "");
                    cell_put(si, r, c, input, 0);
                    free(input);
                } else {
                    cell_type(si, r, c, field);
                }
            } else {
                cell_type(si, r, c, field);
            }
        }
        if (i >= size) {
            break;
        }
        if (data[i] == '\n') {
            r++;
            c = 0;
        } else {
            c++;
        }
        i++;
        if (i == size) {
            break; /* a last line break */
        }
    }
    free(field);
    free(converted);
    return NULL;
}

static bool csv_save(const char *file, int si) {
    struct sheet *s = sheets[si];
    int rows, cols;
    sheet_extent(s, &rows, &cols);
    struct buf out = {0};
    int was = active;
    active = si;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (c > 0) {
                buf_byte(&out, ',');
            }
            struct cell *cell = cell_find(s, r, c);
            if (cell == NULL || cell->input == NULL) {
                continue;
            }
            struct look look;
            cell_look(cell, 17, &look);
            const char *text = look.shown.text;
            bool quote = any_of(text, ",\"\n\r") != NULL
                || (cell->kind == V_STR && text[0] == '=');
            if (!quote) {
                buf_puts(&out, text);
                continue;
            }
            buf_byte(&out, '"');
            for (const char *k = text; *k != '\0'; k++) {
                if (*k == '"') {
                    buf_byte(&out, '"');
                }
                buf_byte(&out, (uint8_t)*k);
            }
            buf_byte(&out, '"');
        }
        buf_puts(&out, "\r\n");
    }
    active = was;
    if (out.failed) {
        free(out.data);
        return false;
    }
    bool ok = write_whole_file(file, out.data != NULL ? out.data : "",
                               out.length);
    free(out.data);
    return ok;
}

#endif
