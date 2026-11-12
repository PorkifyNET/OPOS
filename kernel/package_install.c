#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accounts.h"
#include "block.h"
#include "console.h"
#include "fat16.h"
#include "installer.h"
#include "manifest.h"
#include "package.h"
#include "permissions.h"
#include "permission_grants.h"
#include "scheduler.h"
#include "sha256.h"
#include "timer.h"
#include "update.h"
#include "vfs.h"

/*
 * Installs and removes packages (docs/packages.md, package.h).
 *
 * Install: the archive is read once. Each member is copied into
 * STAGE/NEW while its SHA-256 is computed, so what is installed is what
 * was checked even if the archive changes meanwhile. The record goes to
 * STAGE/RECORD. Then STATE.TXT names the target folder and record path,
 * and from there the work always rolls forward (package_resume):
 *   1. an existing target folder is renamed to STAGE/OLD;
 *   2. STAGE/NEW is renamed to the target folder;
 *   3. the record is written;
 *   4. STAGE/OLD, STAGE/RECORD and STATE.TXT are removed.
 * Removal is the same with no new folder: target to STAGE/OLD, record
 * deleted, OLD removed. A cut inside one rename is not covered, as for
 * system updates.
 */

#define STAGE "/disk/OPOS/PKGSTAGE"
#define STAGE_NEW STAGE "/NEW"
#define STAGE_OLD STAGE "/OLD"
#define STAGE_RECORD STAGE "/RECORD"
#define STAGE_STATE STAGE "/STATE.TXT"
#define EVERYONE_RECORDS "/disk/OPOS/Packages"
#define EVERYONE_PROGRAMS "/disk/Programs"
#define PATH_CAPACITY 256
#define CHUNK 65536
#define NOTES_MAX 4096
#define RECORD_PREFIX_MAX 32
#define ORIGIN_SIDELOADED "origin: sideloaded\n"

static bool busy;
static bool may_sleep;

/* Install-call buffers (one call at a time, under busy). */
static char text[RECORD_PREFIX_MAX + PACKAGE_TEXT_MAX];
static struct package package;
static char record_text[RECORD_PREFIX_MAX + PACKAGE_TEXT_MAX];
static uint8_t chunk[CHUNK];
static uint8_t notes[NOTES_MAX];
static uint8_t seen[PACKAGE_FILES_MAX];
static char reason[PACKAGE_REASON_MAX + 1];

/* Desktop buffers (Start menu), separate from a running install call. */
static char start_text[RECORD_PREFIX_MAX + PACKAGE_TEXT_MAX];
static struct package start_package;

/* Launch buffers (package_identify). Program starts use them one at a
   time, as permission_grants.c's store buffer. */
static char identify_text[RECORD_PREFIX_MAX + PACKAGE_TEXT_MAX];
static struct package identify_package;

/* ------------------------------------------------------------ helpers */

static size_t text_length(const char *value) {
    size_t length = 0;
    while (value[length] != '\0') {
        length++;
    }
    return length;
}

static size_t append(char *out, size_t capacity, size_t used,
                     const char *value, size_t length) {
    if (used == 0 && capacity > 0) {
        out[0] = '\0';
    }
    if (used + length >= capacity) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        out[used + i] = value[i];
    }
    out[used + length] = '\0';
    return used + length;
}

static size_t append_text(char *out, size_t capacity, size_t used,
                          const char *value) {
    return used == 0 && value[0] == '\0'
        ? 0 : append(out, capacity, used, value, text_length(value));
}

static bool same_text(const char *a, const char *b) {
    size_t i = 0;
    for (; a[i] != '\0' && a[i] == b[i]; i++) {
    }
    return a[i] == b[i];
}

static void copy_text(char *out, size_t capacity, const char *value) {
    size_t i = 0;
    for (; value[i] != '\0' && i + 1 < capacity; i++) {
        out[i] = value[i];
    }
    out[i] = '\0';
}

static void copy_reason(struct package_info *info, const char *value) {
    if (info != NULL) {
        copy_text(info->reason, sizeof(info->reason), value);
    }
}

static bool installed_system(void) {
    return !installer_available() && fat16_system_mounted();
}

static void pause_briefly(void) {
    if (may_sleep) {
        (void)scheduler_sleep_until(timer_ticks() + 1);
    }
}

static bool exists(const char *path, enum vfs_node_type *type) {
    struct vfs_metadata metadata;
    if (!vfs_stat(path, text_length(path), &metadata)) {
        return false;
    }
    if (type != NULL) {
        *type = metadata.type;
    }
    return true;
}

static bool make_directory(const char *path) {
    enum vfs_node_type type;
    if (exists(path, &type)) {
        return type == VFS_NODE_DIRECTORY;
    }
    return vfs_create_directory(path, text_length(path));
}

/* Creates every folder above path (from after skip characters). */
static bool make_parents(char *path, size_t skip) {
    for (size_t i = skip; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            continue;
        }
        path[i] = '\0';
        bool made = make_directory(path);
        path[i] = '/';
        if (!made) {
            return false;
        }
    }
    return true;
}

static void remove_file(const char *path) {
    bool not_empty;
    (void)vfs_remove(path, text_length(path), false, &not_empty);
}

