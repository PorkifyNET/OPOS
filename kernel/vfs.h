#ifndef OPOS_VFS_H
#define OPOS_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum vfs_node_type {
    VFS_NODE_FILE,
    VFS_NODE_DIRECTORY,
};

enum vfs_rename_result {
    VFS_RENAME_OK,
    VFS_RENAME_NOT_FOUND,
    VFS_RENAME_NOT_DIRECTORY,
    VFS_RENAME_IS_DIRECTORY,
    VFS_RENAME_NOT_EMPTY,
    VFS_RENAME_INVALID,
    VFS_RENAME_NAME_TOO_LONG,
    VFS_RENAME_READ_ONLY,
    VFS_RENAME_IO_ERROR,
    /* Source and destination are on different volumes. */
    VFS_RENAME_CROSS_VOLUME,
};

struct vfs_metadata {
    uint64_t device;
    enum vfs_node_type type;
    size_t size;
    uint64_t inode;
    bool read_only;
};

struct vfs_directory_entry {
    char name[256];
    size_t name_length;
    enum vfs_node_type type;
    size_t size;
    uint64_t inode;
};

typedef uint32_t vfs_object_handle;

#define VFS_INVALID_OBJECT_HANDLE 0

bool vfs_read_file(const char *path, size_t path_length, uint8_t *buffer,
                   size_t buffer_capacity, size_t *file_size);
bool vfs_path_read_only(const char *path, size_t path_length);
/* True while any open object refers to the FAT volume mounted at /disk. */
bool vfs_disk_objects_open(void);
bool vfs_write_file(const char *path, size_t path_length,
                    const uint8_t *buffer, size_t file_size);
bool vfs_file_size(const char *path, size_t path_length, size_t *file_size);
bool vfs_create_file(const char *path, size_t path_length, bool truncate);
bool vfs_resize_file(const char *path, size_t path_length, uint32_t new_size);
bool vfs_create_directory(const char *path, size_t path_length);
bool vfs_open_object(const char *path, size_t path_length,
                     vfs_object_handle *handle,
                     struct vfs_metadata *metadata);
bool vfs_close_object(vfs_object_handle handle);
bool vfs_object_metadata(vfs_object_handle handle,
                         struct vfs_metadata *metadata);
bool vfs_object_namespace_path(vfs_object_handle handle, char *path,
                               size_t path_capacity, size_t *path_length);
bool vfs_object_resize(vfs_object_handle handle, uint32_t new_size);
bool vfs_object_read_at(vfs_object_handle handle, size_t offset,
                        uint8_t *buffer, size_t length, size_t *bytes_read);
bool vfs_object_write_at(vfs_object_handle handle, size_t offset,
                         const uint8_t *buffer, size_t length,
                         size_t *bytes_written);
bool vfs_remove(const char *path, size_t path_length, bool directory,
                bool *not_empty);
enum vfs_rename_result vfs_rename(const char *old_path,
                                  size_t old_path_length,
                                  const char *new_path,
                                  size_t new_path_length);
bool vfs_read_at(const char *path, size_t path_length, size_t offset,
                 uint8_t *buffer, size_t length, size_t *bytes_read);
bool vfs_write_at(const char *path, size_t path_length, size_t offset,
                  const uint8_t *buffer, size_t length,
                  size_t *bytes_written);
bool vfs_stat(const char *path, size_t path_length,
              struct vfs_metadata *metadata);
bool vfs_directory_count(const char *path, size_t path_length, size_t *count);
bool vfs_directory_entry_at(const char *path, size_t path_length, size_t index,
                            struct vfs_directory_entry *entry);
bool vfs_object_directory_count(vfs_object_handle handle, size_t *count);
bool vfs_object_directory_entry_at(vfs_object_handle handle, size_t index,
                                   struct vfs_directory_entry *entry);

#endif
