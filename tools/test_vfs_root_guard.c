#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* Exercise the real VFS guard with a small FAT directory fixture. */
#include "../kernel/vfs.c"

bool fat16_use(size_t volume) { return volume == 0; }
bool fat16_mounted(void) { return true; }
size_t fat16_current_volume(void) { return 0; }
uint32_t fat16_volume_generation(size_t volume) { return volume == 0 ? 1 : 0; }
bool iso_resolve(size_t volume, const char *path, size_t length,
                 struct iso_node *node) {
    (void)volume; (void)path; (void)length; (void)node;
    return false;
}
bool initramfs_stat(const char *path, size_t length,
                    struct initramfs_metadata *metadata) {
    (void)path; (void)length; (void)metadata;
    return false;
}

bool fat16_lookup_name(uint32_t parent, const char *name, size_t length,
                       struct fat16_node *node) {
    static const struct {
        uint32_t parent;
        const char *name;
        const char *alias;
        uint32_t cluster;
        uint32_t sector;
        bool directory;
    } entries[] = {
        {0, "OPOS", NULL, 10, 10, true},
        {10, "roots", NULL, 20, 20, true},
        {20, "generation-2026", "GENERA~1", 30, 30, true},
        {30, "bin", NULL, 40, 40, true},
        {40, "notepad", NULL, 0, 50, false},
        {0, "Programs", NULL, 60, 60, true},
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (entries[i].parent != parent) continue;
        size_t long_length = strlen(entries[i].name);
        size_t short_length = entries[i].alias ? strlen(entries[i].alias) : 0;
        if (!((length == long_length
               && strncasecmp(name, entries[i].name, length) == 0)
              || (length == short_length && entries[i].alias
                  && strncasecmp(name, entries[i].alias, length) == 0))) {
            continue;
        }
        *node = (struct fat16_node){
            .attributes = entries[i].directory ? FAT16_ATTRIBUTE_DIRECTORY : 0,
            .first_cluster = entries[i].cluster,
            .entry_sector = entries[i].sector,
        };
        return true;
    }
    return false;
}

int main(void) {
    static const char root[] = "/disk/OPOS/roots/generation-2026";
#define CONTAINS(p) vfs_disk_root_contains(p, sizeof(p) - 1)
#define ANCESTOR(p) vfs_disk_root_ancestor(p, sizeof(p) - 1)
    assert(!CONTAINS("/disk/OPOS/roots/GENERA~1/new-file"));
    assert(!ANCESTOR("/disk/OPOS/roots"));
    memcpy(disk_root_path, root, sizeof(root));
    disk_root_path_length = sizeof(root) - 1;

    assert(CONTAINS("/bin/notepad"));
    assert(CONTAINS("/disk/OPOS/roots/generation-2026/bin/notepad"));
    assert(CONTAINS("/disk/OPOS/roots/GENERA~1"));
    assert(CONTAINS("/disk/opos/ROOTS/GENERA~1/bin/notepad"));
    assert(CONTAINS("/disk/OPOS/roots/GENERA~1/new-file"));
    assert(!CONTAINS("/disk/Programs/new-file"));
    assert(!CONTAINS("/disk2/OPOS/roots/GENERA~1/new-file"));
    assert(!CONTAINS("/disk/OPOS/roots/generation-2027/new-file"));
    assert(ANCESTOR("/disk/OPOS/roots/GENERA~1"));
    assert(ANCESTOR("/disk/OPOS/roots"));
    assert(!ANCESTOR("/disk/Programs"));
    assert(!ANCESTOR("/disk2/OPOS/roots/GENERA~1"));
    puts("VFS disk-root guard: passed");
    return 0;
}
