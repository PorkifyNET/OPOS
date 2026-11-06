#include "image.h"

/*
 * PNG, JPEG and BMP decoders. Everything works out of one caller-supplied
 * scratch buffer (an arena), so the code runs in the kernel and in user
 * programs alike. Files are untrusted: every read is checked against the
 * input size, every write against the buffer it goes into.
 */

/* ---------------------------------------------------------------- helpers */

#define ALIGN16(value) (((value) + 15u) & ~(size_t)15u)

struct arena {
    uint8_t *base;
    size_t size;
    size_t used;
};

static void *arena_take(struct arena *arena, size_t bytes) {
    bytes = ALIGN16(bytes);
    if (arena->used > arena->size || bytes > arena->size - arena->used) {
        return NULL;
    }
    void *block = arena->base + arena->used;
    arena->used += bytes;
    return block;
}

static void bytes_zero(void *memory, size_t length) {
    uint8_t *bytes = memory;
    for (size_t i = 0; i < length; i++) {
        bytes[i] = 0;
    }
}

static void bytes_copy(void *destination, const void *source, size_t length) {
    uint8_t *to = destination;
    const uint8_t *from = source;
    for (size_t i = 0; i < length; i++) {
        to[i] = from[i];
    }
}

static uint32_t read_be32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16
         | (uint32_t)bytes[2] << 8 | bytes[3];
}

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] << 8 | bytes[1]);
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[3] << 24 | (uint32_t)bytes[2] << 16
         | (uint32_t)bytes[1] << 8 | bytes[0];
}

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)(bytes[1] << 8 | bytes[0]);
}

static uint8_t clamp_byte(int32_t value) {
    return value < 0 ? 0 : value > 255 ? 255 : (uint8_t)value;
}

static uint32_t pixel_argb(uint8_t alpha, uint8_t red, uint8_t green,
                           uint8_t blue) {
    return (uint32_t)alpha << 24 | (uint32_t)red << 16
         | (uint32_t)green << 8 | blue;
}

static bool dimensions_ok(uint32_t width, uint32_t height) {
    return width != 0 && height != 0 && width <= IMAGE_MAX_DIMENSION
        && height <= IMAGE_MAX_DIMENSION
        && (uint64_t)width * height <= IMAGE_MAX_PIXELS;
}

static size_t pixel_bytes(uint32_t width, uint32_t height) {
    return ALIGN16((size_t)width * height * 4);
}

/* ---------------------------------------------------------------- inflate */

/*
 * RFC 1950/1951 decoder. Codes up to 9 bits come from a lookup table;
 * longer ones are walked bit by bit (canonical code order).
 */

#define FAST_BITS 9
#define FAST_SIZE (1u << FAST_BITS)

struct huffman {
    /* symbol | length << 9, or 0 when the code is longer than 9 bits. */
    uint16_t fast[FAST_SIZE];
    uint16_t count[16];
    uint16_t symbol[288];
};

struct bit_reader {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint64_t buffer;
    unsigned count;
    /* Zero bytes appended after the end of the input. */
    unsigned padding;
};

static void bits_fill(struct bit_reader *bits) {
    while (bits->count <= 56) {
        uint64_t byte = 0;
        if (bits->position < bits->size) {
            byte = bits->data[bits->position++];
        } else {
            bits->padding++;
        }
        bits->buffer |= byte << bits->count;
        bits->count += 8;
    }
}

static uint32_t bits_take(struct bit_reader *bits, unsigned count) {
    if (count == 0) {
        return 0;
    }
    if (bits->count < count) {
        bits_fill(bits);
    }
    uint32_t value = (uint32_t)(bits->buffer & ((1ull << count) - 1));
    bits->buffer >>= count;
    bits->count -= count;
    return value;
}

/* True once bits past the end of the input were consumed (a little slack
   is left for the final partial byte). */
static bool bits_overrun(const struct bit_reader *bits) {
    return (uint64_t)bits->padding * 8 > (uint64_t)bits->count + 32;
}

static unsigned reverse_bits(unsigned code, unsigned length) {
    unsigned result = 0;
    for (unsigned i = 0; i < length; i++) {
        result = (result << 1) | (code & 1);
        code >>= 1;
    }
    return result;
}

/* Builds a decoder from code lengths. Incomplete codes are allowed
   (deflate uses them for a single distance code); oversubscribed ones
   are not. */
static bool huffman_build(struct huffman *table, const uint8_t *lengths,
                          unsigned symbols) {
    bytes_zero(table, sizeof(*table));
    for (unsigned i = 0; i < symbols; i++) {
        table->count[lengths[i]]++;
    }
    table->count[0] = 0;
    int left = 1;
    for (unsigned length = 1; length < 16; length++) {
        left <<= 1;
        left -= table->count[length];
        if (left < 0) {
            return false;
        }
    }
    uint16_t offset[16];
    offset[1] = 0;
    for (unsigned length = 1; length < 15; length++) {
        offset[length + 1] = (uint16_t)(offset[length] + table->count[length]);
    }
    uint16_t next_code[16];
    unsigned code = 0;
    next_code[0] = 0;
    for (unsigned length = 1; length < 16; length++) {
        code = (code + table->count[length - 1]) << 1;
        next_code[length] = (uint16_t)code;
    }
    for (unsigned symbol = 0; symbol < symbols; symbol++) {
        unsigned length = lengths[symbol];
        if (length == 0) {
            continue;
        }
        table->symbol[offset[length]++] = (uint16_t)symbol;
        unsigned this_code = next_code[length]++;
        if (length <= FAST_BITS) {
            unsigned reversed = reverse_bits(this_code, length);
            for (unsigned j = reversed; j < FAST_SIZE; j += 1u << length) {
                table->fast[j] = (uint16_t)(symbol | length << 9);
            }
        }
    }
    return true;
}

static int huffman_decode(struct bit_reader *bits,
                          const struct huffman *table) {
    if (bits->count < 16) {
        bits_fill(bits);
    }
    uint16_t entry = table->fast[bits->buffer & (FAST_SIZE - 1)];
    if (entry != 0) {
        unsigned length = entry >> 9;
        bits->buffer >>= length;
        bits->count -= length;
        return entry & 511;
    }
    int code = 0;
    int first = 0;
    int index = 0;
    for (unsigned length = 1; length < 16; length++) {
        code |= (int)((bits->buffer >> (length - 1)) & 1);
        int count = table->count[length];
        if (code - count < first) {
            bits->buffer >>= length;
            bits->count -= length;
            return table->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

struct inflate_state {
    struct huffman literals;
    struct huffman distances;
    uint8_t lengths[288 + 32];
};

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};
static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool inflate_codes(struct bit_reader *bits, struct inflate_state *state,
                          uint8_t *out, size_t out_size, size_t *out_pos) {
    size_t pos = *out_pos;
    for (;;) {
        int symbol = huffman_decode(bits, &state->literals);
        if (symbol < 0 || bits_overrun(bits)) {
            return false;
        }
        if (symbol < 256) {
            if (pos >= out_size) {
                return false;
            }
            out[pos++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 256) {
            *out_pos = pos;
            return true;
        }
        symbol -= 257;
        if (symbol >= 29) {
            return false;
        }
        size_t length = length_base[symbol]
                      + bits_take(bits, length_extra[symbol]);
        int code = huffman_decode(bits, &state->distances);
        if (code < 0 || code >= 30) {
            return false;
        }
        size_t distance = distance_base[code]
                        + bits_take(bits, distance_extra[code]);
        if (distance > pos || length > out_size - pos) {
            return false;
        }
        const uint8_t *from = out + pos - distance;
        for (size_t i = 0; i < length; i++) {
            out[pos + i] = from[i];
        }
        pos += length;
    }
}

static bool inflate_dynamic(struct bit_reader *bits,
                            struct inflate_state *state) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                      11, 4, 12, 3, 13, 2, 14, 1, 15};
    unsigned literal_count = bits_take(bits, 5) + 257;
    unsigned distance_count = bits_take(bits, 5) + 1;
    unsigned code_count = bits_take(bits, 4) + 4;
    if (literal_count > 286 || distance_count > 30) {
        return false;
    }
    uint8_t code_lengths[19];
    bytes_zero(code_lengths, sizeof(code_lengths));
    for (unsigned i = 0; i < code_count; i++) {
        code_lengths[order[i]] = (uint8_t)bits_take(bits, 3);
    }
    if (!huffman_build(&state->literals, code_lengths, 19)) {
        return false;
    }
    unsigned total = literal_count + distance_count;
    unsigned index = 0;
    while (index < total) {
        int symbol = huffman_decode(bits, &state->literals);
        if (symbol < 0 || bits_overrun(bits)) {
            return false;
        }
        if (symbol < 16) {
            state->lengths[index++] = (uint8_t)symbol;
            continue;
        }
        uint8_t value = 0;
        unsigned repeat;
        if (symbol == 16) {
            if (index == 0) {
                return false;
            }
            value = state->lengths[index - 1];
            repeat = 3 + bits_take(bits, 2);
        } else if (symbol == 17) {
            repeat = 3 + bits_take(bits, 3);
        } else {
            repeat = 11 + bits_take(bits, 7);
        }
        if (repeat > total - index) {
            return false;
        }
        while (repeat-- != 0) {
            state->lengths[index++] = value;
        }
    }
    if (state->lengths[256] == 0) {
        return false;
    }
    return huffman_build(&state->literals, state->lengths, literal_count)
        && huffman_build(&state->distances, state->lengths + literal_count,
                         distance_count);
}

static bool inflate_fixed(struct inflate_state *state) {
    uint8_t *lengths = state->lengths;
    unsigned i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    for (i = 0; i < 30; i++) lengths[288 + i] = 5;
    return huffman_build(&state->literals, lengths, 288)
        && huffman_build(&state->distances, lengths + 288, 30);
}

/* Inflates a zlib stream. Returns the number of bytes produced through
   out_length; the output must fit in out_size. */
static bool zlib_inflate(const uint8_t *data, size_t size, uint8_t *out,
                         size_t out_size, size_t *out_length,
                         struct inflate_state *state) {
    if (size < 2 || (data[0] & 0x0f) != 8 || (data[0] >> 4) > 7
        || (data[1] & 0x20) != 0 || ((unsigned)data[0] << 8 | data[1]) % 31 != 0) {
        return false;
    }
    struct bit_reader bits = {data + 2, size - 2, 0, 0, 0, 0};
    size_t pos = 0;
    bool last = false;
    while (!last) {
        last = bits_take(&bits, 1) != 0;
        unsigned type = bits_take(&bits, 2);
        if (bits_overrun(&bits)) {
            return false;
        }
        if (type == 0) {
            /* Stored block: drop to a byte boundary, then LEN and NLEN. */
            bits_take(&bits, bits.count % 8);
            uint32_t length = bits_take(&bits, 16);
            uint32_t check = bits_take(&bits, 16);
            if ((length ^ 0xffffu) != check || length > out_size - pos) {
                return false;
            }
            for (uint32_t i = 0; i < length; i++) {
                out[pos++] = (uint8_t)bits_take(&bits, 8);
            }
            if (bits_overrun(&bits)) {
                return false;
            }
        } else if (type == 1) {
            if (!inflate_fixed(state)
                || !inflate_codes(&bits, state, out, out_size, &pos)) {
                return false;
            }
        } else if (type == 2) {
            if (!inflate_dynamic(&bits, state)
                || !inflate_codes(&bits, state, out, out_size, &pos)) {
                return false;
            }
        } else {
            return false;
        }
    }
    *out_length = pos;
    return true;
}

/* -------------------------------------------------------------------- PNG */

static const uint8_t png_signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a,
                                         0x1a, 0x0a};