/* Removes a folder and everything in it, without recursion. */
static bool remove_tree(const char *root) {
    enum vfs_node_type type;
    if (!exists(root, &type)) {
        return true;
    }
    if (type != VFS_NODE_DIRECTORY) {
        bool not_empty;
        return vfs_remove(root, text_length(root), false, &not_empty);
    }
    static char path[PATH_CAPACITY];
    size_t root_length = text_length(root);
    size_t length = append_text(path, sizeof(path), 0, root);
    if (length == 0) {
        return false;
    }
    for (size_t steps = 0; steps < 100000; steps++) {
        size_t count;
        if (!vfs_directory_count(path, length, &count)) {
            return false;
        }
        struct vfs_directory_entry entry;
        bool found = false;
        for (size_t i = 0; i < count && !found; i++) {
            if (!vfs_directory_entry_at(path, length, i, &entry)) {
                return false;
            }
            found = !(entry.name_length == 1 && entry.name[0] == '.')
                && !(entry.name_length == 2 && entry.name[0] == '.'
                     && entry.name[1] == '.');
        }
        bool not_empty;
        if (!found) {
            if (!vfs_remove(path, length, true, &not_empty)) {
                return false;
            }
            if (length == root_length) {
                return true;
            }
            while (length > root_length && path[length - 1] != '/') {
                length--;
            }
            path[--length] = '\0';
            continue;
        }
        size_t child = append(path, sizeof(path), length, "/", 1);
        child = child == 0 ? 0 : append(path, sizeof(path), child,
                                        entry.name, entry.name_length);
        if (child == 0) {
            return false;
        }
        if (entry.type == VFS_NODE_DIRECTORY) {
            length = child;
            continue;
        }
        if (!vfs_remove(path, child, false, &not_empty)) {
            return false;
        }
        path[length] = '\0';
        pause_briefly();
    }
    return false;
}

static bool read_whole(const char *path, char *buffer, size_t capacity,
                       size_t *length) {
    size_t size;
    if (!vfs_file_size(path, text_length(path), &size) || size > capacity) {
        return false;
    }
    size_t got = 0;
    if (size != 0
        && (!vfs_read_at(path, text_length(path), 0, (uint8_t *)buffer, size,
                         &got)
            || got != size)) {
        return false;
    }
    *length = size;
    return true;
}

static bool write_whole(const char *path, const void *data, size_t length) {
    return vfs_write_file(path, text_length(path), data, length);
}

/* "<home>" of account into out; length or 0. */
static size_t home_path(size_t account, char *out, size_t capacity) {
    if (account == PERMISSION_NO_ACCOUNT || account >= accounts_count()) {
        return 0;
    }
    size_t used = append_text(out, capacity, 0, ACCOUNT_HOME_ROOT "/");
    return used == 0 ? 0 : append_text(out, capacity, used,
                                       accounts_name(account));
}

/* Records folder and programs folder for a scope. */
static bool scope_paths(bool everyone, size_t account, char *records,
                        char *programs) {
    if (everyone) {
        copy_text(records, PATH_CAPACITY, EVERYONE_RECORDS);
        copy_text(programs, PATH_CAPACITY, EVERYONE_PROGRAMS);
        return true;
    }
    char home[PATH_CAPACITY];
    size_t length = home_path(account, home, sizeof(home));
    if (length == 0) {
        return false;
    }
    size_t r = append_text(records, PATH_CAPACITY, 0, home);
    r = r == 0 ? 0 : append_text(records, PATH_CAPACITY, r,
                                 "/AppData/Packages");
    size_t p = append_text(programs, PATH_CAPACITY, 0, home);
    p = p == 0 ? 0 : append_text(programs, PATH_CAPACITY, p, "/Programs");
    return r != 0 && p != 0;
}

static bool join_id(char *out, const char *folder, const char *id) {
    size_t used = append_text(out, PATH_CAPACITY, 0, folder);
    used = used == 0 ? 0 : append(out, PATH_CAPACITY, used, "/", 1);
    return used != 0 && append_text(out, PATH_CAPACITY, used, id) != 0;
}

/* A directory entry name that is a valid package id. */
static bool id_name(const char *name, size_t length) {
    return package_id_valid(name, length);
}

/* The version line of a record, or false. */
static bool record_version(const char *record, size_t length, char *version) {
    size_t line = 0;
    while (line < length) {
        size_t end = line;
        while (end < length && record[end] != '\n') {
            end++;
        }
        static const char key[] = "version: ";
        size_t key_length = sizeof(key) - 1;
        bool match = end - line > key_length;
        for (size_t i = 0; i < key_length && match; i++) {
            match = record[line + i] == key[i];
        }
        if (match && end - line - key_length <= PACKAGE_VERSION_MAX) {
            size_t used = 0;
            for (size_t i = line + key_length; i < end; i++) {
                version[used++] = record[i];
            }
            version[used] = '\0';
            return package_version_valid(version, used);
        }
        line = end + 1;
    }
    return false;
}

/* Reads the record of id in a scope into record_text. */
static bool read_record(bool everyone, size_t account, const char *id,
                        size_t *length, bool *store) {
    char records[PATH_CAPACITY];
    char programs[PATH_CAPACITY];
    char path[PATH_CAPACITY];
    if (!scope_paths(everyone, account, records, programs)
        || !join_id(path, records, id)
        || !read_whole(path, record_text, sizeof(record_text), length)) {
        return false;
    }
    static const char store_origin[] = "origin: store\n";
    bool is_store = *length >= sizeof(store_origin) - 1;
    for (size_t i = 0; i < sizeof(store_origin) - 1 && is_store; i++) {
        is_store = record_text[i] == store_origin[i];
    }
    if (store != NULL) {
        *store = is_store;
    }
    return true;
}

/* ------------------------------------------------------------ archive */

static uint64_t round_block(uint64_t size) {
    return (size + 511) & ~(uint64_t)511;
}

static bool read_exact(const char *path, uint64_t offset, void *buffer,
                       size_t length) {
    size_t got = 0;
    return vfs_read_at(path, text_length(path), (size_t)offset,
                       (uint8_t *)buffer, length, &got) && got == length;
}

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
        | (uint32_t)p[3] << 24;
}

static uint64_t u64(const uint8_t *p) {
    return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}

