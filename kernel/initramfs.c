#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "initramfs.h"

#define USTAR_BLOCK_SIZE 512
#define USTAR_NAME_SIZE 100
#define USTAR_PREFIX_SIZE 155

struct ustar_header {
    char name[USTAR_NAME_SIZE];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char modification_time[12];
    char checksum[8];
    char type;
    char link_name[100];
    char magic[6];
    char version[2];
    char user_name[32];
    char group_name[32];
    char device_major[8];
    char device_minor[8];
    char prefix[USTAR_PREFIX_SIZE];
    char padding[12];
} __attribute__((packed));

_Static_assert(sizeof(struct ustar_header) == USTAR_BLOCK_SIZE,
               "USTAR header size");

static const uint8_t *archive_bytes;
static size_t archive_length;
static uint32_t archive_hash;
static bool archive_hash_ready;

static bool octal_value(const char *field, size_t field_size, size_t *value) {
    size_t result = 0;
    size_t digits = 0;
    bool started = false;

    for (size_t i = 0; i < field_size; i++) {
        unsigned char character = (unsigned char)field[i];
        if (character == '\0' || character == ' ') {
            if (started) {
                for (size_t j = i + 1; j < field_size; j++) {
                    if (field[j] != '\0' && field[j] != ' ') {
                        return false;
                    }
                }
                break;
            }
            continue;
        }
        if (character < '0' || character > '7'
            || result > (SIZE_MAX - (character - '0')) / 8) {
            return false;
        }
        started = true;
        digits++;
        result = result * 8 + character - '0';
    }

    if (digits == 0) {
        return false;
    }
    *value = result;
    return true;
}

static bool header_valid(const struct ustar_header *header) {
    if (header->magic[0] != 'u' || header->magic[1] != 's'
        || header->magic[2] != 't' || header->magic[3] != 'a'
        || header->magic[4] != 'r'
        || (header->type != '\0' && header->type != '0'
            && header->type != '5')) {
        return false;
    }

    size_t stored_checksum;
    if (!octal_value(header->checksum, sizeof(header->checksum),
                     &stored_checksum)) {
        return false;
    }
    size_t checksum = 0;
    const uint8_t *bytes = (const uint8_t *)header;
    for (size_t i = 0; i < USTAR_BLOCK_SIZE; i++) {
        checksum += i >= offsetof(struct ustar_header, checksum)
                 && i < offsetof(struct ustar_header, checksum)
                        + sizeof(header->checksum)
            ? ' ' : bytes[i];
    }
    return checksum == stored_checksum;
}

static bool header_name(const struct ustar_header *header,
                        char *path, size_t path_capacity, size_t *path_length) {
    size_t prefix_length = 0;
    size_t name_length = 0;
    while (prefix_length < sizeof(header->prefix)
           && header->prefix[prefix_length] != '\0') {
        prefix_length++;
    }
    while (name_length < sizeof(header->name)
           && header->name[name_length] != '\0') {
        name_length++;
    }
    if (name_length == 0 || prefix_length + (prefix_length != 0)
            + name_length >= path_capacity) {
        return false;
    }

    size_t output = 0;
    for (size_t i = 0; i < prefix_length; i++) {
        path[output++] = header->prefix[i];
    }
    if (prefix_length != 0) {
        path[output++] = '/';
    }
    for (size_t i = 0; i < name_length; i++) {
        path[output++] = header->name[i];
    }
    path[output] = '\0';
    *path_length = output;
    return true;
}

static bool next_header(size_t *offset, const struct ustar_header **header,
                        size_t *file_size) {
    if (*offset > archive_length
        || archive_length - *offset < USTAR_BLOCK_SIZE) {
        return false;
    }

    const struct ustar_header *current =
        (const struct ustar_header *)(archive_bytes + *offset);
    if (current->name[0] == '\0') {
        *header = NULL;
        *offset = archive_length;
        return true;
    }
    if (!header_valid(current)
        || !octal_value(current->size, sizeof(current->size), file_size)) {
        return false;
    }
    if (current->type == '5' && *file_size != 0) {
        return false;
    }

    size_t data_offset = *offset + USTAR_BLOCK_SIZE;
    if (*file_size > archive_length - data_offset) {
        return false;
    }
    size_t padded_size = *file_size;
    size_t remainder = padded_size % USTAR_BLOCK_SIZE;
    if (remainder != 0) {
        size_t padding = USTAR_BLOCK_SIZE - remainder;
        if (padding > SIZE_MAX - padded_size) {
            return false;
        }
        padded_size += padding;
    }
    if (padded_size > archive_length - data_offset) {
        return false;
    }
    *offset = data_offset + padded_size;
    *header = current;
    return true;
}

