/*
 * Word documents (.docx, Office Open XML) for Writer.
 *
 * Saving writes a ZIP with stored (uncompressed) parts: the document,
 * its styles (Normal, Title, Heading 1-3) and list numbering. Every run
 * carries the formatting that differs from its paragraph style, so Word,
 * LibreOffice and Google Docs show what Writer showed.
 *
 * Opening reads word/document.xml, word/styles.xml and word/numbering.xml
 * (stored or deflated) and keeps paragraphs, paragraph styles, alignment,
 * lists and character formatting. Pictures, tables (their text is kept),
 * text boxes and the like are left out; docx_load reports that as lossy.
 *
 * Included by writer.c after the document model.
 */

/* ------------------------------------------------------------- ZIP */

static uint32_t zip_crc_table[256];

static uint32_t zip_crc(const void *data, size_t size) {
    if (zip_crc_table[1] == 0) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            zip_crc_table[n] = c;
        }
    }
    const uint8_t *bytes = data;
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; i++) {
        crc = zip_crc_table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

static void put16(struct buf *b, uint32_t v) {
    buf_byte(b, (uint8_t)v);
    buf_byte(b, (uint8_t)(v >> 8));
}

static void put32(struct buf *b, uint32_t v) {
    put16(b, v & 0xffff);
    put16(b, v >> 16);
}

#define ZIP_MAX_PARTS 8

struct zip_part {
    const char *name;
    uint32_t crc, size, offset;
};

/* Appends a stored part; date 2026-01-01 00:00 (DOS time). */
static void zip_add(struct buf *zip, struct zip_part *part, const char *name,
                    const struct buf *data) {
    part->name = name;
    part->crc = zip_crc(data->data, data->length);
    part->size = (uint32_t)data->length;
    part->offset = (uint32_t)zip->length;
    put32(zip, 0x04034b50);
    put16(zip, 20);         /* version needed */
    put16(zip, 0);          /* flags */
    put16(zip, 0);          /* stored */
    put16(zip, 0);          /* time */
    put16(zip, (46 << 9) | (1 << 5) | 1);
    put32(zip, part->crc);
    put32(zip, part->size);
    put32(zip, part->size);
    put16(zip, (uint32_t)strlen(name));
    put16(zip, 0);
    buf_puts(zip, name);
    buf_put(zip, data->data != NULL ? data->data : "", data->length);
}

static void zip_finish(struct buf *zip, const struct zip_part *parts,
                       int count) {
    uint32_t start = (uint32_t)zip->length;
    for (int i = 0; i < count; i++) {
        const struct zip_part *part = &parts[i];
        put32(zip, 0x02014b50);
        put16(zip, 20);     /* made by */
        put16(zip, 20);     /* version needed */
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, (46 << 9) | (1 << 5) | 1);
        put32(zip, part->crc);
        put32(zip, part->size);
        put32(zip, part->size);
        put16(zip, (uint32_t)strlen(part->name));
        put16(zip, 0);      /* extra */
        put16(zip, 0);      /* comment */
        put16(zip, 0);      /* disk */
        put16(zip, 0);      /* internal attributes */
        put32(zip, 0);      /* external attributes */
        put32(zip, part->offset);
        buf_puts(zip, part->name);
    }
    uint32_t size = (uint32_t)zip->length - start;
    put32(zip, 0x06054b50);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, (uint32_t)count);
    put16(zip, (uint32_t)count);
    put32(zip, size);
    put32(zip, start);
    put16(zip, 0);
}

/* Finds a part and returns its bytes (malloc'd, NUL-terminated), or
   NULL. */
static uint8_t *zip_read(const uint8_t *zip, size_t size, const char *name,
                         size_t *out_size) {
    if (size < 22) {
        return NULL;
    }
    size_t end = size - 22;
    size_t lowest = size > 65557 ? size - 65557 : 0;
    for (;;) {
        if (zip[end] == 0x50 && zip[end + 1] == 0x4b && zip[end + 2] == 5
            && zip[end + 3] == 6) {
            break;
        }
        if (end == lowest) {
            return NULL;
        }
        end--;
    }
#define RD16(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8)
#define RD32(p) (RD16(p) | RD16((p) + 2) << 16)
    uint32_t count = RD16(zip + end + 10);
    size_t at = RD32(zip + end + 16);
    size_t name_length = strlen(name);
    for (uint32_t i = 0; i < count; i++) {
        if (at + 46 > size || RD32(zip + at) != 0x02014b50) {
            return NULL;
        }
        const uint8_t *entry = zip + at;
        uint32_t method = RD16(entry + 10);
        size_t packed = RD32(entry + 20);
        size_t unpacked = RD32(entry + 24);
        size_t n = RD16(entry + 28);
        size_t extra = RD16(entry + 30);
        size_t comment = RD16(entry + 32);
        size_t local = RD32(entry + 42);
        if (at + 46 + n > size) {
            return NULL;
        }
        if (n == name_length && memcmp(entry + 46, name, n) == 0) {
            if (local + 30 > size || RD32(zip + local) != 0x04034b50) {
                return NULL;
            }
            size_t data = local + 30 + RD16(zip + local + 26)
                + RD16(zip + local + 28);
            if (data > size || packed > size - data
                || unpacked > 256u * 1024 * 1024) {
                return NULL;
            }
            if (method == 0) {
                uint8_t *out = malloc(packed + 1);
                if (out == NULL) {
                    return NULL;
                }
                memcpy(out, zip + data, packed);
                out[packed] = 0;
                *out_size = packed;
                return out;
            }
            if (method == 8) {
                uint8_t *out = NULL;
                size_t length = 0;
                if (opos_inflate(zip + data, packed, 1, unpacked + 1, &out,
                                 &length) != 0) {
                    free(out);
                    return NULL;
                }
                *out_size = length;
                return out;
            }
            return NULL;
        }
        at += 46 + n + extra + comment;
    }
    return NULL;
#undef RD16
#undef RD32
}

