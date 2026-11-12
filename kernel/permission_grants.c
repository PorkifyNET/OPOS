#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accounts.h"
#include "manifest.h"
#include "package.h"
#include "permission_grants.h"
#include "permissions.h"
#include "sha256.h"
#include "vfs.h"

/* See permission_grants.h for the format. */

#define GRANTS_VERSION 1
#define NAME_FIELD (PERMISSION_GRANT_NAME_MAX + 1)
#define NAME_OFFSET 56
_Static_assert(NAME_OFFSET + NAME_FIELD == PERMISSION_GRANTS_RECORD_SIZE,
               "grant record layout");
#define PATH_CAPACITY 128

static uint8_t file_buffer[PERMISSION_GRANTS_FILE_MAX];

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static uint32_t read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8
        | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t read_u64(const uint8_t *data) {
    return (uint64_t)read_u32(data) | (uint64_t)read_u32(data + 4) << 32;
}

static void write_u32(uint8_t *data, uint32_t value) {
    for (size_t i = 0; i < 4; i++) {
        data[i] = (uint8_t)(value >> (i * 8));
    }
}

static void write_u64(uint8_t *data, uint64_t value) {
    write_u32(data, (uint32_t)value);
    write_u32(data + 4, (uint32_t)(value >> 32));
}

static bool same_program(const uint8_t *a, const uint8_t *b) {
    uint8_t difference = 0;
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
        difference |= (uint8_t)(a[i] ^ b[i]);
    }
    return difference == 0;
}

void permission_grants_clear(struct permission_grants *grants) {
    uint8_t *bytes = (uint8_t *)grants;
    for (size_t i = 0; i < sizeof(*grants); i++) {
        bytes[i] = 0;
    }
}

bool permission_grants_decode(const uint8_t *data, size_t size,
                              struct permission_grants *grants) {
    permission_grants_clear(grants);
    if (data == NULL || size < PERMISSION_GRANTS_HEADER_SIZE
        || data[0] != 'O' || data[1] != 'P' || data[2] != 'G'
        || data[3] != 'R' || data[4] != GRANTS_VERSION
        || data[5] != 0 || data[6] != 0 || data[7] != 0) {
        return false;
    }
    uint32_t count = read_u32(data + 12);
    if (count > PERMISSION_GRANTS_MAX
        || size != PERMISSION_GRANTS_HEADER_SIZE
                       + (size_t)count * PERMISSION_GRANTS_RECORD_SIZE) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *record = data + PERMISSION_GRANTS_HEADER_SIZE
            + (size_t)i * PERMISSION_GRANTS_RECORD_SIZE;
        struct permission_grant *grant = &grants->records[i];
        for (size_t j = 0; j < SHA256_DIGEST_SIZE; j++) {
            grant->program[j] = record[j];
        }
        grant->granted = read_u32(record + 32) & PERMISSION_CAPS_ALL;
        grant->denied = read_u32(record + 36) & PERMISSION_CAPS_ALL;
        grant->ask = read_u32(record + 40) & PERMISSION_CAPS_ALL;
        grant->updated = read_u64(record + 48);
        bool terminated = false;
        for (size_t j = 0; j < NAME_FIELD; j++) {
            grant->name[j] = (char)record[NAME_OFFSET + j];
            terminated = terminated || record[NAME_OFFSET + j] == 0;
        }
        bool duplicate = false;
        for (uint32_t j = 0; j < i; j++) {
            duplicate = duplicate
                || same_program(grants->records[j].program, grant->program);
        }
        if (!terminated || (grant->granted & grant->denied) != 0
            || (grant->ask & (grant->granted | grant->denied)) != 0
            || read_u32(record + 44) != 0 || duplicate) {
            permission_grants_clear(grants);
            return false;
        }
    }
    grants->flags = read_u32(data + 8);
    grants->count = count;
    return true;
}

size_t permission_grants_encode(const struct permission_grants *grants,
                                uint8_t *out, size_t capacity) {
    if (grants == NULL || out == NULL
        || grants->count > PERMISSION_GRANTS_MAX) {
        return 0;
    }
    size_t size = PERMISSION_GRANTS_HEADER_SIZE
        + grants->count * PERMISSION_GRANTS_RECORD_SIZE;
    if (capacity < size) {
        return 0;
    }
    for (size_t i = 0; i < size; i++) {
        out[i] = 0;
    }
    out[0] = 'O';
    out[1] = 'P';
    out[2] = 'G';
    out[3] = 'R';
    out[4] = GRANTS_VERSION;
    write_u32(out + 8, grants->flags);
    write_u32(out + 12, (uint32_t)grants->count);
    for (size_t i = 0; i < grants->count; i++) {
        uint8_t *record = out + PERMISSION_GRANTS_HEADER_SIZE
            + i * PERMISSION_GRANTS_RECORD_SIZE;
        const struct permission_grant *grant = &grants->records[i];
        for (size_t j = 0; j < SHA256_DIGEST_SIZE; j++) {
            record[j] = grant->program[j];
        }
        write_u32(record + 32, grant->granted);
        write_u32(record + 36, grant->denied);
        write_u32(record + 40, grant->ask);
        write_u64(record + 48, grant->updated);
        for (size_t j = 0; j < PERMISSION_GRANT_NAME_MAX
                           && grant->name[j] != '\0'; j++) {
            record[NAME_OFFSET + j] = (uint8_t)grant->name[j];
        }
    }
    return size;
}

