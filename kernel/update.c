#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bearssl_ec.h>

#include "block.h"
#include "boot_display.h"
#include "console.h"
#include "fat16.h"
#include "installer.h"
#include "scheduler.h"
#include "sha256.h"
#include "timer.h"
#include "update.h"
#include "update_key.h"
#include "vfs.h"

#ifndef OPOS_VERSION
#define OPOS_VERSION "0.0.0"
#endif

#define STAGE_DIRECTORY "/disk/OPOS/UPDATE"
#define STAGE_MANIFEST STAGE_DIRECTORY "/MANIFEST.TXT"
#define STAGE_SIGNATURE STAGE_DIRECTORY "/MANIFEST.SIG"
#define STAGE_STATE STAGE_DIRECTORY "/STATE.TXT"
#define OLD_DIRECTORY "/disk/boot/old"
#define SYSTEM_PREFIX "/disk/"
#define KERNEL_PATH "boot/kernel.elf"
#define INITRAMFS_PATH "boot/initramfs.tar"
#define COPY_CHUNK 65536
#define FULL_PATH_MAX (UPDATE_PATH_MAX + 8)
#define VERSION_PARTS 4

/* Steps recorded in STATE.TXT. */
#define STATE_PREPARED "PREPARED"
#define STATE_COPYING "COPYING"

struct entry {
    uint8_t hash[SHA256_DIGEST_SIZE];
    uint64_t size;
    char asset[UPDATE_ASSET_MAX];
    char path[UPDATE_PATH_MAX];
    bool needed;
    bool complete;
    uint64_t received;
    struct sha256_context hashing;
};

struct version {
    uint32_t parts[VERSION_PARTS];
};

static uint8_t manifest_text[UPDATE_MANIFEST_MAX];
static size_t manifest_length;
static uint8_t signature_bytes[UPDATE_SIGNATURE_MAX];
static size_t signature_length;
static struct entry entries[UPDATE_MAX_FILES];
static size_t entry_count;
static struct version manifest_version;
static bool session_active;
static uint8_t chunk[COPY_CHUNK];
/* Set while a program's call runs: then long file work pauses between
   chunks so the desktop keeps drawing (not during the boot resume,
   which runs before the scheduler starts). */
static bool may_sleep;
/* A call is running (it may sleep); a second updater must wait. */
static bool busy;

const char *update_current_version(void) {
    return OPOS_VERSION;
}

bool update_call_enter(void) {
    if (busy) {
        return false;
    }
    busy = true;
    may_sleep = true;
    return true;
}

void update_call_leave(void) {
    may_sleep = false;
    busy = false;
}

/* ------------------------------------------------------------ helpers */

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool starts_with_folded(const char *text, const char *prefix) {
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        if (lower(text[i]) != prefix[i]) {
            return false;
        }
    }
    return true;
}

static bool equal_folded(const char *a, const char *b) {
    size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return a[i] == b[i];
}

static bool bytes_equal(const uint8_t *a, const uint8_t *b, size_t length) {
    uint8_t difference = 0;
    for (size_t i = 0; i < length; i++) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0;
}

static void full_path(const char *relative, char out[FULL_PATH_MAX]) {
    size_t length = 0;
    for (const char *p = SYSTEM_PREFIX; *p != '\0'; p++) {
        out[length++] = *p;
    }
    for (size_t i = 0; relative[i] != '\0' && length + 1 < FULL_PATH_MAX;
         i++) {
        out[length++] = relative[i];
    }
    out[length] = '\0';
}

/* /disk/OPOS/UPDATE/Fnn for manifest entry `index`. */
static void stage_path(size_t index, char out[FULL_PATH_MAX]) {
    static const char base[] = STAGE_DIRECTORY "/F";
    size_t length = 0;
    for (size_t i = 0; base[i] != '\0'; i++) {
        out[length++] = base[i];
    }
    out[length++] = (char)('0' + index / 10);
    out[length++] = (char)('0' + index % 10);
    out[length] = '\0';
}

static bool installed(void) {
    return !installer_available() && fat16_system_mounted();
}

static void pause_briefly(void) {
    if (may_sleep) {
        (void)scheduler_sleep_until(timer_ticks() + 1);
    }
}