/* ------------------------------------------------------------ saving */

#define W_NS "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
#define XML_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"

static const char *const style_ids[K_COUNT] = {"Normal", "Title", "Heading1",
                                               "Heading2", "Heading3"};
static const char *const style_names[K_COUNT] = {"Normal", "Title",
                                                 "heading 1", "heading 2",
                                                 "heading 3"};

static void xml_text(struct buf *b, const char *text, size_t length) {
    size_t start = 0;
    for (size_t i = 0; i < length; i++) {
        const char *entity = text[i] == '&' ? "&amp;" : text[i] == '<'
            ? "&lt;" : text[i] == '>' ? "&gt;" : NULL;
        if (entity != NULL) {
            buf_put(b, text + start, i - start);
            buf_puts(b, entity);
            start = i + 1;
        }
    }
    buf_put(b, text + start, length - start);
}

static void put_hex_colour(struct buf *b, uint32_t colour) {
    char hex[8];
    snprintf(hex, sizeof(hex), "%06X", colour & 0xffffff);
    buf_puts(b, hex);
}

static void put_run_props(struct buf *b, cstyle s, cstyle base) {
    uint32_t f = ST_FLAGS(s), bf = ST_FLAGS(base);
    struct buf props = {0};
    if (f & S_MONO) {
        buf_puts(&props, "<w:rFonts w:ascii=\"Noto Sans Mono\" "
                         "w:hAnsi=\"Noto Sans Mono\" w:cs=\"Noto Sans Mono\"/>");
    }
    if ((f & S_BOLD) != (bf & S_BOLD)) {
        buf_puts(&props, f & S_BOLD ? "<w:b/>" : "<w:b w:val=\"0\"/>");
    }
    if (f & S_ITALIC) {
        buf_puts(&props, "<w:i/>");
    }
    if (f & S_STRIKE) {
        buf_puts(&props, "<w:strike/>");
    }
    if (ST_COLOR(s) != ST_COLOR(base)) {
        buf_puts(&props, "<w:color w:val=\"");
        put_hex_colour(&props, ST_COLOR(s));
        buf_puts(&props, "\"/>");
    }
    if (ST_HALF(s) != ST_HALF(base)) {
        buf_puts(&props, "<w:sz w:val=\"");
        buf_int(&props, ST_HALF(s));
        buf_puts(&props, "\"/><w:szCs w:val=\"");
        buf_int(&props, ST_HALF(s));
        buf_puts(&props, "\"/>");
    }
    if (f & S_UNDER) {
        buf_puts(&props, "<w:u w:val=\"single\"/>");
    }
    if (props.failed) {
        b->failed = true;
    } else if (props.length > 0) {
        buf_puts(b, "<w:rPr>");
        buf_put(b, props.data, props.length);
        buf_puts(b, "</w:rPr>");
    }
    free(props.data);
}

static void put_run(struct buf *b, const struct para *p, int from, int to) {
    cstyle base = kind_style(p->kind);
    buf_puts(b, "<w:r>");
    put_run_props(b, p->st[from], base);
    int start = from;
    for (int i = from; i <= to; i++) {
        bool special = i < to && (p->text[i] == '\t' || p->text[i] == '\n');
        if (i == to || special) {
            if (i > start) {
                buf_puts(b, "<w:t xml:space=\"preserve\">");
                xml_text(b, p->text + start, (size_t)(i - start));
                buf_puts(b, "</w:t>");
            }
            if (special) {
                buf_puts(b, p->text[i] == '\t' ? "<w:tab/>" : "<w:br/>");
            }
            start = i + 1;
        }
    }
    buf_puts(b, "</w:r>");
}

