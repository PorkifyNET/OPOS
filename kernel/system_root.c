#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "initramfs.h"
#include "system_root.h"
#include "vfs.h"

#define ROOT_PREFIX "/disk/OPOS/roots/"
#define PATH_CAPACITY 512
#define VERIFY_CHUNK 4096

static uint8_t verify_buffer[VERIFY_CHUNK];
static const char ready_text[] = "OPOS-ROOT-READY-1\n" OPOS_VERSION "\n";

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') length++;
    return length;
}

bool system_root_generation(char *name, size_t capacity) {
    static const char prefix[] = "generation-";
    if (name == NULL || capacity < sizeof(prefix) + 1) return false;
    size_t used = 0;
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) name[used++] = prefix[i];
    for (size_t i = 0; OPOS_VERSION[i] != '\0'; i++) {
        char c = OPOS_VERSION[i];
        if (c == '.') c = '_';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
              || (c >= 'A' && c <= 'Z') || c == '_' || c == '-')) {
            return false;
        }
        if (used + 1 >= capacity) return false;
        name[used++] = c;
    }
    if (used == sizeof(prefix) - 1) return false;
    /* The archive hash keeps a rebuilt initramfs with an unchanged
       VERSION from colliding with the ready root of the earlier build,
       which would fail verification and leave only the recovery shell. */
    if (used + 10 > capacity) return false;
    name[used++] = '-';
    uint32_t hash = initramfs_archive_hash();
    for (int shift = 28; shift >= 0; shift -= 4) {
        name[used++] = "0123456789abcdef"[(hash >> shift) & 0xf];
    }
    name[used] = '\0';
    return true;
}

bool system_root_path(char *path, size_t capacity) {
    char generation[64];
    if (path == NULL || !system_root_generation(generation,
                                                 sizeof(generation))) {
        return false;
    }
    size_t prefix = sizeof(ROOT_PREFIX) - 1;
    size_t name = text_length(generation);
    if (prefix + name >= capacity) return false;
    for (size_t i = 0; i < prefix; i++) path[i] = ROOT_PREFIX[i];
    for (size_t i = 0; i < name; i++) path[prefix + i] = generation[i];
    path[prefix + name] = '\0';
    return true;
}

static bool ready_path(const char *root, size_t root_length,
                       char *path, size_t capacity) {
    static const char suffix[] = ".ready";
    if (root == NULL || root_length + sizeof(suffix) > capacity) {
        return false;
    }
    for (size_t i = 0; i < root_length; i++) path[i] = root[i];
    for (size_t i = 0; i < sizeof(suffix); i++) {
        path[root_length + i] = suffix[i];
    }
    return true;
}

static bool verify_file(const char *path, size_t path_length,
                        const struct initramfs_file *expected) {
    size_t size;
    if (!vfs_file_size(path, path_length, &size) || size != expected->size) {
        return false;
    }
    for (size_t offset = 0; offset < size; offset += VERIFY_CHUNK) {
        size_t chunk = size - offset < VERIFY_CHUNK
            ? size - offset : VERIFY_CHUNK;
        size_t read;
        if (!vfs_read_at(path, path_length, offset, verify_buffer,
                         chunk, &read) || read != chunk) {
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (verify_buffer[i] != expected->data[offset + i]) return false;
        }
    }
    return true;
}

/* Reject additions too: an unexpected /bin executable must not inherit
   built-in program trust simply because it sits inside a selected root. */