static bool make_directory(const char *path) {
    struct vfs_metadata metadata;
    size_t length = text_length(path);
    return vfs_create_directory(path, length)
        || (vfs_stat(path, length, &metadata)
            && metadata.type == VFS_NODE_DIRECTORY);
}

static void remove_file(const char *path) {
    bool not_empty;
    (void)vfs_remove(path, text_length(path), false, &not_empty);
}

static bool file_size(const char *path, uint64_t *size) {
    size_t value;
    if (!vfs_file_size(path, text_length(path), &value)) {
        return false;
    }
    *size = value;
    return true;
}

/* True when the file at path has this size and SHA-256. */
static bool file_matches(const char *path, uint64_t size,
                         const uint8_t hash[SHA256_DIGEST_SIZE]) {
    uint64_t actual;
    if (!file_size(path, &actual) || actual != size) {
        return false;
    }
    size_t length = text_length(path);
    struct sha256_context context;
    sha256_init(&context);
    for (uint64_t offset = 0; offset < size; offset += COPY_CHUNK) {
        size_t want = size - offset < COPY_CHUNK ? (size_t)(size - offset)
                                                 : COPY_CHUNK;
        size_t got = 0;
        if (!vfs_read_at(path, length, (size_t)offset, chunk, want, &got)
            || got != want) {
            return false;
        }
        sha256_update(&context, chunk, want);
        pause_briefly();
    }
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_final(&context, digest);
    return bytes_equal(digest, hash, SHA256_DIGEST_SIZE);
}

static bool copy_file(const char *from, const char *to) {
    uint64_t size;
    size_t from_length = text_length(from);
    size_t to_length = text_length(to);
    if (!file_size(from, &size) || !vfs_create_file(to, to_length, true)) {
        return false;
    }
    for (uint64_t offset = 0; offset < size; offset += COPY_CHUNK) {
        size_t want = size - offset < COPY_CHUNK ? (size_t)(size - offset)
                                                 : COPY_CHUNK;
        size_t got = 0;
        size_t written = 0;
        if (!vfs_read_at(from, from_length, (size_t)offset, chunk, want, &got)
            || got != want
            || !vfs_write_at(to, to_length, (size_t)offset, chunk, want,
                             &written)
            || written != want) {
            return false;
        }
        pause_briefly();
    }
    return true;
}

static bool write_text(const char *path, const void *data, size_t length) {
    return vfs_write_file(path, text_length(path), data, length);
}

/* ----------------------------------------------------------- manifest */

static bool signature_valid(const uint8_t *text, size_t length,
                            const uint8_t *signature, size_t sig_length) {
    uint8_t hash[SHA256_DIGEST_SIZE];
    struct sha256_context context;
    sha256_init(&context);
    sha256_update(&context, text, length);
    sha256_final(&context, hash);
    br_ec_public_key key = {
        BR_EC_secp256r1, (unsigned char *)update_public_key,
        sizeof(update_public_key)};
    return br_ecdsa_i31_vrfy_asn1(&br_ec_p256_m31, hash, sizeof(hash), &key,
                                  signature, sig_length) == 1;
}

/* "1.2.3": up to four numbers of up to 9 digits; nothing else. */
static bool parse_version(const char *text, size_t length,
                          struct version *version) {
    for (size_t i = 0; i < VERSION_PARTS; i++) {
        version->parts[i] = 0;
    }
    size_t part = 0;
    size_t digits = 0;
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            if (++digits > 9) {
                return false;
            }
            version->parts[part] = version->parts[part] * 10
                + (uint32_t)(c - '0');
        } else if (c == '.' && digits != 0 && part + 1 < VERSION_PARTS) {
            part++;
            digits = 0;
        } else {
            return false;
        }
    }
    return digits != 0;
}

static int compare_versions(const struct version *a, const struct version *b) {
    for (size_t i = 0; i < VERSION_PARTS; i++) {
        if (a->parts[i] != b->parts[i]) {
            return a->parts[i] < b->parts[i] ? -1 : 1;
        }
    }
    return 0;
}