struct png_header {
    uint32_t width;
    uint32_t height;
    uint8_t depth;
    uint8_t color_type;
    uint8_t interlace;
    unsigned channels;
    size_t idat_bytes;
    size_t raw_bytes;
};

static const uint8_t adam7_x[7] = {0, 4, 0, 2, 0, 1, 0};
static const uint8_t adam7_y[7] = {0, 0, 4, 0, 2, 0, 1};
static const uint8_t adam7_dx[7] = {8, 8, 4, 4, 2, 2, 1};
static const uint8_t adam7_dy[7] = {8, 8, 8, 4, 4, 2, 2};

static uint32_t pass_extent(uint32_t full, uint32_t start, uint32_t step) {
    return full > start ? (full - start + step - 1) / step : 0;
}

static size_t png_row_bytes(const struct png_header *png, uint32_t width) {
    return ((size_t)width * png->channels * png->depth + 7) / 8;
}

static enum image_result png_read_header(const uint8_t *data, size_t size,
                                         struct png_header *png) {
    if (size < 33) {
        return IMAGE_CORRUPT;
    }
    if (read_be32(data + 8) != 13 || data[12] != 'I' || data[13] != 'H'
        || data[14] != 'D' || data[15] != 'R') {
        return IMAGE_CORRUPT;
    }
    const uint8_t *ihdr = data + 16;
    png->width = read_be32(ihdr);
    png->height = read_be32(ihdr + 4);
    png->depth = ihdr[8];
    png->color_type = ihdr[9];
    png->interlace = ihdr[12];
    if (ihdr[10] != 0 || ihdr[11] != 0 || png->interlace > 1) {
        return IMAGE_CORRUPT;
    }
    uint8_t depth = png->depth;
    switch (png->color_type) {
    case 0:
        png->channels = 1;
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8
            && depth != 16) {
            return IMAGE_CORRUPT;
        }
        break;
    case 3:
        png->channels = 1;
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8) {
            return IMAGE_CORRUPT;
        }
        break;
    case 2:
    case 4:
    case 6:
        png->channels = png->color_type == 2 ? 3 : png->color_type == 4 ? 2 : 4;
        if (depth != 8 && depth != 16) {
            return IMAGE_CORRUPT;
        }
        break;
    default:
        return IMAGE_CORRUPT;
    }
    if (png->width == 0 || png->height == 0
        || png->width > 0x7fffffffu || png->height > 0x7fffffffu) {
        return IMAGE_CORRUPT;
    }
    if (!dimensions_ok(png->width, png->height)) {
        return IMAGE_TOO_LARGE;
    }
    /* Inflated size: one filter byte plus the row, for every row of every
       pass. */
    size_t raw = 0;
    if (png->interlace == 0) {
        raw = (png_row_bytes(png, png->width) + 1) * png->height;
    } else {
        for (unsigned pass = 0; pass < 7; pass++) {
            uint32_t w = pass_extent(png->width, adam7_x[pass], adam7_dx[pass]);
            uint32_t h = pass_extent(png->height, adam7_y[pass], adam7_dy[pass]);
            if (w != 0 && h != 0) {
                raw += (png_row_bytes(png, w) + 1) * h;
            }
        }
    }
    png->raw_bytes = raw;
    /* Total IDAT size, needed to join the chunks into one zlib stream. */
    size_t idat = 0;
    size_t pos = 8;
    while (pos + 12 <= size) {
        uint32_t length = read_be32(data + pos);
        if (length > size - pos - 12) {
            return IMAGE_CORRUPT;
        }
        const uint8_t *type = data + pos + 4;
        if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A'
            && type[3] == 'T') {
            idat += length;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N'
                   && type[3] == 'D') {
            break;
        }
        pos += 12 + (size_t)length;
    }
    if (idat == 0) {
        return IMAGE_CORRUPT;
    }
    png->idat_bytes = idat;
    return IMAGE_OK;
}

struct png_palette {
    uint32_t colors[256];
    unsigned count;
    bool key;
    uint16_t key_value[3];
};

