#ifndef OPOS_SYSTEM_ROOT_H
#define OPOS_SYSTEM_ROOT_H

#include <stdbool.h>
#include <stddef.h>

/* The generation paired with this kernel's full initramfs. */
bool system_root_generation(char *name, size_t capacity);
bool system_root_path(char *path, size_t capacity);
bool system_root_verify_tree(const char *path, size_t path_length);
bool system_root_ready(const char *path, size_t path_length);
bool system_root_write_ready(const char *path, size_t path_length);
bool system_root_select(const char *generation, size_t length);

#endif