/* The executable at path[base..base+size) suits the platform. */
static bool executable_ok(const char *path, uint64_t base, uint64_t size,
                          enum package_platform platform) {
    uint8_t header[64];
    if (platform == PACKAGE_PLATFORM_WINDOWS) {
        return size >= 2 && read_exact(path, base, header, 2)
            && header[0] == 'M' && header[1] == 'Z';
    }
    if (size < sizeof(header) || !read_exact(path, base, header, 64)
        || header[0] != 0x7F || header[1] != 'E' || header[2] != 'L'
        || header[3] != 'F' || header[4] != 2 || header[5] != 1) {
        return false;
    }
    if (platform == PACKAGE_PLATFORM_LINUX) {
        return u16(header + 18) == 62;
    }
    uint64_t table = u64(header + 32);
    uint16_t entry_size = u16(header + 54);
    uint16_t count = u16(header + 56);
    if (entry_size < 56 || table > size
        || (uint64_t)entry_size * count > size - table) {
        return false;
    }
    struct manifest manifest;
    int found = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint8_t entry[56];
        if (!read_exact(path, base + table + (uint64_t)i * entry_size, entry,
                        sizeof(entry))) {
            return false;
        }
        if (u32(entry) != 4) { /* PT_NOTE */
            continue;
        }
        uint64_t offset = u64(entry + 8);
        uint64_t length = u64(entry + 32);
        if (offset > size || length > size - offset || length > NOTES_MAX
            || (length != 0
                && !read_exact(path, base + offset, notes, (size_t)length))) {
            return false;
        }
        found = manifest_scan_segment(notes, (size_t)length, &manifest,
                                      found);
        if (found < 0) {
            return false;
        }
    }
    return found == 1;
}

static enum package_result bad(const char *first, const char *name,
                               const char *last) {
    size_t used = append_text(reason, sizeof(reason), 0, first);
    if (name[0] != '\0') {
        used = append_text(reason, sizeof(reason), used == 0 ? 0 : used, name);
    }
    if (used != 0) {
        (void)append_text(reason, sizeof(reason), used, last);
    }
    return PACKAGE_BAD;
}

/* Staged location of a package path ("files/a/b" -> STAGE_NEW "/a/b"). */
static bool stage_path(const char *member, char *out) {
    size_t used = append_text(out, PATH_CAPACITY, 0, STAGE_NEW "/");
    return used != 0 && append_text(out, PATH_CAPACITY, used, member + 6) != 0;
}

static void fill_info(struct package_info *info) {
    if (info == NULL) {
        return;
    }
    info->platform = (uint32_t)package.platform;
    info->file_count = (uint32_t)package.file_count;
    info->start_count = (uint32_t)package.start_count;
    info->total_size = package.total_size;
    info->store = 0;
    copy_text(info->id, sizeof(info->id), package.id);
    copy_text(info->name, sizeof(info->name), package.name);
    copy_text(info->version, sizeof(info->version), package.version);
    copy_text(info->publisher, sizeof(info->publisher), package.publisher);
    copy_text(info->requires_opos, sizeof(info->requires_opos),
              package.requires_opos);
}

enum scan_mode {
    SCAN_HEADER, /* PACKAGE only */
    SCAN_CHECK,  /* every member, nothing written */
    SCAN_STAGE,  /* every member, copied to STAGE_NEW */
};

/* Size of the PACKAGE member last read. */
static size_t package_size;

/*
 * Reads the archive at path. PACKAGE is parsed into package (its text at
 * text + RECORD_PREFIX_MAX). Fills info as soon as PACKAGE is known.
 */