static bool docx_save(const char *file) {
    struct buf doc = {0}, styles = {0}, numbering = {0}, types = {0},
               rels = {0}, doc_rels = {0}, zip = {0};
    /* Numbered lists restart after other paragraphs: each block gets its
       own numbering instance (ids 2 and up; 1 is the bullets). */
    int blocks = 0;
    buf_puts(&doc, XML_HEAD "<w:document " W_NS "><w:body>");
    for (int i = 0; i < npara; i++) {
        const struct para *p = &paras[i];
        int num_id = 0;
        if (p->list == L_BULLET) {
            num_id = 1;
        } else if (p->list == L_NUMBER) {
            bool starts = true;
            for (int k = i - 1; k >= 0; k--) {
                if (paras[k].list == L_NONE) {
                    break;
                }
                if (paras[k].list == L_NUMBER) {
                    starts = false;
                    break;
                }
            }
            if (starts) {
                blocks++;
            }
            num_id = 1 + blocks;
        }
        buf_puts(&doc, "<w:p>");
        bool tight = p->list != L_NONE && i + 1 < npara
            && paras[i + 1].list != L_NONE;
        bool mark = p->len == 0 && p->end_style != kind_style(p->kind);
        if (p->kind != K_NORMAL || num_id != 0 || p->align != A_LEFT
            || tight || mark) {
            buf_puts(&doc, "<w:pPr>");
            if (p->kind != K_NORMAL) {
                buf_puts(&doc, "<w:pStyle w:val=\"");
                buf_puts(&doc, style_ids[p->kind]);
                buf_puts(&doc, "\"/>");
            }
            if (num_id != 0) {
                buf_puts(&doc, "<w:numPr><w:ilvl w:val=\"");
                buf_int(&doc, p->level);
                buf_puts(&doc, "\"/><w:numId w:val=\"");
                buf_int(&doc, num_id);
                buf_puts(&doc, "\"/></w:numPr>");
            }
            if (tight) {
                buf_puts(&doc, "<w:spacing w:after=\"40\"/>");
            }
            if (p->align != A_LEFT) {
                static const char *const jc[4] = {"left", "center", "right",
                                                  "both"};
                buf_puts(&doc, "<w:jc w:val=\"");
                buf_puts(&doc, jc[p->align]);
                buf_puts(&doc, "\"/>");
            }
            /* The paragraph mark's look, so an empty paragraph keeps its
               size. */
            if (mark) {
                put_run_props(&doc, p->end_style, kind_style(p->kind));
            }
            buf_puts(&doc, "</w:pPr>");
        }
        int start = 0;
        for (int k = 1; k <= p->len; k++) {
            if (k == p->len || p->st[k] != p->st[start]) {
                put_run(&doc, p, start, k);
                start = k;
            }
        }
        buf_puts(&doc, "</w:p>");
    }
    buf_puts(&doc, "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
                   "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
                   "w:left=\"1440\" w:header=\"708\" w:footer=\"708\" "
                   "w:gutter=\"0\"/></w:sectPr></w:body></w:document>");

    buf_puts(&styles, XML_HEAD "<w:styles " W_NS "><w:docDefaults>"
             "<w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Noto Sans\" "
             "w:hAnsi=\"Noto Sans\" w:eastAsia=\"Noto Sans\" "
             "w:cs=\"Noto Sans\"/><w:sz w:val=\"24\"/><w:szCs w:val=\"24\"/>"
             "</w:rPr></w:rPrDefault><w:pPrDefault><w:pPr>"
             "<w:spacing w:after=\"160\" w:line=\"276\" w:lineRule=\"auto\"/>"
             "</w:pPr></w:pPrDefault></w:docDefaults>");
    for (int k = 0; k < K_COUNT; k++) {
        buf_puts(&styles, "<w:style w:type=\"paragraph\" ");
        if (k == K_NORMAL) {
            buf_puts(&styles, "w:default=\"1\" ");
        }
        buf_puts(&styles, "w:styleId=\"");
        buf_puts(&styles, style_ids[k]);
        buf_puts(&styles, "\"><w:name w:val=\"");
        buf_puts(&styles, style_names[k]);
        buf_puts(&styles, "\"/>");
        if (k != K_NORMAL) {
            buf_puts(&styles, "<w:basedOn w:val=\"Normal\"/>"
                              "<w:next w:val=\"Normal\"/>");
        }
        buf_puts(&styles, "<w:qFormat/>");
        if (k != K_NORMAL) {
            buf_puts(&styles, "<w:pPr>");
            if (k >= K_H1) {
                buf_puts(&styles, "<w:keepNext/>");
            }
            buf_puts(&styles, "<w:spacing w:before=\"");
            buf_int(&styles, kinds[k].before * 20);
            buf_puts(&styles, "\" w:after=\"");
            buf_int(&styles, kinds[k].after * 20);
            buf_puts(&styles, "\"/>");
            if (k >= K_H1) {
                buf_puts(&styles, "<w:outlineLvl w:val=\"");
                buf_int(&styles, k - K_H1);
                buf_puts(&styles, "\"/>");
            }
            buf_puts(&styles, "</w:pPr><w:rPr>");
            if (kinds[k].flags & S_BOLD) {
                buf_puts(&styles, "<w:b/><w:bCs/>");
            }
            buf_puts(&styles, "<w:color w:val=\"");
            put_hex_colour(&styles, kinds[k].colour);
            buf_puts(&styles, "\"/><w:sz w:val=\"");
            buf_int(&styles, kinds[k].half);
            buf_puts(&styles, "\"/><w:szCs w:val=\"");
            buf_int(&styles, kinds[k].half);
            buf_puts(&styles, "\"/></w:rPr>");
        }
        buf_puts(&styles, "</w:style>");
    }
    buf_puts(&styles, "</w:styles>");

    buf_puts(&numbering, XML_HEAD "<w:numbering " W_NS ">");
    static const char *const bullets[3] = {"\xe2\x80\xa2", "\xe2\x97\xa6",
                                           "\xe2\x96\xaa"};
    static const char *const formats[3] = {"decimal", "lowerLetter",
                                           "lowerRoman"};
    for (int a = 0; a < 2; a++) {
        buf_puts(&numbering, "<w:abstractNum w:abstractNumId=\"");
        buf_int(&numbering, a);
        buf_puts(&numbering, "\"><w:multiLevelType w:val=\"hybridMultilevel\"/>");
        for (int l = 0; l < 9; l++) {
            buf_puts(&numbering, "<w:lvl w:ilvl=\"");
            buf_int(&numbering, l);
            buf_puts(&numbering, "\"><w:start w:val=\"1\"/><w:numFmt w:val=\"");
            buf_puts(&numbering, a == 0 ? "bullet" : formats[l % 3]);
            buf_puts(&numbering, "\"/><w:lvlText w:val=\"");
            if (a == 0) {
                buf_puts(&numbering, bullets[l % 3]);
            } else {
                buf_puts(&numbering, "%");
                buf_int(&numbering, l + 1);
                buf_puts(&numbering, ".");
            }
            buf_puts(&numbering, "\"/><w:lvlJc w:val=\"left\"/><w:pPr>"
                                 "<w:ind w:left=\"");
            buf_int(&numbering, 720 * (l + 1));
            buf_puts(&numbering, "\" w:hanging=\"360\"/></w:pPr></w:lvl>");
        }
        buf_puts(&numbering, "</w:abstractNum>");
    }
    buf_puts(&numbering, "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/>"
                         "</w:num>");
    for (int n = 0; n < blocks; n++) {
        buf_puts(&numbering, "<w:num w:numId=\"");
        buf_int(&numbering, n + 2);
        buf_puts(&numbering, "\"><w:abstractNumId w:val=\"1\"/>");
        for (int l = 0; l < 9; l++) {
            buf_puts(&numbering, "<w:lvlOverride w:ilvl=\"");
            buf_int(&numbering, l);
            buf_puts(&numbering, "\"><w:startOverride w:val=\"1\"/>"
                                 "</w:lvlOverride>");
        }
        buf_puts(&numbering, "</w:num>");
    }
    buf_puts(&numbering, "</w:numbering>");

    buf_puts(&types, XML_HEAD "<Types xmlns=\"http://schemas.openxmlformats.org/"
             "package/2006/content-types\"><Default Extension=\"rels\" "
             "ContentType=\"application/vnd.openxmlformats-package."
             "relationships+xml\"/><Default Extension=\"xml\" "
             "ContentType=\"application/xml\"/><Override "
             "PartName=\"/word/document.xml\" ContentType=\"application/"
             "vnd.openxmlformats-officedocument.wordprocessingml.document."
             "main+xml\"/><Override PartName=\"/word/styles.xml\" "
             "ContentType=\"application/vnd.openxmlformats-officedocument."
             "wordprocessingml.styles+xml\"/><Override "
             "PartName=\"/word/numbering.xml\" ContentType=\"application/"
             "vnd.openxmlformats-officedocument.wordprocessingml."
             "numbering+xml\"/></Types>");
    buf_puts(&rels, XML_HEAD "<Relationships xmlns=\"http://schemas."
             "openxmlformats.org/package/2006/relationships\"><Relationship "
             "Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
             "officeDocument/2006/relationships/officeDocument\" "
             "Target=\"word/document.xml\"/></Relationships>");
    buf_puts(&doc_rels, XML_HEAD "<Relationships xmlns=\"http://schemas."
             "openxmlformats.org/package/2006/relationships\"><Relationship "
             "Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/"
             "officeDocument/2006/relationships/styles\" "
             "Target=\"styles.xml\"/><Relationship Id=\"rId2\" "
             "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
             "relationships/numbering\" Target=\"numbering.xml\"/>"
             "</Relationships>");

    struct zip_part parts[ZIP_MAX_PARTS];
    zip_add(&zip, &parts[0], "[Content_Types].xml", &types);
    zip_add(&zip, &parts[1], "_rels/.rels", &rels);
    zip_add(&zip, &parts[2], "word/document.xml", &doc);
    zip_add(&zip, &parts[3], "word/_rels/document.xml.rels", &doc_rels);
    zip_add(&zip, &parts[4], "word/styles.xml", &styles);
    zip_add(&zip, &parts[5], "word/numbering.xml", &numbering);
    zip_finish(&zip, parts, 6);
    bool ok = !(doc.failed || styles.failed || numbering.failed
                || types.failed || rels.failed || doc_rels.failed
                || zip.failed)
        && write_whole_file(file, zip.data, zip.length);
    free(doc.data);
    free(styles.data);
    free(numbering.data);
    free(types.data);
    free(rels.data);
    free(doc_rels.data);
    free(zip.data);
    return ok;
}