struct permission_grant *permission_grants_find(
    struct permission_grants *grants,
    const uint8_t program[SHA256_DIGEST_SIZE]) {
    if (grants == NULL || program == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < grants->count; i++) {
        if (same_program(grants->records[i].program, program)) {
            return &grants->records[i];
        }
    }
    return NULL;
}

bool permission_grants_set(struct permission_grants *grants,
                           const uint8_t program[SHA256_DIGEST_SIZE],
                           const char *name, uint32_t granted,
                           uint32_t denied, uint32_t ask, uint64_t updated) {
    if (grants == NULL || program == NULL) {
        return false;
    }
    struct permission_grant *grant = permission_grants_find(grants, program);
    if (grant == NULL) {
        if (grants->count >= PERMISSION_GRANTS_MAX) {
            return false;
        }
        grant = &grants->records[grants->count++];
        for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
            grant->program[i] = program[i];
        }
    }
    denied &= PERMISSION_CAPS_ALL;
    grant->denied = denied;
    grant->granted = granted & PERMISSION_CAPS_ALL & ~denied;
    grant->ask = ask & PERMISSION_CAPS_ALL & ~(grant->granted | denied);
    grant->updated = updated;
    size_t length = 0;
    if (name != NULL) {
        while (length < PERMISSION_GRANT_NAME_MAX && name[length] != '\0') {
            grant->name[length] = name[length];
            length++;
        }
    }
    while (length < NAME_FIELD) {
        grant->name[length++] = '\0';
    }
    return true;
}

bool permission_grants_remove(struct permission_grants *grants,
                              const uint8_t program[SHA256_DIGEST_SIZE]) {
    struct permission_grant *grant = permission_grants_find(grants, program);
    if (grant == NULL) {
        return false;
    }
    size_t index = (size_t)(grant - grants->records);
    for (size_t i = index; i + 1 < grants->count; i++) {
        grants->records[i] = grants->records[i + 1];
    }
    grants->count--;
    uint8_t *last = (uint8_t *)&grants->records[grants->count];
    for (size_t i = 0; i < sizeof(grants->records[0]); i++) {
        last[i] = 0;
    }
    return true;
}

/* Appends "/<name>" to out[0..used); returns the new length or 0. */
static size_t append_component(char *out, size_t capacity, size_t used,
                               const char *name) {
    size_t length = text_length(name);
    if (used == 0 || used + 1 + length >= capacity) {
        return 0;
    }
    out[used++] = '/';
    for (size_t i = 0; i < length; i++) {
        out[used++] = name[i];
    }
    out[used] = '\0';
    return used;
}

/* Builds "<home>/AppData" into out; returns its length or 0. */
static size_t appdata_path(size_t account, char *out, size_t capacity) {
    if (out == NULL || account == PERMISSION_NO_ACCOUNT
        || account >= accounts_count()
        || capacity <= sizeof(ACCOUNT_HOME_ROOT)) {
        return 0;
    }
    size_t used = sizeof(ACCOUNT_HOME_ROOT) - 1;
    for (size_t i = 0; i <= used; i++) {
        out[i] = ACCOUNT_HOME_ROOT[i];
    }
    used = append_component(out, capacity, used, accounts_name(account));
    return append_component(out, capacity, used, "AppData");
}

size_t permission_grants_path(size_t account, char *out, size_t capacity) {
    size_t used = appdata_path(account, out, capacity);
    return append_component(out, capacity, used, "Permissions.dat");
}

bool permission_grants_load(size_t account, struct permission_grants *grants) {
    permission_grants_clear(grants);
    char path[PATH_CAPACITY];
    size_t length = permission_grants_path(account, path, sizeof(path));
    if (length == 0) {
        return false;
    }
    struct vfs_metadata metadata;
    if (!vfs_stat(path, length, &metadata)) {
        return true; /* nothing granted yet */
    }
    size_t size = 0;
    if (metadata.type != VFS_NODE_FILE
        || !vfs_read_file(path, length, file_buffer, sizeof(file_buffer),
                          &size)) {
        return false;
    }
    return permission_grants_decode(file_buffer, size, grants);
}

