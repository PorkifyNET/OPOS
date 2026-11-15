/*
 * png_encode.h: writes a picture as a PNG file (8-bit RGB, no alpha).
 *
 *     size_t size;
 *     uint8_t *png = png_encode(pixels, width, height, stride, &size);
 *     ... write png[0..size) ...
 *     free(png);
 *
 * Pixels are 0x00RRGGBB. Each row gets the PNG filter that suits it best
 * (by the smallest sum of differences), then deflate compresses the rows
 * with LZ77 matches and the fixed Huffman codes. That is far smaller than
 * stored blocks for drawings, with little code. Needs malloc, realloc and
 * free; returns NULL when out of memory.
 */
#ifndef OPOS_PROGRAMS_PNG_ENCODE_H
#define OPOS_PROGRAMS_PNG_ENCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct png_out {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
    /* deflate's bit buffer, least significant bit first */
    uint32_t bits;
    int bit_count;
};

static void png_reserve(struct png_out *out, size_t more) {
    if (out->failed || out->size + more <= out->capacity) {
        return;
    }
    size_t capacity = out->capacity != 0 ? out->capacity : 4096;
    while (capacity < out->size + more) {
        capacity *= 2;
    }
    uint8_t *data = realloc(out->data, capacity);
    if (data == NULL) {
        out->failed = true;
        return;
    }
    out->data = data;
    out->capacity = capacity;
}

static void png_byte(struct png_out *out, uint8_t value) {
    png_reserve(out, 1);
    if (!out->failed) {
        out->data[out->size++] = value;
    }
}

static void png_u32(struct png_out *out, uint32_t value) {
    png_byte(out, (uint8_t)(value >> 24));
    png_byte(out, (uint8_t)(value >> 16));
    png_byte(out, (uint8_t)(value >> 8));
    png_byte(out, (uint8_t)value);
}

static uint32_t png_crc_table[256];

static uint32_t png_crc(const uint8_t *data, size_t size) {
    if (png_crc_table[1] == 0) {
        for (uint32_t n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            }
            png_crc_table[n] = c;
        }
    }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; i++) {
        crc = png_crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

/* A chunk's length and type; png_chunk_end adds the CRC over type and
   data. */
static size_t png_chunk_begin(struct png_out *out, const char *type,
                              uint32_t length) {
    png_u32(out, length);
    size_t start = out->size;
    for (int i = 0; i < 4; i++) {
        png_byte(out, (uint8_t)type[i]);
    }
    return start;
}

static void png_chunk_end(struct png_out *out, size_t start) {
    if (!out->failed) {
        png_u32(out, png_crc(out->data + start, out->size - start));
    }
}

/* ------------------------------------------------------------ deflate */

static void png_bits(struct png_out *out, uint32_t value, int count) {
    out->bits |= value << out->bit_count;
    out->bit_count += count;
    while (out->bit_count >= 8) {
        png_byte(out, (uint8_t)out->bits);
        out->bits >>= 8;
        out->bit_count -= 8;
    }
}

/* Huffman codes go most significant bit first. */
static void png_code(struct png_out *out, uint32_t code, int length) {
    uint32_t reversed = 0;
    for (int i = 0; i < length; i++) {
        reversed = reversed << 1 | ((code >> i) & 1);
    }
    png_bits(out, reversed, length);
}

/* A literal byte or length symbol (0-287) in the fixed code. */
static void png_symbol(struct png_out *out, int symbol) {
    if (symbol < 144) {
        png_code(out, 0x30 + (uint32_t)symbol, 8);
    } else if (symbol < 256) {
        png_code(out, 0x190 + (uint32_t)(symbol - 144), 9);
    } else if (symbol < 280) {
        png_code(out, (uint32_t)(symbol - 256), 7);
    } else {
        png_code(out, 0xc0 + (uint32_t)(symbol - 280), 8);
    }
}

static const uint16_t png_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t png_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const uint16_t png_distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
    12289, 16385, 24577,
};
static const uint8_t png_distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

static void png_match(struct png_out *out, int length, int distance) {
    int code = 28;
    while (png_length_base[code] > length) {
        code--;
    }
    png_symbol(out, 257 + code);
    png_bits(out, (uint32_t)(length - png_length_base[code]),
             png_length_extra[code]);
    int dcode = 29;
    while (png_distance_base[dcode] > distance) {
        dcode--;
    }
    png_code(out, (uint32_t)dcode, 5);
    png_bits(out, (uint32_t)(distance - png_distance_base[dcode]),
             png_distance_extra[dcode]);
}

#define PNG_WINDOW 32768
#define PNG_HASH_SIZE 65536
#define PNG_CHAIN 48
#define PNG_MAX_MATCH 258

static uint32_t png_hash(const uint8_t *p) {
    return ((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2])
         * 2654435761u >> 16;
}