/* ----------------------------------------------------------- XML */

enum { X_END, X_OPEN, X_CLOSE, X_EMPTY, X_TEXT };

struct xml {
    const char *s;
    size_t n, i;
};

struct xtok {
    int type;
    const char *name;   /* local name, without the prefix */
    size_t name_length;
    const char *attrs;
    size_t attrs_length;
    const char *text;
    size_t text_length;
};

static bool x_is(const struct xtok *t, const char *name) {
    size_t length = strlen(name);
    return t->name_length == length && memcmp(t->name, name, length) == 0;
}

static int xml_next(struct xml *x, struct xtok *t) {
    memset(t, 0, sizeof(*t));
    for (;;) {
        if (x->i >= x->n) {
            return t->type = X_END;
        }
        if (x->s[x->i] != '<') {
            size_t start = x->i;
            while (x->i < x->n && x->s[x->i] != '<') {
                x->i++;
            }
            t->text = x->s + start;
            t->text_length = x->i - start;
            return t->type = X_TEXT;
        }
        const char *rest = x->s + x->i;
        size_t left = x->n - x->i;
        if (left >= 4 && memcmp(rest, "<!--", 4) == 0) {
            const char *end = NULL;
            for (size_t k = 4; k + 3 <= left; k++) {
                if (memcmp(rest + k, "-->", 3) == 0) {
                    end = rest + k + 3;
                    break;
                }
            }
            x->i = end != NULL ? (size_t)(end - x->s) : x->n;
            continue;
        }
        if (left >= 9 && memcmp(rest, "<![CDATA[", 9) == 0) {
            size_t k = 9;
            while (k + 3 <= left && memcmp(rest + k, "]]>", 3) != 0) {
                k++;
            }
            t->text = rest + 9;
            t->text_length = k - 9;
            x->i += k + 3 <= left ? k + 3 : left;
            return t->type = X_TEXT;
        }
        size_t k = 1;
        char quote = 0;
        while (k < left && (quote != 0 || rest[k] != '>')) {
            if (quote != 0 && rest[k] == quote) {
                quote = 0;
            } else if (quote == 0 && (rest[k] == '"' || rest[k] == '\'')) {
                quote = rest[k];
            }
            k++;
        }
        x->i += k < left ? k + 1 : left;
        if (rest[1] == '?' || rest[1] == '!') {
            continue;
        }
        size_t a = 1;
        int type = X_OPEN;
        if (rest[1] == '/') {
            type = X_CLOSE;
            a = 2;
        }
        size_t name_start = a;
        while (a < k && rest[a] != ' ' && rest[a] != '\t' && rest[a] != '\n'
               && rest[a] != '\r' && rest[a] != '/' && rest[a] != '>') {
            a++;
        }
        const char *name = rest + name_start;
        size_t name_length = a - name_start;
        for (size_t c = 0; c < name_length; c++) {
            if (name[c] == ':') {
                name += c + 1;
                name_length -= c + 1;
                break;
            }
        }
        size_t attrs_end = k;
        if (type == X_OPEN && k > 1 && rest[k - 1] == '/') {
            type = X_EMPTY;
            attrs_end = k - 1;
        }
        t->type = type;
        t->name = name;
        t->name_length = name_length;
        t->attrs = rest + a;
        t->attrs_length = attrs_end > a ? attrs_end - a : 0;
        return type;
    }
}