/* A path under boot/ that the updater may replace. */
static bool path_allowed(const char *path) {
    if (!starts_with_folded(path, "boot/")
        || starts_with_folded(path, "boot/limine/")
        || starts_with_folded(path, "boot/old/")) {
        return false;
    }
    size_t segment = 0;
    size_t i = 0;
    for (; path[i] != '\0'; i++) {
        char c = path[i];
        if (c == '/') {
            /* No empty, "." or ".." segments. */
            if (segment == 0
                || (segment == 1 && path[i - 1] == '.')
                || (segment == 2 && path[i - 1] == '.' && path[i - 2] == '.')) {
                return false;
            }
            segment = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '.' || c == '-'
              || c == '_')) {
            return false;
        }
        segment++;
    }
    return segment != 0 && !(segment == 1 && path[i - 1] == '.')
        && !(segment == 2 && path[i - 1] == '.' && path[i - 2] == '.');
}

static bool asset_allowed(const char *asset) {
    if (asset[0] == '\0' || asset[0] == '.') {
        return false;
    }
    for (size_t i = 0; asset[i] != '\0'; i++) {
        char c = asset[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
              || (c >= '0' && c <= '9') || c == '.' || c == '-'
              || c == '_')) {
            return false;
        }
    }
    return true;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Splits [start, end) at single spaces into at most `max` words. */
static size_t split_words(const char *text, size_t start, size_t end,
                          size_t word_start[], size_t word_end[],
                          size_t max) {
    size_t count = 0;
    size_t at = start;
    while (at < end) {
        if (count == max) {
            return max + 1;
        }
        word_start[count] = at;
        while (at < end && text[at] != ' ') {
            at++;
        }
        word_end[count] = at;
        if (word_end[count] == word_start[count]) {
            return max + 1; /* double space or a leading space */
        }
        count++;
        if (at < end) {
            at++;
            if (at == end) {
                return max + 1; /* trailing space */
            }
        }
    }
    return count;
}

static bool copy_word(const char *text, size_t start, size_t end, char *out,
                      size_t capacity) {
    if (end - start + 1 > capacity) {
        return false;
    }
    for (size_t i = start; i < end; i++) {
        out[i - start] = text[i];
    }
    out[end - start] = '\0';
    return true;
}

static bool word_is(const char *text, size_t start, size_t end,
                    const char *word) {
    size_t length = text_length(word);
    if (end - start != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (text[start + i] != word[i]) {
            return false;
        }
    }
    return true;
}

static enum update_result parse_manifest(const char *text, size_t length) {
    entry_count = 0;
    bool have_header = false;
    bool have_version = false;
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && text[end] != '\n') {
            end++;
        }
        size_t next = end < length ? end + 1 : end;
        size_t word_start[6];
        size_t word_end[6];
        for (size_t i = start; i < end; i++) {
            if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] > 0x7e) {
                return UPDATE_BAD_MANIFEST;
            }
        }
        size_t words = split_words(text, start, end, word_start, word_end, 5);
        if (start == end) {
            start = next;
            continue;
        }
        if (words > 5) {
            return UPDATE_BAD_MANIFEST;
        }
        if (!have_header) {
            if (words != 2
                || !word_is(text, word_start[0], word_end[0], "OPOS-UPDATE")) {
                return UPDATE_BAD_MANIFEST;
            }
            if (!word_is(text, word_start[1], word_end[1], "1")) {
                return UPDATE_UNSUPPORTED;
            }
            have_header = true;
        } else if (word_is(text, word_start[0], word_end[0], "version")) {
            if (words != 2 || have_version
                || !parse_version(text + word_start[1],
                                  word_end[1] - word_start[1],
                                  &manifest_version)) {
                return UPDATE_BAD_MANIFEST;
            }
            have_version = true;
        } else if (word_is(text, word_start[0], word_end[0], "file")) {
            if (words != 5 || entry_count == UPDATE_MAX_FILES) {
                return UPDATE_BAD_MANIFEST;
            }
            struct entry *e = &entries[entry_count];
            if (word_end[1] - word_start[1] != 2 * SHA256_DIGEST_SIZE) {
                return UPDATE_BAD_MANIFEST;
            }
            for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
                int high = hex_value(text[word_start[1] + 2 * i]);
                int low = hex_value(text[word_start[1] + 2 * i + 1]);
                if (high < 0 || low < 0) {
                    return UPDATE_BAD_MANIFEST;
                }
                e->hash[i] = (uint8_t)(high << 4 | low);
            }
            e->size = 0;
            if (word_end[2] - word_start[2] > 10) {
                return UPDATE_BAD_MANIFEST;
            }
            for (size_t i = word_start[2]; i < word_end[2]; i++) {
                if (text[i] < '0' || text[i] > '9') {
                    return UPDATE_BAD_MANIFEST;
                }
                e->size = e->size * 10 + (uint64_t)(text[i] - '0');
            }
            /* FAT32 files stop below 4 GiB. */
            if (e->size > 0xffffffffull
                || !copy_word(text, word_start[3], word_end[3], e->asset,
                              sizeof(e->asset))
                || !copy_word(text, word_start[4], word_end[4], e->path,
                              sizeof(e->path))
                || !asset_allowed(e->asset) || !path_allowed(e->path)) {
                return UPDATE_BAD_MANIFEST;
            }
            for (size_t i = 0; i < entry_count; i++) {
                if (equal_folded(entries[i].path, e->path)) {
                    return UPDATE_BAD_MANIFEST;
                }
            }
            entry_count++;
        } else {
            /* A newer manifest format. */
            return UPDATE_UNSUPPORTED;
        }
        start = next;
    }
    return have_header && have_version && entry_count != 0
        ? UPDATE_OK : UPDATE_BAD_MANIFEST;
}

