#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "initramfs.h"
#include "installer.h"
#include "system_root.h"
#include "update.h"
#include "vfs.h"

static size_t length(const char *text) {
    size_t count = 0;
    while (text[count] != '\0') count++;
    return count;
}

static void require(bool ok, const char *step) {
    if (ok) return;
    console_write_string("UPDATE-ROOT: FAIL ");
    console_write_string(step);
    console_write_string("\n");
    for (;;) __asm__ volatile ("hlt");
}

static bool stat_retry(const char *path, size_t path_length,
                       struct vfs_metadata *metadata) {
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        if (vfs_stat(path, path_length, metadata)) return true;
        for (volatile unsigned i = 0; i < 1000000; i++) {
            __asm__ volatile ("pause");
        }
    }
    return false;
}

void update_root_install(void) {
    const char *error = NULL;
    bool installed = installer_install(0, NULL, &error);
    require(installed, error != NULL ? error : "installer");
    console_write_string("UPDATE-ROOT: base installed\n");
}

static bool root_is_selected(void) {
    char root[128];
    char path[160];
    static const char suffix[] = "/bin/fetch";
    if (!system_root_path(root, sizeof(root))) {
        console_write_string("UPDATE-ROOT: root path unavailable\n");
        return false;
    }
    size_t used = length(root);
    if (used + sizeof(suffix) > sizeof(path)) {
        console_write_string("UPDATE-ROOT: root path too long\n");
        return false;
    }
    for (size_t i = 0; i < used; i++) path[i] = root[i];
    for (size_t i = 0; i < sizeof(suffix); i++) path[used + i] = suffix[i];
    struct vfs_metadata alias, backing;
    if (!stat_retry("/bin/fetch", sizeof(suffix) - 1, &alias)) {
        console_write_string("UPDATE-ROOT: root alias unavailable\n");
        return false;
    }
    if (!stat_retry(path, length(path), &backing)) {
        console_write_string("UPDATE-ROOT: backing file unavailable\n");
        return false;
    }
    if (alias.device != backing.device || alias.inode != backing.inode) {
        console_write_string("UPDATE-ROOT: root inode mismatch\n");
        return false;
    }
    return true;
}

static void check_new_boot(void) {
    static const char menu[] = "/disk/boot/limine/limine.conf";
    static const char previous[] = "/OPOS (previous version)";
    static uint8_t text[8192];
    size_t size, read, old_kernel_size, old_initramfs_size;
    require(root_is_selected(), "new generation root");
    size_t generation_size;
    require(vfs_file_size("/tests/update-generation.txt",
                          sizeof("/tests/update-generation.txt") - 1,
                          &generation_size) && generation_size == 4,
            "new initramfs generation");
    require(vfs_file_size("/disk/boot/old/kernel.elf", 25,
                          &old_kernel_size) && old_kernel_size != 0
        && vfs_file_size("/disk/boot/old/initramfs.tar",
                         sizeof("/disk/boot/old/initramfs.tar") - 1,
                         &old_initramfs_size) && old_initramfs_size != 0,
        "saved previous boot pair");
    require(vfs_file_size(menu, sizeof(menu) - 1, &size)
        && size < sizeof(text)
        && vfs_read_at(menu, sizeof(menu) - 1, 0, text, size, &read)
        && read == size, "read boot menu");
    bool found = false;
    for (size_t i = 0; i + sizeof(previous) - 1 <= size; i++) {
        bool match = true;
        for (size_t j = 0; j < sizeof(previous) - 1; j++) {
            if (text[i + j] != (uint8_t)previous[j]) match = false;
        }
        found |= match;
    }
    require(found, "previous boot menu entry");
    console_write_string("UPDATE-ROOT: new generation and rollback pair PASS\n");
}

static void append(char *out, size_t *used, const char *text) {
    for (size_t i = 0; text[i] != '\0'; i++) out[(*used)++] = text[i];
}

static void update_old_boot(void) {
    static const char marker[] = "/disk/OPOS/UPDATE.TEST";
    size_t marker_size;
    require(root_is_selected(), "old generation root");
    size_t generation_size;
    require(!vfs_file_size("/tests/update-generation.txt",
                           sizeof("/tests/update-generation.txt") - 1,
                           &generation_size), "old initramfs generation");
    if (vfs_file_size(marker, sizeof(marker) - 1, &marker_size)) {
        console_write_string("UPDATE-ROOT: old generation resumed PASS\n");
        return;
    }
    struct initramfs_file next_kernel, next_initramfs;
    require(initramfs_find_file("tests/update-kernel.elf", &next_kernel),
            "new kernel payload");
    require(initramfs_find_file("tests/update-initramfs.tar", &next_initramfs),
            "new initramfs payload");
    static char manifest[256];
    size_t used = 0;
    append(manifest, &used, "OPOS-UPDATE 1\nversion 0.1.4\n");
    const struct initramfs_file *files[] = {&next_initramfs, &next_kernel};
    const char *paths[] = {"initramfs.tar", "kernel.elf"};
    for (size_t file = 0; file < 2; file++) {
        append(manifest, &used, "file ");
        for (size_t i = 0; i < 64; i++) manifest[used++] = '0';
        manifest[used++] = ' ';
        char decimal[24];
        size_t digits = 0;
        for (size_t value = files[file]->size; value != 0; value /= 10) {
            decimal[digits++] = (char)('0' + value % 10);
        }
        require(digits != 0, "payload size");
        while (digits != 0) manifest[used++] = decimal[--digits];
        append(manifest, &used, " kernel boot/");
        append(manifest, &used, paths[file]);
        append(manifest, &used, "\n");
    }
    require(vfs_write_file(marker, sizeof(marker) - 1,
                           (const uint8_t *)"1", 1), "test marker");
    size_t count = 0;
    require(update_begin(true, UPDATE_OPTION_FORCE,
                         (const uint8_t *)manifest, used, NULL, 0,
                         &count) == UPDATE_OK && count == 2,
            "begin update");
    require(update_write(true, 0, 0, next_initramfs.data,
                         next_initramfs.size) == UPDATE_OK,
            "stage new initramfs");
    require(update_write(true, 1, 0, next_kernel.data,
                         next_kernel.size) == UPDATE_OK,
            "stage new kernel");
    console_write_string("UPDATE-ROOT: payload staged\n");
    require(update_commit(true) == UPDATE_OK, "commit update");
    console_write_string("UPDATE-ROOT: commit complete PASS\n");
}

void update_root_step(void) {
    if (update_current_version()[4] == '4') {
        check_new_boot();
    } else {
        update_old_boot();
    }
}