/* Decodes XML text (entities) into out as UTF-8; returns the length. */
static size_t xml_decode(const char *in, size_t length, char *out) {
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        if (in[i] != '&') {
            out[used++] = in[i];
            continue;
        }
        size_t end = i + 1;
        while (end < length && end < i + 12 && in[end] != ';') {
            end++;
        }
        if (end >= length || in[end] != ';') {
            out[used++] = in[i];
            continue;
        }
        const char *e = in + i + 1;
        size_t n = end - i - 1;
        uint32_t code = 0;
        if (n == 3 && memcmp(e, "amp", 3) == 0) {
            code = '&';
        } else if (n == 2 && memcmp(e, "lt", 2) == 0) {
            code = '<';
        } else if (n == 2 && memcmp(e, "gt", 2) == 0) {
            code = '>';
        } else if (n == 4 && memcmp(e, "quot", 4) == 0) {
            code = '"';
        } else if (n == 4 && memcmp(e, "apos", 4) == 0) {
            code = '\'';
        } else if (n >= 2 && e[0] == '#') {
            char digits[12];
            memcpy(digits, e + 1, n - 1);
            digits[n - 1] = '\0';
            code = digits[0] == 'x' || digits[0] == 'X'
                ? (uint32_t)strtoul(digits + 1, NULL, 16)
                : (uint32_t)strtoul(digits, NULL, 10);
            if (code == 0 || code > 0x10ffff) {
                code = 0xfffd;
            }
        }
        if (code == 0) {
            out[used++] = in[i];
            continue;
        }
        used += (size_t)utf8_put(out + used, code);
        i = end;
    }
    return used;
}

/* The value of an attribute by local name (w:val -> "val"), decoded. */
static bool x_attr(const struct xtok *t, const char *local, char *out,
                   size_t capacity) {
    const char *a = t->attrs, *end = t->attrs + t->attrs_length;
    size_t want = strlen(local);
    while (a < end) {
        while (a < end && (*a == ' ' || *a == '\t' || *a == '\n'
                           || *a == '\r')) {
            a++;
        }
        const char *name = a;
        while (a < end && *a != '=' && *a != ' ') {
            a++;
        }
        const char *name_end = a;
        while (a < end && *a != '"' && *a != '\'') {
            a++;
        }
        if (a >= end) {
            return false;
        }
        char quote = *a++;
        const char *value = a;
        while (a < end && *a != quote) {
            a++;
        }
        const char *value_end = a;
        if (a < end) {
            a++;
        }
        const char *local_name = name;
        for (const char *c = name; c < name_end; c++) {
            if (*c == ':') {
                local_name = c + 1;
            }
        }
        if ((size_t)(name_end - local_name) == want
            && memcmp(local_name, local, want) == 0) {
            size_t length = (size_t)(value_end - value);
            if (length >= capacity) {
                length = capacity - 1;
            }
            char raw[256];
            if (length >= sizeof(raw)) {
                length = sizeof(raw) - 1;
            }
            memcpy(raw, value, length);
            size_t used = xml_decode(raw, length, out);
            out[used < capacity ? used : capacity - 1] = '\0';
            return true;
        }
    }
    return false;
}

/* An on/off property: <w:b/>, <w:b w:val="0"/>, "false", "off". */
static bool x_on(const struct xtok *t) {
    char value[16];
    if (!x_attr(t, "val", value, sizeof(value))) {
        return true;
    }
    return !(strcmp(value, "0") == 0 || strcmp(value, "false") == 0
             || strcmp(value, "off") == 0 || strcmp(value, "none") == 0);
}

/* ------------------------------------------------------------ opening */

/* Character properties as read: which flags are given and their values,
   the size (0: not given) and the colour (-1: not given). */
struct rprops {
    uint32_t mask, flags;
    uint32_t half;
    long colour;
};

static void rprops_apply(struct rprops *to, const struct rprops *from) {
    to->flags = (to->flags & ~from->mask) | (from->flags & from->mask);
    to->mask |= from->mask;
    if (from->half != 0) {
        to->half = from->half;
    }
    if (from->colour >= 0) {
        to->colour = from->colour;
    }
}

static void rprops_set(struct rprops *r, uint32_t flag, bool on) {
    r->mask |= flag;
    if (on) {
        r->flags |= flag;
    } else {
        r->flags &= ~flag;
    }
}

static bool mono_font_name(const char *name) {
    static const char *const monos[] = {"Mono", "Courier", "Consolas",
                                        "Menlo", "Lucida Console", "Monaco",
                                        "Code", "Fixed"};
    for (size_t i = 0; i < sizeof(monos) / sizeof(monos[0]); i++) {
        if (strstr(name, monos[i]) != NULL) {
            return true;
        }
    }
    return false;
}

/* Reads one run property element into r; false if it is not one. */
static bool read_rprop(const struct xtok *t, struct rprops *r) {
    char value[64];
    if (x_is(t, "b")) {
        rprops_set(r, S_BOLD, x_on(t));
    } else if (x_is(t, "i")) {
        rprops_set(r, S_ITALIC, x_on(t));
    } else if (x_is(t, "strike") || x_is(t, "dstrike")) {
        rprops_set(r, S_STRIKE, x_on(t));
    } else if (x_is(t, "u")) {
        rprops_set(r, S_UNDER, x_on(t));
    } else if (x_is(t, "sz")) {
        if (x_attr(t, "val", value, sizeof(value))) {
            long half = strtol(value, NULL, 10);
            if (half >= 2 && half <= 3276) {
                r->half = (uint32_t)half;
            }
        }
    } else if (x_is(t, "color")) {
        if (x_attr(t, "val", value, sizeof(value))) {
            r->colour = strcmp(value, "auto") == 0
                ? 0 : (long)(strtoul(value, NULL, 16) & 0xffffff);
        }
    } else if (x_is(t, "rFonts")) {
        if (x_attr(t, "ascii", value, sizeof(value))
            || x_attr(t, "hAnsi", value, sizeof(value))) {
            rprops_set(r, S_MONO, mono_font_name(value));
        }
    } else {
        return false;
    }
    return true;
}

