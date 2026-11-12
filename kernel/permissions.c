#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accounts.h"
#include "package.h"
#include "permissions.h"
#include "sha256.h"
#include "vfs.h"

/*
 * See permissions.h for the policy. Names on FAT are case-insensitive and
 * a long name also answers to its 8.3 alias (e.g. "ALEXAN~1"), so a name
 * that exists is compared by identity (device and inode from vfs_stat),
 * not by spelling. A name that does not exist yet can only be spelled one
 * way, so it is compared case-insensitively.
 */

#define PATH_CAPACITY 256

enum path_class {
    CLASS_SHARED,
    CLASS_SYSTEM,
    CLASS_SECRET,
    CLASS_USERS,
    CLASS_HOME,
    /* <home>/AppData/Permissions.dat: kernel-written capability grants. */
    CLASS_GRANTS,
    /* An installed package folder; owner says whose (NO_ACCOUNT: everyone). */
    CLASS_PACKAGE,
    /* <home>/AppData/Packages: kernel-written package records. */
    CLASS_RECORDS,
};

struct path_info {
    enum path_class class;
    /* The path is the protected folder or file itself (removing or
       renaming it would remove the protection). */
    bool anchor;
    /* CLASS_HOME: owning account, or PERMISSION_NO_ACCOUNT for a folder
       left behind by a deleted account. */
    size_t owner;
};

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool equal_ignore_case(const char *a, size_t a_length, const char *b) {
    size_t i = 0;
    for (; i < a_length; i++) {
        if (b[i] == '\0' || lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return b[i] == '\0';
}

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static size_t join(char buffer[PATH_CAPACITY], const char *prefix,
                   size_t prefix_length, const char *name,
                   size_t name_length);

static bool begins_component(const char *path, size_t length,
                             const char *prefix) {
    size_t count = text_length(prefix);
    if (length < count) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (lower(path[i]) != lower(prefix[i])) {
            return false;
        }
    }
    return length == count || path[count] == '/';
}

static bool ensure_directory(const char *path) {
    size_t length = text_length(path);
    struct vfs_metadata metadata;
    if (vfs_stat(path, length, &metadata)) {
        return metadata.type == VFS_NODE_DIRECTORY;
    }
    return vfs_create_directory(path, length);
}

bool permission_is_disk_program(const char *path, size_t length) {
    return path != NULL && length >= 5 && path[0] == '/'
        && lower(path[1]) == 'd' && lower(path[2]) == 'i'
        && lower(path[3]) == 's' && lower(path[4]) == 'k'
        && (length == 5 || path[5] == '/'
            || (path[5] >= '0' && path[5] <= '9'));
}

static size_t next_component(const char *path, size_t length, size_t *offset,
                             const char **component);

/* Length of "<volume>/Programs/<folder>" or
   "<volume>/Users/<name>/Programs/<folder>" when path is an executable
   somewhere below such a folder, else 0. */
static size_t program_folder_length(const char *path, size_t length) {
    size_t offset = 0;
    const char *part;
    (void)next_component(path, length, &offset, &part); /* volume */
    size_t programs = next_component(path, length, &offset, &part);
    if (equal_ignore_case(part, programs, "Users")) {
        if (next_component(path, length, &offset, &part) == 0) {
            return 0;
        }
        programs = next_component(path, length, &offset, &part);
    }
    if (!equal_ignore_case(part, programs, "Programs")) {
        return 0;
    }
    size_t folder = next_component(path, length, &offset, &part);
    size_t end = offset;
    size_t rest = next_component(path, length, &offset, &part);
    return folder != 0 && rest != 0 ? end : 0;
}

void permission_confine_disk_program(struct permission_identity *identity,
                                     const char *path, size_t length) {
    permission_confine_disk_program_named(identity, path, length, NULL);
}

void permission_confine_disk_program_named(
    struct permission_identity *identity, const char *path, size_t length,
    const char *storage) {
    if (identity == NULL || !permission_is_disk_program(path, length)) {
        return;
    }
    /* A confined parent running another disk program keeps the
       intersection of both sets (permission_apply_program adds grants). */
    identity->capabilities &= PERMISSION_CAPS_DISK_DEFAULT;
    size_t own = program_folder_length(path, length);
    if (own >= sizeof(identity->program_folder)) {
        own = 0;
    }
    if (identity->confined) {
        /* Never widen: keep the folder only when it is the same one. */
        if (own == 0
            || text_length(identity->program_folder) != own
            || !begins_component(path, length, identity->program_folder)) {
            identity->program_folder[0] = '\0';
        }
        return;
    }
    for (size_t i = 0; i < own; i++) {
        identity->program_folder[i] = path[i];
    }
    identity->program_folder[own] = '\0';
    identity->confined = true;
    identity->private_path[0] = '\0';
    if (identity->account == PERMISSION_NO_ACCOUNT
        || identity->account >= accounts_count()) {
        return;
    }
    const char *name = accounts_name(identity->account);
    char folder[PATH_CAPACITY];
    size_t used = join(folder, ACCOUNT_HOME_ROOT,
                       sizeof(ACCOUNT_HOME_ROOT) - 1,
                       name, text_length(name));
    if (used == 0 || used >= sizeof(identity->private_path)
        || !ensure_directory(folder)) {
        return;
    }
    used = join(folder, folder, used, "AppData", 7);
    if (used == 0 || !ensure_directory(folder)) {
        return;
    }
    used = join(folder, folder, used, "Programs", 8);
    if (used == 0 || !ensure_directory(folder)) {
        return;
    }
    /* A package's storage is named by its id, so an update or a move
       inside the package keeps it. A loose program's is named by a hash
       of its path. */
    size_t storage_length = storage == NULL ? 0 : text_length(storage);
    char directory[64];
    if (storage_length != 0) {
        /* A package id has a dot, so it never looks like a hex folder. */
        if (!package_id_valid(storage, storage_length)) {
            return;
        }
        for (size_t i = 0; i <= storage_length; i++) {
            directory[i] = storage[i];
        }
    } else {
        struct sha256_context hash;
        uint8_t digest[SHA256_DIGEST_SIZE];
        sha256_init(&hash);
        for (size_t i = 0; i < length; i++) {
            char c = lower(path[i]);
            sha256_update(&hash, &c, 1);
        }
        sha256_final(&hash, digest);
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < 8; i++) {
            directory[i * 2] = hex[digest[i] >> 4];
            directory[i * 2 + 1] = hex[digest[i] & 15];
        }
        directory[16] = '\0';
        storage_length = 16;
    }
    used = join(folder, folder, used, directory, storage_length);
    if (used == 0 || used >= sizeof(identity->private_path)
        || !ensure_directory(folder)) {
        return;
    }
    for (size_t i = 0; i <= used; i++) {
        identity->private_path[i] = folder[i];
    }
}