/* One fixed-Huffman block holding all of data, in a zlib stream. */
static bool png_deflate(struct png_out *out, const uint8_t *data,
                        size_t size) {
    int32_t *head = malloc(PNG_HASH_SIZE * sizeof(int32_t));
    int32_t *previous = malloc(PNG_WINDOW * sizeof(int32_t));
    if (head == NULL || previous == NULL) {
        free(head);
        free(previous);
        return false;
    }
    for (size_t i = 0; i < PNG_HASH_SIZE; i++) {
        head[i] = -1;
    }
    png_byte(out, 0x78);
    png_byte(out, 0x9c);
    png_bits(out, 1, 1); /* the last block */
    png_bits(out, 1, 2); /* fixed codes */
    size_t at = 0;
    while (at < size && !out->failed) {
        int best_length = 0;
        size_t best_distance = 0;
        if (at + 3 <= size) {
            uint32_t hash = png_hash(data + at);
            int32_t candidate = head[hash];
            size_t limit = size - at < PNG_MAX_MATCH ? size - at
                                                       : PNG_MAX_MATCH;
            for (int chain = 0; chain < PNG_CHAIN && candidate >= 0
                 && at - (size_t)candidate <= PNG_WINDOW - 1; chain++) {
                const uint8_t *a = data + candidate, *b = data + at;
                if (a[best_length] == b[best_length]) {
                    size_t length = 0;
                    while (length < limit && a[length] == b[length]) {
                        length++;
                    }
                    if ((int)length > best_length) {
                        best_length = (int)length;
                        best_distance = at - (size_t)candidate;
                        if (length == limit) {
                            break;
                        }
                    }
                }
                int32_t next = previous[candidate % PNG_WINDOW];
                if (next >= candidate) {
                    break;
                }
                candidate = next;
            }
        }
        size_t step = best_length >= 3 ? (size_t)best_length : 1;
        if (best_length >= 3) {
            png_match(out, best_length, (int)best_distance);
        } else {
            png_symbol(out, data[at]);
        }
        for (size_t i = 0; i < step; i++, at++) {
            if (at + 3 <= size) {
                uint32_t hash = png_hash(data + at);
                previous[at % PNG_WINDOW] = head[hash];
                head[hash] = (int32_t)at;
            }
        }
    }
    png_symbol(out, 256);
    if (out->bit_count > 0) {
        png_bits(out, 0, 8 - out->bit_count);
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    png_u32(out, b << 16 | a);
    free(head);
    free(previous);
    return !out->failed;
}

/* --------------------------------------------------------------- rows */

static uint8_t png_paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

/* Filters the RGB rows into data: each row is a filter byte, then 3 *
   width bytes. */
static void png_filter_rows(const uint32_t *pixels, uint32_t width,
                            uint32_t height, uint32_t stride,
                            uint8_t *data, uint8_t *raw, uint8_t *above) {
    size_t row_bytes = (size_t)width * 3;
    memset(above, 0, row_bytes);
    uint8_t *trial = raw + row_bytes;
    for (uint32_t y = 0; y < height; y++) {
        const uint32_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            raw[x * 3] = (uint8_t)(row[x] >> 16);
            raw[x * 3 + 1] = (uint8_t)(row[x] >> 8);
            raw[x * 3 + 2] = (uint8_t)row[x];
        }
        uint8_t *out = data + (size_t)y * (row_bytes + 1);
        unsigned long best_cost = (unsigned long)-1;
        for (int filter = 0; filter < 5; filter++) {
            unsigned long cost = 0;
            for (size_t i = 0; i < row_bytes; i++) {
                int left = i >= 3 ? raw[i - 3] : 0;
                int up = above[i];
                int corner = i >= 3 ? above[i - 3] : 0;
                int predicted = filter == 0 ? 0 : filter == 1 ? left
                              : filter == 2 ? up
                              : filter == 3 ? (left + up) / 2
                              : png_paeth(left, up, corner);
                uint8_t value = (uint8_t)(raw[i] - predicted);
                trial[i] = value;
                cost += value < 128 ? value : 256 - value;
            }
            if (cost < best_cost) {
                best_cost = cost;
                out[0] = (uint8_t)filter;
                memcpy(out + 1, trial, row_bytes);
            }
        }
        memcpy(above, raw, row_bytes);
    }
}

static uint8_t *png_encode(const uint32_t *pixels, uint32_t width,
                           uint32_t height, uint32_t stride, size_t *size) {
    size_t row_bytes = (size_t)width * 3;
    size_t data_size = (row_bytes + 1) * height;
    uint8_t *data = malloc(data_size);
    uint8_t *raw = malloc(row_bytes * 2);
    uint8_t *above = malloc(row_bytes);
    struct png_out out = {0};
    if (data == NULL || raw == NULL || above == NULL || width == 0
        || height == 0) {
        free(data);
        free(raw);
        free(above);
        return NULL;
    }
    png_filter_rows(pixels, width, height, stride, data, raw, above);
    free(raw);
    free(above);
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n',
                                         0x1a, '\n'};
    for (int i = 0; i < 8; i++) {
        png_byte(&out, signature[i]);
    }
    size_t start = png_chunk_begin(&out, "IHDR", 13);
    png_u32(&out, width);
    png_u32(&out, height);
    png_byte(&out, 8); /* bits per channel */
    png_byte(&out, 2); /* RGB */
    png_byte(&out, 0);
    png_byte(&out, 0);
    png_byte(&out, 0);
    png_chunk_end(&out, start);
    /* IDAT's length is known only afterwards: written in place. */
    size_t length_at = out.size;
    start = png_chunk_begin(&out, "IDAT", 0);
    bool deflated = png_deflate(&out, data, data_size);
    free(data);
    if (!deflated || out.failed) {
        free(out.data);
        return NULL;
    }
    uint32_t idat = (uint32_t)(out.size - start - 4);
    out.data[length_at] = (uint8_t)(idat >> 24);
    out.data[length_at + 1] = (uint8_t)(idat >> 16);
    out.data[length_at + 2] = (uint8_t)(idat >> 8);
    out.data[length_at + 3] = (uint8_t)idat;
    png_chunk_end(&out, start);
    start = png_chunk_begin(&out, "IEND", 0);
    png_chunk_end(&out, start);
    if (out.failed) {
        free(out.data);
        return NULL;
    }
    *size = out.size;
    return out.data;
}

#endif
