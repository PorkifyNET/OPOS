#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "manifest.h"
#include "permissions.h"

/* See manifest.h. Every offset comes from the file, so every read is
   checked against the image size first, without overflow. */

#define PT_NOTE 4
#define ELF_HEADER_SIZE 64
#define PROGRAM_HEADER_SIZE 56

static uint16_t read_u16(const uint8_t *data) {
    return (uint16_t)(data[0] | data[1] << 8);
}

static uint32_t read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8
        | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t read_u64(const uint8_t *data) {
    return (uint64_t)read_u32(data) | (uint64_t)read_u32(data + 4) << 32;
}

static void clear(struct manifest *manifest) {
    uint8_t *bytes = (uint8_t *)manifest;
    for (size_t i = 0; i < sizeof(*manifest); i++) {
        bytes[i] = 0;
    }
}

/* A NUL-terminated printable string in field; copies it into out. */
static bool text_field(const uint8_t *field, size_t field_size, char *out,
                       bool may_be_empty) {
    size_t length = 0;
    while (length < field_size && field[length] != 0) {
        if (field[length] < 0x20 || field[length] == 0x7F) {
            return false;
        }
        length++;
    }
    if (length == field_size || (length == 0 && !may_be_empty)) {
        return false;
    }
    for (size_t i = 0; i <= length; i++) {
        out[i] = (char)field[i];
    }
    return true;
}

static size_t align4(size_t value) {
    return (value + 3) & ~(size_t)3;
}

/* Walks the notes in [start, start + length). Returns -1 when malformed,
   otherwise how many manifests it found (the first goes to manifest). */
static int scan_notes(const uint8_t *image, size_t start, size_t length,
                      struct manifest *manifest, int found) {
    size_t offset = 0;
    while (offset < length) {
        if (length - offset < 12) {
            return -1;
        }
        const uint8_t *note = image + start + offset;
        uint32_t name_size = read_u32(note);
        uint32_t descriptor_size = read_u32(note + 4);
        uint32_t type = read_u32(note + 8);
        size_t name_space = align4(name_size);
        size_t descriptor_space = align4(descriptor_size);
        size_t remaining = length - offset - 12;
        if (name_space < name_size || name_space > remaining
            || descriptor_space < descriptor_size
            || descriptor_space > remaining - name_space) {
            return -1;
        }
        const uint8_t *name = note + 12;
        const uint8_t *descriptor = name + name_space;
        bool ours = name_size == 5 && name[0] == 'O' && name[1] == 'P'
            && name[2] == 'O' && name[3] == 'S' && name[4] == 0;
        if (ours && type == MANIFEST_NOTE_TYPE) {
            if (descriptor_size != MANIFEST_DESCRIPTOR_SIZE
                || read_u32(descriptor) != MANIFEST_FORMAT
                || found != 0) {
                return -1;
            }
            manifest->capabilities =
                read_u32(descriptor + 4) & PERMISSION_CAPS_ALL;
            if (!text_field(descriptor + 8, MANIFEST_NAME_MAX + 1,
                            manifest->name, false)
                || !text_field(descriptor + 40, MANIFEST_VERSION_MAX + 1,
                               manifest->version, true)) {
                return -1;
            }
            found++;
        }
        offset += 12 + name_space + descriptor_space;
    }
    return found;
}

int manifest_scan_segment(const uint8_t *notes, size_t length,
                          struct manifest *manifest, int found) {
    if (notes == NULL || manifest == NULL || found < 0) {
        return -1;
    }
    return scan_notes(notes, 0, length, manifest, found);
}

bool manifest_read(const uint8_t *image, size_t size,
                   struct manifest *manifest) {
    if (manifest == NULL) {
        return false;
    }
    clear(manifest);
    if (image == NULL || size < ELF_HEADER_SIZE || image[0] != 0x7F
        || image[1] != 'E' || image[2] != 'L' || image[3] != 'F'
        || image[4] != 2 /* 64-bit */ || image[5] != 1 /* little-endian */) {
        return false;
    }
    uint64_t table = read_u64(image + 32);
    uint16_t entry_size = read_u16(image + 54);
    uint16_t count = read_u16(image + 56);
    if (entry_size < PROGRAM_HEADER_SIZE || table > size
        || (uint64_t)entry_size * count > size - table) {
        return false;
    }
    int found = 0;
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *header = image + table + (size_t)i * entry_size;
        if (read_u32(header) != PT_NOTE) {
            continue;
        }
        uint64_t offset = read_u64(header + 8);
        uint64_t length = read_u64(header + 32);
        if (offset > size || length > size - offset) {
            clear(manifest);
            return false;
        }
        found = scan_notes(image, (size_t)offset, (size_t)length, manifest,
                           found);
        if (found < 0) {
            clear(manifest);
            return false;
        }
    }
    if (found != 1) {
        clear(manifest);
        return false;
    }
    return true;
}