/* Builds "<prefix>/<name>" into buffer; returns its length or 0. */
static size_t join(char buffer[PATH_CAPACITY], const char *prefix,
                   size_t prefix_length, const char *name,
                   size_t name_length) {
    if (prefix_length + 1 + name_length >= PATH_CAPACITY) {
        return 0;
    }
    size_t length = 0;
    for (size_t i = 0; i < prefix_length; i++) {
        buffer[length++] = prefix[i];
    }
    buffer[length++] = '/';
    for (size_t i = 0; i < name_length; i++) {
        buffer[length++] = name[i];
    }
    buffer[length] = '\0';
    return length;
}

/*
 * True when the entry <folder>/<name> is the entry <folder>/<expected>:
 * same spelling ignoring case, or both exist and are the same node.
 */
static bool same_entry(const char *folder, size_t folder_length,
                       const char *name, size_t name_length,
                       const char *expected) {
    if (equal_ignore_case(name, name_length, expected)) {
        return true;
    }
    char a[PATH_CAPACITY];
    char b[PATH_CAPACITY];
    size_t a_length = join(a, folder, folder_length, name, name_length);
    size_t b_length = join(b, folder, folder_length, expected,
                           text_length(expected));
    struct vfs_metadata first;
    struct vfs_metadata second;
    return a_length != 0 && b_length != 0
        && vfs_stat(a, a_length, &first) && vfs_stat(b, b_length, &second)
        && first.device == second.device && first.inode == second.inode;
}

