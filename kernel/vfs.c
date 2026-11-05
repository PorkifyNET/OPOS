#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat16.h"
#include "initramfs.h"
#include "vfs.h"

struct vfs_resolved_node {
    bool root;
    uint32_t directory_cluster;
    struct fat16_node node;
};

#define VFS_OPEN_OBJECT_CAPACITY 32
#define VFS_INITRAMFS_INODE_BASE 0x100000000ULL

enum vfs_backend {
    VFS_BACKEND_FAT16,
    VFS_BACKEND_INITRAMFS,
};

struct vfs_open_object {
    bool used;
    bool linked;
    bool root;
    uint32_t references;
    uint64_t inode;
    enum vfs_backend backend;
    /* FAT volume the node lives on (0 = /disk, 1 = /disk2, ...). */
    size_t volume;
    struct fat16_node node;
    size_t initramfs_path_length;
    char initramfs_path[256];
    enum vfs_node_type initramfs_type;
    size_t initramfs_size;
    size_t namespace_path_length;
    char namespace_path[256];
};

static struct vfs_open_object open_objects[VFS_OPEN_OBJECT_CAPACITY];

static uint64_t node_inode(const struct fat16_node *node) {
    return ((uint64_t)node->entry_sector << 5)
        + node->entry_offset / 32 + 2;
}

static struct vfs_open_object *open_object(vfs_object_handle handle) {
    if (handle == VFS_INVALID_OBJECT_HANDLE
        || handle > VFS_OPEN_OBJECT_CAPACITY
        || !open_objects[handle - 1].used) {
        return NULL;
    }
    struct vfs_open_object *object = &open_objects[handle - 1];
    /* Make the object's volume current before any fat16_* call on it. */
    if (object->backend == VFS_BACKEND_FAT16 && !fat16_use(object->volume)) {
        return NULL;
    }
    return object;
}

static struct vfs_open_object *open_object_for_node(
    const struct fat16_node *node) {
    if (node == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < VFS_OPEN_OBJECT_CAPACITY; i++) {
        if (open_objects[i].used && open_objects[i].linked
            && open_objects[i].backend == VFS_BACKEND_FAT16
            && open_objects[i].volume == fat16_current_volume()
            && !open_objects[i].root
            && open_objects[i].node.entry_sector == node->entry_sector
            && open_objects[i].node.entry_offset == node->entry_offset) {
            return &open_objects[i];
        }
    }
    return NULL;
}

static bool path_character_equal(char left, char right) {
    if (left >= 'A' && left <= 'Z') {
        left += 'a' - 'A';
    }
    return left == right;
}

static bool namespace_prefix(const char *path, size_t path_length,
                             const char *prefix, size_t prefix_length) {
    if (path_length < prefix_length
        || (path_length != prefix_length && path[prefix_length] != '/')) {
        return false;
    }
    for (size_t i = 0; i < prefix_length; i++) {
        if (!path_character_equal(path[i], prefix[i])) {
            return false;
        }
    }
    return true;
}

static bool set_namespace_path(struct vfs_open_object *object,
                               const char *path, size_t path_length) {
    if (object == NULL || path == NULL
        || path_length >= sizeof(object->namespace_path)) {
        return false;
    }
    object->namespace_path_length = path_length;
    for (size_t i = 0; i < path_length; i++) {
        object->namespace_path[i] = path[i];
    }
    return true;
}

static bool renamed_namespace_paths_fit(const char *old_path,
                                        size_t old_length,
                                        size_t new_length) {
    for (size_t i = 0; i < VFS_OPEN_OBJECT_CAPACITY; i++) {
        struct vfs_open_object *object = &open_objects[i];
        if (object->used && object->linked
            && namespace_prefix(object->namespace_path,
                                object->namespace_path_length,
                                old_path, old_length)
            && new_length + object->namespace_path_length - old_length
                >= sizeof(object->namespace_path)) {
            return false;
        }
    }
    return true;
}

static void rewrite_namespace_paths(const char *old_path, size_t old_length,
                                    const char *new_path,
                                    size_t new_length) {
    for (size_t slot = 0; slot < VFS_OPEN_OBJECT_CAPACITY; slot++) {
        struct vfs_open_object *object = &open_objects[slot];
        if (!object->used || !object->linked
            || !namespace_prefix(object->namespace_path,
                                 object->namespace_path_length,
                                 old_path, old_length)) {
            continue;
        }
        size_t suffix_length = object->namespace_path_length - old_length;
        if (new_length > old_length) {
            for (size_t i = suffix_length; i > 0; i--) {
                object->namespace_path[new_length + i - 1] =
                    object->namespace_path[old_length + i - 1];
            }
        } else if (new_length < old_length) {
            for (size_t i = 0; i < suffix_length; i++) {
                object->namespace_path[new_length + i] =
                    object->namespace_path[old_length + i];
            }
        }
        for (size_t i = 0; i < new_length; i++) {
            object->namespace_path[i] = new_path[i];
        }
        object->namespace_path_length = new_length + suffix_length;
    }
}