static size_t png_scratch(const struct png_header *png) {
    return pixel_bytes(png->width, png->height) + ALIGN16(png->idat_bytes)
         + ALIGN16(png->raw_bytes) + ALIGN16(sizeof(struct inflate_state))
         + ALIGN16(sizeof(struct png_palette)) + 64;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

/* Undoes the per-row filters in place. */
static bool png_unfilter(uint8_t *raw, size_t row_bytes, uint32_t rows,
                         unsigned unit) {
    uint8_t *previous = NULL;
    for (uint32_t y = 0; y < rows; y++) {
        uint8_t *line = raw + (size_t)y * (row_bytes + 1);
        uint8_t filter = line[0];
        uint8_t *row = line + 1;
        switch (filter) {
        case 0:
            break;
        case 1:
            for (size_t i = unit; i < row_bytes; i++) {
                row[i] = (uint8_t)(row[i] + row[i - unit]);
            }
            break;
        case 2:
            if (previous != NULL) {
                for (size_t i = 0; i < row_bytes; i++) {
                    row[i] = (uint8_t)(row[i] + previous[i]);
                }
            }
            break;
        case 3:
            for (size_t i = 0; i < row_bytes; i++) {
                unsigned left = i >= unit ? row[i - unit] : 0;
                unsigned up = previous != NULL ? previous[i] : 0;
                row[i] = (uint8_t)(row[i] + ((left + up) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < row_bytes; i++) {
                uint8_t left = i >= unit ? row[i - unit] : 0;
                uint8_t up = previous != NULL ? previous[i] : 0;
                uint8_t corner = i >= unit && previous != NULL
                               ? previous[i - unit] : 0;
                row[i] = (uint8_t)(row[i] + paeth(left, up, corner));
            }
            break;
        default:
            return false;
        }
        previous = row;
    }
    return true;
}

/* Raw sample c of pixel x in an unfiltered row (16-bit kept whole). */
static uint32_t png_sample(const struct png_header *png, const uint8_t *row,
                           uint32_t x, unsigned c) {
    unsigned depth = png->depth;
    if (depth == 8) {
        return row[(size_t)x * png->channels + c];
    }
    if (depth == 16) {
        const uint8_t *at = row + ((size_t)x * png->channels + c) * 2;
        return (uint32_t)at[0] << 8 | at[1];
    }
    size_t bit = (size_t)x * depth;
    unsigned shift = 8 - depth - (unsigned)(bit % 8);
    return (row[bit / 8] >> shift) & ((1u << depth) - 1);
}

static uint8_t png_to_byte(const struct png_header *png, uint32_t sample) {
    switch (png->depth) {
    case 16:
        return (uint8_t)(sample >> 8);
    case 8:
        return (uint8_t)sample;
    default:
        return (uint8_t)(sample * 255 / ((1u << png->depth) - 1));
    }
}

static uint32_t png_pixel(const struct png_header *png,
                          const struct png_palette *palette,
                          const uint8_t *row, uint32_t x) {
    switch (png->color_type) {
    case 0: {
        uint32_t raw = png_sample(png, row, x, 0);
        uint8_t gray = png_to_byte(png, raw);
        uint8_t alpha = palette->key && raw == palette->key_value[0] ? 0 : 255;
        return pixel_argb(alpha, gray, gray, gray);
    }
    case 2: {
        uint32_t r = png_sample(png, row, x, 0);
        uint32_t g = png_sample(png, row, x, 1);
        uint32_t b = png_sample(png, row, x, 2);
        uint8_t alpha = palette->key && r == palette->key_value[0]
                     && g == palette->key_value[1] && b == palette->key_value[2]
                      ? 0 : 255;
        return pixel_argb(alpha, png_to_byte(png, r), png_to_byte(png, g),
                          png_to_byte(png, b));
    }
    case 3: {
        uint32_t index = png_sample(png, row, x, 0);
        return index < palette->count ? palette->colors[index] : 0xff000000u;
    }
    case 4: {
        uint8_t gray = png_to_byte(png, png_sample(png, row, x, 0));
        return pixel_argb(png_to_byte(png, png_sample(png, row, x, 1)),
                          gray, gray, gray);
    }
    default:
        return pixel_argb(png_to_byte(png, png_sample(png, row, x, 3)),
                          png_to_byte(png, png_sample(png, row, x, 0)),
                          png_to_byte(png, png_sample(png, row, x, 1)),
                          png_to_byte(png, png_sample(png, row, x, 2)));
    }
}

static enum image_result png_decode(const uint8_t *data, size_t size,
                                    struct arena *arena, struct image *image) {
    struct png_header png;
    enum image_result result = png_read_header(data, size, &png);
    if (result != IMAGE_OK) {
        return result;
    }
    uint32_t *pixels = arena_take(arena, (size_t)png.width * png.height * 4);
    uint8_t *idat = arena_take(arena, png.idat_bytes);
    uint8_t *raw = arena_take(arena, png.raw_bytes);
    struct inflate_state *state = arena_take(arena, sizeof(*state));
    struct png_palette *palette = arena_take(arena, sizeof(*palette));
    if (pixels == NULL || idat == NULL || raw == NULL || state == NULL
        || palette == NULL) {
        return IMAGE_NO_MEMORY;
    }
    bytes_zero(palette, sizeof(*palette));

    size_t joined = 0;
    size_t pos = 8;
    while (pos + 12 <= size) {
        uint32_t length = read_be32(data + pos);
        const uint8_t *type = data + pos + 4;
        const uint8_t *body = data + pos + 8;
        if (length > size - pos - 12) {
            return IMAGE_CORRUPT;
        }
        if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T'
            && type[3] == 'E') {
            if (length % 3 != 0 || length > 768) {
                return IMAGE_CORRUPT;
            }
            palette->count = length / 3;
            for (unsigned i = 0; i < palette->count; i++) {
                palette->colors[i] = pixel_argb(255, body[i * 3],
                                                body[i * 3 + 1],
                                                body[i * 3 + 2]);
            }
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N'
                   && type[3] == 'S') {
            if (png.color_type == 3) {
                for (unsigned i = 0; i < length && i < palette->count; i++) {
                    palette->colors[i] = (palette->colors[i] & 0x00ffffffu)
                                       | (uint32_t)body[i] << 24;
                }
            } else if (png.color_type == 0 && length >= 2) {
                palette->key = true;
                palette->key_value[0] = read_be16(body);
            } else if (png.color_type == 2 && length >= 6) {
                palette->key = true;
                palette->key_value[0] = read_be16(body);
                palette->key_value[1] = read_be16(body + 2);
                palette->key_value[2] = read_be16(body + 4);
            }
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A'
                   && type[3] == 'T') {
            if (length > png.idat_bytes - joined) {
                return IMAGE_CORRUPT;
            }
            bytes_copy(idat + joined, body, length);
            joined += length;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N'
                   && type[3] == 'D') {
            break;
        }
        pos += 12 + (size_t)length;
    }
    if (png.color_type == 3 && palette->count == 0) {
        return IMAGE_CORRUPT;
    }
    if (png.depth < 8 && palette->key) {
        /* The colour key is given at full depth; compare raw samples. */
        palette->key_value[0] &= (uint16_t)((1u << png.depth) - 1);
    }

    size_t produced = 0;
    if (!zlib_inflate(idat, joined, raw, png.raw_bytes, &produced, state)
        || produced < png.raw_bytes) {
        return IMAGE_CORRUPT;
    }

    unsigned unit = (png.channels * png.depth + 7) / 8;
    size_t offset = 0;
    unsigned passes = png.interlace ? 7 : 1;
    for (unsigned pass = 0; pass < passes; pass++) {
        uint32_t x0 = png.interlace ? adam7_x[pass] : 0;
        uint32_t y0 = png.interlace ? adam7_y[pass] : 0;
        uint32_t dx = png.interlace ? adam7_dx[pass] : 1;
        uint32_t dy = png.interlace ? adam7_dy[pass] : 1;
        uint32_t w = pass_extent(png.width, x0, dx);
        uint32_t h = pass_extent(png.height, y0, dy);
        if (w == 0 || h == 0) {
            continue;
        }
        size_t row_bytes = png_row_bytes(&png, w);
        uint8_t *pass_raw = raw + offset;
        if (!png_unfilter(pass_raw, row_bytes, h, unit)) {
            return IMAGE_CORRUPT;
        }
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *row = pass_raw + (size_t)y * (row_bytes + 1) + 1;
            uint32_t *out = pixels + (size_t)(y0 + y * dy) * png.width;
            for (uint32_t x = 0; x < w; x++) {
                out[x0 + x * dx] = png_pixel(&png, palette, row, x);
            }
        }
        offset += (row_bytes + 1) * h;
    }
    image->width = png.width;
    image->height = png.height;
    image->pixels = pixels;
    return IMAGE_OK;
}

/* -------------------------------------------------------------------- BMP */

struct bmp_header {
    uint32_t pixel_offset;
    uint32_t header_size;
    int32_t width;
    int32_t height;
    uint16_t bits;
    uint32_t compression;
    uint32_t colors_used;
    uint32_t masks[4];
};

static enum image_result bmp_read_header(const uint8_t *data, size_t size,
                                         struct bmp_header *bmp) {
    if (size < 26) {
        return IMAGE_CORRUPT;
    }
    bytes_zero(bmp, sizeof(*bmp));
    bmp->pixel_offset = read_le32(data + 10);
    bmp->header_size = read_le32(data + 14);
    if (bmp->header_size == 12) {
        bmp->width = read_le16(data + 18);
        bmp->height = (int16_t)read_le16(data + 20);
        bmp->bits = read_le16(data + 24);
    } else if (bmp->header_size >= 40 && size >= 54
               && bmp->header_size <= size - 14) {
        bmp->width = (int32_t)read_le32(data + 18);
        bmp->height = (int32_t)read_le32(data + 22);
        bmp->bits = read_le16(data + 28);
        bmp->compression = read_le32(data + 30);
        bmp->colors_used = read_le32(data + 46);
        if (bmp->compression == 3 || bmp->compression == 6) {
            /* BITFIELDS: masks follow a 40-byte header, or sit inside a
               V2+ header. */
            if (size < 54 + 12) {
                return IMAGE_CORRUPT;
            }
            bmp->masks[0] = read_le32(data + 54);
            bmp->masks[1] = read_le32(data + 58);
            bmp->masks[2] = read_le32(data + 62);
            if (bmp->header_size >= 56 || bmp->compression == 6) {
                if (size < 70) {
                    return IMAGE_CORRUPT;
                }
                bmp->masks[3] = read_le32(data + 66);
            }
        }
    } else {
        return IMAGE_CORRUPT;
    }
    if (bmp->compression == 1 || bmp->compression == 2
        || bmp->compression == 4 || bmp->compression == 5) {
        return IMAGE_UNSUPPORTED;
    }
    if (bmp->compression != 0 && bmp->compression != 3
        && bmp->compression != 6) {
        return IMAGE_CORRUPT;
    }
    switch (bmp->bits) {
    case 1: case 4: case 8: case 16: case 24: case 32:
        break;
    default:
        return IMAGE_CORRUPT;
    }
    if (bmp->width <= 0 || bmp->height == 0 || bmp->height == INT32_MIN) {
        return IMAGE_CORRUPT;
    }
    uint32_t height = (uint32_t)(bmp->height < 0 ? -bmp->height : bmp->height);
    if (!dimensions_ok((uint32_t)bmp->width, height)) {
        return IMAGE_TOO_LARGE;
    }
    return IMAGE_OK;
}

/* Scales a masked channel to 0-255. A zero mask reads as 255 (alpha). */
static uint8_t mask_channel(uint32_t value, uint32_t mask) {
    if (mask == 0) {
        return 255;
    }
    unsigned shift = 0;
    while ((mask & 1) == 0) {
        mask >>= 1;
        shift++;
    }
    unsigned width = 0;
    while ((mask & 1) != 0 && width < 32) {
        mask >>= 1;
        width++;
    }
    uint32_t channel = (value >> shift) & ((width >= 32 ? 0 : 1u << width) - 1);
    if (width >= 8) {
        return (uint8_t)(channel >> (width - 8));
    }
    return (uint8_t)(channel * 255 / ((1u << width) - 1));
}

static enum image_result bmp_decode(const uint8_t *data, size_t size,
                                    struct arena *arena, struct image *image) {
    struct bmp_header bmp;
    enum image_result result = bmp_read_header(data, size, &bmp);
    if (result != IMAGE_OK) {
        return result;
    }
    uint32_t width = (uint32_t)bmp.width;
    bool top_down = bmp.height < 0;
    uint32_t height = (uint32_t)(top_down ? -bmp.height : bmp.height);
    uint32_t *pixels = arena_take(arena, (size_t)width * height * 4);
    if (pixels == NULL) {
        return IMAGE_NO_MEMORY;
    }
    uint32_t palette[256];
    unsigned palette_count = 0;
    if (bmp.bits <= 8) {
        unsigned entry = bmp.header_size == 12 ? 3 : 4;
        palette_count = bmp.colors_used != 0 && bmp.colors_used <= 256
                      ? bmp.colors_used : 1u << bmp.bits;
        size_t start = 14 + (size_t)bmp.header_size;
        if (start + (size_t)palette_count * entry > size) {
            return IMAGE_CORRUPT;
        }
        for (unsigned i = 0; i < palette_count; i++) {
            const uint8_t *at = data + start + (size_t)i * entry;
            palette[i] = pixel_argb(255, at[2], at[1], at[0]);
        }
    }
    uint32_t masks[4] = {bmp.masks[0], bmp.masks[1], bmp.masks[2],
                         bmp.masks[3]};
    bool use_masks = bmp.compression == 3 || bmp.compression == 6;
    if (!use_masks && bmp.bits == 16) {
        masks[0] = 0x7c00;
        masks[1] = 0x03e0;
        masks[2] = 0x001f;
        masks[3] = 0;
        use_masks = true;
    }
    size_t stride = (((size_t)width * bmp.bits + 31) / 32) * 4;
    if (bmp.pixel_offset > size || stride * height > size - bmp.pixel_offset) {
        return IMAGE_CORRUPT;
    }
    /* 32-bit files without an alpha mask often leave the byte at 0. */
    bool any_alpha = false;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = data + bmp.pixel_offset + stride * y;
        uint32_t *out = pixels + (size_t)(top_down ? y : height - 1 - y) * width;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t color;
            if (bmp.bits <= 8) {
                size_t bit = (size_t)x * bmp.bits;
                unsigned shift = 8 - bmp.bits - (unsigned)(bit % 8);
                unsigned index = (row[bit / 8] >> shift) & ((1u << bmp.bits) - 1);
                color = index < palette_count ? palette[index] : 0xff000000u;
            } else if (bmp.bits == 24) {
                const uint8_t *at = row + (size_t)x * 3;
                color = pixel_argb(255, at[2], at[1], at[0]);
            } else {
                uint32_t value = bmp.bits == 16 ? read_le16(row + (size_t)x * 2)
                                               : read_le32(row + (size_t)x * 4);
                if (use_masks) {
                    color = pixel_argb(mask_channel(value, masks[3]),
                                       mask_channel(value, masks[0]),
                                       mask_channel(value, masks[1]),
                                       mask_channel(value, masks[2]));
                } else {
                    color = value;
                    any_alpha |= (value >> 24) != 0;
                }
            }
            out[x] = color;
        }
    }
    if (bmp.bits == 32 && !use_masks && !any_alpha) {
        for (size_t i = 0; i < (size_t)width * height; i++) {
            pixels[i] |= 0xff000000u;
        }
    }
    image->width = width;
    image->height = height;
    image->pixels = pixels;
    return IMAGE_OK;
}