/* Splits off the component starting at *offset; returns its length. */
/* Each built-in gets what its job needs. Not enforced on unconfined
   programs: files.shared, storage and files.pick, so file access is as
   before. What changes for them is net and the clipboard. */
#define BUILTIN_BASE                                                       \
    (PERMISSION_CAP_STORAGE | PERMISSION_CAP_FILES_PICK                    \
     | PERMISSION_CAP_FILES_SHARED | PERMISSION_CAP_CLIPBOARD_WRITE        \
     | PERMISSION_CAP_WINDOW)

static const struct {
    const char *name;
    uint32_t capabilities;
} builtin_programs[] = {
    {"notepad", BUILTIN_BASE},
    {"fetch", BUILTIN_BASE | PERMISSION_CAP_NET},
    {"update", BUILTIN_BASE | PERMISSION_CAP_NET},
    {"pkg", BUILTIN_BASE | PERMISSION_CAP_NET},
    {"notify", BUILTIN_BASE | PERMISSION_CAP_NOTIFY},
    {"browser", (BUILTIN_BASE & ~PERMISSION_CAP_FILES_SHARED)
                    | PERMISSION_CAP_NET | PERMISSION_CAP_CLIPBOARD_READ},
};

uint32_t permission_builtin_capabilities(const char *path, size_t length) {
    if (path == NULL || permission_is_disk_program(path, length)) {
        return PERMISSION_CAPS_DISK_DEFAULT;
    }
    size_t offset = 0;
    const char *top;
    size_t top_length = next_component(path, length, &offset, &top);
    const char *name;
    size_t name_length = next_component(path, length, &offset, &name);
    const char *deeper;
    bool nested = next_component(path, length, &offset, &deeper) != 0;
    /* Self-tests and libraries the kernel shell runs. */
    if (equal_ignore_case(top, top_length, "tests")
        || equal_ignore_case(top, top_length, "lib")) {
        return PERMISSION_CAPS_ALL;
    }
    if (!equal_ignore_case(top, top_length, "bin") || name_length == 0
        || nested) {
        return PERMISSION_CAPS_DISK_DEFAULT; /* e.g. a CD: nothing extra */
    }
    for (size_t i = 0; i < sizeof(builtin_programs)
                               / sizeof(builtin_programs[0]); i++) {
        if (equal_ignore_case(name, name_length, builtin_programs[i].name)) {
            return builtin_programs[i].capabilities;
        }
    }
    return BUILTIN_BASE;
}

static size_t next_component(const char *path, size_t length, size_t *offset,
                             const char **component) {
    size_t start = *offset;
    while (start < length && path[start] == '/') {
        start++;
    }
    size_t end = start;
    while (end < length && path[end] != '/') {
        end++;
    }
    *component = path + start;
    *offset = end;
    return end - start;
}

/*
 * The policy reads paths by their components, so "/disk/Users/me/../you"
 * must never reach it: only canonical paths are accepted.
 */
static bool canonical(const char *path, size_t length) {
    if (length == 0 || path[0] != '/') {
        return false;
    }
    if (length == 1) {
        return true;
    }
    size_t start = 1;
    for (size_t i = 1; i <= length; i++) {
        if (i < length && path[i] != '/') {
            continue;
        }
        size_t part = i - start;
        if (part == 0 || (part == 1 && path[start] == '.')
            || (part == 2 && path[start] == '.' && path[start + 1] == '.')) {
            return false;
        }
        start = i + 1;
    }
    return true;
}

/* ---------------------------------------------------- package folders */

static struct {
    char path[128];
    size_t owner;
    uint64_t device;
    uint64_t inode;
} package_folders[PERMISSION_PACKAGE_FOLDERS_MAX];
static size_t package_folder_count;

void permission_package_folders_clear(void) {
    package_folder_count = 0;
}

