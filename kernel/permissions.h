#ifndef OPOS_PERMISSIONS_H
#define OPOS_PERMISSIONS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * File access policy. FAT has no owners or permission bits, so access is
 * decided by where a path is:
 *
 *   /disk/OPOS, /disk/boot, /disk/EFI, /disk/OPOS.CFG   system files
 *   /disk/OPOS/ACCOUNTS.DAT                             password hashes
 *   /disk/Users                                         list of homes
 *   /disk/Users/<name>                                  one account's home
 *   anything else (/disk/..., /disk2..., /)              shared
 *
 * Programs ("applications are guests") may read system files but never
 * change them, never see ACCOUNTS.DAT, and only reach their own account's
 * home. OPOS's own windows (Files, editor) act for the person at the
 * keyboard: an administrator may change system files and open other
 * homes there; a standard account may not. ACCOUNTS.DAT is only ever
 * touched by the kernel's account code.
 */

#define PERMISSION_NO_ACCOUNT ((size_t)-1)

struct permission_identity {
    /* Account index, or PERMISSION_NO_ACCOUNT. */
    size_t account;
    bool admin;
};

enum permission_access {
    /* List, stat, open for reading, run. */
    PERMISSION_READ,
    /* Create inside, write, truncate. */
    PERMISSION_WRITE,
    /* Delete or rename the entry itself. */
    PERMISSION_REMOVE,
};

/*
 * Identity for programs started now: the logged-in account. With no
 * account logged in, a machine without accounts is its owner's (admin);
 * a machine with accounts gives a standard identity without a home.
 */
struct permission_identity permission_session_identity(void);

/* Programs. path is absolute and canonical (no ".", "..", or "//"). */
bool permission_program_allows(const struct permission_identity *identity,
                               const char *path, size_t length,
                               enum permission_access access);

/*
 * Resolves "//", "." and ".." in an absolute path into out. Returns the
 * length, or 0 when the path is not absolute or does not fit.
 */
size_t permission_canonicalize(const char *path, size_t length, char *out,
                               size_t capacity);

/* OPOS's own windows, acting for the logged-in person. */
bool permission_ui_allows(const char *path, size_t length,
                          enum permission_access access);

#endif