/* ------------------------------------------------------------------- JPEG */

/*
 * Baseline and progressive Huffman-coded JPEG (SOF0/1/2), 8-bit, with 1
 * (grey) or 3 components and any sampling factors. Baseline blocks go
 * straight through the inverse DCT into per-component planes; progressive
 * scans collect coefficients first. Chroma is upsampled by replication.
 */

#define JPEG_MAX_COMPONENTS 3

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* cos table for the IDCT, scaled by 4096, with C(u)/2 folded in. */
static const int16_t idct_table[8][8] = {
    {1448, 2009, 1892, 1703, 1448, 1138, 784, 400},
    {1448, 1703, 784, -400, -1448, -2009, -1892, -1138},
    {1448, 1138, -784, -2009, -1448, 400, 1892, 1703},
    {1448, 400, -1892, -1138, 1448, 1703, -784, -2009},
    {1448, -400, -1892, 1138, 1448, -1703, -784, 2009},
    {1448, -1138, -784, 2009, -1448, -400, 1892, -1703},
    {1448, -1703, 784, 400, -1448, 2009, -1892, 1138},
    {1448, -2009, 1892, -1703, 1448, -1138, 784, -400},
};

struct jpeg_huffman {
    bool present;
    /* value | length << 8, or 0 when the code is longer than 9 bits. */
    uint16_t fast[FAST_SIZE];
    int32_t max_code[18];
    int32_t value_offset[17];
    uint8_t values[256];
};

struct jpeg_component {
    uint8_t id;
    uint8_t h;
    uint8_t v;
    uint8_t quant;
    uint8_t dc_table;
    uint8_t ac_table;
    int32_t dc_predictor;
    /* Plane of decoded samples, padded to whole MCUs. */
    uint8_t *plane;
    uint32_t plane_width;
    uint32_t plane_height;
    /* Blocks covering the image (non-interleaved scan order). */
    uint32_t blocks_x;
    uint32_t blocks_y;
    /* Progressive only: coefficients, 64 per block of the padded plane. */
    int16_t *coefficients;
};

struct jpeg_bits {
    const uint8_t *data;
    size_t size;
    size_t position;
    uint64_t buffer; /* next bits at the top */
    unsigned count;
    bool marker;
};

struct jpeg {
    uint32_t width;
    uint32_t height;
    bool progressive;
    unsigned component_count;
    struct jpeg_component components[JPEG_MAX_COMPONENTS];
    uint8_t h_max;
    uint8_t v_max;
    uint32_t mcus_x;
    uint32_t mcus_y;
    uint16_t quant[4][64]; /* zigzag order */
    bool quant_present[4];
    struct jpeg_huffman dc[4];
    struct jpeg_huffman ac[4];
    uint32_t restart_interval;
    int adobe_transform; /* -1 when there is no Adobe marker */
    uint32_t eob_run;
    struct jpeg_bits bits;
};

static void jpeg_bits_fill(struct jpeg_bits *bits) {
    while (bits->count <= 56) {
        uint64_t byte = 0;
        if (!bits->marker && bits->position < bits->size) {
            uint8_t value = bits->data[bits->position];
            if (value == 0xff) {
                uint8_t next = bits->position + 1 < bits->size
                             ? bits->data[bits->position + 1] : 0xd9;
                if (next == 0x00) {
                    bits->position += 2;
                    byte = 0xff;
                } else {
                    bits->marker = true;
                }
            } else {
                bits->position++;
                byte = value;
            }
        }
        bits->buffer |= byte << (56 - bits->count);
        bits->count += 8;
    }
}

static uint32_t jpeg_bits_take(struct jpeg_bits *bits, unsigned count) {
    if (count == 0) {
        return 0;
    }
    if (bits->count < count) {
        jpeg_bits_fill(bits);
    }
    uint32_t value = (uint32_t)(bits->buffer >> (64 - count));
    bits->buffer <<= count;
    bits->count -= count;
    return value;
}

static int32_t jpeg_extend(struct jpeg_bits *bits, unsigned size) {
    if (size == 0) {
        return 0;
    }
    if (size > 16) {
        return 0;
    }
    int32_t value = (int32_t)jpeg_bits_take(bits, size);
    if (value < (1 << (size - 1))) {
        value -= (1 << size) - 1;
    }
    return value;
}