bool permission_package_folder_add(const char *path, size_t owner) {
    size_t length = path == NULL ? 0 : text_length(path);
    struct vfs_metadata metadata;
    if (length == 0 || length >= sizeof(package_folders[0].path)
        || package_folder_count == PERMISSION_PACKAGE_FOLDERS_MAX
        || !vfs_stat(path, length, &metadata)
        || metadata.type != VFS_NODE_DIRECTORY) {
        return false;
    }
    for (size_t i = 0; i <= length; i++) {
        package_folders[package_folder_count].path[i] = path[i];
    }
    package_folders[package_folder_count].owner = owner;
    package_folders[package_folder_count].device = metadata.device;
    package_folders[package_folder_count].inode = metadata.inode;
    package_folder_count++;
    return true;
}

/* True when path[0..length) is a listed package folder of owner. The
   listed path must still be that node, so a folder removed behind the
   installer's back (and its inode reused) protects nothing. */
static bool package_folder(const char *path, size_t length, size_t owner) {
    bool any = false;
    for (size_t i = 0; i < package_folder_count && !any; i++) {
        any = package_folders[i].owner == owner;
    }
    struct vfs_metadata metadata;
    if (!any || !vfs_stat(path, length, &metadata)
        || metadata.type != VFS_NODE_DIRECTORY) {
        return false;
    }
    for (size_t i = 0; i < package_folder_count; i++) {
        struct vfs_metadata listed;
        if (package_folders[i].owner == owner
            && package_folders[i].device == metadata.device
            && package_folders[i].inode == metadata.inode
            && vfs_stat(package_folders[i].path,
                        text_length(package_folders[i].path), &listed)
            && listed.device == metadata.device
            && listed.inode == metadata.inode) {
            return true;
        }
    }
    return false;
}

static struct path_info classify(const char *path, size_t length) {
    struct path_info info = {CLASS_SHARED, false, PERMISSION_NO_ACCOUNT};
    size_t offset = 0;
    const char *first;
    size_t first_length = next_component(path, length, &offset, &first);
    /* Only the system volume (/disk) holds OPOS's own files. */
    if (!equal_ignore_case(first, first_length, "disk")) {
        return info;
    }
    const char *top;
    size_t top_length = next_component(path, length, &offset, &top);
    if (top_length == 0) {
        info.anchor = true; /* /disk itself */
        return info;
    }
    const char *second;
    size_t second_length = next_component(path, length, &offset, &second);
    static const char disk[] = "/disk";
    static const char *const system_names[] = {
        "OPOS", "boot", "EFI", "OPOS.CFG",
    };
    for (size_t i = 0; i < sizeof(system_names) / sizeof(system_names[0]);
         i++) {
        if (!same_entry(disk, sizeof(disk) - 1, top, top_length,
                        system_names[i])) {
            continue;
        }
        info.class = CLASS_SYSTEM;
        info.anchor = second_length == 0;
        if (i == 0 && second_length != 0) {
            static const char opos[] = "/disk/OPOS";
            if (same_entry(opos, sizeof(opos) - 1, second, second_length,
                           "ACCOUNTS.DAT")) {
                info.class = CLASS_SECRET;
                info.anchor = true;
            }
        }
        return info;
    }
    if (same_entry(disk, sizeof(disk) - 1, top, top_length, "Programs")) {
        /* /disk/Programs itself cannot be moved aside: package folders
           are only protected under this name. */
        if (second_length == 0) {
            info.anchor = true;
            return info;
        }
        const char *third;
        size_t folder_end = offset;
        if (package_folder(path, folder_end, PERMISSION_NO_ACCOUNT)) {
            info.class = CLASS_PACKAGE;
            info.anchor = next_component(path, length, &offset, &third) == 0;
        }
        return info;
    }
    if (!same_entry(disk, sizeof(disk) - 1, top, top_length, "Users")) {
        return info;
    }
    info.class = CLASS_USERS;
    if (second_length == 0) {
        info.anchor = true;
        return info;
    }
    size_t home_length = offset;
    const char *third;
    size_t third_length = next_component(path, length, &offset, &third);
    size_t third_end = offset;
    const char *fourth;
    size_t fourth_length = next_component(path, length, &offset, &fourth);
    info.class = CLASS_HOME;
    info.anchor = third_length == 0;
    static const char users[] = ACCOUNT_HOME_ROOT;
    for (size_t i = 0; i < accounts_count(); i++) {
        if (same_entry(users, sizeof(users) - 1, second, second_length,
                       accounts_name(i))) {
            info.owner = i;
            break;
        }
    }
    /* Any home, owned or orphaned. Anything below a directory spelled
       Permissions.dat is covered too, so it cannot be used as a disguise. */
    if (fourth_length != 0
        && same_entry(path, home_length, third, third_length, "AppData")
        && same_entry(path, third_end, fourth, fourth_length,
                      "Permissions.dat")) {
        info.class = CLASS_GRANTS;
        info.anchor = true;
    }
    /* AppData itself cannot be removed or renamed. Moving it aside would
       leave Permissions.dat outside the path checked above, writable. */
    if (fourth_length == 0
        && same_entry(path, home_length, third, third_length, "AppData")) {
        info.anchor = true;
    }
    /* <home>/AppData/Packages and below: the account's package records. */
    if (fourth_length != 0
        && same_entry(path, home_length, third, third_length, "AppData")
        && same_entry(path, third_end, fourth, fourth_length, "Packages")) {
        info.class = CLASS_RECORDS;
        info.anchor = true;
    }
    /* <home>/Programs: the account's own packages. */
    if (third_length != 0
        && same_entry(path, home_length, third, third_length, "Programs")) {
        if (fourth_length == 0) {
            info.anchor = true;
        } else if (info.owner != PERMISSION_NO_ACCOUNT
                   && package_folder(path, offset, info.owner)) {
            const char *fifth;
            info.class = CLASS_PACKAGE;
            info.anchor = next_component(path, length, &offset, &fifth) == 0;
        }
    }
    return info;
}