static bool ensure_directory(const char *path, size_t length) {
    struct vfs_metadata metadata;
    if (vfs_stat(path, length, &metadata)) {
        return metadata.type == VFS_NODE_DIRECTORY;
    }
    return vfs_create_directory(path, length);
}

bool permission_grants_save(size_t account,
                            const struct permission_grants *grants) {
    char path[PATH_CAPACITY];
    size_t folder = appdata_path(account, path, sizeof(path));
    if (folder == 0) {
        return false;
    }
    size_t home = folder - (sizeof("/AppData") - 1);
    path[home] = '\0';
    if (!ensure_directory(path, home)) {
        return false;
    }
    path[home] = '/';
    if (!ensure_directory(path, folder)) {
        return false;
    }
    size_t length = append_component(path, sizeof(path), folder,
                                     "Permissions.dat");
    size_t size = permission_grants_encode(grants, file_buffer,
                                           sizeof(file_buffer));
    return length != 0 && size != 0
        && vfs_write_file(path, length, file_buffer, size);
}

bool permission_program_hash(const char *path, size_t length,
                             uint8_t out[SHA256_DIGEST_SIZE]) {
    static uint8_t chunk[4096];
    size_t size = 0;
    if (path == NULL || out == NULL
        || !vfs_file_size(path, length, &size)) {
        return false;
    }
    struct sha256_context hash;
    sha256_init(&hash);
    size_t offset = 0;
    while (offset < size) {
        size_t want = size - offset < sizeof(chunk) ? size - offset
                                                    : sizeof(chunk);
        size_t got = 0;
        if (!vfs_read_at(path, length, offset, chunk, want, &got)
            || got == 0 || got > want) {
            return false;
        }
        sha256_update(&hash, chunk, got);
        offset += got;
    }
    sha256_final(&hash, out);
    return true;
}

void permission_store_key(const char *id, uint8_t out[SHA256_DIGEST_SIZE]) {
    static const char prefix[] = "store:";
    struct sha256_context hash;
    sha256_init(&hash);
    sha256_update(&hash, prefix, sizeof(prefix) - 1);
    size_t length = 0;
    while (id[length] != '\0') {
        length++;
    }
    sha256_update(&hash, id, length);
    sha256_final(&hash, out);
}

/* Loaded on demand. Kernel code is not preempted and nothing below
   blocks while it is in use, so one copy serves every caller. */
static struct permission_grants scratch;

void permission_apply_program(struct permission_identity *identity,
                              const char *path, size_t length,
                              const uint8_t *image, size_t size) {
    if (identity == NULL || path == NULL || length == 0) {
        return; /* kernel-embedded test programs have no path */
    }
    if (!permission_is_disk_program(path, length)) {
        /* Built-ins narrow too, never widen what the parent held. */
        uint32_t allowed = permission_builtin_capabilities(path, length);
        identity->capabilities &= allowed;
        identity->askable &= allowed;
        return;
    }
    uint32_t parent = identity->capabilities;
    /* An installed package's own executable (docs/packages.md,
       "Identity"): storage named by the package id, and for a Store
       package grants keyed by store:<id>, so an update keeps both. */
    bool hashed = image != NULL && size != 0;
    uint8_t digest[SHA256_DIGEST_SIZE];
    char package_id[PACKAGE_ID_MAX + 1];
    bool store = false;
    bool packaged = false;
    if (hashed) {
        struct sha256_context hash;
        sha256_init(&hash);
        sha256_update(&hash, image, size);
        sha256_final(&hash, digest);
        packaged = package_identify(path, length, identity->account, digest,
                                    package_id, &store);
    }
    permission_confine_disk_program_named(identity, path, length,
                                          packaged ? package_id : NULL);
    identity->askable = 0;
    identity->program_known = false;
    identity->program_name[0] = '\0';
    if (!hashed) {
        return;
    }
    if (packaged && store) {
        permission_store_key(package_id, identity->program);
    } else {
        for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
            identity->program[i] = digest[i];
        }
    }
    identity->program_known = true;
    /* With a manifest, the program may ask only for what it declared
       (files.shared included). Without one: net and clipboard.read, plus
       what migration recorded. */
    struct manifest manifest;
    bool declared = manifest_read(image, size, &manifest);
    for (size_t i = 0; i < sizeof(identity->program_name); i++) {
        identity->program_name[i] = declared ? manifest.name[i] : '\0';
    }
    uint32_t requestable = declared
        ? manifest.capabilities & ~PERMISSION_CAPS_DISK_DEFAULT
        : PERMISSION_CAPS_ASK_ON_USE;
    identity->askable = requestable & parent;
    if (identity->account == PERMISSION_NO_ACCOUNT
        || identity->account >= accounts_count()) {
        return; /* nowhere to keep answers: ask, and remember per process */
    }
    if (!permission_grants_load(identity->account, &scratch)) {
        identity->askable = 0; /* damaged store: refuse, never guess */
        return;
    }
    const struct permission_grant *grant =
        permission_grants_find(&scratch, identity->program);
    if (grant == NULL) {
        return;
    }
    uint32_t decided = grant->granted | grant->denied;
    if (!declared) {
        requestable |= grant->ask;
    }
    identity->capabilities = parent
        & (PERMISSION_CAPS_DISK_DEFAULT | grant->granted) & ~grant->denied;
    identity->askable = requestable & ~decided & parent;
}