#define DOCX_STYLES 512
#define DOCX_NUMS 512

struct dstyle {
    char id[48];
    char based[48];
    bool character;
    int kind;            /* from the name: Title, heading 1-9; -1 none */
    int align;           /* -1: not given */
    int num_id, level;   /* -1: not given */
    struct rprops r;
};

struct docx_reader {
    struct dstyle *styles;
    int nstyles;
    int default_para;    /* index of the default paragraph style, or -1 */
    struct rprops defaults;
    /* numbering: numId -> abstractNumId -> format per level */
    int num_abstract[DOCX_NUMS];
    uint8_t abstract_bullet[DOCX_NUMS][9];
};

static int find_style(const struct docx_reader *d, const char *id) {
    for (int i = 0; i < d->nstyles; i++) {
        if (strcmp(d->styles[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* A style's properties with those of the styles it is based on. */
static void resolve_style(const struct docx_reader *d, int index,
                          struct rprops *r, int *kind, int *align,
                          int *num_id, int *level, int depth) {
    if (index < 0 || depth > 10) {
        return;
    }
    const struct dstyle *s = &d->styles[index];
    if (s->based[0] != '\0') {
        resolve_style(d, find_style(d, s->based), r, kind, align, num_id,
                      level, depth + 1);
    }
    rprops_apply(r, &s->r);
    if (s->kind >= 0) {
        *kind = s->kind;
    }
    if (s->align >= 0) {
        *align = s->align;
    }
    if (s->num_id >= 0) {
        *num_id = s->num_id;
    }
    if (s->level >= 0) {
        *level = s->level;
    }
}

static int kind_from_name(const char *name) {
    char lower[48];
    size_t n = 0;
    for (; name[n] != '\0' && n < sizeof(lower) - 1; n++) {
        lower[n] = (char)tolower((unsigned char)name[n]);
    }
    lower[n] = '\0';
    if (strcmp(lower, "title") == 0) {
        return K_TITLE;
    }
    if (strncmp(lower, "heading ", 8) == 0 && lower[8] >= '1'
        && lower[8] <= '9' && lower[9] == '\0') {
        return lower[8] == '1' ? K_H1 : lower[8] == '2' ? K_H2 : K_H3;
    }
    return -1;
}

static int align_from(const char *value) {
    if (strcmp(value, "center") == 0) {
        return A_CENTER;
    }
    if (strcmp(value, "right") == 0 || strcmp(value, "end") == 0) {
        return A_RIGHT;
    }
    if (strcmp(value, "both") == 0 || strcmp(value, "distribute") == 0) {
        return A_JUSTIFY;
    }
    return A_LEFT;
}

static void read_styles(struct docx_reader *d, const char *xml_data,
                        size_t length) {
    struct xml x = {xml_data, length, 0};
    struct xtok t;
    struct dstyle *s = NULL;
    bool in_rpr = false, in_defaults_rpr = false, in_ppr = false;
    char value[64];
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN || t.type == X_EMPTY) {
            if (x_is(&t, "style") && t.type == X_OPEN) {
                if (d->nstyles == DOCX_STYLES) {
                    s = NULL;
                    continue;
                }
                s = &d->styles[d->nstyles++];
                memset(s, 0, sizeof(*s));
                s->kind = s->align = s->num_id = s->level = -1;
                s->r.colour = -1;
                x_attr(&t, "styleId", s->id, sizeof(s->id));
                if (x_attr(&t, "type", value, sizeof(value))) {
                    s->character = strcmp(value, "character") == 0;
                }
                if (x_attr(&t, "default", value, sizeof(value))
                    && !s->character
                    && (strcmp(value, "1") == 0
                        || strcmp(value, "true") == 0)) {
                    d->default_para = d->nstyles - 1;
                }
            } else if (x_is(&t, "rPrDefault")) {
                in_defaults_rpr = true;
            } else if (x_is(&t, "rPr")) {
                in_rpr = true;
            } else if (x_is(&t, "pPr")) {
                in_ppr = true;
            } else if (in_defaults_rpr && s == NULL) {
                read_rprop(&t, &d->defaults);
            } else if (s != NULL && in_rpr) {
                read_rprop(&t, &s->r);
            } else if (s != NULL && x_is(&t, "name")) {
                if (x_attr(&t, "val", value, sizeof(value))) {
                    s->kind = kind_from_name(value);
                }
            } else if (s != NULL && x_is(&t, "basedOn")) {
                x_attr(&t, "val", s->based, sizeof(s->based));
            } else if (s != NULL && in_ppr && x_is(&t, "jc")) {
                if (x_attr(&t, "val", value, sizeof(value))) {
                    s->align = align_from(value);
                }
            } else if (s != NULL && in_ppr && x_is(&t, "numId")) {
                if (x_attr(&t, "val", value, sizeof(value))) {
                    s->num_id = (int)strtol(value, NULL, 10);
                }
            } else if (s != NULL && in_ppr && x_is(&t, "ilvl")) {
                if (x_attr(&t, "val", value, sizeof(value))) {
                    s->level = (int)strtol(value, NULL, 10);
                }
            }
        } else if (t.type == X_CLOSE) {
            if (x_is(&t, "style")) {
                s = NULL;
            } else if (x_is(&t, "rPrDefault")) {
                in_defaults_rpr = false;
            } else if (x_is(&t, "rPr")) {
                in_rpr = false;
            } else if (x_is(&t, "pPr")) {
                in_ppr = false;
            }
        }
    }
}

static void read_numbering(struct docx_reader *d, const char *xml_data,
                           size_t length) {
    struct xml x = {xml_data, length, 0};
    struct xtok t;
    int abstract = -1, level = -1, num = -1;
    char value[64];
    while (xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN || t.type == X_EMPTY) {
            if (x_is(&t, "abstractNum")) {
                abstract = x_attr(&t, "abstractNumId", value, sizeof(value))
                    ? (int)strtol(value, NULL, 10) : -1;
                if (abstract < 0 || abstract >= DOCX_NUMS) {
                    abstract = -1;
                }
            } else if (x_is(&t, "lvl") && abstract >= 0) {
                level = x_attr(&t, "ilvl", value, sizeof(value))
                    ? (int)strtol(value, NULL, 10) : -1;
                if (level < 0 || level >= 9) {
                    level = -1;
                }
            } else if (x_is(&t, "numFmt") && abstract >= 0 && level >= 0) {
                if (x_attr(&t, "val", value, sizeof(value))) {
                    d->abstract_bullet[abstract][level]
                        = strcmp(value, "bullet") == 0 ? 1
                        : strcmp(value, "none") == 0 ? 0 : 2;
                }
            } else if (x_is(&t, "num")) {
                num = x_attr(&t, "numId", value, sizeof(value))
                    ? (int)strtol(value, NULL, 10) : -1;
                if (num < 0 || num >= DOCX_NUMS) {
                    num = -1;
                }
            } else if (x_is(&t, "abstractNumId") && num >= 0) {
                if (x_attr(&t, "val", value, sizeof(value))) {
                    int a = (int)strtol(value, NULL, 10);
                    if (a >= 0 && a < DOCX_NUMS) {
                        d->num_abstract[num] = a;
                    }
                }
            }
        } else if (t.type == X_CLOSE) {
            if (x_is(&t, "abstractNum")) {
                abstract = -1;
            } else if (x_is(&t, "lvl")) {
                level = -1;
            } else if (x_is(&t, "num")) {
                num = -1;
            }
        }
    }
}

static int list_kind(const struct docx_reader *d, int num_id, int level) {
    if (num_id <= 0 || num_id >= DOCX_NUMS) {
        return L_NONE;
    }
    int a = d->num_abstract[num_id];
    if (a < 0 || level < 0 || level >= 9) {
        return L_NONE;
    }
    int format = d->abstract_bullet[a][level];
    return format == 1 ? L_BULLET : format == 2 ? L_NUMBER : L_NONE;
}

static cstyle cstyle_from(const struct rprops *r) {
    uint32_t half = r->half != 0 ? r->half : 20;
    return st_make(r->flags & r->mask, half,
                   r->colour >= 0 ? (uint32_t)r->colour : 0);
}

/* State while reading word/document.xml. */
struct body_reader {
    struct para *p;
    struct rprops para_r;   /* the paragraph style's character look */
    int para_style;
    int align, num_id, level; /* given in the paragraph; -1: from style */
    struct rprops run_r;    /* direct run formatting */
    struct rprops mark_r;   /* the paragraph mark's own formatting */
    int run_style;          /* character style, or -1 */
    bool in_ppr, in_rpr, in_ppr_rpr, in_text;
};

/* Applies the paragraph style and the paragraph's own properties. */
static void body_finish_ppr(struct docx_reader *d, struct body_reader *b) {
    b->para_r = d->defaults;
    int kind = K_NORMAL, align = A_LEFT, num_id = 0, level = 0;
    resolve_style(d, b->para_style >= 0 ? b->para_style : d->default_para,
                  &b->para_r, &kind, &align, &num_id, &level, 0);
    if (b->align >= 0) {
        align = b->align;
    }
    if (b->num_id >= 0) {
        num_id = b->num_id;
    }
    if (b->level >= 0) {
        level = b->level;
    }
    struct para *p = b->p;
    p->kind = (uint8_t)kind;
    p->align = (uint8_t)align;
    int list = list_kind(d, num_id, level);
    p->list = (uint8_t)list;
    p->level = (uint8_t)(list == L_NONE ? 0 : level >= LIST_LEVELS
                         ? LIST_LEVELS - 1 : level < 0 ? 0 : level);
    struct rprops mark = b->para_r;
    rprops_apply(&mark, &b->mark_r);
    p->end_style = cstyle_from(&mark);
}

static cstyle body_run_style(struct docx_reader *d, struct body_reader *b) {
    struct rprops r = b->para_r;
    if (b->run_style >= 0) {
        int kind = 0, align = 0, num = 0, level = 0;
        resolve_style(d, b->run_style, &r, &kind, &align, &num, &level, 0);
    }
    rprops_apply(&r, &b->run_r);
    return cstyle_from(&r);
}

static const char *docx_load(const uint8_t *data, size_t size, bool *lossy) {
    size_t doc_size = 0, styles_size = 0, numbering_size = 0;
    uint8_t *doc = zip_read(data, size, "word/document.xml", &doc_size);
    if (doc == NULL) {
        return "THIS IS NOT A WORD DOCUMENT WRITER CAN READ";
    }
    struct docx_reader *d = calloc(1, sizeof(*d));
    if (d != NULL) {
        d->styles = calloc(DOCX_STYLES, sizeof(*d->styles));
    }
    if (d == NULL || d->styles == NULL) {
        if (d != NULL) {
            free(d->styles);
        }
        free(d);
        free(doc);
        return "NOT ENOUGH MEMORY TO OPEN IT";
    }
    d->default_para = -1;
    d->defaults.colour = -1;
    d->defaults.half = 20;
    for (int i = 0; i < DOCX_NUMS; i++) {
        d->num_abstract[i] = -1;
    }
    uint8_t *styles = zip_read(data, size, "word/styles.xml", &styles_size);
    if (styles != NULL) {
        read_styles(d, (const char *)styles, styles_size);
        free(styles);
    }
    uint8_t *numbering = zip_read(data, size, "word/numbering.xml",
                                  &numbering_size);
    if (numbering != NULL) {
        read_numbering(d, (const char *)numbering, numbering_size);
        free(numbering);
    }

    doc_clear();
    struct xml x = {(const char *)doc, doc_size, 0};
    struct xtok t;
    struct body_reader b;
    memset(&b, 0, sizeof(b));
    b.run_style = -1;
    int depth = 0, skip_depth = -1;
    bool in_body = false, ok = true;
    char value[64];
    char *decoded = NULL;
    size_t decoded_cap = 0;
    while (ok && xml_next(&x, &t) != X_END) {
        if (t.type == X_OPEN) {
            depth++;
        }
        if (skip_depth >= 0) {
            if (t.type == X_CLOSE) {
                depth--;
                if (depth < skip_depth) {
                    skip_depth = -1;
                }
            }
            continue;
        }
        if (t.type == X_CLOSE) {
            depth--;
        }
        if (!in_body) {
            if (t.type == X_OPEN && x_is(&t, "body")) {
                in_body = true;
            }
            continue;
        }
        if (t.type == X_OPEN || t.type == X_EMPTY) {
            if (x_is(&t, "drawing") || x_is(&t, "pict") || x_is(&t, "object")
                || x_is(&t, "AlternateContent")) {
                *lossy = true;
                if (t.type == X_OPEN) {
                    skip_depth = depth;
                }
                continue;
            }
            if (x_is(&t, "tbl")) {
                *lossy = true; /* the cells' paragraphs are read */
            } else if (x_is(&t, "sectPr") && b.p == NULL) {
                if (t.type == X_OPEN) {
                    skip_depth = depth;
                }
            } else if (x_is(&t, "p")) {
                b.p = doc_insert_para(npara, K_NORMAL);
                if (b.p == NULL) {
                    ok = false;
                    break;
                }
                b.para_style = -1;
                b.align = -1;
                b.num_id = -1;
                b.level = -1;
                memset(&b.mark_r, 0, sizeof(b.mark_r));
                b.mark_r.colour = -1;
                body_finish_ppr(d, &b);
                if (t.type == X_EMPTY) {
                    b.p = NULL;
                }
            } else if (b.p == NULL) {
                continue;
            } else if (x_is(&t, "pPr")) {
                b.in_ppr = t.type == X_OPEN;
            } else if (b.in_ppr && x_is(&t, "rPr")) {
                b.in_ppr_rpr = t.type == X_OPEN;
            } else if (b.in_ppr_rpr) {
                read_rprop(&t, &b.mark_r);
            } else if (b.in_ppr) {
                if (x_is(&t, "pStyle")
                    && x_attr(&t, "val", value, sizeof(value))) {
                    b.para_style = find_style(d, value);
                } else if (x_is(&t, "jc")
                           && x_attr(&t, "val", value, sizeof(value))) {
                    b.align = align_from(value);
                } else if (x_is(&t, "numId")
                           && x_attr(&t, "val", value, sizeof(value))) {
                    b.num_id = (int)strtol(value, NULL, 10);
                } else if (x_is(&t, "ilvl")
                           && x_attr(&t, "val", value, sizeof(value))) {
                    b.level = (int)strtol(value, NULL, 10);
                }
            } else if (x_is(&t, "r")) {
                memset(&b.run_r, 0, sizeof(b.run_r));
                b.run_r.colour = -1;
                b.run_style = -1;
            } else if (x_is(&t, "rPr")) {
                b.in_rpr = t.type == X_OPEN;
            } else if (b.in_rpr) {
                if (x_is(&t, "rStyle")
                    && x_attr(&t, "val", value, sizeof(value))) {
                    b.run_style = find_style(d, value);
                } else {
                    read_rprop(&t, &b.run_r);
                }
            } else if (x_is(&t, "t") && t.type == X_OPEN) {
                b.in_text = true;
            } else if (x_is(&t, "tab") || x_is(&t, "br") || x_is(&t, "cr")
                       || x_is(&t, "noBreakHyphen")) {
                const char *what = x_is(&t, "tab") ? "\t"
                    : x_is(&t, "noBreakHyphen") ? "-" : "\n";
                if (!para_insert(b.p, b.p->len, what, 1, NULL,
                                 body_run_style(d, &b))) {
                    ok = false;
                }
            } else if (x_is(&t, "txbxContent") || x_is(&t, "footnoteReference")
                       || x_is(&t, "commentReference")) {
                *lossy = true;
                if (t.type == X_OPEN) {
                    skip_depth = depth;
                }
            }
        } else if (t.type == X_CLOSE) {
            if (x_is(&t, "body")) {
                break;
            }
            if (b.p == NULL) {
                continue;
            }
            if (x_is(&t, "p")) {
                b.p = NULL;
            } else if (x_is(&t, "pPr")) {
                b.in_ppr = false;
                b.in_ppr_rpr = false;
                body_finish_ppr(d, &b);
            } else if (x_is(&t, "rPr")) {
                if (b.in_ppr) {
                    b.in_ppr_rpr = false;
                } else {
                    b.in_rpr = false;
                }
            } else if (x_is(&t, "t")) {
                b.in_text = false;
            }
        } else if (t.type == X_TEXT && b.in_text && b.p != NULL) {
            if (t.text_length + 1 > decoded_cap) {
                decoded_cap = t.text_length + 64;
                char *grown = realloc(decoded, decoded_cap);
                if (grown == NULL) {
                    ok = false;
                    break;
                }
                decoded = grown;
            }
            size_t n = xml_decode(t.text, t.text_length, decoded);
            /* Control characters other than tabs are left out. */
            size_t w = 0;
            for (size_t k = 0; k < n; k++) {
                if ((unsigned char)decoded[k] >= 0x20 || decoded[k] == '\t') {
                    decoded[w++] = decoded[k];
                }
            }
            if (doc_bytes() + w > TEXT_MAX
                || !para_insert(b.p, b.p->len, decoded, (int)w, NULL,
                                body_run_style(d, &b))) {
                ok = false;
            }
        }
    }
    free(decoded);
    free(d->styles);
    free(d);
    free(doc);
    if (!ok) {
        return "THE DOCUMENT IS TOO LARGE TO OPEN";
    }
    if (!in_body) {
        return "THIS IS NOT A WORD DOCUMENT WRITER CAN READ";
    }
    return NULL;
}