static bool jpeg_huffman_build(struct jpeg_huffman *table,
                               const uint8_t counts[16],
                               const uint8_t *values, unsigned total) {
    bytes_zero(table, sizeof(*table));
    if (total > 256) {
        return false;
    }
    bytes_copy(table->values, values, total);
    unsigned code = 0;
    unsigned index = 0;
    for (unsigned length = 1; length <= 16; length++) {
        unsigned count = counts[length - 1];
        table->value_offset[length] = (int32_t)index - (int32_t)code;
        if (count != 0) {
            for (unsigned i = 0; i < count; i++) {
                if (length <= FAST_BITS) {
                    unsigned first = (code + i) << (FAST_BITS - length);
                    unsigned span = 1u << (FAST_BITS - length);
                    for (unsigned j = 0; j < span; j++) {
                        table->fast[first + j] =
                            (uint16_t)(values[index + i] | length << 8);
                    }
                }
            }
            code += count;
            index += count;
            table->max_code[length] = (int32_t)code - 1;
        } else {
            table->max_code[length] = -1;
        }
        if (code > (1u << length)) {
            return false;
        }
        code <<= 1;
    }
    table->max_code[17] = 0x7fffffff;
    table->present = true;
    return true;
}

static int jpeg_decode_symbol(struct jpeg_bits *bits,
                              const struct jpeg_huffman *table) {
    if (bits->count < 16) {
        jpeg_bits_fill(bits);
    }
    unsigned peek = (unsigned)(bits->buffer >> (64 - FAST_BITS));
    uint16_t entry = table->fast[peek];
    if (entry != 0) {
        unsigned length = entry >> 8;
        bits->buffer <<= length;
        bits->count -= length;
        return entry & 0xff;
    }
    for (unsigned length = FAST_BITS + 1; length <= 16; length++) {
        int32_t code = (int32_t)(bits->buffer >> (64 - length));
        if (code <= table->max_code[length]) {
            int32_t index = table->value_offset[length] + code;
            if (index < 0 || index > 255) {
                return -1;
            }
            bits->buffer <<= length;
            bits->count -= length;
            return table->values[index];
        }
    }
    return -1;
}

/* Inverse DCT of one block (natural order) into 8x8 samples. */
static void jpeg_idct(const int32_t *coefficients, uint8_t *out,
                      uint32_t stride) {
    bool dc_only = true;
    for (unsigned i = 1; i < 64; i++) {
        if (coefficients[i] != 0) {
            dc_only = false;
            break;
        }
    }
    if (dc_only) {
        uint8_t value = clamp_byte(((coefficients[0] + 4) >> 3) + 128);
        for (unsigned y = 0; y < 8; y++) {
            for (unsigned x = 0; x < 8; x++) {
                out[y * stride + x] = value;
            }
        }
        return;
    }
    int32_t rows[64];
    for (unsigned v = 0; v < 8; v++) {
        const int32_t *in = coefficients + v * 8;
        for (unsigned x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (unsigned u = 0; u < 8; u++) {
                sum += (int64_t)in[u] * idct_table[x][u];
            }
            rows[v * 8 + x] = (int32_t)((sum + 8) >> 4);
        }
    }
    for (unsigned y = 0; y < 8; y++) {
        for (unsigned x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (unsigned v = 0; v < 8; v++) {
                sum += (int64_t)rows[v * 8 + x] * idct_table[y][v];
            }
            out[y * stride + x] =
                clamp_byte((int32_t)((sum + (1 << 19)) >> 20) + 128);
        }
    }
}

static void jpeg_restart(struct jpeg *jpeg) {
    struct jpeg_bits *bits = &jpeg->bits;
    bits->buffer = 0;
    bits->count = 0;
    bits->marker = false;
    if (bits->position + 1 < bits->size && bits->data[bits->position] == 0xff
        && bits->data[bits->position + 1] >= 0xd0
        && bits->data[bits->position + 1] <= 0xd7) {
        bits->position += 2;
    }
    for (unsigned i = 0; i < jpeg->component_count; i++) {
        jpeg->components[i].dc_predictor = 0;
    }
    jpeg->eob_run = 0;
}

/* Baseline: decodes one block and writes its samples into the plane. */
static bool jpeg_baseline_block(struct jpeg *jpeg,
                                struct jpeg_component *component,
                                uint32_t block_x, uint32_t block_y) {
    const struct jpeg_huffman *dc = &jpeg->dc[component->dc_table];
    const struct jpeg_huffman *ac = &jpeg->ac[component->ac_table];
    const uint16_t *quant = jpeg->quant[component->quant];
    int32_t block[64];
    for (unsigned i = 0; i < 64; i++) {
        block[i] = 0;
    }
    int t = jpeg_decode_symbol(&jpeg->bits, dc);
    if (t < 0 || t > 16) {
        return false;
    }
    component->dc_predictor += jpeg_extend(&jpeg->bits, (unsigned)t);
    block[0] = component->dc_predictor * quant[0];
    unsigned k = 1;
    while (k < 64) {
        int rs = jpeg_decode_symbol(&jpeg->bits, ac);
        if (rs < 0) {
            return false;
        }
        unsigned run = (unsigned)rs >> 4;
        unsigned size = (unsigned)rs & 15;
        if (size == 0) {
            if (run != 15) {
                break;
            }
            k += 16;
            continue;
        }
        k += run;
        if (k > 63) {
            return false;
        }
        block[zigzag[k]] = jpeg_extend(&jpeg->bits, size) * quant[k];
        k++;
    }
    if (block_x * 8 + 8 > component->plane_width
        || block_y * 8 + 8 > component->plane_height) {
        return true;
    }
    jpeg_idct(block, component->plane
                     + (size_t)block_y * 8 * component->plane_width
                     + block_x * 8,
              component->plane_width);
    return true;
}

static int16_t *jpeg_coefficients(struct jpeg_component *component,
                                  uint32_t block_x, uint32_t block_y) {
    uint32_t per_row = component->plane_width / 8;
    return component->coefficients
         + ((size_t)block_y * per_row + block_x) * 64;
}

/* Progressive DC scan (first pass or refinement). */
static bool jpeg_progressive_dc(struct jpeg *jpeg,
                                struct jpeg_component *component,
                                int16_t *block, unsigned ah, unsigned al) {
    if (ah == 0) {
        int t = jpeg_decode_symbol(&jpeg->bits, &jpeg->dc[component->dc_table]);
        if (t < 0 || t > 16) {
            return false;
        }
        component->dc_predictor += jpeg_extend(&jpeg->bits, (unsigned)t);
        block[0] = (int16_t)(component->dc_predictor * (1 << al));
    } else if (jpeg_bits_take(&jpeg->bits, 1) != 0) {
        block[0] = (int16_t)(block[0] | (1 << al));
    }
    return true;
}

/* Progressive AC scan (first pass or refinement) for one block. */
static bool jpeg_progressive_ac(struct jpeg *jpeg,
                                struct jpeg_component *component,
                                int16_t *block, unsigned ss, unsigned se,
                                unsigned ah, unsigned al) {
    const struct jpeg_huffman *table = &jpeg->ac[component->ac_table];
    struct jpeg_bits *bits = &jpeg->bits;
    if (ah == 0) {
        if (jpeg->eob_run != 0) {
            jpeg->eob_run--;
            return true;
        }
        unsigned k = ss;
        while (k <= se) {
            int rs = jpeg_decode_symbol(bits, table);
            if (rs < 0) {
                return false;
            }
            unsigned run = (unsigned)rs >> 4;
            unsigned size = (unsigned)rs & 15;
            if (size == 0) {
                if (run < 15) {
                    jpeg->eob_run = (1u << run) - 1;
                    if (run != 0) {
                        jpeg->eob_run += jpeg_bits_take(bits, run);
                    }
                    break;
                }
                k += 16;
                continue;
            }
            k += run;
            if (k > 63) {
                return false;
            }
            block[zigzag[k]] =
                (int16_t)(jpeg_extend(bits, size) * (1 << al));
            k++;
        }
        return true;
    }

    int16_t bit = (int16_t)(1 << al);
    if (jpeg->eob_run != 0) {
        jpeg->eob_run--;
        for (unsigned k = ss; k <= se; k++) {
            int16_t *p = &block[zigzag[k]];
            if (*p != 0 && jpeg_bits_take(bits, 1) != 0 && (*p & bit) == 0) {
                *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
            }
        }
        return true;
    }
    unsigned k = ss;
    do {
        int rs = jpeg_decode_symbol(bits, table);
        if (rs < 0) {
            return false;
        }
        int run = rs >> 4;
        int size = rs & 15;
        int value = 0;
        if (size == 0) {
            if (run < 15) {
                jpeg->eob_run = (1u << run) - 1;
                if (run != 0) {
                    jpeg->eob_run += jpeg_bits_take(bits, (unsigned)run);
                }
                run = 64;
            }
        } else {
            if (size != 1) {
                return false;
            }
            value = jpeg_bits_take(bits, 1) != 0 ? bit : -bit;
        }
        while (k <= se) {
            int16_t *p = &block[zigzag[k++]];
            if (*p != 0) {
                if (jpeg_bits_take(bits, 1) != 0 && (*p & bit) == 0) {
                    *p = (int16_t)(*p > 0 ? *p + bit : *p - bit);
                }
            } else {
                if (run == 0) {
                    *p = (int16_t)value;
                    break;
                }
                run--;
            }
        }
    } while (k <= se);
    return true;
}