static enum package_result scan_archive(const char *path, enum scan_mode mode,
                                        struct package_info *info) {
    bool stage = mode == SCAN_STAGE;
    reason[0] = '\0';
    size_t archive_size;
    if (!vfs_file_size(path, text_length(path), &archive_size)) {
        return PACKAGE_NOT_FOUND;
    }
    uint8_t block[512];
    char name[PACKAGE_PATH_MAX + 1];
    uint64_t size;
    bool end;
    if (archive_size < 512 || !read_exact(path, 0, block, 512)) {
        return bad("archive: ends without the two empty end blocks", "", "");
    }
    if (!package_ustar_header(block, name, &size, &end, reason)) {
        return PACKAGE_BAD;
    }
    bool is_package = !end && name[0] == 'P';
    static const char package_name[] = "PACKAGE";
    for (size_t i = 0; i < sizeof(package_name) && is_package; i++) {
        is_package = name[i] == package_name[i];
    }
    if (!is_package) {
        return bad("archive: PACKAGE must be the first member", "", "");
    }
    if (size > PACKAGE_TEXT_MAX) {
        return bad("PACKAGE: larger than 65536 bytes", "", "");
    }
    if (size > archive_size - 512) {
        return bad("PACKAGE", "", ": archive is cut short");
    }
    char *package_text = text + RECORD_PREFIX_MAX;
    if (size != 0 && !read_exact(path, 512, package_text, (size_t)size)) {
        return PACKAGE_DISK_ERROR;
    }
    enum package_result result = package_parse(package_text, (size_t)size,
                                               false, &package, reason);
    if (result != PACKAGE_OK) {
        return result;
    }
    package_size = (size_t)size;
    fill_info(info);
    if (mode == SCAN_HEADER) {
        return PACKAGE_OK;
    }
    uint64_t offset = 512 + round_block(size);
    for (size_t i = 0; i < package.file_count; i++) {
        seen[i] = 0;
    }
    if (stage && !make_directory(STAGE_NEW)) {
        return PACKAGE_DISK_ERROR;
    }
    for (;;) {
        if (archive_size < 512 || offset > archive_size - 512) {
            return bad("archive: ends without the two empty end blocks", "",
                       "");
        }
        if (!read_exact(path, offset, block, 512)) {
            return PACKAGE_DISK_ERROR;
        }
        if (!package_ustar_header(block, name, &size, &end, reason)) {
            return PACKAGE_BAD;
        }
        if (end) {
            break;
        }
        bool again = true;
        for (size_t i = 0; i < sizeof(package_name) && again; i++) {
            again = name[i] == package_name[i];
        }
        if (again) {
            return bad("archive: PACKAGE appears twice", "", "");
        }
        long index = package_find_file(&package, package_text, name,
                                       text_length(name));
        if (index < 0) {
            return bad("", name, ": in the archive but not listed in PACKAGE");
        }
        const struct package_file *file = &package.files[index];
        if (seen[index]) {
            return bad("", name, ": appears twice in the archive");
        }
        seen[index] = 1;
        uint64_t data = offset + 512;
        if (size > archive_size - data) {
            return bad("", name, ": archive is cut short");
        }
        if (size != file->size) {
            return bad("", name, ": size differs from PACKAGE");
        }
        char staged[PATH_CAPACITY];
        if (stage
            && (!stage_path(name, staged) || !make_parents(staged, 1)
                || !vfs_create_file(staged, text_length(staged), true))) {
            return PACKAGE_DISK_ERROR;
        }
        struct sha256_context hash;
        sha256_init(&hash);
        for (uint64_t done = 0; done < size; done += CHUNK) {
            size_t want = size - done < CHUNK ? (size_t)(size - done) : CHUNK;
            if (!read_exact(path, data + done, chunk, want)) {
                return PACKAGE_DISK_ERROR;
            }
            sha256_update(&hash, chunk, want);
            size_t written = 0;
            if (stage
                && (!vfs_write_at(staged, text_length(staged), (size_t)done,
                                  chunk, want, &written)
                    || written != want)) {
                return PACKAGE_DISK_ERROR;
            }
            pause_briefly();
        }
        uint8_t digest[SHA256_DIGEST_SIZE];
        sha256_final(&hash, digest);
        uint8_t difference = 0;
        for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
            difference |= (uint8_t)(digest[i] ^ file->hash[i]);
        }
        if (difference != 0) {
            return bad("", name, ": SHA-256 does not match PACKAGE");
        }
        /* Checked in the staged copy when installing: that is what runs. */
        if (file->exec
            && !(stage ? executable_ok(staged, 0, size, package.platform)
                       : executable_ok(path, data, size, package.platform))) {
            return bad("", name,
                       package.platform == PACKAGE_PLATFORM_OPOS
                           ? ": an opos program needs one valid OPOS_MANIFEST"
                           : ": not a 64-bit x86-64 ELF executable");
        }
        offset = data + round_block(size);
    }
    /* Second end block, then nothing but zeros. */
    offset += 512;
    bool second = offset <= archive_size && archive_size - offset >= 512
        && read_exact(path, offset, block, 512);
    for (size_t i = 0; i < 512 && second; i++) {
        second = block[i] == 0;
    }
    if (!second) {
        return bad("archive: one empty block where two are needed", "", "");
    }
    offset += 512;
    for (uint64_t at = offset; at < archive_size; at += CHUNK) {
        size_t want = archive_size - at < CHUNK ? (size_t)(archive_size - at)
                                                : CHUNK;
        if (!read_exact(path, at, chunk, want)) {
            return PACKAGE_DISK_ERROR;
        }
        uint8_t any = 0;
        for (size_t i = 0; i < want; i++) {
            any |= chunk[i];
        }
        if (any != 0) {
            return bad("archive: data after the end blocks", "", "");
        }
    }
    for (size_t i = 0; i < package.file_count; i++) {
        if (!seen[i]) {
            char missing[PACKAGE_PATH_MAX + 1];
            const struct package_file *file = &package.files[i];
            size_t used = append(missing, sizeof(missing), 0,
                                 package_text + file->path.offset,
                                 file->path.length);
            if (used == 0) {
                missing[0] = '\0';
            }
            return bad("", missing,
                       ": listed in PACKAGE but missing from the archive");
        }
    }
    return PACKAGE_OK;
}

/* Platform and requires-opos. */
static enum package_result runnable(void) {
    if (package.platform == PACKAGE_PLATFORM_WINDOWS) {
        copy_text(reason, sizeof(reason), "a Windows program");
        return PACKAGE_PLATFORM;
    }
    const char *current = update_current_version();
    if (package.requires_opos[0] != '\0'
        && package_version_valid(current, text_length(current))
        && package_compare_versions(package.requires_opos, current) > 0) {
        size_t used = append_text(reason, sizeof(reason), 0, "needs OPOS ");
        used = used == 0 ? 0
            : append_text(reason, sizeof(reason), used, package.requires_opos);
        if (used != 0) {
            (void)append_text(reason, sizeof(reason), used, " or newer");
        }
        return PACKAGE_NEEDS_NEWER_OPOS;
    }
    return PACKAGE_OK;
}

/* Where id is installed as seen by account: its own copy first. */
static void installed_info(size_t account, struct package_info *info) {
    if (info == NULL) {
        return;
    }
    info->installed = PACKAGE_INSTALLED_NONE;
    info->installed_version[0] = '\0';
    size_t length;
    char version[PACKAGE_VERSION_MAX + 1];
    if (account != PERMISSION_NO_ACCOUNT
        && read_record(false, account, package.id, &length, NULL)
        && record_version(record_text, length, version)) {
        info->installed = PACKAGE_INSTALLED_ACCOUNT;
    } else if (read_record(true, account, package.id, &length, NULL)
               && record_version(record_text, length, version)) {
        info->installed = PACKAGE_INSTALLED_EVERYONE;
    } else {
        return;
    }
    copy_text(info->installed_version, sizeof(info->installed_version),
              version);
}

/* ------------------------------------------------------------- locking */

bool package_call_enter(void) {
    if (busy || update_call_busy()) {
        return false;
    }
    busy = true;
    may_sleep = true;
    return true;
}

void package_call_leave(void) {
    may_sleep = false;
    busy = false;
}

