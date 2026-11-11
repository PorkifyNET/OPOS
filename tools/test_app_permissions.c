#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "accounts.h"
#include "permissions.h"
#include "vfs.h"

static char directories[32][128];
static size_t directory_count;
static uint64_t document_inode = 7;
static bool disk_root_selected;
static const char root_backing[] = "/disk/OPOS/roots/test";

bool vfs_disk_root_backing_path(const char *path, size_t length, char *backing,
                                size_t capacity, size_t *backing_length) {
    if (!disk_root_selected || length == 0 || path[0] != '/'
        || strncmp(path, "/disk", 5) == 0
        || strncmp(path, "/cdrom", 6) == 0) {
        return false;
    }
    size_t prefix = strlen(root_backing);
    size_t suffix = length == 1 ? 0 : length;
    if (prefix + suffix >= capacity) {
        return false;
    }
    memcpy(backing, root_backing, prefix);
    memcpy(backing + prefix, path, suffix);
    backing[prefix + suffix] = '\0';
    *backing_length = prefix + suffix;
    return true;
}

bool vfs_disk_root_contains(const char *path, size_t length) {
    return disk_root_selected
        && ((length > 0 && path[0] == '/' && strncmp(path, "/disk", 5) != 0)
            || (length >= strlen(root_backing)
                && strncmp(path, root_backing, strlen(root_backing)) == 0));
}

bool vfs_disk_root_ancestor(const char *path, size_t length) {
    return disk_root_selected && length <= strlen(root_backing)
        && strncmp(path, root_backing, length) == 0;
}

size_t accounts_count(void) { return 2; }
size_t accounts_current(void) { return 0; }
bool accounts_is_admin(size_t account) { return account == 0; }
const char *accounts_name(size_t account) {
    return account == 0 ? "Alice" : "Bob";
}

bool vfs_stat(const char *path, size_t length, struct vfs_metadata *metadata) {
    if ((length == strlen("/disk/Users/Alice/note.txt")
         && strncmp(path, "/disk/Users/Alice/note.txt", length) == 0)
        || (length == strlen("/disk/Users/Alice/other.txt")
            && strncmp(path, "/disk/Users/Alice/other.txt", length) == 0)
        || (length == strlen("/disk/picked.txt")
            && strncmp(path, "/disk/picked.txt", length) == 0)) {
        *metadata = (struct vfs_metadata){
            .type = VFS_NODE_FILE, .device = 1,
            .inode = document_inode
        };
        return true;
    }
    for (size_t i = 0; i < directory_count; i++) {
        if (strlen(directories[i]) == length
            && strncmp(path, directories[i], length) == 0) {
            *metadata = (struct vfs_metadata){.type = VFS_NODE_DIRECTORY};
            return true;
        }
    }
    return false;
}

bool vfs_create_directory(const char *path, size_t length) {
    assert(directory_count < 32 && length < sizeof(directories[0]));
    memcpy(directories[directory_count], path, length);
    directories[directory_count++][length] = '\0';
    return true;
}

static bool allows(const struct permission_identity *identity,
                   const char *path, enum permission_access access) {
    return permission_program_allows(identity, path, strlen(path), access);
}