static struct path_info classify_visible(const char *path, size_t length) {
    char backing[512];
    size_t backing_length;
    if (vfs_disk_root_backing_path(path, length, backing,
                                   sizeof(backing), &backing_length)) {
        return classify(backing, backing_length);
    }
    return classify(path, length);
}

size_t permission_canonicalize(const char *path, size_t length, char *out,
                               size_t capacity) {
    if (path == NULL || out == NULL || capacity < 2 || length == 0
        || path[0] != '/') {
        return 0;
    }
    size_t used = 0;
    size_t offset = 0;
    while (offset < length) {
        const char *part;
        size_t part_length = next_component(path, length, &offset, &part);
        if (part_length == 0 || (part_length == 1 && part[0] == '.')) {
            continue;
        }
        if (part_length == 2 && part[0] == '.' && part[1] == '.') {
            while (used > 0 && out[used - 1] != '/') {
                used--;
            }
            if (used > 0) {
                used--;
            }
            continue;
        }
        if (used + 1 + part_length >= capacity) {
            return 0;
        }
        out[used++] = '/';
        for (size_t i = 0; i < part_length; i++) {
            out[used++] = part[i];
        }
    }
    if (used == 0) {
        out[used++] = '/';
    }
    out[used] = '\0';
    return used;
}

struct permission_identity permission_session_identity(void) {
    size_t current = accounts_current();
    if (current != ACCOUNT_NONE) {
        return (struct permission_identity){
            .account = current, .admin = accounts_is_admin(current),
            .capabilities = PERMISSION_CAPS_ALL
        };
    }
    /* No accounts at all (live media, or a disk set up without any): the
       person at the keyboard owns the machine. */
    return (struct permission_identity){
        .account = PERMISSION_NO_ACCOUNT, .admin = accounts_count() == 0,
        .capabilities = PERMISSION_CAPS_ALL
    };
}

bool permission_has(const struct permission_identity *identity,
                    uint32_t capabilities) {
    return identity != NULL
        && (identity->capabilities & capabilities) == capabilities;
}

/* /disk, /disk2.., /cdrom, /cdrom2..: the person's volumes, as opposed to
   the boot image (/bin, /lib, fonts, certificates). */