bool initramfs_initialize(const uint8_t *archive, size_t archive_size) {
    if (archive == NULL || archive_size < USTAR_BLOCK_SIZE
        || archive_size % USTAR_BLOCK_SIZE != 0) {
        return false;
    }

    const uint8_t *previous_archive = archive_bytes;
    size_t previous_length = archive_length;
    archive_bytes = archive;
    archive_length = archive_size;

    size_t offset = 0;
    size_t file_size;
    const struct ustar_header *header;
    while (offset < archive_length) {
        if (!next_header(&offset, &header, &file_size)) {
            archive_bytes = previous_archive;
            archive_length = previous_length;
            return false;
        }
    }
    archive_hash_ready = false;
    return true;
}

/* FNV-1a over the whole archive. Not a security check (the archive is
   already trusted as the kernel's signed pair); it only tells two builds
   with the same version number apart. */
uint32_t initramfs_archive_hash(void) {
    if (!archive_hash_ready) {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < archive_length; i++) {
            hash ^= archive_bytes[i];
            hash *= 16777619u;
        }
        archive_hash = hash;
        archive_hash_ready = true;
    }
    return archive_hash;
}

bool initramfs_find_file(const char *path, struct initramfs_file *file) {
    if (archive_bytes == NULL || path == NULL || file == NULL) {
        return false;
    }

    size_t requested_length = 0;
    while (path[requested_length] != '\0') {
        requested_length++;
    }

    size_t offset = 0;
    size_t file_size;
    const struct ustar_header *header;
    char current_path[USTAR_PREFIX_SIZE + USTAR_NAME_SIZE + 1];
    while (offset < archive_length) {
        if (!next_header(&offset, &header, &file_size)) {
            return false;
        }
        if (header == NULL) {
            break;
        }
        if (header->type == '5') {
            continue;
        }

        size_t current_length;
        if (!header_name(header, current_path, sizeof(current_path),
                         &current_length)) {
            return false;
        }
        if (current_length != requested_length) {
            continue;
        }
        size_t i = 0;
        while (i < current_length && current_path[i] == path[i]) {
            i++;
        }
        if (i != current_length) {
            continue;
        }

        size_t data_offset = (size_t)((const uint8_t *)header - archive_bytes)
                           + USTAR_BLOCK_SIZE;
        file->data = archive_bytes + data_offset;
        file->size = file_size;
        return true;
    }
    return false;
}

size_t initramfs_file_count(void) {
    if (archive_bytes == NULL) {
        return 0;
    }

    size_t count = 0;
    size_t offset = 0;
    size_t file_size;
    const struct ustar_header *header;
    while (offset < archive_length) {
        if (!next_header(&offset, &header, &file_size)) {
            return 0;
        }
        if (header == NULL) {
            break;
        }
        if (header->type == '5') {
            continue;
        }
        count++;
    }
    return count;
}

bool initramfs_file_at(size_t index, char *path, size_t path_capacity,
                       size_t *file_size) {
    if (archive_bytes == NULL || path == NULL || path_capacity == 0
        || file_size == NULL) {
        return false;
    }

    size_t offset = 0;
    size_t current_index = 0;
    const struct ustar_header *header;
    while (offset < archive_length) {
        size_t current_size;
        if (!next_header(&offset, &header, &current_size)) {
            return false;
        }
        if (header == NULL) {
            break;
        }
        if (header->type == '5') {
            continue;
        }
        if (current_index++ != index) {
            continue;
        }
        size_t path_length;
        if (!header_name(header, path, path_capacity, &path_length)) {
            return false;
        }
        *file_size = current_size;
        return true;
    }
    return false;
}

static bool archive_node(const struct ustar_header *header, char path[256],
                         size_t *path_length,
                         enum initramfs_node_type *type) {
    if (!header_name(header, path, 256, path_length)) {
        return false;
    }
    *type = header->type == '5'
        ? INITRAMFS_NODE_DIRECTORY : INITRAMFS_NODE_FILE;
    if (*type == INITRAMFS_NODE_DIRECTORY && *path_length != 0
        && path[*path_length - 1] == '/') {
        path[--*path_length] = '\0';
    }
    return *path_length != 0;
}

static bool path_equal(const char *left, size_t left_length,
                       const char *right, size_t right_length) {
    if (left_length != right_length) {
        return false;
    }
    for (size_t i = 0; i < left_length; i++) {
        if (left[i] != right[i]) {
            return false;
        }
    }
    return true;
}

bool initramfs_stat(const char *path, size_t path_length,
                    struct initramfs_metadata *metadata) {
    if (archive_bytes == NULL || path == NULL || metadata == NULL) {
        return false;
    }
    if (path_length == 0) {
        *metadata = (struct initramfs_metadata) {
            .type = INITRAMFS_NODE_DIRECTORY,
            .inode = 1,
        };
        return true;
    }
    size_t offset = 0;
    size_t ordinal = 0;
    while (offset < archive_length) {
        size_t file_size;
        const struct ustar_header *header;
        if (!next_header(&offset, &header, &file_size) || header == NULL) {
            return false;
        }
        ordinal++;
        char current[256];
        size_t current_length;
        enum initramfs_node_type type;
        if (!archive_node(header, current, &current_length, &type)) {
            return false;
        }
        if (path_equal(path, path_length, current, current_length)) {
            *metadata = (struct initramfs_metadata) {
                .type = type,
                .size = file_size,
                .inode = ordinal + 1,
            };
            return true;
        }
    }
    return false;
}