int main(void) {
    struct permission_identity built_in = permission_session_identity();
    assert(allows(&built_in, "/disk/shared.txt", PERMISSION_WRITE));
    assert(allows(&built_in, "/disk/Users/Alice/note.txt", PERMISSION_WRITE));
    assert(!allows(&built_in, "/disk/Users/Bob/note.txt", PERMISSION_READ));

    struct permission_identity app = built_in;
    permission_confine_disk_program(&app, "/disk/Programs/editor", 21);
    assert(app.confined && app.private_path[0]);
    char file[160];
    snprintf(file, sizeof(file), "%s/settings.dat", app.private_path);
    assert(allows(&app, file, PERMISSION_READ));
    assert(allows(&app, file, PERMISSION_WRITE));
    assert(allows(&app, file, PERMISSION_REMOVE));
    assert(!allows(&app, app.private_path, PERMISSION_REMOVE));
    assert(!allows(&app, "/disk/shared.txt", PERMISSION_WRITE));
    assert(!allows(&app, "/disk2/shared.txt", PERMISSION_WRITE));
    assert(!allows(&app, "/disk/Users/Alice/note.txt", PERMISSION_WRITE));
    assert(!allows(&app, "/disk/Users/Alice/note.txt", PERMISSION_READ));
    assert(permission_grant_read_file(&app, "/disk/Users/Alice/note.txt",
                                      strlen("/disk/Users/Alice/note.txt")));
    assert(allows(&app, "/disk/Users/Alice/note.txt", PERMISSION_READ));
    document_inode++;
    assert(!allows(&app, "/disk/Users/Alice/note.txt", PERMISSION_READ));
    assert(permission_grant_read_file(&app, "/disk/Users/Alice/other.txt",
                                      strlen("/disk/Users/Alice/other.txt")));
    assert(!allows(&app, "/disk/Users/Alice/note.txt", PERMISSION_READ));
    assert(allows(&app, "/disk/Users/Alice/other.txt", PERMISSION_READ));
    assert(!permission_grant_read_file(&app, "/disk/Users/Bob/note.txt",
                                       strlen("/disk/Users/Bob/note.txt")));
    assert(!allows(&app, "/disk/Users/Bob/note.txt", PERMISSION_READ));
    assert(!allows(&app, "/disk/OPOS/ACCOUNTS.DAT", PERMISSION_READ));

    struct permission_identity child = app;
    assert(allows(&child, "/disk/Users/Alice/other.txt", PERMISSION_READ));
    permission_confine_disk_program(&child, "/bin/hello", 10);
    assert(strcmp(child.private_path, app.private_path) == 0);
    permission_confine_disk_program(&child, "/disk/Programs/other", 20);
    assert(strcmp(child.private_path, app.private_path) == 0);

    struct permission_identity other = built_in;
    permission_confine_disk_program(&other, "/disk/Programs/other", 20);
    assert(strcmp(other.private_path, app.private_path) != 0);
    assert(!allows(&other, file, PERMISSION_WRITE));

    struct permission_identity guest = {
        .account = PERMISSION_NO_ACCOUNT
    };
    permission_confine_disk_program(&guest, "/disk/guest", 11);
    assert(guest.confined && guest.private_path[0] == '\0');
    assert(!allows(&guest, "/disk/shared.txt", PERMISSION_WRITE));

    disk_root_selected = true;
    assert(allows(&built_in, "/bin/notepad", PERMISSION_READ));
    assert(!allows(&built_in, "/bin/notepad", PERMISSION_WRITE));
    assert(!allows(&app, "/bin/notepad", PERMISSION_WRITE));
    assert(!allows(&built_in, "/disk/OPOS/roots/test/bin/notepad",
                   PERMISSION_WRITE));
    struct permission_identity standard = {.account = 1};
    assert(!allows(&standard, "/bin/notepad", PERMISSION_WRITE));
    assert(!allows(&standard, "/disk/OPOS/roots/test/bin/notepad",
                   PERMISSION_WRITE));
    assert(!allows(&standard, "/disk/OPOS/ACCOUNTS.DAT", PERMISSION_READ));
    assert(!allows(&app, "/disk/OPOS/roots/test/bin/notepad",
                   PERMISSION_WRITE));
    assert(!permission_ui_allows("/bin/notepad", 12, PERMISSION_WRITE));
    assert(!permission_ui_allows("/disk/OPOS/roots/test/bin/notepad",
                                 strlen("/disk/OPOS/roots/test/bin/notepad"),
                                 PERMISSION_WRITE));
    assert(!permission_ui_requires_confirmation(
        "/disk/OPOS/roots/test/bin/notepad",
        strlen("/disk/OPOS/roots/test/bin/notepad"), PERMISSION_WRITE));
    assert(!permission_ui_allows(root_backing, strlen(root_backing),
                                 PERMISSION_REMOVE));
    assert(!permission_ui_allows("/disk/OPOS/roots",
                                 strlen("/disk/OPOS/roots"),
                                 PERMISSION_REMOVE));
    assert(permission_ui_allows("/disk/Programs/new",
                                strlen("/disk/Programs/new"),
                                PERMISSION_WRITE));
    struct permission_identity copied = built_in;
    permission_confine_disk_program(&copied, "/disk/Programs/notepad",
                                    strlen("/disk/Programs/notepad"));
    assert(copied.confined);
    assert(!allows(&copied, "/disk/OPOS/ACCOUNTS.DAT", PERMISSION_READ));
    assert(!allows(&copied, "/disk/OPOS/roots/test/bin/notepad",
                   PERMISSION_WRITE));
    disk_root_selected = false;

    /* Capabilities: recorded, inherited, only narrowed. */
    assert(built_in.capabilities == PERMISSION_CAPS_ALL);
    assert(permission_has(&built_in, PERMISSION_CAP_NET
                                         | PERMISSION_CAP_FILES_SHARED));
    assert(app.capabilities == PERMISSION_CAPS_DISK_DEFAULT);
    assert(permission_has(&app, PERMISSION_CAP_STORAGE
                                    | PERMISSION_CAP_WINDOW));
    assert(!permission_has(&app, PERMISSION_CAP_NET));
    assert(!permission_has(&app, PERMISSION_CAP_FILES_SHARED));
    assert(!permission_has(&app, PERMISSION_CAP_CLIPBOARD_READ));
    assert(!permission_has(&app, PERMISSION_CAP_WINDOW
                                     | PERMISSION_CAP_NET));
    assert(!permission_has(NULL, 0));
    struct permission_identity narrowed = app;
    narrowed.capabilities &= ~PERMISSION_CAP_WINDOW;
    permission_confine_disk_program(&narrowed, "/disk/Programs/other", 20);
    assert(narrowed.capabilities
           == (PERMISSION_CAPS_DISK_DEFAULT & ~PERMISSION_CAP_WINDOW));
    assert(strcmp(narrowed.private_path, app.private_path) == 0);
    struct permission_identity tool = built_in;
    permission_confine_disk_program(&tool, "/bin/hello", 10);
    assert(!tool.confined && tool.capabilities == PERMISSION_CAPS_ALL);
    struct permission_identity second_disk = built_in;
    permission_confine_disk_program(&second_disk, "/disk2/app", 10);
    assert(second_disk.confined
           && second_disk.capabilities == PERMISSION_CAPS_DISK_DEFAULT);

    /* The grants store is the kernel's alone. */
    static const char grants[] = "/disk/Users/Alice/AppData/Permissions.dat";
    static const char bob_grants[] = "/disk/Users/Bob/AppData/Permissions.dat";
    static const char upper[] = "/disk/Users/Alice/APPDATA/PERMISSIONS.DAT";
    static const char below[] =
        "/disk/Users/Alice/AppData/Permissions.dat/inner";
    for (int access = PERMISSION_READ; access <= PERMISSION_REMOVE;
         access++) {
        assert(!allows(&built_in, grants, (enum permission_access)access));
        assert(!allows(&built_in, upper, (enum permission_access)access));
        assert(!allows(&built_in, below, (enum permission_access)access));
        assert(!allows(&app, grants, (enum permission_access)access));
    }
    assert(permission_ui_allows(grants, strlen(grants), PERMISSION_READ));
    assert(!permission_ui_allows(grants, strlen(grants), PERMISSION_WRITE));
    assert(!permission_ui_allows(grants, strlen(grants), PERMISSION_REMOVE));
    assert(!permission_ui_allows(below, strlen(below), PERMISSION_WRITE));
    /* Alice is the admin here, so she may look at Bob's. */
    assert(permission_ui_allows(bob_grants, strlen(bob_grants),
                                PERMISSION_READ));
    assert(!permission_ui_allows(bob_grants, strlen(bob_grants),
                                 PERMISSION_WRITE));
    /* Neighbours stay ordinary home files. */
    assert(allows(&built_in, "/disk/Users/Alice/AppData/Other.dat",
                  PERMISSION_WRITE));
    assert(allows(&built_in, "/disk/Users/Alice/Permissions.dat",
                  PERMISSION_WRITE));
    /* AppData stays put, so Permissions.dat cannot be moved aside. */
    assert(!allows(&built_in, "/disk/Users/Alice/AppData", PERMISSION_REMOVE));
    assert(!allows(&built_in, "/disk/Users/Alice/appdata", PERMISSION_REMOVE));
    assert(!permission_ui_allows("/disk/Users/Alice/AppData", 25,
                                 PERMISSION_REMOVE));
    assert(allows(&built_in, "/disk/Users/Alice/AppData", PERMISSION_WRITE));
    assert(allows(&built_in, "/disk/Users/Alice/AppData/Other.dat",
                  PERMISSION_REMOVE));

    /* Built-ins hold what their job needs, matched by /bin name. */
    assert(permission_builtin_capabilities("/bin/notepad", 12)
           == (PERMISSION_CAPS_DISK_DEFAULT | PERMISSION_CAP_FILES_SHARED));
    assert(permission_builtin_capabilities("/BIN/Notepad", 12)
           == permission_builtin_capabilities("/bin/notepad", 12));
    assert(permission_builtin_capabilities("/bin/fetch", 10)
           & PERMISSION_CAP_NET);
    assert(!(permission_builtin_capabilities("/bin/fetch", 10)
             & PERMISSION_CAP_CLIPBOARD_READ));
    assert(permission_builtin_capabilities("/bin/browser", 12)
           & PERMISSION_CAP_CLIPBOARD_READ);
    assert(!(permission_builtin_capabilities("/bin/unknown", 12)
             & (PERMISSION_CAP_NET | PERMISSION_CAP_CLIPBOARD_READ)));
    assert(permission_builtin_capabilities("/tests/musl-net-test", 20)
           == PERMISSION_CAPS_ALL);
    assert(permission_builtin_capabilities("/bin/x/fetch", 12)
           == PERMISSION_CAPS_DISK_DEFAULT);
    assert(permission_builtin_capabilities("/cdrom/fetch", 12)
           == PERMISSION_CAPS_DISK_DEFAULT);
    assert(permission_builtin_capabilities("/disk/bin/fetch", 15)
           == PERMISSION_CAPS_DISK_DEFAULT);

    /* files.shared: a confined program reads the person's volumes only
       with it, or inside its own program folder. */
    assert(allows(&built_in, "/disk/shared.txt", PERMISSION_READ));
    struct permission_identity reader = built_in;
    permission_confine_disk_program(&reader, "/disk/Programs/editor", 21);
    assert(reader.program_folder[0] == '\0');
    assert(!allows(&reader, "/disk/shared.txt", PERMISSION_READ));
    assert(!allows(&reader, "/disk", PERMISSION_READ));
    assert(!allows(&reader, "/disk/Programs", PERMISSION_READ));
    assert(!allows(&reader, "/disk2/photo.png", PERMISSION_READ));
    assert(!allows(&reader, "/cdrom/setup.txt", PERMISSION_READ));
    assert(!allows(&reader, "/cdrom3/setup.txt", PERMISSION_READ));
    assert(permission_program_missing(&reader, "/disk/shared.txt", 16,
                                      PERMISSION_READ)
           == PERMISSION_CAP_FILES_SHARED);
    assert(permission_program_missing(&reader, "/disk/shared.txt", 16,
                                      PERMISSION_WRITE) == 0);
    assert(permission_program_missing(&reader, "/disk/OPOS/ACCOUNTS.DAT",
                                      23, PERMISSION_READ) == 0);
    assert(permission_program_missing(&reader, "/disk/Users/Bob/note.txt",
                                      24, PERMISSION_READ) == 0);
    /* Still readable without it: the boot image, system files, the list
       of homes, its own storage. */
    assert(allows(&reader, "/bin/hello", PERMISSION_READ));
    assert(allows(&reader, "/fonts/default.psf", PERMISSION_READ));
    assert(allows(&reader, "/diskette", PERMISSION_READ));
    assert(allows(&reader, "/disk/OPOS.CFG", PERMISSION_READ));
    assert(allows(&reader, "/disk/Users", PERMISSION_READ));
    snprintf(file, sizeof(file), "%s/settings.dat", reader.private_path);
    assert(allows(&reader, file, PERMISSION_READ));
    assert(permission_program_missing(&reader, "/bin/hello", 10,
                                      PERMISSION_READ) == 0);
    /* Running another disk program needs no files.shared. */
    assert(permission_program_may_run(&reader, "/disk/Programs/other", 20));
    assert(!permission_program_may_run(&reader, "/disk/Users/Bob/x", 17));
    assert(!permission_program_may_run(&reader, "/disk/OPOS/ACCOUNTS.DAT",
                                       23));
    /* A picked shared file is readable; nothing else is. */
    assert(permission_grant_read_file(&reader, "/disk/picked.txt", 16));
    assert(allows(&reader, "/disk/picked.txt", PERMISSION_READ));
    assert(!allows(&reader, "/disk/shared.txt", PERMISSION_READ));
    assert(!allows(&reader, "/disk/picked.txt", PERMISSION_WRITE));
    /* With the capability, everything shared reads again. */
    reader.capabilities |= PERMISSION_CAP_FILES_SHARED;
    assert(allows(&reader, "/disk/shared.txt", PERMISSION_READ));
    assert(allows(&reader, "/disk2/photo.png", PERMISSION_READ));
    assert(!allows(&reader, "/disk/shared.txt", PERMISSION_WRITE));
    assert(!allows(&reader, "/disk/Users/Bob/note.txt", PERMISSION_READ));
    assert(permission_program_missing(&reader, "/disk/Users/Bob/note.txt",
                                      24, PERMISSION_READ) == 0);

    /* Program folders. */
    struct permission_identity suite = built_in;
    permission_confine_disk_program(&suite, "/disk/Programs/Suite/bin/tool",
                                    strlen("/disk/Programs/Suite/bin/tool"));
    assert(strcmp(suite.program_folder, "/disk/Programs/Suite") == 0);
    assert(allows(&suite, "/disk/Programs/Suite/data/levels.dat",
                  PERMISSION_READ));
    assert(allows(&suite, "/disk/programs/suite/readme.txt",
                  PERMISSION_READ));
    assert(allows(&suite, "/disk/Programs/Suite", PERMISSION_READ));
    assert(!allows(&suite, "/disk/Programs/SuiteX/data", PERMISSION_READ));
    assert(!allows(&suite, "/disk/Programs/Other/data", PERMISSION_READ));
    assert(!allows(&suite, "/disk/Programs", PERMISSION_READ));
    assert(!allows(&suite, "/disk/Programs/Suite/data/levels.dat",
                   PERMISSION_WRITE));
    struct permission_identity d2 = built_in;
    permission_confine_disk_program(&d2, "/disk2/programs/Game/game",
                                    strlen("/disk2/programs/Game/game"));
    assert(strcmp(d2.program_folder, "/disk2/programs/Game") == 0);
    struct permission_identity loose = built_in;
    permission_confine_disk_program(&loose, "/disk/Games/game",
                                    strlen("/disk/Games/game"));
    assert(loose.program_folder[0] == '\0');
    assert(!allows(&loose, "/disk/Games/save.dat", PERMISSION_READ));
    /* Running a program from the same folder keeps it; another drops it. */
    struct permission_identity same = suite;
    permission_confine_disk_program(&same, "/disk/Programs/Suite/helper",
                                    strlen("/disk/Programs/Suite/helper"));
    assert(strcmp(same.program_folder, suite.program_folder) == 0);
    struct permission_identity moved = suite;
    permission_confine_disk_program(&moved, "/disk/Programs/Other/helper",
                                    strlen("/disk/Programs/Other/helper"));
    assert(moved.program_folder[0] == '\0');
    assert(!allows(&moved, "/disk/Programs/Other/data", PERMISSION_READ));
    assert(!allows(&moved, "/disk/Programs/Suite/data", PERMISSION_READ));
    /* An unconfined parent running a loose program never gains a folder. */
    struct permission_identity parent_suite = suite;
    permission_confine_disk_program(&parent_suite, "/bin/hello", 10);
    assert(strcmp(parent_suite.program_folder, "/disk/Programs/Suite") == 0);

    assert(permission_is_disk_program("/disk/x", 7));
    assert(permission_is_disk_program("/disk3/x", 8));
    assert(!permission_is_disk_program("/diskette/x", 11));
    assert(!permission_is_disk_program("/bin/x", 6));
    assert(!permission_is_disk_program(NULL, 0));
    puts("app permissions: passed");
    return 0;
}
