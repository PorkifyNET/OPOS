#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "accounts.h"
#include "permissions.h"
#include "vfs.h"

static char directories[8][128];
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
            && strncmp(path, "/disk/Users/Alice/other.txt", length) == 0)) {
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
    assert(directory_count < 8 && length < sizeof(directories[0]));
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
    puts("app permissions: passed");
    return 0;
}