static bool jpeg_scan(struct jpeg *jpeg, const uint8_t *segment,
                      size_t length) {
    if (length < 1) {
        return false;
    }
    unsigned count = segment[0];
    if (count < 1 || count > jpeg->component_count || length < 4 + count * 2) {
        return false;
    }
    struct jpeg_component *scan[JPEG_MAX_COMPONENTS];
    for (unsigned i = 0; i < count; i++) {
        uint8_t id = segment[1 + i * 2];
        uint8_t tables = segment[2 + i * 2];
        scan[i] = NULL;
        for (unsigned c = 0; c < jpeg->component_count; c++) {
            if (jpeg->components[c].id == id) {
                scan[i] = &jpeg->components[c];
            }
        }
        if (scan[i] == NULL || (tables >> 4) > 3 || (tables & 15) > 3) {
            return false;
        }
        scan[i]->dc_table = tables >> 4;
        scan[i]->ac_table = tables & 15;
    }
    unsigned ss = segment[1 + count * 2];
    unsigned se = segment[2 + count * 2];
    unsigned ah = segment[3 + count * 2] >> 4;
    unsigned al = segment[3 + count * 2] & 15;
    if (jpeg->progressive) {
        if (ss > 63 || se > 63 || ss > se || al > 13 || ah > 13
            || (ss == 0 && se != 0) || (ss != 0 && count != 1)) {
            return false;
        }
    } else if (ss != 0 || se != 63 || ah != 0 || al != 0) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        bool need_dc = ss == 0 && ah == 0;
        bool need_ac = !jpeg->progressive || ss != 0;
        if ((need_dc && !jpeg->dc[scan[i]->dc_table].present)
            || (need_ac && !jpeg->ac[scan[i]->ac_table].present)) {
            return false;
        }
        if (!jpeg->progressive && !jpeg->quant_present[scan[i]->quant]) {
            return false;
        }
    }

    for (unsigned c = 0; c < jpeg->component_count; c++) {
        jpeg->components[c].dc_predictor = 0;
    }
    jpeg->eob_run = 0;
    jpeg->bits.buffer = 0;
    jpeg->bits.count = 0;
    jpeg->bits.marker = false;

    uint32_t restart = jpeg->restart_interval;
    uint32_t todo = restart != 0 ? restart : 0xffffffffu;
    if (count == 1) {
        struct jpeg_component *component = scan[0];
        for (uint32_t by = 0; by < component->blocks_y; by++) {
            for (uint32_t bx = 0; bx < component->blocks_x; bx++) {
                bool ok;
                if (!jpeg->progressive) {
                    ok = jpeg_baseline_block(jpeg, component, bx, by);
                } else {
                    int16_t *block = jpeg_coefficients(component, bx, by);
                    ok = ss == 0
                       ? jpeg_progressive_dc(jpeg, component, block, ah, al)
                       : jpeg_progressive_ac(jpeg, component, block, ss, se,
                                             ah, al);
                }
                if (!ok) {
                    return false;
                }
                if (--todo == 0) {
                    jpeg_restart(jpeg);
                    todo = restart;
                }
            }
        }
        return true;
    }
    for (uint32_t my = 0; my < jpeg->mcus_y; my++) {
        for (uint32_t mx = 0; mx < jpeg->mcus_x; mx++) {
            for (unsigned i = 0; i < count; i++) {
                struct jpeg_component *component = scan[i];
                for (unsigned y = 0; y < component->v; y++) {
                    for (unsigned x = 0; x < component->h; x++) {
                        uint32_t bx = mx * component->h + x;
                        uint32_t by = my * component->v + y;
                        bool ok;
                        if (!jpeg->progressive) {
                            ok = jpeg_baseline_block(jpeg, component, bx, by);
                        } else {
                            ok = jpeg_progressive_dc(
                                jpeg, component,
                                jpeg_coefficients(component, bx, by), ah, al);
                        }
                        if (!ok) {
                            return false;
                        }
                    }
                }
            }
            if (--todo == 0) {
                jpeg_restart(jpeg);
                todo = restart;
            }
        }
    }
    return true;
}

struct jpeg_frame {
    uint32_t width;
    uint32_t height;
    bool progressive;
    unsigned components;
    uint8_t h[JPEG_MAX_COMPONENTS];
    uint8_t v[JPEG_MAX_COMPONENTS];
    uint8_t id[JPEG_MAX_COMPONENTS];
    uint8_t quant[JPEG_MAX_COMPONENTS];
};

/* Parses a SOF segment body. */
static enum image_result jpeg_parse_frame(const uint8_t *body, size_t length,
                                          bool progressive,
                                          struct jpeg_frame *frame) {
    if (length < 6) {
        return IMAGE_CORRUPT;
    }
    if (body[0] != 8) {
        return IMAGE_UNSUPPORTED;
    }
    frame->height = read_be16(body + 1);
    frame->width = read_be16(body + 3);
    frame->components = body[5];
    frame->progressive = progressive;
    if (frame->components == 4) {
        return IMAGE_UNSUPPORTED;
    }
    if ((frame->components != 1 && frame->components != 3)
        || length < 6 + frame->components * 3u) {
        return IMAGE_CORRUPT;
    }
    if (frame->width == 0 || frame->height == 0) {
        /* Height 0 means "set by a DNL marker"; not supported. */
        return frame->height == 0 ? IMAGE_UNSUPPORTED : IMAGE_CORRUPT;
    }
    for (unsigned i = 0; i < frame->components; i++) {
        const uint8_t *at = body + 6 + i * 3;
        frame->id[i] = at[0];
        frame->h[i] = at[1] >> 4;
        frame->v[i] = at[1] & 15;
        frame->quant[i] = at[2];
        if (frame->h[i] < 1 || frame->h[i] > 4 || frame->v[i] < 1
            || frame->v[i] > 4 || frame->quant[i] > 3) {
            return IMAGE_CORRUPT;
        }
    }
    if (!dimensions_ok(frame->width, frame->height)) {
        return IMAGE_TOO_LARGE;
    }
    return IMAGE_OK;
}

/* Finds the SOF marker. */
static enum image_result jpeg_find_frame(const uint8_t *data, size_t size,
                                         struct jpeg_frame *frame) {
    size_t pos = 2;
    while (pos + 4 <= size) {
        if (data[pos] != 0xff) {
            return IMAGE_CORRUPT;
        }
        uint8_t marker = data[pos + 1];
        if (marker == 0xff) {
            pos++;
            continue;
        }
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7)
            || marker == 0x01) {
            pos += 2;
            continue;
        }
        if (marker == 0xd9 || marker == 0xda) {
            return IMAGE_CORRUPT;
        }
        size_t length = read_be16(data + pos + 2);
        if (length < 2 || length > size - pos - 2) {
            return IMAGE_CORRUPT;
        }
        if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
            return jpeg_parse_frame(data + pos + 4, length - 2,
                                    marker == 0xc2, frame);
        }
        if ((marker >= 0xc3 && marker <= 0xcf) && marker != 0xc4
            && marker != 0xc8 && marker != 0xcc) {
            return IMAGE_UNSUPPORTED; /* lossless, hierarchical, arithmetic */
        }
        pos += 2 + length;
    }
    return IMAGE_CORRUPT;
}

/* Lays out the components: sampling, MCU counts and plane sizes. */
static void jpeg_layout(struct jpeg *jpeg, const struct jpeg_frame *frame) {
    jpeg->width = frame->width;
    jpeg->height = frame->height;
    jpeg->progressive = frame->progressive;
    jpeg->component_count = frame->components;
    jpeg->h_max = 1;
    jpeg->v_max = 1;
    for (unsigned i = 0; i < frame->components; i++) {
        if (frame->h[i] > jpeg->h_max) jpeg->h_max = frame->h[i];
        if (frame->v[i] > jpeg->v_max) jpeg->v_max = frame->v[i];
    }
    if (frame->components == 1) {
        /* A single component is always coded one block at a time. */
        jpeg->h_max = 1;
        jpeg->v_max = 1;
    }
    jpeg->mcus_x = (frame->width + 8u * jpeg->h_max - 1) / (8u * jpeg->h_max);
    jpeg->mcus_y = (frame->height + 8u * jpeg->v_max - 1) / (8u * jpeg->v_max);
    for (unsigned i = 0; i < frame->components; i++) {
        struct jpeg_component *component = &jpeg->components[i];
        component->id = frame->id[i];
        component->h = frame->components == 1 ? 1 : frame->h[i];
        component->v = frame->components == 1 ? 1 : frame->v[i];
        component->quant = frame->quant[i];
        component->plane_width = jpeg->mcus_x * component->h * 8;
        component->plane_height = jpeg->mcus_y * component->v * 8;
        uint32_t samples_x = (frame->width * component->h + jpeg->h_max - 1)
                           / jpeg->h_max;
        uint32_t samples_y = (frame->height * component->v + jpeg->v_max - 1)
                           / jpeg->v_max;
        component->blocks_x = (samples_x + 7) / 8;
        component->blocks_y = (samples_y + 7) / 8;
    }
}

