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
 *   /disk/Users/<name>/AppData/Permissions.dat          capability grants
 *   /disk/Programs/<id>, /disk/Users/<name>/Programs/<id>
 *                                   installed packages (docs/packages.md),
 *                                   only for folders the installer listed
 *   /disk/Users/<name>/AppData/Packages                 package records
 *   anything else (/disk/..., /disk2..., /)              shared
 *
 * Programs may read system files but never change them, never see
 * ACCOUNTS.DAT, and only reach their own account's home. Disk-launched
 * programs are further limited to writing in their own app directory.
 * OPOS's own windows (Files, editor) act for the person at the
 * keyboard: an administrator may change system files and open other
 * homes there; a standard account may not. ACCOUNTS.DAT is only ever
 * touched by the kernel's account code. Permissions.dat is written only by
 * the kernel (permission_grants.c); programs never see it and OPOS's
 * windows may only read it.
 */

#define PERMISSION_NO_ACCOUNT ((size_t)-1)

/*
 * Capabilities: named rights a process holds (docs/app-sandbox.md). They
 * are recorded on every identity and inherited by children, which can
 * only narrow them. Enforced so far: files.shared, which a confined
 * program needs to read the shared class on /disk.., /cdrom.. outside its
 * own program folder; net, checked at socket(); clipboard.read and
 * clipboard.write, checked by the clipboard calls.
 */
#define PERMISSION_CAP_STORAGE         (1u << 0) /* own app storage */
#define PERMISSION_CAP_FILES_PICK      (1u << 1) /* picker-granted files */
#define PERMISSION_CAP_FILES_SHARED    (1u << 2) /* shared class */
#define PERMISSION_CAP_NET             (1u << 3) /* sockets */
#define PERMISSION_CAP_CLIPBOARD_READ  (1u << 4)
#define PERMISSION_CAP_CLIPBOARD_WRITE (1u << 5)
#define PERMISSION_CAP_WINDOW          (1u << 6)
/* Post notifications (docs/notifications.md, step 4). */
#define PERMISSION_CAP_NOTIFY          (1u << 7)
#define PERMISSION_CAPS_ALL            0xFFu
/* What any disk program may ask the person for on first use. */
#define PERMISSION_CAPS_ASK_ON_USE                                         \
    (PERMISSION_CAP_NET | PERMISSION_CAP_CLIPBOARD_READ                    \
     | PERMISSION_CAP_NOTIFY)
/* What a disk program holds before the user grants it anything. */
#define PERMISSION_CAPS_DISK_DEFAULT                                       \
    (PERMISSION_CAP_STORAGE | PERMISSION_CAP_FILES_PICK                    \
     | PERMISSION_CAP_CLIPBOARD_WRITE | PERMISSION_CAP_WINDOW)

struct permission_identity {
    /* Account index, or PERMISSION_NO_ACCOUNT. */
    size_t account;
    bool admin;
    /* PERMISSION_CAP_* bits. */
    uint32_t capabilities;
    /* Bits the program may still ask the person for, within what its
       parent held. A bit leaves this set once asked. */
    uint32_t askable;
    /* SHA-256 of the confined program's executable, when known. */
    bool program_known;
    uint8_t program[32];
    /* Name from the program's manifest, or empty. Chosen by the program,
       so never shown without the executable's own name beside it. */
    char program_name[32];
    /* <volume>/Programs/<folder> when the executable lives in such a
       folder: readable without files.shared. Empty otherwise. */
    char program_folder[128];
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
   widens an identity already inherited from a confined parent. Every disk
   executable narrows capabilities to PERMISSION_CAPS_DISK_DEFAULT. */
void permission_confine_disk_program(struct permission_identity *identity,
                                     const char *path, size_t length);

/* The same, with app storage <home>/AppData/Programs/<storage> for an
   installed package (storage is its id) instead of a folder named by a
   hash of path. NULL storage is permission_confine_disk_program. A name
   that is not a package id leaves the program without storage. */
void permission_confine_disk_program_named(
    struct permission_identity *identity, const char *path, size_t length,
    const char *storage);

/* True when identity holds every bit in capabilities. */
bool permission_has(const struct permission_identity *identity,
                    uint32_t capabilities);

/* True for an executable on a disk volume (/disk, /disk2..): the programs
   permission_confine_disk_program confines. */
bool permission_is_disk_program(const char *path, size_t length);

/*
 * The most a program that is not on a disk may hold: a fixed set per
 * /bin program (notepad has no net, the browser may read the clipboard),
 * everything for /tests and /lib, and PERMISSION_CAPS_DISK_DEFAULT for
 * anything else (a CD, for example). Disk programs get the disk default.
 */
uint32_t permission_builtin_capabilities(const char *path, size_t length);

/*
 * When permission_program_allows refuses, the one capability that would
 * allow it (today only PERMISSION_CAP_FILES_SHARED), or 0 when no
 * capability would.
 */
uint32_t permission_program_missing(const struct permission_identity *identity,
                                    const char *path, size_t length,
                                    enum permission_access access);

/* Whether a program may execute (not read) path: reading is allowed, or
   only files.shared is missing. Running a program confers no data. */
bool permission_program_may_run(const struct permission_identity *identity,
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

/*
 * Installed package folders, kept by package_install.c: /disk/Programs/<id>
 * (owner PERMISSION_NO_ACCOUNT: installed for everyone) or
 * /disk/Users/<name>/Programs/<id> (owner: that account). Programs may
 * only read inside them. OPOS's windows treat one installed for everyone
 * like system files and an account's own copy as read-only. A folder is
 * recognised by device and inode, so another spelling (an 8.3 alias)
 * cannot get around it.
 */
#define PERMISSION_PACKAGE_FOLDERS_MAX 128
void permission_package_folders_clear(void);
bool permission_package_folder_add(const char *path, size_t owner);

/* OPOS's own windows, acting for the logged-in person. */
bool permission_ui_allows(const char *path, size_t length,
                          enum permission_access access);

/* True when an allowed built-in UI mutation touches protected system files
   and therefore needs an explicit one-shot administrator confirmation. */
bool permission_ui_requires_confirmation(const char *path, size_t length,
                                         enum permission_access access);

#endif
