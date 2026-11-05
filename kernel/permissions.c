#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accounts.h"
#include "permissions.h"
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
    if (!same_entry(disk, sizeof(disk) - 1, top, top_length, "Users")) {
        return info;
    }
    info.class = CLASS_USERS;
    if (second_length == 0) {
        info.anchor = true;
        return info;
    }
    const char *third;
    size_t third_length = next_component(path, length, &offset, &third);
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
    return info;
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
        return (struct permission_identity){current, accounts_is_admin(current)};
    }
    /* No accounts at all (live media, or a disk set up without any): the
       person at the keyboard owns the machine. */
    return (struct permission_identity){
        PERMISSION_NO_ACCOUNT, accounts_count() == 0
    };
}

bool permission_program_allows(const struct permission_identity *identity,
                               const char *path, size_t length,
                               enum permission_access access) {
    if (identity == NULL || path == NULL || !canonical(path, length)) {
        return false;
    }
    struct path_info info = classify(path, length);
    switch (info.class) {
    case CLASS_SHARED:
        return access != PERMISSION_REMOVE || !info.anchor;
    case CLASS_SYSTEM:
        return access == PERMISSION_READ;
    case CLASS_SECRET:
        return false;
    case CLASS_USERS:
        return access == PERMISSION_READ;
    case CLASS_HOME:
        if (info.owner == PERMISSION_NO_ACCOUNT
            || info.owner != identity->account) {
            return false;
        }
        return access != PERMISSION_REMOVE || !info.anchor;
    }
    return false;
}

bool permission_ui_allows(const char *path, size_t length,
                          enum permission_access access) {
    if (path == NULL || !canonical(path, length)) {
        return false;
    }
    struct permission_identity identity = permission_session_identity();
    struct path_info info = classify(path, length);
    switch (info.class) {
    case CLASS_SHARED:
        return access != PERMISSION_REMOVE || !info.anchor;
    case CLASS_SYSTEM:
        if (access == PERMISSION_READ) {
            return true;
        }
        /* Administrators may edit boot files, but the protected folders
           themselves stay where OPOS expects them. */
        return identity.admin && !info.anchor;
    case CLASS_SECRET:
        return false;
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