static size_t jpeg_scratch(const struct jpeg_frame *frame) {
    unsigned h_max = 1, v_max = 1;
    for (unsigned i = 0; i < frame->components && frame->components > 1; i++) {
        if (frame->h[i] > h_max) h_max = frame->h[i];
        if (frame->v[i] > v_max) v_max = frame->v[i];
    }
    size_t mcus_x = (frame->width + 8u * h_max - 1) / (8u * h_max);
    size_t mcus_y = (frame->height + 8u * v_max - 1) / (8u * v_max);
    size_t total = pixel_bytes(frame->width, frame->height)
                 + ALIGN16(sizeof(struct jpeg)) + 64;
    for (unsigned i = 0; i < frame->components; i++) {
        size_t h = frame->components == 1 ? 1 : frame->h[i];
        size_t v = frame->components == 1 ? 1 : frame->v[i];
        size_t samples = mcus_x * h * 8 * mcus_y * v * 8;
        total += ALIGN16(samples);
        if (frame->progressive) {
            total += ALIGN16(samples * 2);
        }
    }
    return total;
}

static bool jpeg_read_dht(struct jpeg *jpeg, const uint8_t *body,
                          size_t length) {
    size_t pos = 0;
    while (pos < length) {
        if (length - pos < 17) {
            return false;
        }
        uint8_t info = body[pos];
        unsigned total = 0;
        for (unsigned i = 0; i < 16; i++) {
            total += body[pos + 1 + i];
        }
        if ((info >> 4) > 1 || (info & 15) > 3 || total > 256
            || length - pos - 17 < total) {
            return false;
        }
        struct jpeg_huffman *table = (info >> 4) == 0 ? &jpeg->dc[info & 15]
                                                      : &jpeg->ac[info & 15];
        if (!jpeg_huffman_build(table, body + pos + 1, body + pos + 17,
                                total)) {
            return false;
        }
        pos += 17 + total;
    }
    return true;
}

static bool jpeg_read_dqt(struct jpeg *jpeg, const uint8_t *body,
                          size_t length) {
    size_t pos = 0;
    while (pos < length) {
        uint8_t info = body[pos++];
        unsigned precision = info >> 4;
        unsigned id = info & 15;
        size_t need = precision ? 128 : 64;
        if (precision > 1 || id > 3 || length - pos < need) {
            return false;
        }
        for (unsigned k = 0; k < 64; k++) {
            jpeg->quant[id][k] = precision ? read_be16(body + pos + k * 2)
                                           : body[pos + k];
        }
        jpeg->quant_present[id] = true;
        pos += need;
    }
    return true;
}

static void jpeg_finish_progressive(struct jpeg *jpeg) {
    for (unsigned c = 0; c < jpeg->component_count; c++) {
        struct jpeg_component *component = &jpeg->components[c];
        const uint16_t *quant = jpeg->quant[component->quant];
        uint32_t blocks_x = component->plane_width / 8;
        uint32_t blocks_y = component->plane_height / 8;
        for (uint32_t by = 0; by < blocks_y; by++) {
            for (uint32_t bx = 0; bx < blocks_x; bx++) {
                const int16_t *source = jpeg_coefficients(component, bx, by);
                int32_t block[64];
                for (unsigned k = 0; k < 64; k++) {
                    block[zigzag[k]] = (int32_t)source[zigzag[k]] * quant[k];
                }
                jpeg_idct(block, component->plane
                                 + (size_t)by * 8 * component->plane_width
                                 + bx * 8,
                          component->plane_width);
            }
        }
    }
}

static void jpeg_convert(const struct jpeg *jpeg, uint32_t *pixels) {
    const struct jpeg_component *c = jpeg->components;
    bool rgb = false;
    if (jpeg->component_count == 3) {
        rgb = jpeg->adobe_transform == 0
           || (c[0].id == 'R' && c[1].id == 'G' && c[2].id == 'B');
    }
    for (uint32_t y = 0; y < jpeg->height; y++) {
        uint32_t *out = pixels + (size_t)y * jpeg->width;
        if (jpeg->component_count == 1) {
            const uint8_t *row = c[0].plane + (size_t)y * c[0].plane_width;
            for (uint32_t x = 0; x < jpeg->width; x++) {
                out[x] = pixel_argb(255, row[x], row[x], row[x]);
            }
            continue;
        }
        const uint8_t *rows[3];
        uint32_t scale_x[3];
        for (unsigned i = 0; i < 3; i++) {
            uint32_t sy = y * c[i].v / jpeg->v_max;
            rows[i] = c[i].plane + (size_t)sy * c[i].plane_width;
            scale_x[i] = c[i].h;
        }
        for (uint32_t x = 0; x < jpeg->width; x++) {
            int32_t s0 = rows[0][x * scale_x[0] / jpeg->h_max];
            int32_t s1 = rows[1][x * scale_x[1] / jpeg->h_max];
            int32_t s2 = rows[2][x * scale_x[2] / jpeg->h_max];
            if (rgb) {
                out[x] = pixel_argb(255, (uint8_t)s0, (uint8_t)s1, (uint8_t)s2);
                continue;
            }
            int32_t cb = s1 - 128;
            int32_t cr = s2 - 128;
            int32_t luma = s0 << 16;
            int32_t r = (luma + 91881 * cr + 32768) >> 16;
            int32_t g = (luma - 22554 * cb - 46802 * cr + 32768) >> 16;
            int32_t b = (luma + 116130 * cb + 32768) >> 16;
            out[x] = pixel_argb(255, clamp_byte(r), clamp_byte(g),
                                clamp_byte(b));
        }
    }
}

static enum image_result jpeg_decode(const uint8_t *data, size_t size,
                                     struct arena *arena,
                                     struct image *image) {
    struct jpeg_frame frame;
    enum image_result result = jpeg_find_frame(data, size, &frame);
    if (result != IMAGE_OK) {
        return result;
    }
    uint32_t *pixels = arena_take(arena,
                                  (size_t)frame.width * frame.height * 4);
    struct jpeg *jpeg = arena_take(arena, sizeof(*jpeg));
    if (pixels == NULL || jpeg == NULL) {
        return IMAGE_NO_MEMORY;
    }
    bytes_zero(jpeg, sizeof(*jpeg));
    jpeg_layout(jpeg, &frame);
    jpeg->adobe_transform = -1;
    for (unsigned i = 0; i < jpeg->component_count; i++) {
        struct jpeg_component *component = &jpeg->components[i];
        size_t samples = (size_t)component->plane_width
                       * component->plane_height;
        component->plane = arena_take(arena, samples);
        if (component->plane == NULL) {
            return IMAGE_NO_MEMORY;
        }
        /* Blocks a damaged file never codes stay mid-grey. */
        for (size_t s = 0; s < samples; s++) {
            component->plane[s] = 128;
        }
        if (jpeg->progressive) {
            component->coefficients = arena_take(arena, samples * 2);
            if (component->coefficients == NULL) {
                return IMAGE_NO_MEMORY;
            }
            bytes_zero(component->coefficients, samples * 2);
        }
    }

    bool frame_seen = false;
    bool scanned = false;
    size_t pos = 2;
    while (pos + 2 <= size) {
        if (data[pos] != 0xff) {
            /* Garbage between segments: look for the next marker. */
            pos++;
            continue;
        }
        uint8_t marker = data[pos + 1];
        if (marker == 0xff) {
            pos++;
            continue;
        }
        if (marker == 0xd9) {
            break;
        }
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7)
            || marker == 0x00 || marker == 0x01) {
            pos += 2;
            continue;
        }
        if (pos + 4 > size) {
            break;
        }
        size_t length = read_be16(data + pos + 2);
        if (length < 2 || length > size - pos - 2) {
            return scanned ? IMAGE_OK : IMAGE_CORRUPT;
        }
        const uint8_t *body = data + pos + 4;
        size_t body_length = length - 2;
        pos += 2 + length;
        switch (marker) {
        case 0xc0:
        case 0xc1:
        case 0xc2:
            if (frame_seen) {
                return IMAGE_UNSUPPORTED; /* several frames */
            }
            frame_seen = true;
            break;
        case 0xc4:
            if (!jpeg_read_dht(jpeg, body, body_length)) {
                return IMAGE_CORRUPT;
            }
            break;
        case 0xdb:
            if (!jpeg_read_dqt(jpeg, body, body_length)) {
                return IMAGE_CORRUPT;
            }
            break;
        case 0xdd:
            if (body_length < 2) {
                return IMAGE_CORRUPT;
            }
            jpeg->restart_interval = read_be16(body);
            break;
        case 0xee:
            if (body_length >= 12 && body[0] == 'A' && body[1] == 'd'
                && body[2] == 'o' && body[3] == 'b' && body[4] == 'e') {
                jpeg->adobe_transform = body[11];
            }
            break;
        case 0xda: {
            if (!frame_seen) {
                return IMAGE_CORRUPT;
            }
            jpeg->bits.data = data;
            jpeg->bits.size = size;
            jpeg->bits.position = pos;
            if (!jpeg_scan(jpeg, body, body_length)) {
                /* Show what decoded before the damage. */
                if (!scanned && !jpeg->progressive) {
                    return IMAGE_CORRUPT;
                }
                pos = size;
                break;
            }
            scanned = true;
            pos = jpeg->bits.position;
            break;
        }
        default:
            break;
        }
    }
    if (!scanned) {
        return IMAGE_CORRUPT;
    }
    if (jpeg->progressive) {
        for (unsigned i = 0; i < jpeg->component_count; i++) {
            if (!jpeg->quant_present[jpeg->components[i].quant]) {
                return IMAGE_CORRUPT;
            }
        }
        jpeg_finish_progressive(jpeg);
    }
    jpeg_convert(jpeg, pixels);
    image->width = jpeg->width;
    image->height = jpeg->height;
    image->pixels = pixels;
    return IMAGE_OK;
}

