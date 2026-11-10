#ifndef OPOS_PERMISSIONS_H
#define OPOS_PERMISSIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
 * Programs may read system files but never change them, never see
 * ACCOUNTS.DAT, and only reach their own account's home. Disk-launched
 * programs are further limited to writing in their own app directory.
 * OPOS's own windows (Files, editor) act for the person at the
 * keyboard: an administrator may change system files and open other
 * homes there; a standard account may not. ACCOUNTS.DAT is only ever
 * touched by the kernel's account code.
 */

#define PERMISSION_NO_ACCOUNT ((size_t)-1)

struct permission_identity {
    /* Account index, or PERMISSION_NO_ACCOUNT. */
    size_t account;
    bool admin;
    /* Disk-launched programs may write only below this directory. Empty
       means the directory could not be prepared, so writes are denied. */
    bool confined;
    char private_path[128];
    /* One user-selected document. A replacement at the same path does not
       inherit the grant: device and inode must still match. */
    char read_grant[256];
    uint64_t read_grant_device;
    uint64_t read_grant_inode;
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

/* Narrow a process identity when it runs an executable from a disk. Never
   widens an identity already inherited from a confined parent. */
void permission_confine_disk_program(struct permission_identity *identity,
                                     const char *path, size_t length);

/* Called only after the desktop picker chooses a file. */
bool permission_grant_read_file(struct permission_identity *identity,
                                const char *path, size_t length);

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

/* True when an allowed built-in UI mutation touches protected system files
   and therefore needs an explicit one-shot administrator confirmation. */
bool permission_ui_requires_confirmation(const char *path, size_t length,
                                         enum permission_access access);

#endif
