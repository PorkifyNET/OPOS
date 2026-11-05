#ifndef OPOS_INITRAMFS_H
#define OPOS_INITRAMFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct initramfs_file {
    const uint8_t *data;
    size_t size;
};

enum initramfs_node_type {
    INITRAMFS_NODE_FILE,
    INITRAMFS_NODE_DIRECTORY,
};

struct initramfs_metadata {
    enum initramfs_node_type type;
    size_t size;
    uint64_t inode;
};

struct initramfs_directory_entry {
    char name[256];
    size_t name_length;
    enum initramfs_node_type type;
    size_t size;
    uint64_t inode;
};

bool initramfs_initialize(const uint8_t *archive, size_t archive_size);
bool initramfs_find_file(const char *path, struct initramfs_file *file);
size_t initramfs_file_count(void);
bool initramfs_file_at(size_t index, char *path, size_t path_capacity,
                       size_t *file_size);
bool initramfs_stat(const char *path, size_t path_length,
                    struct initramfs_metadata *metadata);
bool initramfs_read_at(const char *path, size_t path_length, size_t offset,
                       uint8_t *buffer, size_t length, size_t *bytes_read);
bool initramfs_directory_count(const char *path, size_t path_length,
                               size_t *count);
bool initramfs_directory_entry_at(const char *path, size_t path_length,
                                  size_t index,
                                  struct initramfs_directory_entry *entry);

#endif
