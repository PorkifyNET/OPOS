#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "initramfs.h"
#include "installer.h"
#include "permissions.h"
#include "system_root.h"
#include "user.h"
#include "vfs.h"

static const char alias[] = "/disk/OPOS/roots/GENERA~1";
static char root[128];

static size_t length(const char *text) {
    size_t size = 0;
    while (text[size] != '\0') size++;
    return size;
}

static void require(bool condition, const char *step) {
    if (condition) return;
    console_write_string("ROOT-SMOKE: FAIL ");
    console_write_string(step);
    console_write_string("\n");
    for (;;) __asm__ volatile ("hlt");
}

void disk_root_smoke_unstage(void) {
    char path[160];
    require(system_root_path(root, sizeof(root)), "unstage root path");
    size_t root_length = length(root);
    static const char marker[] = ".ready";
    require(root_length + sizeof(marker) <= sizeof(path), "marker path");
    for (size_t i = 0; i < root_length; i++) path[i] = root[i];
    for (size_t i = 0; i < sizeof(marker); i++) {
        path[root_length + i] = marker[i];
    }
    bool not_empty = false;
    require(vfs_remove(path, length(path), false, &not_empty),
            "remove ready marker for first-boot stage");
    console_write_string("ROOT-SMOKE: ready marker removed before boot\n");
}

void disk_root_smoke_prepare(void) {
    console_write_string("ROOT-SMOKE: installing onto blank disk\n");
    const char *error = NULL;
    bool installed = installer_install(0, NULL, &error);
    require(installed, error != NULL ? error : "installer");
    console_write_string("ROOT-SMOKE: installer completed\n");
    require(system_root_path(root, sizeof(root)), "generation path");
    size_t root_length = length(root);
    require(system_root_ready(root, root_length), "ready marker");
    require(system_root_verify_tree(root, root_length), "verify tree");
    char generation[64];
    require(system_root_generation(generation, sizeof(generation)),
            "generation name");
    char fetch_path[160];
    static const char fetch_suffix[] = "/bin/fetch";
    require(root_length + sizeof(fetch_suffix) <= sizeof(fetch_path),
            "fetch path length");
    for (size_t i = 0; i < root_length; i++) fetch_path[i] = root[i];
    for (size_t i = 0; i < sizeof(fetch_suffix); i++) {
        fetch_path[root_length + i] = fetch_suffix[i];
    }
    static const uint8_t damaged[] = {0};
    require(vfs_write_file(fetch_path, length(fetch_path), damaged,
                           sizeof(damaged))
        && !system_root_select(generation, length(generation)),
        "reject damaged generation");
    struct initramfs_file fetch;
    require(initramfs_find_file("bin/fetch", &fetch)
        && vfs_write_file(fetch_path, length(fetch_path),
                          fetch.data, fetch.size),
        "restore generation");
    console_write_string("ROOT-SMOKE: damaged generation rejected PASS\n");
    char rogue_path[160];
    static const char rogue_suffix[] = "/bin/rogue";
    require(root_length + sizeof(rogue_suffix) <= sizeof(rogue_path),
            "rogue path length");
    for (size_t i = 0; i < root_length; i++) rogue_path[i] = root[i];
    for (size_t i = 0; i < sizeof(rogue_suffix); i++) {
        rogue_path[root_length + i] = rogue_suffix[i];
    }
    require(vfs_write_file(rogue_path, length(rogue_path), damaged,
                           sizeof(damaged)), "write extra system program");
    require(!system_root_select(generation, length(generation)),
            "reject extra system program");
    bool not_empty;
    require(vfs_remove(rogue_path, length(rogue_path), false, &not_empty),
            "remove extra system program");
    console_write_string("ROOT-SMOKE: extra program rejected PASS\n");
    require(system_root_select(generation, length(generation)),
        "verified root selection");

    static const char visible[] = "/bin/notepad";
    char physical[160];
    static const char suffix[] = "/bin/notepad";
    require(root_length + sizeof(suffix) <= sizeof(physical),
            "physical path length");
    for (size_t i = 0; i < root_length; i++) physical[i] = root[i];
    for (size_t i = 0; i < sizeof(suffix); i++) {
        physical[root_length + i] = suffix[i];
    }
    static const char short_path[] =
        "/disk/OPOS/roots/GENERA~1/bin/notepad";
    static const char new_path[] =
        "/disk/OPOS/roots/GENERA~1/bin/new-file";
    struct vfs_metadata a, b, c;
    require(vfs_stat(visible, sizeof(visible) - 1, &a)
        && vfs_stat(physical, length(physical), &b)
        && vfs_stat(short_path, sizeof(short_path) - 1, &c)
        && a.device == b.device && b.device == c.device
        && a.inode == b.inode && b.inode == c.inode,
        "visible, physical and 8.3 identity");
    require(vfs_path_read_only(visible, sizeof(visible) - 1),
            "visible root read-only");
    require(vfs_disk_root_contains(short_path, sizeof(short_path) - 1)
        && vfs_disk_root_contains(new_path, sizeof(new_path) - 1)
        && vfs_disk_root_ancestor(alias, sizeof(alias) - 1),
        "FAT alias root guard");

    struct permission_identity admin = permission_session_identity();
    struct permission_identity standard = {
        .account = PERMISSION_NO_ACCOUNT, .admin = false
    };
    require(permission_program_allows(&admin, visible,
                                      sizeof(visible) - 1, PERMISSION_READ)
        && !permission_program_allows(&admin, visible,
                                      sizeof(visible) - 1, PERMISSION_WRITE)
        && !permission_program_allows(&standard, short_path,
                                      sizeof(short_path) - 1, PERMISSION_WRITE)
        && !permission_ui_allows(short_path, sizeof(short_path) - 1,
                                 PERMISSION_WRITE)
        && !permission_ui_allows(new_path, sizeof(new_path) - 1,
                                 PERMISSION_WRITE)
        && !permission_ui_allows(alias, sizeof(alias) - 1,
                                 PERMISSION_REMOVE),
        "selected tree permissions");
    console_write_string("ROOT-SMOKE: namespace and permissions PASS\n");
}

void disk_root_smoke_launch(void) {
    static const char program[] = "/bin/fetch";
    uint64_t process_id, exit_status;
    enum user_run_result result = user_process_run_file(
        program, sizeof(program) - 1, "", 0, NULL,
        &process_id, &exit_status);
    console_write_string("ROOT-SMOKE: launch result ");
    console_write_hex((uint64_t)result);
    console_write_string(" status ");
    console_write_hex(exit_status);
    console_write_string("\n");
    require(result == USER_RUN_STARTED && exit_status == 2,
            "disk-root fetch launch");
    console_write_string("ROOT-SMOKE: program launch PASS\n");
}