bool package_call_busy(void) {
    return busy;
}

/* --------------------------------------------------------------- check */

enum package_result package_check(const char *path, size_t length,
                                  size_t account, struct package_info *info) {
    (void)length;
    /* Platform and version first: a Windows program is not "damaged". */
    enum package_result result = scan_archive(path, SCAN_HEADER, info);
    if (result == PACKAGE_OK) {
        result = runnable();
    }
    if (result == PACKAGE_OK) {
        result = scan_archive(path, SCAN_CHECK, info);
    }
    if (result == PACKAGE_OK || result == PACKAGE_NEEDS_NEWER_OPOS
        || result == PACKAGE_PLATFORM) {
        installed_info(account, info);
    }
    copy_reason(info, reason);
    return result;
}

/* ------------------------------------------------------------- applying */

/* STATE.TXT: "INSTALL\n<target>\n<record>\n" or "REMOVE\n...". */
static char state_target[PATH_CAPACITY];
static char state_record[PATH_CAPACITY];

static bool write_state(bool install) {
    char state[3 * PATH_CAPACITY];
    size_t used = append_text(state, sizeof(state), 0,
                              install ? "INSTALL\n" : "REMOVE\n");
    used = used == 0 ? 0 : append_text(state, sizeof(state), used,
                                       state_target);
    used = used == 0 ? 0 : append(state, sizeof(state), used, "\n", 1);
    used = used == 0 ? 0 : append_text(state, sizeof(state), used,
                                       state_record);
    used = used == 0 ? 0 : append(state, sizeof(state), used, "\n", 1);
    return used != 0 && write_whole(STAGE_STATE, state, used);
}

static bool clear_staging(void) {
    remove_file(STAGE_STATE);
    return remove_tree(STAGE);
}

static bool rename_path(const char *from, const char *to) {
    return vfs_rename(from, text_length(from), to, text_length(to))
        == VFS_RENAME_OK;
}

/* Steps 1-4 (see the top of this file). install: a new folder exists. */
static bool apply(bool install) {
    bool target_exists = exists(state_target, NULL);
    bool old_exists = exists(STAGE_OLD, NULL);
    bool new_exists = install && exists(STAGE_NEW, NULL);
    block_begin_batch();
    bool ok = true;
    if (target_exists && !old_exists && (new_exists || !install)) {
        ok = rename_path(state_target, STAGE_OLD);
    }
    if (!block_end_batch() || !ok) {
        return false;
    }
    if (new_exists) {
        block_begin_batch();
        ok = rename_path(STAGE_NEW, state_target);
        if (!block_end_batch() || !ok) {
            return false;
        }
    }
    block_begin_batch();
    if (install) {
        size_t length;
        ok = !exists(STAGE_RECORD, NULL)
            || (read_whole(STAGE_RECORD, record_text, sizeof(record_text),
                           &length)
                && write_whole(state_record, record_text, length));
    } else {
        remove_file(state_record);
        ok = !exists(state_record, NULL);
    }
    if (!block_end_batch() || !ok) {
        return false;
    }
    ok = remove_tree(STAGE_OLD);
    block_begin_batch();
    remove_file(STAGE_RECORD);
    ok = clear_staging() && ok;
    return block_end_batch() && ok;
}

/* Paths STATE.TXT may name: a package folder and its record. */
static bool state_paths_valid(void) {
    char records[PATH_CAPACITY];
    char programs[PATH_CAPACITY];
    for (size_t scope = 0; scope <= accounts_count(); scope++) {
        bool everyone = scope == accounts_count();
        if (!scope_paths(everyone, scope, records, programs)) {
            continue;
        }
        size_t programs_length = text_length(programs);
        size_t records_length = text_length(records);
        size_t target_length = text_length(state_target);
        size_t record_length = text_length(state_record);
        if (target_length <= programs_length + 1
            || record_length <= records_length + 1
            || target_length - programs_length
                   != record_length - records_length) {
            continue;
        }
        bool match = state_target[programs_length] == '/'
            && state_record[records_length] == '/';
        for (size_t i = 0; i < programs_length && match; i++) {
            match = state_target[i] == programs[i];
        }
        for (size_t i = 0; i < records_length && match; i++) {
            match = state_record[i] == records[i];
        }
        const char *id = state_target + programs_length + 1;
        size_t id_length = target_length - programs_length - 1;
        for (size_t i = 0; i < id_length && match; i++) {
            match = id[i] == state_record[records_length + 1 + i];
        }
        if (match && id_name(id, id_length)) {
            return true;
        }
    }
    return false;
}

static bool load_state(bool *install) {
    char state[3 * PATH_CAPACITY];
    size_t length;
    if (!read_whole(STAGE_STATE, state, sizeof(state) - 1, &length)) {
        return false;
    }
    state[length] = '\0';
    size_t line = 0;
    char *fields[3];
    size_t count = 0;
    for (size_t i = 0; i < length && count < 3; i++) {
        if (state[i] == '\n') {
            state[i] = '\0';
            fields[count++] = state + line;
            line = i + 1;
        }
    }
    if (count != 3) {
        return false;
    }
    bool is_install = same_text(fields[0], "INSTALL");
    bool is_remove = same_text(fields[0], "REMOVE");
    if ((!is_install && !is_remove) || text_length(fields[1]) == 0
        || text_length(fields[2]) == 0) {
        return false;
    }
    copy_text(state_target, sizeof(state_target), fields[1]);
    copy_text(state_record, sizeof(state_record), fields[2]);
    *install = is_install;
    return state_paths_valid();
}

/* Finishes interrupted work; false when it could not. */
static bool finish_pending(void) {
    if (!exists(STAGE_STATE, NULL)) {
        return true;
    }
    bool install;
    if (!load_state(&install)) {
        console_write_string("OPOS: packages: unreadable staging state\n");
        return false;
    }
    console_write_string(install ? "OPOS: finishing a package install\n"
                                 : "OPOS: finishing a package removal\n");
    return apply(install);
}