/* Checks and parses manifest_text/signature_bytes. */
static enum update_result load_manifest(void) {
    if (!signature_valid(manifest_text, manifest_length, signature_bytes,
                         signature_length)) {
        return UPDATE_BAD_SIGNATURE;
    }
    return parse_manifest((const char *)manifest_text, manifest_length);
}

/* ------------------------------------------------------------- session */

enum update_result update_begin(bool admin, const uint8_t *manifest,
                                size_t length, const uint8_t *signature,
                                size_t sig_length, size_t *file_count) {
    session_active = false;
    if (!installed()) {
        return UPDATE_NOT_INSTALLED;
    }
    if (!admin) {
        return UPDATE_NOT_ADMIN;
    }
    if (length == 0 || length > UPDATE_MANIFEST_MAX || sig_length == 0
        || sig_length > UPDATE_SIGNATURE_MAX) {
        return UPDATE_INVALID;
    }
    for (size_t i = 0; i < length; i++) {
        manifest_text[i] = manifest[i];
    }
    manifest_length = length;
    for (size_t i = 0; i < sig_length; i++) {
        signature_bytes[i] = signature[i];
    }
    signature_length = sig_length;
    enum update_result result = load_manifest();
    if (result != UPDATE_OK) {
        return result;
    }
    struct version current;
    if (parse_version(OPOS_VERSION, text_length(OPOS_VERSION), &current)
        && compare_versions(&manifest_version, &current) <= 0) {
        return UPDATE_NOT_NEWER;
    }
    for (size_t i = 0; i < entry_count; i++) {
        char path[FULL_PATH_MAX];
        full_path(entries[i].path, path);
        entries[i].needed = !file_matches(path, entries[i].size,
                                          entries[i].hash);
        entries[i].complete = false;
        entries[i].received = 0;
    }
    if (!make_directory("/disk/OPOS") || !make_directory(STAGE_DIRECTORY)) {
        return UPDATE_DISK_ERROR;
    }
    /* STATE.TXT of an interrupted install stays until a commit replaces
       it, so the install can still be finished at the next start. */
    session_active = true;
    *file_count = entry_count;
    return UPDATE_OK;
}

enum update_result update_file(size_t index, struct update_file_info *info) {
    if (!session_active) {
        return UPDATE_NO_SESSION;
    }
    if (index >= entry_count) {
        return UPDATE_INVALID;
    }
    const struct entry *e = &entries[index];
    info->size = e->size;
    info->needed = e->needed ? 1u : 0u;
    info->reserved = 0;
    for (size_t i = 0; i < UPDATE_ASSET_MAX; i++) {
        info->asset[i] = e->asset[i];
    }
    for (size_t i = 0; i < UPDATE_PATH_MAX; i++) {
        info->path[i] = e->path[i];
    }
    return UPDATE_OK;
}

