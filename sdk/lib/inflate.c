/*
 * Inflate for OPOS applications: raw deflate, zlib and gzip streams into a
 * growing memory buffer. Canonical Huffman decoding in the style of Mark
 * Adler's puff.c (one bit at a time with per-length counts); simple and
 * fast enough for web pages.
 *
 * The code has no dependencies besides malloc, so tools/host tests can
 * build it on its own.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/opos_http.h"

#ifndef OPOS_EINVAL
#define OPOS_EINVAL 22
#endif
#ifndef OPOS_ENOMEM
#define OPOS_ENOMEM 12
#endif

#define MAX_BITS 15
#define LITERAL_CODES 288
#define DISTANCE_CODES 30

struct huffman {
    uint16_t count[MAX_BITS + 1]; /* codes of each length */
    uint16_t symbol[LITERAL_CODES]; /* symbols ordered by code */
};

struct state {
    const uint8_t *in;
    size_t in_length;
    size_t in_position;
    uint32_t bit_buffer;
    int bit_count;

    uint8_t *out;
    size_t out_length;
    size_t out_capacity;
    size_t out_max;
    long error; /* 0 or a negative error */
};

static bool fail(struct state *s, long error) {
    if (s->error == 0) {
        s->error = error;
    }
    return false;
}

static int get_bits(struct state *s, int need) {
    uint32_t value = s->bit_buffer;
    while (s->bit_count < need) {
        if (s->in_position >= s->in_length) {
            fail(s, -OPOS_EINVAL);
            return 0;
        }
        value |= (uint32_t)s->in[s->in_position++] << s->bit_count;
        s->bit_count += 8;
    }
    s->bit_buffer = value >> need;
    s->bit_count -= need;
    return (int)(value & ((1u << need) - 1));
}

static bool put_byte(struct state *s, uint8_t byte) {
    if (s->out_length == s->out_capacity) {
        if (s->out_capacity >= s->out_max) {
            return fail(s, -OPOS_EFBIG);
        }
        size_t capacity = s->out_capacity == 0 ? 4096 : s->out_capacity * 2;
        if (capacity > s->out_max) {
            capacity = s->out_max;
        }
        /* One extra byte keeps room for the terminating NUL. */
        uint8_t *grown = realloc(s->out, capacity + 1);
        if (grown == NULL) {
            return fail(s, -OPOS_ENOMEM);
        }
        s->out = grown;
        s->out_capacity = capacity;
    }
    s->out[s->out_length++] = byte;
    return true;
}

/* Builds the decoding table from code lengths. Returns the number of
   unused codes: 0 for a complete code, > 0 incomplete, < 0 oversubscribed. */
static int build(struct huffman *h, const uint16_t *lengths, int n) {
    for (int length = 0; length <= MAX_BITS; length++) {
        h->count[length] = 0;
    }
    for (int symbol = 0; symbol < n; symbol++) {
        h->count[lengths[symbol]]++;
    }
    if (h->count[0] == n) {
        return 0;
    }
    int left = 1;
    for (int length = 1; length <= MAX_BITS; length++) {
        left <<= 1;
        left -= h->count[length];
        if (left < 0) {
            return left;
        }
    }
    uint16_t offsets[MAX_BITS + 1];
    offsets[1] = 0;
    for (int length = 1; length < MAX_BITS; length++) {
        offsets[length + 1] = (uint16_t)(offsets[length] + h->count[length]);
    }
    for (int symbol = 0; symbol < n; symbol++) {
        if (lengths[symbol] != 0) {
            h->symbol[offsets[lengths[symbol]]++] = (uint16_t)symbol;
        }
    }
    return left;
}

/* Returns the next symbol, or -1 on a bad code or end of input. */
static int decode(struct state *s, const struct huffman *h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= MAX_BITS; length++) {
        code |= get_bits(s, 1);
        if (s->error != 0) {
            return -1;
        }
        int count = h->count[length];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    fail(s, -OPOS_EINVAL);
    return -1;
}

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint16_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577};
static const uint16_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool codes(struct state *s, const struct huffman *literals,
                  const struct huffman *distances) {
    for (;;) {
        int symbol = decode(s, literals);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 256) {
            if (!put_byte(s, (uint8_t)symbol)) {
                return false;
            }
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) {
                return fail(s, -OPOS_EINVAL);
            }
            int length = length_base[symbol] + get_bits(s, length_extra[symbol]);
            int distance_symbol = decode(s, distances);
            if (distance_symbol < 0 || distance_symbol >= DISTANCE_CODES) {
                return fail(s, -OPOS_EINVAL);
            }
            size_t distance = (size_t)distance_base[distance_symbol]
                + (size_t)get_bits(s, distance_extra[distance_symbol]);
            if (s->error != 0) {
                return false;
            }
            if (distance > s->out_length) {
                return fail(s, -OPOS_EINVAL);
            }
            while (length-- > 0) {
                if (!put_byte(s, s->out[s->out_length - distance])) {
                    return false;
                }
            }
        }
    }
}

static bool stored(struct state *s) {
    s->bit_buffer = 0;
    s->bit_count = 0;
    if (s->in_position + 4 > s->in_length) {
        return fail(s, -OPOS_EINVAL);
    }
    unsigned length = s->in[s->in_position] | (unsigned)s->in[s->in_position + 1] << 8;
    unsigned inverse = s->in[s->in_position + 2]
                     | (unsigned)s->in[s->in_position + 3] << 8;
    s->in_position += 4;
    if (length != (~inverse & 0xffff)
        || s->in_position + length > s->in_length) {
        return fail(s, -OPOS_EINVAL);
    }
    while (length-- > 0) {
        if (!put_byte(s, s->in[s->in_position++])) {
            return false;
        }
    }
    return true;
}