/* Length of the volume prefix of a disk path: 5 for "/disk" (volume 0),
   6 for "/disk2".."/disk8" (volumes 1-7), 0 for any other path. */
static size_t disk_prefix_length(const char *path, size_t path_length,
                                 size_t *volume) {
    static const char prefix[] = "/disk";
    size_t length = sizeof(prefix) - 1;
    if (path == NULL || path_length < length) {
        return 0;
    }
    for (size_t i = 0; i < length; i++) {
        if (!path_character_equal(path[i], prefix[i])) {
            return 0;
        }
    }
    size_t index = 0;
    if (path_length > length && path[length] >= '2'
        && path[length] < (char)('1' + FAT16_MAX_VOLUMES)) {
        index = (size_t)(path[length] - '1');
        length++;
    }
    if (path_length != length && path[length] != '/') {
        return 0;
    }
    if (volume != NULL) {
        *volume = index;
    }
    return length;
}

static bool disk_prefix(const char *path, size_t path_length) {
    return disk_prefix_length(path, path_length, NULL) != 0;
}

static bool initramfs_mount_path(const char *path, size_t path_length,
                                 const char **relative,
                                 size_t *relative_length) {
    if (path == NULL || relative == NULL || relative_length == NULL
        || path_length == 0 || path[0] != '/'
        || disk_prefix(path, path_length)) {
        return false;
    }
    *relative = path + 1;
    *relative_length = path_length - 1;
    return true;
}

/* The root folder lists "disk" (system volume) and "disk2".."disk8" (other
   mounted FAT volumes) before the initramfs entries. */
static size_t root_disk_count(void) {
    size_t count = 0;
    struct fat16_volume_info info;
    for (size_t i = 0; i < FAT16_MAX_VOLUMES; i++) {
        if (fat16_volume_info(i, &info)) {
            count++;
        }
    }
    return count;
}

static bool root_disk_entry(size_t index, struct vfs_directory_entry *entry) {
    struct fat16_volume_info info;
    for (size_t i = 0; i < FAT16_MAX_VOLUMES; i++) {
        if (!fat16_volume_info(i, &info) || index-- != 0) {
            continue;
        }
        *entry = (struct vfs_directory_entry) {
            .name = "disk",
            .name_length = 4,
            .type = VFS_NODE_DIRECTORY,
            .inode = 1,
        };
        if (i != 0) {
            entry->name[4] = (char)('1' + i);
            entry->name[5] = '\0';
            entry->name_length = 5;
        }
        return true;
    }
    return false;
}

bool vfs_disk_objects_open(void) {
    for (size_t i = 0; i < VFS_OPEN_OBJECT_CAPACITY; i++) {
        if (open_objects[i].used
            && open_objects[i].backend == VFS_BACKEND_FAT16) {
            return true;
        }
    }
    return false;
}

bool vfs_path_read_only(const char *path, size_t path_length) {
    const char *relative;
    size_t relative_length;
    return initramfs_mount_path(path, path_length, &relative,
                                &relative_length);
}