static bool on_person_volume(const char *path, size_t length) {
    size_t offset = 0;
    const char *first;
    size_t first_length = next_component(path, length, &offset, &first);
    size_t base;
    if (first_length >= 4 && lower(first[0]) == 'd' && lower(first[1]) == 'i'
        && lower(first[2]) == 's' && lower(first[3]) == 'k') {
        base = 4;
    } else if (first_length >= 5 && lower(first[0]) == 'c'
               && lower(first[1]) == 'd' && lower(first[2]) == 'r'
               && lower(first[3]) == 'o' && lower(first[4]) == 'm') {
        base = 5;
    } else {
        return false;
    }
    for (size_t i = base; i < first_length; i++) {
        if (first[i] < '0' || first[i] > '9') {
            return false;
        }
    }
    return true;
}

/* path is the file the person chose in the picker, still the same node. */
static bool picked(const struct permission_identity *identity,
                   const char *path, size_t length) {
    if (identity->read_grant[0] == '\0'
        || !equal_ignore_case(path, length, identity->read_grant)) {
        return false;
    }
    struct vfs_metadata metadata;
    return vfs_stat(path, length, &metadata)
        && metadata.type == VFS_NODE_FILE
        && metadata.device == identity->read_grant_device
        && metadata.inode == identity->read_grant_inode;
}

/* A read of a shared-class path that needs files.shared. */
static bool shared_read_needs_capability(
    const struct permission_identity *identity, const char *path,
    size_t length) {
    return identity->confined
        && !permission_has(identity, PERMISSION_CAP_FILES_SHARED)
        && on_person_volume(path, length)
        && !(identity->program_folder[0] != '\0'
             && begins_component(path, length, identity->program_folder));
}

bool permission_program_allows(const struct permission_identity *identity,
                               const char *path, size_t length,
                               enum permission_access access) {
    if (identity == NULL || path == NULL || !canonical(path, length)) {
        return false;
    }
    struct path_info info = classify_visible(path, length);
    if (identity->confined && access != PERMISSION_READ
        && (identity->private_path[0] == '\0'
            || !begins_component(path, length, identity->private_path)
            || (access == PERMISSION_REMOVE
                && length == text_length(identity->private_path)))) {
        return false;
    }
    switch (info.class) {
    case CLASS_SHARED:
        if (access == PERMISSION_READ
            && shared_read_needs_capability(identity, path, length)) {
            return picked(identity, path, length);
        }
        return access != PERMISSION_REMOVE || !info.anchor;
    case CLASS_SYSTEM:
        return access == PERMISSION_READ;
    case CLASS_SECRET:
    case CLASS_GRANTS:
        return false;
    case CLASS_RECORDS:
        /* The account's own unconfined tools may list its packages. */
        return access == PERMISSION_READ && !identity->confined
            && info.owner != PERMISSION_NO_ACCOUNT
            && info.owner == identity->account;
    case CLASS_PACKAGE:
        /* Only the installer changes a package. */
        if (access != PERMISSION_READ) {
            return false;
        }
        if (info.owner == PERMISSION_NO_ACCOUNT) {
            return !shared_read_needs_capability(identity, path, length)
                || picked(identity, path, length);
        }
        if (info.owner != identity->account) {
            return false;
        }
        return !identity->confined
            || (identity->program_folder[0] != '\0'
                && begins_component(path, length, identity->program_folder))
            || picked(identity, path, length);
    case CLASS_USERS:
        return access == PERMISSION_READ;
    case CLASS_HOME:
        if (info.owner == PERMISSION_NO_ACCOUNT
            || info.owner != identity->account) {
            return false;
        }
        if (identity->confined && access == PERMISSION_READ) {
            if (identity->private_path[0] != '\0'
                && begins_component(path, length, identity->private_path)) {
                return true;
            }
            /* A program in <home>/Programs/<folder> reads its own files. */
            if (identity->program_folder[0] != '\0'
                && begins_component(path, length, identity->program_folder)) {
                return true;
            }
            return picked(identity, path, length);
        }
        return access != PERMISSION_REMOVE || !info.anchor;
    }
    return false;
}

uint32_t permission_program_missing(const struct permission_identity *identity,
                                    const char *path, size_t length,
                                    enum permission_access access) {
    if (identity == NULL || path == NULL || access != PERMISSION_READ
        || !canonical(path, length)
        || permission_program_allows(identity, path, length, access)) {
        return 0;
    }
    struct path_info info = classify_visible(path, length);
    bool shared = info.class == CLASS_SHARED
        || (info.class == CLASS_PACKAGE
            && info.owner == PERMISSION_NO_ACCOUNT);
    if (!shared || !shared_read_needs_capability(identity, path, length)) {
        return 0;
    }
    return PERMISSION_CAP_FILES_SHARED;
}