enum update_result update_write(bool admin, size_t index, uint64_t offset,
                                const uint8_t *data, size_t length) {
    if (!session_active) {
        return UPDATE_NO_SESSION;
    }
    if (!admin) {
        return UPDATE_NOT_ADMIN;
    }
    if (index >= entry_count || !entries[index].needed) {
        return UPDATE_INVALID;
    }
    struct entry *e = &entries[index];
    /* Offset 0 starts the file again (a retried download). */
    if (offset != 0 && (offset != e->received || e->complete)) {
        return UPDATE_INVALID;
    }
    if (length > e->size - e->received) {
        return UPDATE_BAD_FILE;
    }
    char path[FULL_PATH_MAX];
    stage_path(index, path);
    size_t path_length = text_length(path);
    block_begin_batch();
    bool written = true;
    if (offset == 0) {
        e->received = 0;
        e->complete = false;
        sha256_init(&e->hashing);
        written = vfs_create_file(path, path_length, true);
    }
    size_t count = 0;
    if (written && length != 0) {
        written = vfs_write_at(path, path_length, (size_t)offset, data, length,
                               &count)
            && count == length;
    }
    if (!block_end_batch()) {
        written = false;
    }
    if (!written) {
        e->received = 0;
        return UPDATE_DISK_ERROR;
    }
    sha256_update(&e->hashing, data, length);
    e->received += length;
    if (e->received == e->size) {
        uint8_t digest[SHA256_DIGEST_SIZE];
        sha256_final(&e->hashing, digest);
        if (!bytes_equal(digest, e->hash, SHA256_DIGEST_SIZE)) {
            e->received = 0;
            remove_file(path);
            return UPDATE_BAD_FILE;
        }
        e->complete = true;
    }
    return UPDATE_OK;
}

static void remove_staging(void) {
    /* The state goes first: without it, leftovers are ignored. */
    remove_file(STAGE_STATE);
    for (size_t i = 0; i < UPDATE_MAX_FILES; i++) {
        char path[FULL_PATH_MAX];
        stage_path(i, path);
        remove_file(path);
    }
    remove_file(STAGE_MANIFEST);
    remove_file(STAGE_SIGNATURE);
    bool not_empty;
    (void)vfs_remove(STAGE_DIRECTORY, text_length(STAGE_DIRECTORY), true,
                     &not_empty);
}

/* Copies the running system's kernel and initramfs to boot/old and adds
   the boot menu entry for them. */
static bool keep_previous_version(void) {
    if (!make_directory(OLD_DIRECTORY)) {
        return false;
    }
    static const char *const kept[] = {KERNEL_PATH, INITRAMFS_PATH};
    for (size_t i = 0; i < sizeof(kept) / sizeof(kept[0]); i++) {
        char from[FULL_PATH_MAX];
        char to[FULL_PATH_MAX];
        full_path(kept[i], from);
        full_path(kept[i], to);
        /* "boot/x" -> "boot/old/x" */
        static const char old_prefix[] = OLD_DIRECTORY "/";
        size_t length = 0;
        for (size_t j = 0; old_prefix[j] != '\0'; j++) {
            to[length++] = old_prefix[j];
        }
        for (const char *p = kept[i] + 5; *p != '\0'; p++) {
            to[length++] = *p;
        }
        to[length] = '\0';
        if (!copy_file(from, to)) {
            return false;
        }
    }
    if (!boot_display_add_previous_entry()) {
        console_write_string(
            "OPOS: update: could not add the previous-version boot entry\n");
    }
    return true;
}