bool permission_grants_decide(size_t account,
                              const uint8_t program[SHA256_DIGEST_SIZE],
                              const char *name, uint32_t capability,
                              bool allow, uint64_t now) {
    capability &= PERMISSION_CAPS_ALL;
    /* A malformed store is left for the person to inspect, not replaced. */
    if (capability == 0 || program == NULL
        || !permission_grants_load(account, &scratch)) {
        return false;
    }
    uint32_t granted = 0;
    uint32_t denied = 0;
    uint32_t ask = 0;
    char kept[PERMISSION_GRANT_NAME_MAX + 1] = {0};
    const struct permission_grant *grant =
        permission_grants_find(&scratch, program);
    if (grant != NULL) {
        granted = grant->granted;
        denied = grant->denied;
        ask = grant->ask;
        for (size_t i = 0; i < sizeof(kept); i++) {
            kept[i] = grant->name[i];
        }
    }
    if (allow) {
        granted |= capability;
        denied &= ~capability;
    } else {
        denied |= capability;
        granted &= ~capability;
    }
    ask &= ~capability;
    return permission_grants_set(&scratch, program,
                                 name != NULL ? name : kept, granted, denied,
                                 ask, now)
        && permission_grants_save(account, &scratch);
}

bool permission_grants_forget(size_t account,
                              const uint8_t program[SHA256_DIGEST_SIZE]) {
    return program != NULL && permission_grants_load(account, &scratch)
        && permission_grants_remove(&scratch, program)
        && permission_grants_save(account, &scratch);
}

static bool is_elf(const char *path, size_t length) {
    uint8_t magic[4];
    size_t got = 0;
    return vfs_read_at(path, length, 0, magic, sizeof(magic), &got)
        && got == sizeof(magic) && magic[0] == 0x7F && magic[1] == 'E'
        && magic[2] == 'L' && magic[3] == 'F';
}

#define MIGRATE_DEPTH 3

static void migrate_directory(const char *path, size_t length, size_t depth,
                              uint64_t now) {
    size_t count = 0;
    if (!vfs_directory_count(path, length, &count)) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(path, length, i, &entry)
            || entry.name_length == 0
            || (entry.name[0] == '.'
                && (entry.name_length == 1
                    || (entry.name_length == 2 && entry.name[1] == '.')))
            || length + 1 + entry.name_length >= 256) {
            continue;
        }
        char child[256];
        for (size_t j = 0; j < length; j++) {
            child[j] = path[j];
        }
        child[length] = '/';
        for (size_t j = 0; j < entry.name_length; j++) {
            child[length + 1 + j] = entry.name[j];
        }
        size_t child_length = length + 1 + entry.name_length;
        child[child_length] = '\0';
        if (entry.type == VFS_NODE_DIRECTORY) {
            if (depth + 1 < MIGRATE_DEPTH) {
                migrate_directory(child, child_length, depth + 1, now);
            }
            continue;
        }
        uint8_t program[SHA256_DIGEST_SIZE];
        if (scratch.count >= PERMISSION_GRANTS_MAX
            || !is_elf(child, child_length)
            || !permission_program_hash(child, child_length, program)
            || permission_grants_find(&scratch, program) != NULL) {
            continue;
        }
        (void)permission_grants_set(&scratch, program, child + length + 1,
                                    0, 0,
                                    PERMISSION_CAP_FILES_SHARED, now);
    }
}

bool permission_grants_migrate(size_t account, uint64_t now) {
    if (!permission_grants_load(account, &scratch)) {
        return false;
    }
    if ((scratch.flags & PERMISSION_GRANTS_MIGRATED) != 0) {
        return true;
    }
    static const char programs[] = "/disk/Programs";
    migrate_directory(programs, sizeof(programs) - 1, 0, now);
    scratch.flags |= PERMISSION_GRANTS_MIGRATED;
    return permission_grants_save(account, &scratch);
}