bool permission_program_may_run(const struct permission_identity *identity,
                                const char *path, size_t length) {
    return permission_program_allows(identity, path, length, PERMISSION_READ)
        || permission_program_missing(identity, path, length, PERMISSION_READ)
               == PERMISSION_CAP_FILES_SHARED;
}

bool permission_grant_read_file(struct permission_identity *identity,
                                const char *path, size_t length) {
    if (identity == NULL || !identity->confined || path == NULL
        || length == 0 || length >= sizeof(identity->read_grant)) {
        return false;
    }
    struct permission_identity baseline = *identity;
    baseline.confined = false;
    if (!permission_program_allows(&baseline, path, length, PERMISSION_READ)) {
        return false;
    }
    struct vfs_metadata metadata;
    if (!vfs_stat(path, length, &metadata)
        || metadata.type != VFS_NODE_FILE) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        identity->read_grant[i] = path[i];
    }
    identity->read_grant[length] = '\0';
    identity->read_grant_device = metadata.device;
    identity->read_grant_inode = metadata.inode;
    return true;
}

bool permission_ui_allows(const char *path, size_t length,
                          enum permission_access access) {
    if (path == NULL || !canonical(path, length)) {
        return false;
    }
    struct permission_identity identity = permission_session_identity();
    struct path_info info = classify_visible(path, length);
    if (access != PERMISSION_READ
        && (vfs_disk_root_contains(path, length)
            || (access == PERMISSION_REMOVE
                && vfs_disk_root_ancestor(path, length)))) {
        return false;
    }
    switch (info.class) {
    case CLASS_SHARED:
        return access != PERMISSION_REMOVE || !info.anchor;
    case CLASS_SYSTEM:
        if (access == PERMISSION_READ) {
            return true;
        }
        /* Writing an anchor folder means creating inside it; removing the
           anchor itself remains forbidden. OPOS.CFG can be edited likewise. */
        return identity.admin
            && (access == PERMISSION_WRITE || !info.anchor);
    case CLASS_SECRET:
        return false;
    case CLASS_GRANTS:
    case CLASS_RECORDS:
        /* Visible so the person can see it exists; changed only through
           the kernel. Other homes stay admin-only, as for CLASS_HOME. */
        return access == PERMISSION_READ
            && (info.owner == identity.account
                    ? info.owner != PERMISSION_NO_ACCOUNT || identity.admin
                    : identity.admin);
    case CLASS_PACKAGE:
        if (info.owner == PERMISSION_NO_ACCOUNT) {
            /* Like system files: an administrator, after confirming. */
            return access == PERMISSION_READ
                || (identity.admin
                    && (access == PERMISSION_WRITE || !info.anchor));
        }
        /* An account's own copy changes only through the installer. */
        return access == PERMISSION_READ
            && (info.owner == identity.account || identity.admin);
    case CLASS_USERS:
        return access == PERMISSION_READ;
    case CLASS_HOME:
        if (access == PERMISSION_REMOVE && info.anchor) {
            return false; /* homes go away with their account */
        }
        return info.owner == identity.account
            ? info.owner != PERMISSION_NO_ACCOUNT || identity.admin
            : identity.admin;
    }
    return false;
}

bool permission_ui_requires_confirmation(const char *path, size_t length,
                                          enum permission_access access) {
    if (path == NULL || access == PERMISSION_READ
        || !canonical(path, length)) {
        return false;
    }
    struct permission_identity identity = permission_session_identity();
    struct path_info info = classify_visible(path, length);
    if (vfs_disk_root_contains(path, length)
        || vfs_disk_root_ancestor(path, length)) {
        return false;
    }
    bool system = info.class == CLASS_SYSTEM
        || (info.class == CLASS_PACKAGE
            && info.owner == PERMISSION_NO_ACCOUNT);
    return identity.admin && system
        && (access == PERMISSION_WRITE || !info.anchor);
}