/* Copies the staged files (needed entries) into place, the kernel last. */
static bool install_staged(void) {
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < entry_count; i++) {
            const struct entry *e = &entries[i];
            bool kernel = equal_folded(e->path, KERNEL_PATH);
            if (!e->needed || kernel != (pass == 1)) {
                continue;
            }
            char from[FULL_PATH_MAX];
            char to[FULL_PATH_MAX];
            stage_path(i, from);
            full_path(e->path, to);
            /* Folders a new file lives in. */
            for (size_t j = text_length(SYSTEM_PREFIX); to[j] != '\0'; j++) {
                if (to[j] == '/') {
                    to[j] = '\0';
                    bool made = make_directory(to);
                    to[j] = '/';
                    if (!made) {
                        return false;
                    }
                }
            }
            if (!copy_file(from, to) || !file_matches(to, e->size, e->hash)) {
                return false;
            }
        }
    }
    return true;
}

/* Runs the install from `copying` (false: from the backup step). */
static enum update_result apply(bool copying) {
    block_begin_batch();
    bool ok = true;
    if (!copying) {
        ok = keep_previous_version()
            && write_text(STAGE_STATE, STATE_COPYING,
                          text_length(STATE_COPYING));
    }
    ok = ok && install_staged();
    if (!block_end_batch()) {
        ok = false;
    }
    if (!ok) {
        return UPDATE_DISK_ERROR;
    }
    block_begin_batch();
    remove_staging();
    (void)block_end_batch();
    return UPDATE_OK;
}

enum update_result update_commit(bool admin) {
    if (!session_active) {
        return UPDATE_NO_SESSION;
    }
    if (!admin) {
        return UPDATE_NOT_ADMIN;
    }
    bool any = false;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].needed && !entries[i].complete) {
            return UPDATE_INCOMPLETE;
        }
        any |= entries[i].needed;
    }
    if (!any) {
        /* Same files, newer version number: nothing to copy. */
        update_cancel();
        return UPDATE_OK;
    }
    session_active = false;
    block_begin_batch();
    bool saved = write_text(STAGE_MANIFEST, manifest_text, manifest_length)
        && write_text(STAGE_SIGNATURE, signature_bytes, signature_length)
        && write_text(STAGE_STATE, STATE_PREPARED,
                      text_length(STATE_PREPARED));
    if (!block_end_batch() || !saved) {
        return UPDATE_DISK_ERROR;
    }
    return apply(false);
}

void update_cancel(void) {
    uint64_t size;
    if (session_active) {
        session_active = false;
        /* Keep the files an interrupted install still needs. */
        if (!file_size(STAGE_STATE, &size)) {
            remove_staging();
        }
    }
}

/* --------------------------------------------------------------- resume */

static bool read_small(const char *path, uint8_t *buffer, size_t capacity,
                       size_t *length) {
    return vfs_read_file(path, text_length(path), buffer, capacity, length);
}

void update_resume(void) {
    if (!installed()) {
        return;
    }
    char state[16];
    size_t state_length;
    if (!read_small(STAGE_STATE, (uint8_t *)state, sizeof(state) - 1,
                    &state_length)) {
        return;
    }
    state[state_length] = '\0';
    bool copying;
    if (equal_folded(state, STATE_COPYING)) {
        copying = true;
    } else if (equal_folded(state, STATE_PREPARED)) {
        copying = false;
    } else {
        return;
    }
    console_write_string("OPOS: finishing an interrupted update\n");
    if (!read_small(STAGE_MANIFEST, manifest_text, sizeof(manifest_text),
                    &manifest_length)
        || !read_small(STAGE_SIGNATURE, signature_bytes,
                       sizeof(signature_bytes), &signature_length)
        || load_manifest() != UPDATE_OK) {
        console_write_string("OPOS: update: staged manifest unusable\n");
        if (!copying) {
            remove_staging();
        }
        return;
    }
    for (size_t i = 0; i < entry_count; i++) {
        char path[FULL_PATH_MAX];
        stage_path(i, path);
        uint64_t size;
        entries[i].needed = file_size(path, &size);
        if (entries[i].needed
            && !file_matches(path, entries[i].size, entries[i].hash)) {
            console_write_string("OPOS: update: a staged file is damaged\n");
            if (!copying) {
                remove_staging();
            }
            return;
        }
    }
    console_write_string(apply(copying) == UPDATE_OK
        ? "OPOS: update installed\n"
        : "OPOS: update: installing failed; the previous version is in the boot menu\n");
}
