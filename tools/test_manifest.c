#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "manifest.h"
#include "permissions.h"
#include <opos.h>

/* The SDK and the kernel must agree on the note and the bits. */
_Static_assert(sizeof(struct opos_manifest_note)
                   == 12 + 8 + MANIFEST_DESCRIPTOR_SIZE,
               "note layout");
_Static_assert(OPOS_CAP_STORAGE == PERMISSION_CAP_STORAGE
                   && OPOS_CAP_FILES_PICK == PERMISSION_CAP_FILES_PICK
                   && OPOS_CAP_FILES_SHARED == PERMISSION_CAP_FILES_SHARED
                   && OPOS_CAP_NET == PERMISSION_CAP_NET
                   && OPOS_CAP_CLIPBOARD_READ == PERMISSION_CAP_CLIPBOARD_READ
                   && OPOS_CAP_CLIPBOARD_WRITE
                          == PERMISSION_CAP_CLIPBOARD_WRITE
                   && OPOS_CAP_WINDOW == PERMISSION_CAP_WINDOW,
               "capability bits");

OPOS_MANIFEST("Fetch", "1.0", OPOS_CAP_NET | OPOS_CAP_CLIPBOARD_READ);

/* The ABI note crt0.S writes before any manifest. */
static const uint8_t abi_note[] = {
    5, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 'O', 'P', 'O', 'S', 0, 0, 0, 0,
    1, 0, 0, 0,
};

static uint8_t image[4096];

static void put_u16(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *at, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        at[i] = (uint8_t)(value >> (8 * i));
    }
}

static void put_u64(uint8_t *at, uint64_t value) {
    put_u32(at, (uint32_t)value);
    put_u32(at + 4, (uint32_t)(value >> 32));
}

/* An ELF64 header, one PT_LOAD and one PT_NOTE holding notes. Returns
   the image size; *notes_at is where the notes start. */
static size_t build(const uint8_t *notes, size_t notes_size,
                    size_t *notes_at) {
    memset(image, 0, sizeof(image));
    memcpy(image, "\x7f" "ELF", 4);
    image[4] = 2;
    image[5] = 1;
    put_u64(image + 32, 64);   /* program header table */
    put_u16(image + 54, 56);
    put_u16(image + 56, 2);
    uint8_t *load = image + 64;
    put_u32(load, 1);          /* PT_LOAD, ignored */
    uint8_t *note = image + 64 + 56;
    size_t start = 64 + 2 * 56;
    put_u32(note, 4);          /* PT_NOTE */
    put_u64(note + 8, start);
    put_u64(note + 32, notes_size);
    memcpy(image + start, notes, notes_size);
    if (notes_at != NULL) {
        *notes_at = start;
    }
    return start + notes_size;
}

static uint8_t notes[512];

static size_t with_manifest(void) {
    memcpy(notes, abi_note, sizeof(abi_note));
    memcpy(notes + sizeof(abi_note), &opos_manifest_note_,
           sizeof(opos_manifest_note_));
    return sizeof(abi_note) + sizeof(opos_manifest_note_);
}

int main(void) {
    struct manifest manifest;
    size_t at;

    /* Written by the SDK macro, read by the kernel. */
    size_t size = build(notes, with_manifest(), &at);
    assert(manifest_read(image, size, &manifest));
    assert(strcmp(manifest.name, "Fetch") == 0);
    assert(strcmp(manifest.version, "1.0") == 0);
    assert(manifest.capabilities
           == (PERMISSION_CAP_NET | PERMISSION_CAP_CLIPBOARD_READ));

    /* No manifest: only the ABI note, or no notes at all. */
    size = build(abi_note, sizeof(abi_note), NULL);
    assert(!manifest_read(image, size, &manifest));
    assert(manifest.name[0] == '\0' && manifest.capabilities == 0);
    size = build(notes, 0, NULL);
    assert(!manifest_read(image, size, &manifest));

    /* Not ELF64 little-endian, or too short. */
    size = build(notes, with_manifest(), &at);
    image[0] = 0;
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    image[4] = 1;
    assert(!manifest_read(image, size, &manifest));
    assert(!manifest_read(image, 10, &manifest));
    assert(!manifest_read(NULL, 0, &manifest));

    /* Header and segment bounds. */
    size = build(notes, with_manifest(), &at);
    put_u64(image + 32, size);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    put_u16(image + 56, 0xFFFF);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    put_u16(image + 54, 8);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    put_u64(image + 64 + 56 + 32, UINT64_MAX - 4);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    put_u64(image + 64 + 56 + 8, size + 1);
    assert(!manifest_read(image, size, &manifest));

    /* Note sizes that run past the segment, including wrap-around. */
    size = build(notes, with_manifest(), &at);
    put_u32(image + at + sizeof(abi_note) + 4, 0xFFFFFFFF);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    put_u32(image + at + sizeof(abi_note), 0xFFFFFFFE);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest() - 2, &at);
    assert(!manifest_read(image, size, &manifest));

    size_t descriptor = sizeof(abi_note) + 20;
    /* Wrong descriptor size or format. */
    size = build(notes, with_manifest(), &at);
    put_u32(image + at + sizeof(abi_note) + 4, 52);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    put_u32(image + at + descriptor, 2);
    assert(!manifest_read(image, size, &manifest));

    /* Names: empty, unterminated, control characters. */
    size = build(notes, with_manifest(), &at);
    image[at + descriptor + 8] = 0;
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    memset(image + at + descriptor + 8, 'A', 32);
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    image[at + descriptor + 9] = '\n';
    assert(!manifest_read(image, size, &manifest));
    size = build(notes, with_manifest(), &at);
    memset(image + at + descriptor + 40, 'v', 16);
    assert(!manifest_read(image, size, &manifest));
    /* An empty version is fine. */
    size = build(notes, with_manifest(), &at);
    memset(image + at + descriptor + 40, 0, 16);
    assert(manifest_read(image, size, &manifest));
    assert(manifest.version[0] == '\0');

    /* Unknown capability bits are dropped. */
    size = build(notes, with_manifest(), &at);
    put_u32(image + at + descriptor + 4, 0x80000000u | PERMISSION_CAP_NET);
    assert(manifest_read(image, size, &manifest));
    assert(manifest.capabilities == PERMISSION_CAP_NET);

    /* Two manifests: refuse both rather than pick one. */
    size_t one = with_manifest();
    memcpy(notes + one, &opos_manifest_note_, sizeof(opos_manifest_note_));
    size = build(notes, one + sizeof(opos_manifest_note_), &at);
    assert(!manifest_read(image, size, &manifest));

    /* Another owner's type 2 note is not ours. */
    one = with_manifest();
    notes[sizeof(abi_note) + 12] = 'X';
    size = build(notes, one, &at);
    assert(!manifest_read(image, size, &manifest));

    puts("manifest: passed");
    return 0;
}
