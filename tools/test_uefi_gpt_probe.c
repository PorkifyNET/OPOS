#include <stdbool.h>
#include <stddef.h>

#include "console.h"
#include "installer.h"
#include "system_root.h"
#include "vfs.h"

static void require(bool ok, const char *step) {
    if (ok) return;
    console_write_string("UEFI-GPT: FAIL ");
    console_write_string(step);
    console_write_string("\n");
    for (;;) __asm__ volatile ("hlt");
}

void uefi_gpt_install_probe(void) {
    struct installer_layout layout;
    const char *error = NULL;
    require(installer_layout_load(0, &layout, &error) && layout.gpt,
            error != NULL ? error : "GPT layout");
    require(layout.slots[0].type == 0xef && layout.slots[0].sectors != 0,
            "EFI partition");
    require(layout.slots[1].type != 0 && layout.slots[1].sectors != 0,
            "OPOS partition");
    require(layout.slots[2].type == 0x07
            && layout.slots[2].sectors == 65536,
            "data partition");
    layout.target = 1;
    require(installer_install_partition(&layout, INSTALLER_BOOT_UEFI_DEFAULT,
                                        NULL, &error),
            error != NULL ? error : "partition install");
    struct installer_layout after;
    require(installer_layout_load(0, &after, &error) && after.gpt,
            error != NULL ? error : "installed GPT layout");
    require(after.slots[2].type == layout.slots[2].type
            && after.slots[2].first == layout.slots[2].first
            && after.slots[2].sectors == layout.slots[2].sectors,
            "data partition entry");
    console_write_string("UEFI-GPT: partition install PASS\n");
}

void uefi_gpt_boot_probe(void) {
    char root[128];
    char generation[64];
    struct vfs_metadata metadata;
    require(system_root_generation(generation, sizeof(generation)),
            "generation");
    require(system_root_path(root, sizeof(root)), "root path");
    size_t n = 0;
    while (root[n] != '\0') n++;
    require(system_root_ready(root, n), "ready generation");
    require(system_root_verify_tree(root, n), "root contents");
    static const char suffix[] = "/bin/fetch";
    char program[160];
    size_t root_length = 0;
    while (root[root_length] != '\0') root_length++;
    require(root_length + sizeof(suffix) <= sizeof(program), "program path");
    for (size_t i = 0; i < root_length; i++) program[i] = root[i];
    for (size_t i = 0; i < sizeof(suffix); i++) {
        program[root_length + i] = suffix[i];
    }
    require(vfs_stat(program, root_length + sizeof(suffix) - 1, &metadata),
            "root program");
    console_write_string("UEFI-GPT: installed root preflight PASS\n");
}