/* ------------------------------------------------------------- install */

/* Refusals that depend on what is installed. Fills target and record. */
static enum package_result install_allowed(bool everyone, size_t account,
                                           uint32_t options,
                                           const char *records,
                                           const char *programs,
                                           char *target, char *record) {
    size_t record_length;
    bool store;
    if (!join_id(target, programs, package.id)
        || !join_id(record, records, package.id)) {
        return PACKAGE_INVALID;
    }
    bool own = read_record(everyone, account, package.id, &record_length,
                           &store);
    /* A new copy for one account is refused when everyone has it; a copy
       the account already had can still be updated. */
    if (!everyone && !own
        && read_record(true, account, package.id, &record_length, NULL)) {
        copy_text(reason, sizeof(reason), "already installed for everyone");
        return PACKAGE_CONFLICT;
    }
    if (own && !read_record(everyone, account, package.id, &record_length,
                            &store)) {
        return PACKAGE_DISK_ERROR;
    }
    if (own) {
        char version[PACKAGE_VERSION_MAX + 1];
        if (store) {
            copy_text(reason, sizeof(reason),
                      "installed from the OPOS Store: remove it first");
            return PACKAGE_CONFLICT;
        }
        if ((options & PACKAGE_OPTION_IGNORE_VERSION) == 0
            && record_version(record_text, record_length, version)
            && package_compare_versions(package.version, version) <= 0) {
            copy_text(reason, sizeof(reason),
                      "the same or a newer version is installed");
            return PACKAGE_NOT_NEWER;
        }
    } else if (exists(target, NULL)) {
        copy_text(reason, sizeof(reason),
                  "a folder with the package's id is in the way");
        return PACKAGE_CONFLICT;
    }
    return PACKAGE_OK;
}

enum package_result package_install(const char *path, size_t length,
                                    size_t account, bool admin,
                                    uint32_t options,
                                    struct package_info *info) {
    (void)length;
    reason[0] = '\0';
    bool everyone = (options & PACKAGE_OPTION_EVERYONE) != 0;
    if ((options & ~PACKAGE_OPTION_MASK) != 0) {
        return PACKAGE_INVALID;
    }
    if (!installed_system()) {
        return PACKAGE_NOT_INSTALLED;
    }
    if (everyone ? !admin : account == PERMISSION_NO_ACCOUNT) {
        return PACKAGE_NOT_ALLOWED;
    }
    char records[PATH_CAPACITY];
    char programs[PATH_CAPACITY];
    char target[PATH_CAPACITY];
    char record[PATH_CAPACITY];
    if (!scope_paths(everyone, account, records, programs)) {
        return PACKAGE_NOT_ALLOWED;
    }
    if (!finish_pending() || !clear_staging() || !make_directory(STAGE)) {
        return PACKAGE_DISK_ERROR;
    }
    /* Read PACKAGE alone first, so a refusal costs no copying. */
    enum package_result result = scan_archive(path, SCAN_HEADER, info);
    if (result == PACKAGE_OK) {
        result = runnable();
    }
    if (result == PACKAGE_OK || result == PACKAGE_NEEDS_NEWER_OPOS
        || result == PACKAGE_PLATFORM) {
        installed_info(account, info);
    }
    /* Only packages from outside the Store exist so far. */
    if (result == PACKAGE_OK && (options & PACKAGE_OPTION_CONFIRMED) == 0) {
        copy_text(reason, sizeof(reason),
                  "not from the OPOS Store: the person must agree first");
        result = PACKAGE_NOT_CONFIRMED;
    }
    if (result == PACKAGE_OK) {
        result = install_allowed(everyone, account, options, records,
                                 programs, target, record);
    }
    /* Copy and check every file into the staging folder. The archive is
       read again, and only this read counts: the checks are repeated on
       it, in case the file changed in between. */
    if (result == PACKAGE_OK) {
        result = scan_archive(path, SCAN_STAGE, info);
    }
    if (result == PACKAGE_OK) {
        result = runnable();
    }
    if (result == PACKAGE_OK) {
        result = install_allowed(everyone, account, options, records,
                                 programs, target, record);
    }
    if (result != PACKAGE_OK) {
        copy_reason(info, reason);
        (void)clear_staging();
        return result;
    }
    /* The record: origin, then PACKAGE as it came. */
    static const char origin[] = ORIGIN_SIDELOADED;
    size_t prefix = sizeof(origin) - 1;
    char *record_start = text + RECORD_PREFIX_MAX - prefix;
    for (size_t i = 0; i < prefix; i++) {
        record_start[i] = origin[i];
    }
    copy_text(state_target, sizeof(state_target), target);
    copy_text(state_record, sizeof(state_record), record);
    block_begin_batch();
    bool ok = make_directory(programs) && make_directory(records)
        && write_whole(STAGE_RECORD, record_start, prefix + package_size)
        && write_state(true);
    if (!block_end_batch() || !ok) {
        (void)clear_staging();
        return PACKAGE_DISK_ERROR;
    }
    ok = apply(true);
    package_registry_load();
    return ok ? PACKAGE_OK : PACKAGE_DISK_ERROR;
}

/* -------------------------------------------------------------- remove */

/*
 * After a removal, in the removing account only: forget the program's
 * grants (store:<id>, or the hash of each executable file of a
 * sideloaded package, read into package before the removal) and, when
 * asked, delete its app storage. Other accounts keep both.
 */
