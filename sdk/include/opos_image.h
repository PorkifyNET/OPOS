#ifndef OPOS_IMAGE_H
#define OPOS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Image decoding for PNG, JPEG (baseline and progressive) and BMP. The
 * decoder needs no C library: the caller passes one scratch buffer, sized
 * by image_probe(), and the decoded pixels are placed at its start.
 * Input files are untrusted; every read is bounds-checked.
 *
 * The same source builds into the kernel and into the SDK's libopos.
 */

/* Largest accepted image: 16384 pixels per side, 40 million in total. */
#define IMAGE_MAX_DIMENSION 16384u
#define IMAGE_MAX_PIXELS (40u * 1024u * 1024u)

enum image_format {
    IMAGE_FORMAT_UNKNOWN,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_BMP,
};

enum image_result {
    IMAGE_OK,
    IMAGE_UNKNOWN_FORMAT,
    /* A valid file using a feature this decoder lacks (arithmetic-coded
       or lossless JPEG, CMYK, RLE-compressed BMP, ...). */
    IMAGE_UNSUPPORTED,
    IMAGE_CORRUPT,
    IMAGE_TOO_LARGE,
    IMAGE_NO_MEMORY,
    /* Not produced by the decoder: for callers that store a result. */
    IMAGE_SAVE_FAILED,
};

struct image_info {
    enum image_format format;
    uint32_t width;
    uint32_t height;
    /* Size of the scratch buffer image_decode() needs. */
    size_t scratch_bytes;
};

/* Pixels are 0xAARRGGBB (alpha 255 = opaque), width pixels per row. */
struct image {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
};

/* True if the first bytes look like a supported image file. */
enum image_format image_sniff(const uint8_t *data, size_t size);
/* Reads the header only: format, size and the scratch space needed. */
enum image_result image_probe(const uint8_t *data, size_t size,
                              struct image_info *info);
/* Decodes into scratch (16-byte aligned). image->pixels points into it. */
enum image_result image_decode(const uint8_t *data, size_t size,
                               void *scratch, size_t scratch_size,
                               struct image *image);
const char *image_result_text(enum image_result result);
const char *image_format_name(enum image_format format);

/*
 * Scales the source rectangle (sx, sy, sw, sh) of an image with the given
 * stride onto a dw x dh destination with its own stride. Shrinking
 * averages every covered source pixel; growing is bilinear.
 */
void image_resample(const uint32_t *source, uint32_t source_stride,
                    uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh,
                    uint32_t *destination, uint32_t destination_stride,
                    uint32_t dw, uint32_t dh);

#endif