static bool fixed(struct state *s) {
    static struct huffman literals, distances;
    static bool built;
    if (!built) {
        uint16_t lengths[LITERAL_CODES];
        int symbol = 0;
        for (; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < LITERAL_CODES; symbol++) lengths[symbol] = 8;
        build(&literals, lengths, LITERAL_CODES);
        for (symbol = 0; symbol < DISTANCE_CODES; symbol++) lengths[symbol] = 5;
        build(&distances, lengths, DISTANCE_CODES);
        built = true;
    }
    return codes(s, &literals, &distances);
}

static bool dynamic(struct state *s) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint16_t lengths[LITERAL_CODES + DISTANCE_CODES];
    struct huffman literals, distances, lengths_code;

    int literal_count = get_bits(s, 5) + 257;
    int distance_count = get_bits(s, 5) + 1;
    int length_count = get_bits(s, 4) + 4;
    if (s->error != 0) {
        return false;
    }
    if (literal_count > 286 || distance_count > DISTANCE_CODES) {
        return fail(s, -OPOS_EINVAL);
    }
    int index = 0;
    for (; index < length_count; index++) {
        lengths[order[index]] = (uint16_t)get_bits(s, 3);
    }
    for (; index < 19; index++) {
        lengths[order[index]] = 0;
    }
    if (build(&lengths_code, lengths, 19) != 0) {
        return fail(s, -OPOS_EINVAL);
    }
    index = 0;
    while (index < literal_count + distance_count) {
        int symbol = decode(s, &lengths_code);
        if (symbol < 0) {
            return false;
        }
        if (symbol < 16) {
            lengths[index++] = (uint16_t)symbol;
        } else {
            uint16_t previous = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0) {
                    return fail(s, -OPOS_EINVAL);
                }
                previous = lengths[index - 1];
                repeat = 3 + get_bits(s, 2);
            } else if (symbol == 17) {
                repeat = 3 + get_bits(s, 3);
            } else {
                repeat = 11 + get_bits(s, 7);
            }
            if (s->error != 0 || index + repeat > literal_count + distance_count) {
                return fail(s, -OPOS_EINVAL);
            }
            while (repeat-- > 0) {
                lengths[index++] = previous;
            }
        }
    }
    if (lengths[256] == 0) {
        return fail(s, -OPOS_EINVAL);
    }
    int result = build(&literals, lengths, literal_count);
    if (result < 0 || (result > 0 && literal_count - literals.count[0] != 1)) {
        return fail(s, -OPOS_EINVAL);
    }
    result = build(&distances, lengths + literal_count, distance_count);
    if (result < 0 || (result > 0 && distance_count - distances.count[0] != 1)) {
        return fail(s, -OPOS_EINVAL);
    }
    return codes(s, &literals, &distances);
}

static bool inflate_stream(struct state *s) {
    int last;
    do {
        last = get_bits(s, 1);
        int type = get_bits(s, 2);
        if (s->error != 0) {
            return false;
        }
        bool ok = type == 0 ? stored(s) : type == 1 ? fixed(s)
                : type == 2 ? dynamic(s) : fail(s, -OPOS_EINVAL);
        if (!ok) {
            return false;
        }
    } while (!last);
    return true;
}

/* Skips the gzip header; returns the offset of the deflate data or 0. */
static size_t gzip_header(const uint8_t *data, size_t length) {
    if (length < 18 || data[0] != 0x1f || data[1] != 0x8b || data[2] != 8) {
        return 0;
    }
    unsigned flags = data[3];
    size_t position = 10;
    if (flags & 4) { /* FEXTRA */
        if (position + 2 > length) return 0;
        position += 2 + (data[position] | (size_t)data[position + 1] << 8);
    }
    if (flags & 8) { /* FNAME */
        while (position < length && data[position++] != 0) {}
    }
    if (flags & 16) { /* FCOMMENT */
        while (position < length && data[position++] != 0) {}
    }
    if (flags & 2) { /* FHCRC */
        position += 2;
    }
    return position < length ? position : 0;
}

long opos_inflate(const uint8_t *input, size_t length, int format,
                  size_t max_output, uint8_t **out, size_t *out_length) {
    struct state s;
    memset(&s, 0, sizeof(s));
    s.in = input;
    s.in_length = length;
    s.out_max = max_output;
    *out = NULL;
    *out_length = 0;
    if (format == 0) {
        size_t header = gzip_header(input, length);
        if (header != 0) {
            s.in_position = header;
        } else if (length >= 2 && (input[0] & 0x0f) == 8
                   && ((input[0] << 8) | input[1]) % 31 == 0
                   && (input[1] & 0x20) == 0) {
            s.in_position = 2; /* zlib header, no preset dictionary */
        }
    }
    if (!inflate_stream(&s)) {
        free(s.out);
        return s.error != 0 ? s.error : -OPOS_EINVAL;
    }
    if (s.out == NULL) {
        s.out = malloc(1);
        if (s.out == NULL) {
            return -OPOS_ENOMEM;
        }
    }
    s.out[s.out_length] = '\0';
    *out = s.out;
    *out_length = s.out_length;
    return 0;
}