/* ----------------------------------------------------------------- public */

enum image_format image_sniff(const uint8_t *data, size_t size) {
    if (size >= 8) {
        bool png = true;
        for (unsigned i = 0; i < 8; i++) {
            png &= data[i] == png_signature[i];
        }
        if (png) {
            return IMAGE_FORMAT_PNG;
        }
    }
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
        return IMAGE_FORMAT_JPEG;
    }
    if (size >= 18 && data[0] == 'B' && data[1] == 'M') {
        return IMAGE_FORMAT_BMP;
    }
    return IMAGE_FORMAT_UNKNOWN;
}

enum image_result image_probe(const uint8_t *data, size_t size,
                              struct image_info *info) {
    bytes_zero(info, sizeof(*info));
    info->format = image_sniff(data, size);
    switch (info->format) {
    case IMAGE_FORMAT_PNG: {
        struct png_header png;
        enum image_result result = png_read_header(data, size, &png);
        if (result != IMAGE_OK) {
            return result;
        }
        info->width = png.width;
        info->height = png.height;
        info->scratch_bytes = png_scratch(&png);
        return IMAGE_OK;
    }
    case IMAGE_FORMAT_JPEG: {
        struct jpeg_frame frame;
        enum image_result result = jpeg_find_frame(data, size, &frame);
        if (result != IMAGE_OK) {
            return result;
        }
        info->width = frame.width;
        info->height = frame.height;
        info->scratch_bytes = jpeg_scratch(&frame);
        return IMAGE_OK;
    }
    case IMAGE_FORMAT_BMP: {
        struct bmp_header bmp;
        enum image_result result = bmp_read_header(data, size, &bmp);
        if (result != IMAGE_OK) {
            return result;
        }
        info->width = (uint32_t)bmp.width;
        info->height = (uint32_t)(bmp.height < 0 ? -bmp.height : bmp.height);
        info->scratch_bytes = pixel_bytes(info->width, info->height) + 64;
        return IMAGE_OK;
    }
    default:
        return IMAGE_UNKNOWN_FORMAT;
    }
}

enum image_result image_decode(const uint8_t *data, size_t size,
                               void *scratch, size_t scratch_size,
                               struct image *image) {
    image->width = 0;
    image->height = 0;
    image->pixels = NULL;
    struct arena arena = {scratch, scratch_size, 0};
    if (((uintptr_t)scratch & 15) != 0) {
        size_t skip = 16 - ((uintptr_t)scratch & 15);
        if (skip > scratch_size) {
            return IMAGE_NO_MEMORY;
        }
        arena.base += skip;
        arena.size -= skip;
    }
    switch (image_sniff(data, size)) {
    case IMAGE_FORMAT_PNG:
        return png_decode(data, size, &arena, image);
    case IMAGE_FORMAT_JPEG:
        return jpeg_decode(data, size, &arena, image);
    case IMAGE_FORMAT_BMP:
        return bmp_decode(data, size, &arena, image);
    default:
        return IMAGE_UNKNOWN_FORMAT;
    }
}

const char *image_result_text(enum image_result result) {
    switch (result) {
    case IMAGE_OK:
        return "OK";
    case IMAGE_UNKNOWN_FORMAT:
        return "NOT A PNG, JPEG OR BMP IMAGE";
    case IMAGE_UNSUPPORTED:
        return "IMAGE TYPE NOT SUPPORTED";
    case IMAGE_CORRUPT:
        return "IMAGE FILE IS DAMAGED";
    case IMAGE_TOO_LARGE:
        return "IMAGE IS TOO LARGE";
    case IMAGE_NO_MEMORY:
        return "NOT ENOUGH MEMORY FOR THE IMAGE";
    case IMAGE_SAVE_FAILED:
        return "COULD NOT SAVE THE PICTURE";
    }
    return "IMAGE ERROR";
}

const char *image_format_name(enum image_format format) {
    switch (format) {
    case IMAGE_FORMAT_PNG:
        return "PNG";
    case IMAGE_FORMAT_JPEG:
        return "JPEG";
    case IMAGE_FORMAT_BMP:
        return "BMP";
    default:
        return "UNKNOWN";
    }
}

/* ------------------------------------------------------------ resampling */

static void resample_shrink_row(const uint32_t *source, uint32_t stride,
                                uint32_t sx, uint32_t sw, uint32_t y0,
                                uint32_t y1, uint32_t *out, uint32_t dw) {
    for (uint32_t x = 0; x < dw; x++) {
        uint32_t x0 = sx + (uint32_t)((uint64_t)x * sw / dw);
        uint32_t x1 = sx + (uint32_t)(((uint64_t)x + 1) * sw / dw);
        if (x1 <= x0) {
            x1 = x0 + 1;
        }
        uint64_t a = 0, r = 0, g = 0, b = 0, n = 0;
        for (uint32_t y = y0; y < y1; y++) {
            const uint32_t *row = source + (size_t)y * stride;
            for (uint32_t i = x0; i < x1; i++) {
                uint32_t p = row[i];
                a += p >> 24;
                r += (p >> 16) & 0xff;
                g += (p >> 8) & 0xff;
                b += p & 0xff;
                n++;
            }
        }
        out[x] = (uint32_t)((a + n / 2) / n) << 24
               | (uint32_t)((r + n / 2) / n) << 16
               | (uint32_t)((g + n / 2) / n) << 8
               | (uint32_t)((b + n / 2) / n);
    }
}

static uint32_t lerp_pixel(uint32_t p, uint32_t q, uint32_t t) {
    /* t in 0..256 */
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        uint32_t a = (p >> shift) & 0xff;
        uint32_t b = (q >> shift) & 0xff;
        result |= ((a * (256 - t) + b * t + 128) >> 8) << shift;
    }
    return result;
}

void image_resample(const uint32_t *source, uint32_t source_stride,
                    uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                    uint32_t *destination, uint32_t destination_stride,
                    uint32_t dw, uint32_t dh) {
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) {
        return;
    }
    bool shrink = sw >= dw && sh >= dh;
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t *out = destination + (size_t)y * destination_stride;
        if (shrink) {
            uint32_t y0 = sy + (uint32_t)((uint64_t)y * sh / dh);
            uint32_t y1 = sy + (uint32_t)(((uint64_t)y + 1) * sh / dh);
            if (y1 <= y0) {
                y1 = y0 + 1;
            }
            resample_shrink_row(source, source_stride, sx, sw, y0, y1, out,
                                dw);
            continue;
        }
        /* Bilinear: sample at pixel centres, 8 fractional bits. */
        int64_t fy = ((int64_t)(2 * y + 1) * sh * 256) / (2 * (int64_t)dh)
                   - 128;
        if (fy < 0) fy = 0;
        uint32_t row0 = (uint32_t)(fy >> 8);
        uint32_t ty = (uint32_t)(fy & 255);
        if (row0 >= sh - 1) {
            row0 = sh - 1;
            ty = 0;
        }
        uint32_t row1 = row0 + 1 < sh ? row0 + 1 : row0;
        const uint32_t *r0 = source + (size_t)(sy + row0) * source_stride + sx;
        const uint32_t *r1 = source + (size_t)(sy + row1) * source_stride + sx;
        for (uint32_t x = 0; x < dw; x++) {
            int64_t fx = ((int64_t)(2 * x + 1) * sw * 256) / (2 * (int64_t)dw)
                       - 128;
            if (fx < 0) fx = 0;
            uint32_t col0 = (uint32_t)(fx >> 8);
            uint32_t tx = (uint32_t)(fx & 255);
            if (col0 >= sw - 1) {
                col0 = sw - 1;
                tx = 0;
            }
            uint32_t col1 = col0 + 1 < sw ? col0 + 1 : col0;
            uint32_t top = lerp_pixel(r0[col0], r0[col1], tx);
            uint32_t bottom = lerp_pixel(r1[col0], r1[col1], tx);
            out[x] = lerp_pixel(top, bottom, ty);
        }
    }
}
