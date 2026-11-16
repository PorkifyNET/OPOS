/*
 * Shared by the OPOS office programs (Writer, Spreadsheet): whole-file
 * reading and writing, a growing byte buffer, UTF-8 encoding, ZIP archives
 * (stored parts written, stored or deflated parts read with opos_inflate)
 * and a small XML reader, enough for Office Open XML files.
 *
 * Include once, after <stdlib.h>, <string.h>, <stdio.h>, <opos.h> and
 * <opos_http.h>.
 */
#ifndef OPOS_OOXML_H
#define OPOS_OOXML_H

#define XML_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"

/* ------------------------------------------------------------- files */

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

#ifndef ZIP_MAX_PARTS
#define ZIP_MAX_PARTS 8
#endif

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

/* -------------------------------------------------------- XML writing */

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

#endif