static void forget_program(size_t account, const char *id, bool store,
                           bool remove_data) {
    if (account == PERMISSION_NO_ACCOUNT || account >= accounts_count()) {
        return;
    }
    uint8_t key[SHA256_DIGEST_SIZE];
    if (store) {
        permission_store_key(id, key);
        (void)permission_grants_forget(account, key);
    } else {
        for (size_t i = 0; i < package.file_count; i++) {
            if (package.files[i].exec) {
                (void)permission_grants_forget(account,
                                               package.files[i].hash);
            }
        }
    }
    char data[PATH_CAPACITY];
    size_t used = home_path(account, data, sizeof(data));
    used = used == 0 ? 0 : append_text(data, sizeof(data), used,
                                       "/AppData/Programs/");
    used = used == 0 ? 0 : append_text(data, sizeof(data), used, id);
    enum vfs_node_type type;
    if (remove_data && used != 0 && exists(data, &type)
        && type == VFS_NODE_DIRECTORY && !remove_tree(data)) {
        console_write_string("OPOS: packages: could not delete all of ");
        console_write_string(data);
        console_write_string("\n");
    }
}

enum package_result package_remove(const char *id, size_t length,
                                   size_t account, bool admin,
                                   uint32_t options) {
    bool everyone = (options & PACKAGE_OPTION_EVERYONE) != 0;
    bool remove_data = (options & PACKAGE_OPTION_REMOVE_DATA) != 0;
    if (!installed_system()) {
        return PACKAGE_NOT_INSTALLED;
    }
    if (everyone ? !admin : account == PERMISSION_NO_ACCOUNT) {
        return PACKAGE_NOT_ALLOWED;
    }
    char name[PACKAGE_ID_MAX + 1];
    if (!id_name(id, length)) {
        return PACKAGE_INVALID;
    }
    for (size_t i = 0; i < length; i++) {
        name[i] = id[i];
    }
    name[length] = '\0';
    char records[PATH_CAPACITY];
    char programs[PATH_CAPACITY];
    if (!scope_paths(everyone, account, records, programs)
        || !join_id(state_target, programs, name)
        || !join_id(state_record, records, name)) {
        return PACKAGE_INVALID;
    }
    if (!exists(state_record, NULL)) {
        return PACKAGE_NOT_FOUND;
    }
    /* What to forget afterwards. While the account still has the other
       copy of this id (its own and everyone's), grants and data stay:
       that copy uses them. */
    size_t record_length;
    bool store = false;
    bool known = read_record(everyone, account, name, &record_length, &store)
        && package_parse(record_text, record_length, true, &package, NULL)
               == PACKAGE_OK;
    bool other_copy = false;
    if (account != PERMISSION_NO_ACCOUNT) {
        char other_records[PATH_CAPACITY];
        char other_programs[PATH_CAPACITY];
        char other[PATH_CAPACITY];
        other_copy = scope_paths(!everyone, account, other_records,
                                 other_programs)
            && join_id(other, other_records, name) && exists(other, NULL);
    }
    if (!finish_pending() || !clear_staging() || !make_directory(STAGE)) {
        return PACKAGE_DISK_ERROR;
    }
    /* finish_pending may have used the state paths. */
    (void)join_id(state_target, programs, name);
    (void)join_id(state_record, records, name);
    block_begin_batch();
    bool ok = write_state(false);
    if (!block_end_batch() || !ok) {
        return PACKAGE_DISK_ERROR;
    }
    ok = apply(false);
    package_registry_load();
    if (ok && known && !other_copy) {
        forget_program(account, name, store, remove_data);
    }
    return ok ? PACKAGE_OK : PACKAGE_DISK_ERROR;
}

/* ------------------------------------------------------------ identity */

/* The next component of path after *offset. */
static size_t next_part(const char *path, size_t length, size_t *offset,
                        const char **part) {
    while (*offset < length && path[*offset] == '/') {
        (*offset)++;
    }
    size_t start = *offset;
    while (*offset < length && path[*offset] != '/') {
        (*offset)++;
    }
    *part = path + start;
    return *offset - start;
}

static bool same_name(const char *part, size_t length, const char *name) {
    size_t i = 0;
    for (; i < length && name[i] != '\0'; i++) {
        char a = part[i];
        char b = name[i];
        a = a >= 'A' && a <= 'Z' ? (char)(a - 'A' + 'a') : a;
        b = b >= 'A' && b <= 'Z' ? (char)(b - 'A' + 'a') : b;
        if (a != b) {
            return false;
        }
    }
    return i == length && name[i] == '\0';
}