bool initramfs_read_at(const char *path, size_t path_length, size_t offset,
                       uint8_t *buffer, size_t length, size_t *bytes_read) {
    struct initramfs_metadata metadata;
    if (bytes_read == NULL || (length != 0 && buffer == NULL)
        || !initramfs_stat(path, path_length, &metadata)
        || metadata.type != INITRAMFS_NODE_FILE) {
        return false;
    }
    *bytes_read = 0;
    if (offset >= metadata.size || length == 0) {
        return true;
    }
    struct initramfs_file file;
    char terminated[256];
    if (path_length >= sizeof(terminated)) {
        return false;
    }
    for (size_t i = 0; i < path_length; i++) terminated[i] = path[i];
    terminated[path_length] = '\0';
    if (!initramfs_find_file(terminated, &file)) {
        return false;
    }
    size_t amount = file.size - offset;
    if (amount > length) amount = length;
    for (size_t i = 0; i < amount; i++) buffer[i] = file.data[offset + i];
    *bytes_read = amount;
    return true;
}

static bool directory_child(const char *directory, size_t directory_length,
                            const char *path, size_t path_length,
                            const char **name, size_t *name_length,
                            bool *implied_directory) {
    size_t start = 0;
    if (directory_length != 0) {
        if (path_length <= directory_length
            || !path_equal(directory, directory_length, path,
                           directory_length)
            || path[directory_length] != '/') {
            return false;
        }
        start = directory_length + 1;
    }
    size_t end = start;
    while (end < path_length && path[end] != '/') end++;
    if (end == start) return false;
    *name = path + start;
    *name_length = end - start;
    *implied_directory = end != path_length;
    return true;
}

static bool child_seen_before(const char *directory, size_t directory_length,
                              const char *name, size_t name_length,
                              const struct ustar_header *stop) {
    size_t offset = 0;
    while (offset < archive_length) {
        size_t file_size;
        const struct ustar_header *header;
        if (!next_header(&offset, &header, &file_size) || header == NULL
            || header == stop) {
            return false;
        }
        (void)file_size;
        char current[256];
        size_t current_length;
        enum initramfs_node_type type;
        if (!archive_node(header, current, &current_length, &type)) {
            return false;
        }
        (void)type;
        const char *previous_name;
        size_t previous_name_length;
        bool implied;
        if (directory_child(directory, directory_length, current,
                            current_length, &previous_name,
                            &previous_name_length, &implied)
            && path_equal(name, name_length, previous_name,
                          previous_name_length)) {
            return true;
        }
    }
    return false;
}

static bool directory_entry(const char *path, size_t path_length, size_t index,
                            struct initramfs_directory_entry *entry,
                            size_t *total) {
    size_t offset = 0;
    size_t ordinal = 0;
    size_t visible = 0;
    while (offset < archive_length) {
        size_t file_size;
        const struct ustar_header *header;
        if (!next_header(&offset, &header, &file_size) || header == NULL) break;
        ordinal++;
        char current[256];
        size_t current_length;
        enum initramfs_node_type type;
        if (!archive_node(header, current, &current_length, &type)) return false;
        const char *name;
        size_t name_length;
        bool implied_directory;
        if (!directory_child(path, path_length, current, current_length,
                             &name, &name_length, &implied_directory)) continue;
        if (child_seen_before(path, path_length, name, name_length, header)) {
            continue;
        }
        if (entry != NULL && visible == index) {
            *entry = (struct initramfs_directory_entry) {
                .name_length = name_length,
                .type = implied_directory ? INITRAMFS_NODE_DIRECTORY : type,
                .size = implied_directory ? 0 : file_size,
                .inode = ordinal + 1,
            };
            if (name_length >= sizeof(entry->name)) return false;
            for (size_t i = 0; i < name_length; i++) entry->name[i] = name[i];
            entry->name[name_length] = '\0';
            return true;
        }
        visible++;
    }
    if (total != NULL) *total = visible;
    return entry == NULL;
}

bool initramfs_directory_count(const char *path, size_t path_length,
                               size_t *count) {
    struct initramfs_metadata metadata;
    return count != NULL
        && initramfs_stat(path, path_length, &metadata)
        && metadata.type == INITRAMFS_NODE_DIRECTORY
        && directory_entry(path, path_length, 0, NULL, count);
}

bool initramfs_directory_entry_at(const char *path, size_t path_length,
                                  size_t index,
                                  struct initramfs_directory_entry *entry) {
    struct initramfs_metadata metadata;
    return entry != NULL
        && initramfs_stat(path, path_length, &metadata)
        && metadata.type == INITRAMFS_NODE_DIRECTORY
        && directory_entry(path, path_length, index, entry, NULL);
}
