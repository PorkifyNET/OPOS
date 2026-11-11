#ifndef OPOS_MANIFEST_H
#define OPOS_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A program's manifest (docs/app-sandbox.md): what it is called and which
 * capabilities it may ask the person for. It is a request, never a grant.
 *
 * It is an ELF note in a PT_NOTE segment: name "OPOS", type 2, and a
 * 56-byte descriptor, little-endian:
 *   format (u32, 1), capabilities (u32, PERMISSION_CAP_* bits),
 *   name (32 bytes, NUL-terminated, not empty),
 *   version (16 bytes, NUL-terminated)
 * The SDK writes it with OPOS_MANIFEST() from <opos.h>. Type 1 in the same
 * section is the older ABI-version note and is ignored here.
 */

#define MANIFEST_NOTE_TYPE 2
#define MANIFEST_FORMAT 1
#define MANIFEST_NAME_MAX 31
#define MANIFEST_VERSION_MAX 15
#define MANIFEST_DESCRIPTOR_SIZE 56

struct manifest {
    uint32_t capabilities;
    char name[MANIFEST_NAME_MAX + 1];
    char version[MANIFEST_VERSION_MAX + 1];
};

/*
 * Finds the manifest in an ELF64 image. Returns false, with manifest
 * cleared, when there is none or anything about it is malformed: bad
 * headers or note bounds, a wrong size or format, an empty or
 * unterminated name, a control character, or more than one manifest.
 * Capability bits this kernel does not know are dropped.
 */
bool manifest_read(const uint8_t *image, size_t size,
                   struct manifest *manifest);

#endif