/* Resolves a disk path on its volume, which becomes the current one. */
static bool vfs_resolve(const char *path, size_t path_length,
                        struct vfs_resolved_node *resolved) {
    size_t volume;
    size_t prefix = disk_prefix_length(path, path_length, &volume);
    if (resolved == NULL || prefix == 0 || !fat16_use(volume)
        || !fat16_mounted()) {
        return false;
    }
    if (path_length == prefix
        || (path_length == prefix + 1 && path[prefix] == '/')) {
        *resolved = (struct vfs_resolved_node) {
            .root = true,
            .directory_cluster = 0,
        };
        return true;
    }
    if (path_length <= prefix + 1 || path[prefix] != '/') {
        return false;
    }

    uint32_t directory_cluster = 0;
    size_t component_start = prefix + 1;
    while (component_start < path_length) {
        size_t component_end = component_start;
        while (component_end < path_length && path[component_end] != '/') {
            component_end++;
        }
        struct fat16_node node;
        if (!fat16_lookup_name(directory_cluster, path + component_start,
                               component_end - component_start, &node)) {
            return false;
        }
        if (component_end == path_length
            || (component_end + 1 == path_length
                && (node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0)) {
            *resolved = (struct vfs_resolved_node) {
                .root = false,
                .directory_cluster = directory_cluster,
                .node = node,
            };
            return true;
        }
        if ((node.attributes & FAT16_ATTRIBUTE_DIRECTORY) == 0
            || node.first_cluster < 2) {
            return false;
        }
        directory_cluster = node.first_cluster;
        component_start = component_end + 1;
    }
    return false;
}

/* Resolves the parent folder of path; *name points at the last component. */
static bool vfs_resolve_parent(const char *path, size_t path_length,
                               uint32_t *parent_cluster, const char **name,
                               size_t *name_length) {
    size_t prefix = disk_prefix_length(path, path_length, NULL);
    if (parent_cluster == NULL || name == NULL || name_length == NULL
        || prefix == 0 || path_length <= prefix + 1) {
        return false;
    }
    size_t slash = path_length;
    while (slash > prefix && path[slash - 1] != '/') {
        slash--;
    }
    if (slash <= prefix || slash == path_length) {
        return false;
    }
    if (!fat16_long_name_valid(path + slash, path_length - slash)) {
        return false;
    }
    *name = path + slash;
    *name_length = path_length - slash;
    size_t parent_length = slash - 1 == prefix ? prefix : slash - 1;
    struct vfs_resolved_node parent;
    if (!vfs_resolve(path, parent_length, &parent)) {
        return false;
    }
    if (parent.root) {
        *parent_cluster = 0;
        return true;
    }
    if ((parent.node.attributes & FAT16_ATTRIBUTE_DIRECTORY) == 0
        || parent.node.first_cluster < 2) {
        return false;
    }
    *parent_cluster = parent.node.first_cluster;
    return true;
}

static bool resolved_file(const char *path, size_t path_length,
                          struct fat16_node *node) {
    struct vfs_resolved_node resolved;
    if (node == NULL || !vfs_resolve(path, path_length, &resolved)
        || resolved.root
        || (resolved.node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    *node = resolved.node;
    return true;
}

static void open_object_metadata(const struct vfs_open_object *object,
                                 struct vfs_metadata *metadata) {
    if (object->backend == VFS_BACKEND_INITRAMFS) {
        *metadata = (struct vfs_metadata) {
            .device = 1,
            .type = object->initramfs_type,
            .size = object->initramfs_size,
            .inode = object->inode,
            .read_only = true,
        };
        return;
    }
    *metadata = (struct vfs_metadata) {
        .device = 2 + object->volume,
        .type = object->root
            || (object->node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0
            ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
        .size = object->root ? 0 : object->node.size,
        .inode = object->inode,
        .read_only = false,
    };
}

bool vfs_open_object(const char *path, size_t path_length,
                     vfs_object_handle *handle,
                     struct vfs_metadata *metadata) {
    if (handle == NULL || metadata == NULL || path == NULL
        || path_length >= sizeof(open_objects[0].namespace_path)) {
        return false;
    }
    const char *init_path;
    size_t init_path_length;
    if (initramfs_mount_path(path, path_length, &init_path,
                             &init_path_length)) {
        struct initramfs_metadata init_metadata;
        if (!initramfs_stat(init_path, init_path_length, &init_metadata)) {
            return false;
        }
        struct vfs_open_object *object = NULL;
        for (size_t i = 0; i < VFS_OPEN_OBJECT_CAPACITY; i++) {
            if (!open_objects[i].used || !open_objects[i].linked
                || open_objects[i].backend != VFS_BACKEND_INITRAMFS
                || open_objects[i].initramfs_path_length != init_path_length) {
                continue;
            }
            bool same = true;
            for (size_t j = 0; j < init_path_length; j++) {
                if (open_objects[i].initramfs_path[j] != init_path[j]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                object = &open_objects[i];
                break;
            }
        }
        if (object == NULL) {
            for (size_t i = 0; i < VFS_OPEN_OBJECT_CAPACITY; i++) {
                if (!open_objects[i].used) {
                    object = &open_objects[i];
                    *object = (struct vfs_open_object) {
                        .used = true,
                        .linked = true,
                        .root = init_path_length == 0,
                        .backend = VFS_BACKEND_INITRAMFS,
                        .inode = VFS_INITRAMFS_INODE_BASE
                            + init_metadata.inode,
                        .initramfs_path_length = init_path_length,
                        .initramfs_type = init_metadata.type
                            == INITRAMFS_NODE_DIRECTORY
                            ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
                        .initramfs_size = init_metadata.size,
                    };
                    for (size_t j = 0; j < init_path_length; j++) {
                        object->initramfs_path[j] = init_path[j];
                    }
                    (void)set_namespace_path(object, path, path_length);
                    break;
                }
            }
        }
        if (object == NULL || object->references == UINT32_MAX) {
            return false;
        }
        object->references++;
        *handle = (vfs_object_handle)(object - open_objects + 1);
        open_object_metadata(object, metadata);
        return true;
    }

    struct vfs_resolved_node resolved;
    if (!vfs_resolve(path, path_length, &resolved)) {
        return false;
    }
    struct vfs_open_object *object = NULL;
    for (size_t i = 0; i < VFS_OPEN_OBJECT_CAPACITY; i++) {
        bool same = open_objects[i].used && open_objects[i].linked
            && open_objects[i].backend == VFS_BACKEND_FAT16
            && open_objects[i].volume == fat16_current_volume()
            && ((resolved.root && open_objects[i].root)
                || (!resolved.root && !open_objects[i].root
                    && open_objects[i].node.entry_sector
                        == resolved.node.entry_sector
                    && open_objects[i].node.entry_offset
                        == resolved.node.entry_offset));
        if (same) {
            object = &open_objects[i];
            break;
        }
    }
    if (object == NULL) {
        for (size_t i = 0; i < VFS_OPEN_OBJECT_CAPACITY; i++) {
            if (!open_objects[i].used) {
                object = &open_objects[i];
                *object = (struct vfs_open_object) {
                    .used = true,
                    .linked = true,
                    .root = resolved.root,
                    .references = 0,
                    .inode = resolved.root ? 1 : node_inode(&resolved.node),
                    .backend = VFS_BACKEND_FAT16,
                    .volume = fat16_current_volume(),
                    .node = resolved.node,
                };
                (void)set_namespace_path(object, path, path_length);
                break;
            }
        }
    }
    if (object == NULL || object->references == UINT32_MAX) {
        return false;
    }
    object->references++;
    *handle = (vfs_object_handle)(object - open_objects + 1);
    open_object_metadata(object, metadata);
    return true;
}

bool vfs_close_object(vfs_object_handle handle) {
    struct vfs_open_object *object = open_object(handle);
    if (object == NULL || object->references == 0) {
        return false;
    }
    if (object->references > 1) {
        object->references--;
        return true;
    }
    if (object->backend == VFS_BACKEND_FAT16
        && !object->linked && !object->root
        && !fat16_release_node(&object->node)) {
        return false;
    }
    *object = (struct vfs_open_object){0};
    return true;
}

bool vfs_object_metadata(vfs_object_handle handle,
                         struct vfs_metadata *metadata) {
    struct vfs_open_object *object = open_object(handle);
    if (object == NULL || metadata == NULL) {
        return false;
    }
    open_object_metadata(object, metadata);
    return true;
}

bool vfs_object_namespace_path(vfs_object_handle handle, char *path,
                               size_t path_capacity, size_t *path_length) {
    struct vfs_open_object *object = open_object(handle);
    if (object == NULL || !object->linked || path == NULL
        || path_length == NULL
        || path_capacity <= object->namespace_path_length) {
        return false;
    }
    for (size_t i = 0; i < object->namespace_path_length; i++) {
        path[i] = object->namespace_path[i];
    }
    path[object->namespace_path_length] = '\0';
    *path_length = object->namespace_path_length;
    return true;
}

bool vfs_object_resize(vfs_object_handle handle, uint32_t new_size) {
    struct vfs_open_object *object = open_object(handle);
    return object != NULL && object->backend == VFS_BACKEND_FAT16
        && !object->root
        && (object->node.attributes & FAT16_ATTRIBUTE_DIRECTORY) == 0
        && (object->linked
            ? fat16_resize_node(&object->node, new_size)
            : fat16_resize_detached_node(&object->node, new_size));
}

bool vfs_object_read_at(vfs_object_handle handle, size_t offset,
                        uint8_t *buffer, size_t length, size_t *bytes_read) {
    struct vfs_open_object *object = open_object(handle);
    if (object == NULL) {
        return false;
    }
    if (object->backend == VFS_BACKEND_INITRAMFS) {
        return object->initramfs_type == VFS_NODE_FILE
            && initramfs_read_at(object->initramfs_path,
                                 object->initramfs_path_length, offset,
                                 buffer, length, bytes_read);
    }
    return !object->root && fat16_read_node_at(
        &object->node, offset, buffer, length, bytes_read);
}

bool vfs_object_write_at(vfs_object_handle handle, size_t offset,
                         const uint8_t *buffer, size_t length,
                         size_t *bytes_written) {
    struct vfs_open_object *object = open_object(handle);
    return object != NULL && object->backend == VFS_BACKEND_FAT16
        && !object->root
        && (object->linked
            ? fat16_write_node_at(&object->node, offset, buffer, length,
                                  bytes_written)
            : fat16_write_detached_node_at(&object->node, offset, buffer,
                                           length, bytes_written));
}

bool vfs_read_file(const char *path, size_t path_length, uint8_t *buffer,
                   size_t buffer_capacity, size_t *file_size) {
    if (file_size == NULL) {
        return false;
    }
    *file_size = 0;
    const char *init_path;
    size_t init_path_length;
    if (initramfs_mount_path(path, path_length, &init_path,
                             &init_path_length)) {
        struct initramfs_metadata metadata;
        size_t bytes_read;
        if (!initramfs_stat(init_path, init_path_length, &metadata)
            || metadata.type != INITRAMFS_NODE_FILE
            || metadata.size > buffer_capacity
            || !initramfs_read_at(init_path, init_path_length, 0, buffer,
                                  metadata.size, &bytes_read)
            || bytes_read != metadata.size) {
            return false;
        }
        *file_size = metadata.size;
        return true;
    }
    struct fat16_node node;
    if (!resolved_file(path, path_length, &node)) {
        return false;
    }
    struct vfs_open_object *object = open_object_for_node(&node);
    const struct fat16_node *active = object != NULL ? &object->node : &node;
    *file_size = active->size;
    if (active->size > buffer_capacity) {
        return false;
    }
    size_t bytes_read;
    return fat16_read_node_at(active, 0, buffer, active->size, &bytes_read)
        && bytes_read == active->size;
}

bool vfs_write_file(const char *path, size_t path_length,
                    const uint8_t *buffer, size_t file_size) {
    if (!vfs_create_file(path, path_length, true)) {
        return false;
    }
    struct fat16_node node;
    size_t bytes_written;
    if (!resolved_file(path, path_length, &node)) {
        return false;
    }
    struct vfs_open_object *object = open_object_for_node(&node);
    struct fat16_node *active = object != NULL ? &object->node : &node;
    return fat16_write_node_at(active, 0, buffer, file_size, &bytes_written)
        && bytes_written == file_size;
}

bool vfs_file_size(const char *path, size_t path_length, size_t *file_size) {
    const char *init_path;
    size_t init_path_length;
    if (file_size != NULL
        && initramfs_mount_path(path, path_length, &init_path,
                                &init_path_length)) {
        struct initramfs_metadata metadata;
        if (!initramfs_stat(init_path, init_path_length, &metadata)
            || metadata.type != INITRAMFS_NODE_FILE) {
            return false;
        }
        *file_size = metadata.size;
        return true;
    }
    struct fat16_node node;
    if (file_size == NULL || !resolved_file(path, path_length, &node)) {
        return false;
    }
    struct vfs_open_object *object = open_object_for_node(&node);
    *file_size = object != NULL ? object->node.size : node.size;
    return true;
}

bool vfs_create_file(const char *path, size_t path_length, bool truncate) {
    struct vfs_resolved_node resolved;
    if (vfs_resolve(path, path_length, &resolved)) {
        if (resolved.root
            || (resolved.node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0) {
            return false;
        }
        struct vfs_open_object *object = open_object_for_node(&resolved.node);
        return !truncate || (object != NULL
            ? vfs_object_resize((vfs_object_handle)(object - open_objects + 1), 0)
            : fat16_truncate_node(&resolved.node));
    }
    uint32_t parent_cluster;
    const char *name;
    size_t name_length;
    struct fat16_node node;
    return vfs_resolve_parent(path, path_length, &parent_cluster, &name,
                              &name_length)
        && fat16_create_named(parent_cluster, name, name_length, false, &node);
}

bool vfs_resize_file(const char *path, size_t path_length, uint32_t new_size) {
    struct fat16_node node;
    if (!resolved_file(path, path_length, &node)) {
        return false;
    }
    struct vfs_open_object *object = open_object_for_node(&node);
    return object != NULL
        ? vfs_object_resize((vfs_object_handle)(object - open_objects + 1),
                            new_size)
        : fat16_resize_node(&node, new_size);
}

bool vfs_create_directory(const char *path, size_t path_length) {
    struct vfs_resolved_node existing;
    if (vfs_resolve(path, path_length, &existing)) {
        return false;
    }
    uint32_t parent_cluster;
    const char *name;
    size_t name_length;
    struct fat16_node node;
    return vfs_resolve_parent(path, path_length, &parent_cluster, &name,
                              &name_length)
        && fat16_create_named(parent_cluster, name, name_length, true, &node);
}

bool vfs_remove(const char *path, size_t path_length, bool directory,
                bool *not_empty) {
    if (not_empty == NULL) {
        return false;
    }
    *not_empty = false;
    struct vfs_resolved_node resolved;
    if (!vfs_resolve(path, path_length, &resolved) || resolved.root) {
        return false;
    }
    bool node_is_directory =
        (resolved.node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0;
    if (node_is_directory != directory) {
        return false;
    }
    if (directory) {
        bool empty;
        if (resolved.node.first_cluster < 2
            || !fat16_directory_is_empty(resolved.node.first_cluster, &empty)) {
            return false;
        }
        if (!empty) {
            *not_empty = true;
            return false;
        }
    }
    struct vfs_open_object *object = open_object_for_node(&resolved.node);
    if (object == NULL) {
        return fat16_delete_node(&resolved.node);
    }
    if (!fat16_unlink_node(&object->node)) {
        return false;
    }
    object->linked = false;
    object->namespace_path_length = 0;
    return true;
}

/* True if the last component of path is exactly name (case-sensitive). */
static bool last_component_equal(const char *path, size_t path_length,
                                 const char *name, size_t name_length) {
    size_t start = path_length;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    if (path_length - start != name_length) {
        return false;
    }
    for (size_t i = 0; i < name_length; i++) {
        if (path[start + i] != name[i]) {
            return false;
        }
    }
    return true;
}

enum vfs_rename_result vfs_rename(const char *old_path,
                                  size_t old_path_length,
                                  const char *new_path,
                                  size_t new_path_length) {
    if (vfs_path_read_only(old_path, old_path_length)
        || vfs_path_read_only(new_path, new_path_length)) {
        return VFS_RENAME_READ_ONLY;
    }
    size_t old_volume = 0;
    size_t new_volume = 0;
    (void)disk_prefix_length(old_path, old_path_length, &old_volume);
    (void)disk_prefix_length(new_path, new_path_length, &new_volume);
    if (old_volume != new_volume) {
        return VFS_RENAME_CROSS_VOLUME;
    }
    struct vfs_resolved_node source;
    if (!vfs_resolve(old_path, old_path_length, &source) || source.root) {
        return VFS_RENAME_NOT_FOUND;
    }

    uint32_t destination_parent;
    const char *destination_name;
    size_t destination_name_length;
    if (!vfs_resolve_parent(new_path, new_path_length, &destination_parent,
                            &destination_name, &destination_name_length)) {
        size_t start = new_path_length;
        while (start > 0 && new_path[start - 1] != '/') {
            start--;
        }
        return fat16_long_name_valid(new_path + start, new_path_length - start)
            ? VFS_RENAME_NOT_FOUND : VFS_RENAME_INVALID;
    }

    bool source_directory =
        (source.node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0;
    if (source_directory) {
        bool descendant;
        if (!fat16_directory_is_descendant(destination_parent,
                                            source.node.first_cluster,
                                            &descendant)) {
            return VFS_RENAME_IO_ERROR;
        }
        if (descendant) {
            return VFS_RENAME_INVALID;
        }
    }

    struct fat16_node replacement;
    struct fat16_node *replacement_pointer = NULL;
    bool same_entry = false;
    bool found = fat16_lookup_name(destination_parent, destination_name,
                                   destination_name_length, &replacement);
    if (found && source.node.entry_sector == replacement.entry_sector
        && source.node.entry_offset == replacement.entry_offset) {
        /* Same file, e.g. a change of letter case only. */
        same_entry = true;
    } else if (found) {
        replacement_pointer = &replacement;
        bool replacement_directory =
            (replacement.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0;
        if (source_directory && !replacement_directory) {
            return VFS_RENAME_NOT_DIRECTORY;
        }
        if (!source_directory && replacement_directory) {
            return VFS_RENAME_IS_DIRECTORY;
        }
        if (replacement_directory) {
            bool empty;
            if (!fat16_directory_is_empty(replacement.first_cluster, &empty)) {
                return VFS_RENAME_IO_ERROR;
            }
            if (!empty) {
                return VFS_RENAME_NOT_EMPTY;
            }
        }
    }

    struct vfs_open_object *source_object =
        open_object_for_node(&source.node);
    struct vfs_open_object *replacement_object = replacement_pointer != NULL
        ? open_object_for_node(replacement_pointer) : NULL;
    if (!renamed_namespace_paths_fit(old_path, old_path_length,
                                     new_path_length)) {
        return VFS_RENAME_NAME_TOO_LONG;
    }
    if (same_entry && last_component_equal(old_path, old_path_length,
                                destination_name, destination_name_length)) {
        return VFS_RENAME_OK; /* same name, nothing to do */
    }
    if (!fat16_rename_named(&source.node, source.directory_cluster,
                            destination_parent, destination_name,
                            destination_name_length, replacement_pointer)) {
        return VFS_RENAME_IO_ERROR;
    }
    if (source_object != NULL) {
        source_object->node = source.node;
    }
    if (replacement_object != NULL) {
        replacement_object->linked = false;
        replacement_object->namespace_path_length = 0;
    }
    if (replacement_pointer != NULL && replacement_object == NULL) {
        (void)fat16_release_node(replacement_pointer);
    }
    rewrite_namespace_paths(old_path, old_path_length,
                            new_path, new_path_length);
    if (source_object != NULL) {
        (void)set_namespace_path(source_object, new_path, new_path_length);
    }
    return VFS_RENAME_OK;
}

bool vfs_read_at(const char *path, size_t path_length, size_t offset,
                 uint8_t *buffer, size_t length, size_t *bytes_read) {
    const char *init_path;
    size_t init_path_length;
    if (initramfs_mount_path(path, path_length, &init_path,
                             &init_path_length)) {
        return initramfs_read_at(init_path, init_path_length, offset, buffer,
                                 length, bytes_read);
    }
    struct fat16_node node;
    if (!resolved_file(path, path_length, &node)) {
        return false;
    }
    struct vfs_open_object *object = open_object_for_node(&node);
    return fat16_read_node_at(object != NULL ? &object->node : &node,
                              offset, buffer, length, bytes_read);
}

bool vfs_write_at(const char *path, size_t path_length, size_t offset,
                  const uint8_t *buffer, size_t length,
                  size_t *bytes_written) {
    struct fat16_node node;
    if (!resolved_file(path, path_length, &node)) {
        return false;
    }
    struct vfs_open_object *object = open_object_for_node(&node);
    return fat16_write_node_at(object != NULL ? &object->node : &node,
                               offset, buffer, length, bytes_written);
}

bool vfs_stat(const char *path, size_t path_length,
              struct vfs_metadata *metadata) {
    const char *init_path;
    size_t init_path_length;
    if (metadata != NULL
        && initramfs_mount_path(path, path_length, &init_path,
                                &init_path_length)) {
        struct initramfs_metadata init_metadata;
        if (!initramfs_stat(init_path, init_path_length, &init_metadata)) {
            return false;
        }
        *metadata = (struct vfs_metadata) {
            .device = 1,
            .type = init_metadata.type == INITRAMFS_NODE_DIRECTORY
                ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
            .size = init_metadata.size,
            .inode = VFS_INITRAMFS_INODE_BASE + init_metadata.inode,
            .read_only = true,
        };
        return true;
    }
    struct vfs_resolved_node resolved;
    if (metadata == NULL || !vfs_resolve(path, path_length, &resolved)) {
        return false;
    }
    if (resolved.root) {
        *metadata = (struct vfs_metadata) {
            .device = 2 + fat16_current_volume(),
            .type = VFS_NODE_DIRECTORY,
            .size = 0,
            .inode = 1,
            .read_only = false,
        };
    } else {
        struct vfs_open_object *object = open_object_for_node(&resolved.node);
        if (object != NULL) {
            open_object_metadata(object, metadata);
            return true;
        }
        *metadata = (struct vfs_metadata) {
            .device = 2 + fat16_current_volume(),
            .type = (resolved.node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0
                ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
            .size = resolved.node.size,
            .inode = node_inode(&resolved.node),
            .read_only = false,
        };
    }
    return true;
}

static bool visible_node(const struct fat16_node *node) {
    return node->name[0] != 0 && node->name[0] != 0xe5
        && node->name[0] != '.'
        && node->attributes != FAT16_ATTRIBUTE_LONG_NAME
        && (node->attributes & FAT16_ATTRIBUTE_VOLUME_ID) == 0;
}

static bool resolved_directory(const char *path, size_t path_length,
                               uint32_t *directory_cluster) {
    struct vfs_resolved_node resolved;
    if (directory_cluster == NULL
        || !vfs_resolve(path, path_length, &resolved)) {
        return false;
    }
    if (resolved.root) {
        *directory_cluster = 0;
        return true;
    }
    if ((resolved.node.attributes & FAT16_ATTRIBUTE_DIRECTORY) == 0
        || resolved.node.first_cluster < 2) {
        return false;
    }
    *directory_cluster = resolved.node.first_cluster;
    return true;
}

static bool directory_count(uint32_t directory_cluster, size_t *count) {
    if (count == NULL) {
        return false;
    }
    size_t visible = 0;
    size_t cursor = 0;
    struct fat16_node node;
    char name[FAT16_LONG_NAME_MAX + 1];
    size_t name_length;
    while (fat16_directory_next(directory_cluster, &cursor, &node, name,
                                sizeof(name), &name_length)) {
        if (visible_node(&node)) {
            visible++;
        }
    }
    *count = visible;
    return true;
}

bool vfs_directory_count(const char *path, size_t path_length, size_t *count) {
    const char *init_path;
    size_t init_path_length;
    if (count != NULL
        && initramfs_mount_path(path, path_length, &init_path,
                                &init_path_length)) {
        if (!initramfs_directory_count(init_path, init_path_length, count)) {
            return false;
        }
        if (init_path_length == 0) *count += root_disk_count();
        return true;
    }
    uint32_t directory_cluster;
    return resolved_directory(path, path_length, &directory_cluster)
        && directory_count(directory_cluster, count);
}

bool vfs_object_directory_count(vfs_object_handle handle, size_t *count) {
    struct vfs_open_object *object = open_object(handle);
    if (object != NULL && object->backend == VFS_BACKEND_INITRAMFS) {
        if (object->initramfs_type != VFS_NODE_DIRECTORY
            || !initramfs_directory_count(object->initramfs_path,
                                           object->initramfs_path_length,
                                           count)) {
            return false;
        }
        if (object->initramfs_path_length == 0) {
            *count += root_disk_count();
        }
        return true;
    }
    if (object == NULL
        || (!object->root
            && (object->node.attributes & FAT16_ATTRIBUTE_DIRECTORY) == 0)) {
        return false;
    }
    return directory_count(object->root ? 0 : object->node.first_cluster,
                           count);
}

static bool directory_entry_at(uint32_t directory_cluster, size_t index,
                               struct vfs_directory_entry *entry) {
    if (entry == NULL) {
        return false;
    }
    size_t visible = 0;
    size_t cursor = 0;
    struct fat16_node node;
    char name[FAT16_LONG_NAME_MAX + 1];
    size_t name_length;
    while (fat16_directory_next(directory_cluster, &cursor, &node, name,
                                sizeof(name), &name_length)) {
        if (!visible_node(&node) || visible++ != index) {
            continue;
        }
        *entry = (struct vfs_directory_entry) {
            .type = (node.attributes & FAT16_ATTRIBUTE_DIRECTORY) != 0
                ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
            .size = node.size,
            .inode = node_inode(&node),
        };
        for (size_t i = 0; i <= name_length && i < sizeof(entry->name); i++) {
            entry->name[i] = name[i];
        }
        entry->name_length = name_length;
        return true;
    }
    return false;
}

bool vfs_directory_entry_at(const char *path, size_t path_length, size_t index,
                            struct vfs_directory_entry *entry) {
    const char *init_path;
    size_t init_path_length;
    if (entry != NULL
        && initramfs_mount_path(path, path_length, &init_path,
                                &init_path_length)) {
        size_t disks = init_path_length == 0 ? root_disk_count() : 0;
        if (index < disks) {
            return root_disk_entry(index, entry);
        }
        struct initramfs_directory_entry init_entry;
        size_t init_index = index - disks;
        if (!initramfs_directory_entry_at(init_path, init_path_length,
                                          init_index, &init_entry)) {
            return false;
        }
        *entry = (struct vfs_directory_entry) {
            .name_length = init_entry.name_length,
            .type = init_entry.type == INITRAMFS_NODE_DIRECTORY
                ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
            .size = init_entry.size,
            .inode = VFS_INITRAMFS_INODE_BASE + init_entry.inode,
        };
        for (size_t i = 0; i <= init_entry.name_length; i++) {
            entry->name[i] = init_entry.name[i];
        }
        return true;
    }
    uint32_t directory_cluster;
    return resolved_directory(path, path_length, &directory_cluster)
        && directory_entry_at(directory_cluster, index, entry);
}

bool vfs_object_directory_entry_at(vfs_object_handle handle, size_t index,
                                   struct vfs_directory_entry *entry) {
    struct vfs_open_object *object = open_object(handle);
    if (object != NULL && object->backend == VFS_BACKEND_INITRAMFS) {
        if (object->initramfs_type != VFS_NODE_DIRECTORY) {
            return false;
        }
        size_t disks = object->initramfs_path_length == 0
            ? root_disk_count() : 0;
        if (index < disks) {
            return root_disk_entry(index, entry);
        }
        struct initramfs_directory_entry init_entry;
        size_t init_index = index - disks;
        if (!initramfs_directory_entry_at(object->initramfs_path,
                                          object->initramfs_path_length,
                                          init_index, &init_entry)) {
            return false;
        }
        *entry = (struct vfs_directory_entry) {
            .name_length = init_entry.name_length,
            .type = init_entry.type == INITRAMFS_NODE_DIRECTORY
                ? VFS_NODE_DIRECTORY : VFS_NODE_FILE,
            .size = init_entry.size,
            .inode = VFS_INITRAMFS_INODE_BASE + init_entry.inode,
        };
        for (size_t i = 0; i <= init_entry.name_length; i++) {
            entry->name[i] = init_entry.name[i];
        }
        return true;
    }
    if (object == NULL
        || (!object->root
            && (object->node.attributes & FAT16_ATTRIBUTE_DIRECTORY) == 0)) {
        return false;
    }
    return directory_entry_at(object->root ? 0 : object->node.first_cluster,
                              index, entry);
}