bool package_identify(const char *path, size_t length, size_t account,
                      const uint8_t hash[32], char id[PACKAGE_ID_MAX + 1],
                      bool *store) {
    if (path == NULL || hash == NULL || id == NULL || store == NULL
        || !fat16_system_mounted()) {
        return false;
    }
    /* /disk/Programs/<id>/... or /disk/Users/<account>/Programs/<id>/...
       An 8.3 alias of the id folder makes the program a loose one, which
       is safe. */
    size_t offset = 0;
    const char *part;
    size_t part_length = next_part(path, length, &offset, &part);
    if (!same_name(part, part_length, "disk")) {
        return false;
    }
    part_length = next_part(path, length, &offset, &part);
    bool everyone = same_name(part, part_length, "Programs");
    if (!everyone) {
        if (!same_name(part, part_length, "Users")
            || account == PERMISSION_NO_ACCOUNT
            || account >= accounts_count()) {
            return false;
        }
        part_length = next_part(path, length, &offset, &part);
        if (!same_name(part, part_length, accounts_name(account))) {
            return false;
        }
        part_length = next_part(path, length, &offset, &part);
        if (!same_name(part, part_length, "Programs")) {
            return false;
        }
    }
    /* FAT ignores case, so any case names the same folder. */
    part_length = next_part(path, length, &offset, &part);
    char name[PACKAGE_ID_MAX + 1];
    if (part_length > PACKAGE_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < part_length; i++) {
        char c = part[i];
        name[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    name[part_length] = '\0';
    if (!id_name(name, part_length)) {
        return false;
    }
    const char *rest;
    if (next_part(path, length, &offset, &rest) == 0) {
        return false; /* the folder itself, not a file inside it */
    }
    char records[PATH_CAPACITY];
    char programs[PATH_CAPACITY];
    char record[PATH_CAPACITY];
    size_t record_length;
    if (!scope_paths(everyone, account, records, programs)
        || !join_id(record, records, name)
        || !read_whole(record, identify_text, sizeof(identify_text),
                       &record_length)
        || package_parse(identify_text, record_length, true,
                         &identify_package, NULL) != PACKAGE_OK
        || !same_text(identify_package.id, name)) {
        return false;
    }
    bool match = false;
    for (size_t i = 0; i < identify_package.file_count && !match; i++) {
        const struct package_file *file = &identify_package.files[i];
        match = file->exec;
        for (size_t b = 0; b < 32 && match; b++) {
            match = file->hash[b] == hash[b];
        }
    }
    if (!match) {
        return false;
    }
    copy_text(id, PACKAGE_ID_MAX + 1, name);
    *store = identify_package.store;
    return true;
}

/* ------------------------------------------------------------- at boot */

void package_resume(void) {
    if (!installed_system()) {
        return;
    }
    if (!finish_pending()) {
        console_write_string("OPOS: packages: could not finish; "
                             "staging kept in /disk/OPOS/PKGSTAGE\n");
    }
    package_registry_load();
}

/* Adds the package folders of one scope to permissions.c's list. */
static void registry_scope(bool everyone, size_t account) {
    char records[PATH_CAPACITY];
    char programs[PATH_CAPACITY];
    if (!scope_paths(everyone, account, records, programs)) {
        return;
    }
    size_t records_length = text_length(records);
    size_t count;
    if (!vfs_directory_count(records, records_length, &count)) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(records, records_length, i, &entry)) {
            return;
        }
        if (entry.type != VFS_NODE_FILE
            || !id_name(entry.name, entry.name_length)) {
            continue;
        }
        char folder[PATH_CAPACITY];
        size_t used = append_text(folder, sizeof(folder), 0, programs);
        used = used == 0 ? 0 : append(folder, sizeof(folder), used, "/", 1);
        used = used == 0 ? 0 : append(folder, sizeof(folder), used,
                                      entry.name, entry.name_length);
        if (used != 0) {
            (void)permission_package_folder_add(
                folder, everyone ? PERMISSION_NO_ACCOUNT : account);
        }
    }
}

void package_registry_load(void) {
    permission_package_folders_clear();
    if (!fat16_system_mounted()) {
        return;
    }
    registry_scope(true, PERMISSION_NO_ACCOUNT);
    for (size_t i = 0; i < accounts_count(); i++) {
        registry_scope(false, i);
    }
}

/* ----------------------------------------------------------- Start menu */

/* Adds the start entries of each record in one scope. */
static size_t start_scope(bool everyone, size_t account,
                          struct package_start_entry *out, size_t used,
                          size_t capacity) {
    char records[PATH_CAPACITY];
    char programs[PATH_CAPACITY];
    if (!scope_paths(everyone, account, records, programs)) {
        return used;
    }
    size_t records_length = text_length(records);
    size_t count;
    if (!vfs_directory_count(records, records_length, &count)) {
        return used;
    }
    for (size_t i = 0; i < count && used < capacity; i++) {
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(records, records_length, i, &entry)) {
            break;
        }
        if (entry.type != VFS_NODE_FILE
            || !id_name(entry.name, entry.name_length)) {
            continue;
        }
        char record[PATH_CAPACITY];
        size_t record_used = append_text(record, sizeof(record), 0, records);
        record_used = record_used == 0 ? 0
            : append(record, sizeof(record), record_used, "/", 1);
        record_used = record_used == 0 ? 0
            : append(record, sizeof(record), record_used, entry.name,
                     entry.name_length);
        size_t length;
        if (record_used == 0
            || !read_whole(record, start_text, sizeof(start_text), &length)
            || package_parse(start_text, length, true, &start_package, NULL)
                   != PACKAGE_OK) {
            continue;
        }
        /* An account's own copy hides everyone's. */
        if (everyone && account != PERMISSION_NO_ACCOUNT) {
            char own_records[PATH_CAPACITY];
            char own_programs[PATH_CAPACITY];
            char own[PATH_CAPACITY];
            if (scope_paths(false, account, own_records, own_programs)
                && join_id(own, own_records, start_package.id)
                && exists(own, NULL)) {
                continue;
            }
        }
        for (size_t s = 0; s < start_package.start_count && used < capacity;
             s++) {
            const struct package_start *start = &start_package.starts[s];
            const struct package_file *file =
                &start_package.files[start->file];
            struct package_start_entry *item = &out[used];
            size_t label = start->label.length;
            for (size_t c = 0; c < label; c++) {
                item->label[c] = start_text[start->label.offset + c];
            }
            item->label[label] = '\0';
            size_t at = append_text(item->path, sizeof(item->path), 0,
                                    programs);
            at = at == 0 ? 0 : append(item->path, sizeof(item->path), at, "/",
                                      1);
            at = at == 0 ? 0 : append_text(item->path, sizeof(item->path), at,
                                           start_package.id);
            /* "files/x" -> "/x" */
            at = at == 0 ? 0
                : append(item->path, sizeof(item->path), at,
                         start_text + file->path.offset + 5,
                         file->path.length - 5);
            if (at != 0) {
                used++;
            }
        }
    }
    return used;
}

size_t package_start_entries(size_t account, struct package_start_entry *out,
                             size_t capacity) {
    if (out == NULL || !fat16_system_mounted()) {
        return 0;
    }
    size_t used = 0;
    if (account != PERMISSION_NO_ACCOUNT) {
        used = start_scope(false, account, out, used, capacity);
    }
    return start_scope(true, account, out, used, capacity);
}
