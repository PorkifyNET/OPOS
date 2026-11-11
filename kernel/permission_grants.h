#ifndef OPOS_PERMISSION_GRANTS_H
#define OPOS_PERMISSION_GRANTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

/*
 * Per-account record of what the user allowed or refused each disk
 * program (docs/app-sandbox.md). A program is its executable's SHA-256,
 * so a changed executable is a new program.
 *
 * Stored at /disk/Users/<name>/AppData/Permissions.dat. Only this code
 * writes it; permissions.c hides it from programs and makes it read-only
 * in OPOS's own windows.
 *
 * File layout, little-endian:
 *   header (16): "OPGR", version (1), 3 reserved zero bytes,
 *                flags (u32), record count (u32)
 *   record (96): program SHA-256 (32), granted (u32), denied (u32),
 *                ask (u32), 4 reserved zero bytes, updated (u64),
 *                display name (40, NUL-padded)
 * The file is exactly header + count * record bytes.
 */

#define PERMISSION_GRANTS_MAX 64
#define PERMISSION_GRANT_NAME_MAX 39
#define PERMISSION_GRANTS_HEADER_SIZE 16
#define PERMISSION_GRANTS_RECORD_SIZE 96
#define PERMISSION_GRANTS_FILE_MAX                                         \
    (PERMISSION_GRANTS_HEADER_SIZE                                         \
     + PERMISSION_GRANTS_MAX * PERMISSION_GRANTS_RECORD_SIZE)

/* Programs already in <volume>/Programs were recorded (once per account,
   at the first login with files.shared enforced). */
#define PERMISSION_GRANTS_MIGRATED (1u << 0)

struct permission_grant {
    uint8_t program[SHA256_DIGEST_SIZE];
    /* PERMISSION_CAP_* bits the user allowed. */
    uint32_t granted;
    /* Bits the user refused. They are not asked again. A bit is never in
       both sets. */
    uint32_t denied;
    /* Undecided bits the program may ask for on first use, beyond those
       every program may ask for. Never overlaps granted or denied. */
    uint32_t ask;
    /* Real-time clock as decimal YYYYMMDDhhmmss, or 0 when unknown. */
    uint64_t updated;
    char name[PERMISSION_GRANT_NAME_MAX + 1];
};

struct permission_grants {
    uint32_t flags;
    size_t count;
    struct permission_grant records[PERMISSION_GRANTS_MAX];
};

void permission_grants_clear(struct permission_grants *grants);

/*
 * Parses a whole file. Returns false and leaves grants empty when the data
 * is malformed: wrong magic, version, size or count, a name without a
 * terminator, a bit in both sets, or a program listed twice. Capability
 * bits this version does not know are dropped.
 */
bool permission_grants_decode(const uint8_t *data, size_t size,
                              struct permission_grants *grants);

/* Writes the file image. Returns its size, or 0 when capacity is short. */
size_t permission_grants_encode(const struct permission_grants *grants,
                                uint8_t *out, size_t capacity);

/* The record for a program, or NULL. */
struct permission_grant *permission_grants_find(
    struct permission_grants *grants,
    const uint8_t program[SHA256_DIGEST_SIZE]);

/*
 * Stores a record. granted, denied and ask replace the stored sets; a bit
 * in several counts as denied, then granted. name is cut to
 * PERMISSION_GRANT_NAME_MAX bytes. Returns false when the store is full.
 */
bool permission_grants_set(struct permission_grants *grants,
                           const uint8_t program[SHA256_DIGEST_SIZE],
                           const char *name, uint32_t granted,
                           uint32_t denied, uint32_t ask, uint64_t updated);

/* Forgets a program. Returns false when it had no record. */
bool permission_grants_remove(struct permission_grants *grants,
                              const uint8_t program[SHA256_DIGEST_SIZE]);

/* Builds the account's Permissions.dat path. Returns its length or 0. */
size_t permission_grants_path(size_t account, char *out, size_t capacity);

/*
 * Reads the account's store. A missing file gives an empty store and true.
 * An unreadable or malformed file gives an empty store and false, so the
 * caller denies everything rather than trusting part of it.
 */
bool permission_grants_load(size_t account, struct permission_grants *grants);

/* Writes the account's store, creating AppData when needed. */
bool permission_grants_save(size_t account,
                            const struct permission_grants *grants);

/* SHA-256 of the file at path: the key for permission_grants_find. */
bool permission_program_hash(const char *path, size_t length,
                             uint8_t out[SHA256_DIGEST_SIZE]);

struct permission_identity;

/*
 * Called when a process starts or replaces itself with the executable at
 * path, whose bytes are image. For a disk program: confines the identity,
 * records the program's hash, and sets capabilities to what the parent
 * held intersected with the defaults plus the account's grants. askable
 * becomes the stored ask bits the parent also held. Other programs are
 * left alone. A missing or malformed store grants nothing extra.
 */
void permission_apply_program(struct permission_identity *identity,
                              const char *path, size_t length,
                              const uint8_t *image, size_t size);

/*
 * Stores the person's answer for one capability of a program: allowed or
 * refused, and no longer askable. Returns false when the store could not
 * be read or written.
 */
bool permission_grants_decide(size_t account,
                              const uint8_t program[SHA256_DIGEST_SIZE],
                              const char *name, uint32_t capability,
                              bool allow, uint64_t now);

/*
 * Once per account: every ELF executable below /disk/Programs without a
 * record gets one that may ask for files.shared, so programs installed
 * before files.shared was enforced can ask instead of failing silently.
 * Programs that appear later get no such record. Returns true when the
 * store is (now) marked migrated.
 */
bool permission_grants_migrate(size_t account, uint64_t now);

#endif