static bool verify_entries(const char *path, size_t path_length,
                           const char *relative, size_t relative_length,
                           unsigned depth) {
    if (depth > 8) return false;
    size_t count;
    if (!vfs_directory_count(path, path_length, &count)) return false;
    for (size_t i = 0; i < count; i++) {
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(path, path_length, i, &entry)
            || entry.name_length == 0
            || path_length + 1 + entry.name_length >= PATH_CAPACITY
            || relative_length + (relative_length != 0)
                + entry.name_length >= 256) {
            return false;
        }
        char child[PATH_CAPACITY];
        char name[256];
        for (size_t j = 0; j < path_length; j++) child[j] = path[j];
        child[path_length] = '/';
        for (size_t j = 0; j < entry.name_length; j++) {
            child[path_length + 1 + j] = entry.name[j];
        }
        size_t child_length = path_length + 1 + entry.name_length;
        child[child_length] = '\0';
        for (size_t j = 0; j < relative_length; j++) name[j] = relative[j];
        size_t name_length = relative_length;
        if (name_length != 0) name[name_length++] = '/';
        for (size_t j = 0; j < entry.name_length; j++) {
            name[name_length++] = entry.name[j];
        }
        name[name_length] = '\0';
        struct initramfs_metadata expected;
        struct vfs_metadata actual;
        if (!initramfs_stat(name, name_length, &expected)
            || !vfs_stat(child, child_length, &actual)
            || (expected.type == INITRAMFS_NODE_DIRECTORY)
                != (actual.type == VFS_NODE_DIRECTORY)) {
            return false;
        }
        if (actual.type == VFS_NODE_DIRECTORY) {
            if (!verify_entries(child, child_length, name, name_length,
                                depth + 1)) return false;
        } else if (actual.size != expected.size) {
            return false;
        }
    }
    return true;
}

bool system_root_verify_tree(const char *path, size_t path_length) {
    struct vfs_metadata root;
    if (path == NULL || path_length == 0
        || !vfs_stat(path, path_length, &root)
        || root.device != 2 || root.type != VFS_NODE_DIRECTORY
        || initramfs_file_count() == 0
        || !verify_entries(path, path_length, "", 0, 0)) {
        return false;
    }
    for (size_t i = 0; i < initramfs_file_count(); i++) {
        char relative[256];
        size_t expected_size;
        if (!initramfs_file_at(i, relative, sizeof(relative),
                               &expected_size)) return false;
        size_t relative_length = text_length(relative);
        if (path_length + 1 + relative_length >= PATH_CAPACITY) return false;
        char child[PATH_CAPACITY];
        for (size_t j = 0; j < path_length; j++) child[j] = path[j];
        child[path_length] = '/';
        for (size_t j = 0; j < relative_length; j++) {
            child[path_length + 1 + j] = relative[j];
        }
        size_t child_length = path_length + 1 + relative_length;
        child[child_length] = '\0';
        struct initramfs_file expected;
        if (!initramfs_find_file(relative, &expected)
            || expected.size != expected_size
            || !verify_file(child, child_length, &expected)) return false;
    }
    return true;
}

bool system_root_ready(const char *path, size_t path_length) {
    char marker[PATH_CAPACITY];
    size_t marker_length = path_length + sizeof(".ready") - 1;
    size_t size, read;
    uint8_t contents[sizeof(ready_text) - 1];
    if (!ready_path(path, path_length, marker, sizeof(marker))
        || !vfs_file_size(marker, marker_length, &size)
        || size != sizeof(contents)
        || !vfs_read_at(marker, marker_length, 0, contents,
                        sizeof(contents), &read)
        || read != sizeof(contents)) return false;
    for (size_t i = 0; i < sizeof(contents); i++) {
        if (contents[i] != (uint8_t)ready_text[i]) return false;
    }
    return true;
}

bool system_root_write_ready(const char *path, size_t path_length) {
    char marker[PATH_CAPACITY];
    size_t marker_length = path_length + sizeof(".ready") - 1;
    return ready_path(path, path_length, marker, sizeof(marker))
        && vfs_write_file(marker, marker_length,
                          (const uint8_t *)ready_text,
                          sizeof(ready_text) - 1)
        && system_root_ready(path, path_length);
}

bool system_root_select(const char *generation, size_t length) {
    char expected[64];
    char path[PATH_CAPACITY];
    if (generation == NULL
        || !system_root_generation(expected, sizeof(expected))
        || text_length(expected) != length
        || !system_root_path(path, sizeof(path))) return false;
    for (size_t i = 0; i < length; i++) {
        if (generation[i] != expected[i]) return false;
    }
    size_t path_length = text_length(path);
    return system_root_ready(path, path_length)
        && system_root_verify_tree(path, path_length)
        && vfs_select_disk_root(path, path_length);
}
