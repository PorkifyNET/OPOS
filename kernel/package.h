#ifndef OPOS_PACKAGE_H
#define OPOS_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Packages (docs/packages.md). A package is an uncompressed ustar archive
 * whose first member, PACKAGE, lists every other member with its SHA-256
 * and size. sdk/tools/mkopk.py builds packages and is the reference for
 * the rules package_parse applies; the two must agree.
 *
 * This file holds the checks that need no disk: the PACKAGE text and
 * ustar headers. package_install.c does the installing.
 */

#define PACKAGE_TEXT_MAX 65536
#define PACKAGE_FILES_MAX 512
#define PACKAGE_STARTS_MAX 16
#define PACKAGE_PATH_MAX 150
#define PACKAGE_ID_MAX 63
#define PACKAGE_NAME_MAX 31
#define PACKAGE_PUBLISHER_MAX 63
#define PACKAGE_VERSION_MAX 15
#define PACKAGE_REASON_MAX 96

/* Results, also returned (negated) by the package system calls. */
enum package_result {
    PACKAGE_OK = 0,
    PACKAGE_NOT_INSTALLED = 1, /* live media, or no system volume */
    PACKAGE_NOT_ALLOWED = 2,   /* not an administrator, no account, or
                                  called by a confined program */
    PACKAGE_BAD = 3,           /* the package breaks a rule */
    PACKAGE_UNSUPPORTED = 4,   /* format or key this OPOS does not know */
    PACKAGE_NEEDS_NEWER_OPOS = 5, /* requires-opos is newer than this */
    PACKAGE_PLATFORM = 6,      /* built for a platform OPOS cannot run */
    PACKAGE_NOT_NEWER = 7,     /* the same or a newer version is installed */
    PACKAGE_BUSY = 8,          /* another program is installing */
    PACKAGE_DISK_ERROR = 9,
    PACKAGE_NOT_FOUND = 10,    /* no such file, or id not installed */
    PACKAGE_CONFLICT = 11,     /* id installed for everyone (or for this
                                  account), or the folder is in use */
    PACKAGE_NOT_CONFIRMED = 12, /* outside the Store: the caller must say
                                   the person agreed */
    PACKAGE_INVALID = 13,      /* bad argument */
};

enum package_platform {
    PACKAGE_PLATFORM_OPOS,
    PACKAGE_PLATFORM_LINUX,
    PACKAGE_PLATFORM_WINDOWS,
};

/* A piece of the PACKAGE text: offset and length. */
struct package_span {
    uint32_t offset;
    uint32_t length;
};

struct package_file {
    struct package_span path; /* "files/..." */
    uint8_t hash[32];
    uint64_t size;
    bool exec;
};

struct package_start {
    struct package_span label;
    uint32_t file; /* index into files */
};

struct package {
    char id[PACKAGE_ID_MAX + 1];
    char name[PACKAGE_NAME_MAX + 1];
    char version[PACKAGE_VERSION_MAX + 1];
    char publisher[PACKAGE_PUBLISHER_MAX + 1];
    char requires_opos[PACKAGE_VERSION_MAX + 1]; /* empty: none */
    enum package_platform platform;
    /* Non-opos packages: PERMISSION_CAP_* bits from "capabilities:". */
    uint32_t capabilities;
    /* Records only: "origin: sideloaded" or "origin: store". */
    bool store;
    uint64_t total_size;
    size_t file_count;
    struct package_file files[PACKAGE_FILES_MAX];
    size_t start_count;
    struct package_start starts[PACKAGE_STARTS_MAX];
};

/*
 * Checks PACKAGE text (record false) or an installed record, which is
 * "origin: ..." followed by the package's PACKAGE text (record true).
 * Spans in *package point into text, which must outlive it. On failure,
 * reason (if not NULL, PACKAGE_REASON_MAX + 1 bytes) says why.
 */
enum package_result package_parse(const char *text, size_t length,
                                  bool record, struct package *package,
                                  char *reason);

/* A package id: dot-separated labels of a-z, 0-9 and '-', at least two,
   each alphanumeric at both ends, at most PACKAGE_ID_MAX characters. */
bool package_id_valid(const char *text, size_t length);

/* "1.2.3" against "1.2.10": -1, 0 or 1. Both must be valid versions. */
int package_compare_versions(const char *a, const char *b);
bool package_version_valid(const char *text, size_t length);

/* The file whose path is name[0..length), or -1. Exact spelling. */
long package_find_file(const struct package *package, const char *text,
                       const char *name, size_t length);

/*
 * One 512-byte ustar header. On success *name (PACKAGE_PATH_MAX + 1 bytes)
 * holds prefix/name and *size the member size. Folder, link and special
 * entries, a bad checksum or magic, and non-ASCII names fail with a
 * reason. *end is true for an all-zero block (and nothing else is set).
 */
bool package_ustar_header(const uint8_t block[512], char *name,
                          uint64_t *size, bool *end, char *reason);

/* ------------------------------------------------------------ installing
 * package_install.c. A package is installed for everyone in
 * /disk/Programs/<id> with its record in /disk/OPOS/Packages/<id>, or for
 * one account in <home>/Programs/<id> with its record in
 * <home>/AppData/Packages/<id>. A record is "origin: ..." followed by the
 * package's PACKAGE text. Work is staged in /disk/OPOS/PKGSTAGE, and
 * package_resume finishes an install or removal a power cut interrupted.
 */

#define PACKAGE_OPTION_EVERYONE 1u       /* install for every account */
#define PACKAGE_OPTION_CONFIRMED 2u      /* the person agreed to install a
                                            package from outside the Store */
#define PACKAGE_OPTION_IGNORE_VERSION 4u /* reinstall or downgrade */
#define PACKAGE_OPTION_MASK 7u
/* Removal only: also delete the calling account's app storage of the
   program. Removal always forgets its grants in that account. */
#define PACKAGE_OPTION_REMOVE_DATA 8u
#define PACKAGE_REMOVE_OPTION_MASK                                         \
    (PACKAGE_OPTION_EVERYONE | PACKAGE_OPTION_REMOVE_DATA)

#define PACKAGE_INSTALLED_NONE 0
#define PACKAGE_INSTALLED_EVERYONE 1
#define PACKAGE_INSTALLED_ACCOUNT 2

/* What a package is, for the person to decide (opos.h:
   struct opos_package_info, same layout). */
struct package_info {
    uint32_t size;        /* sizeof(struct package_info), set by caller */
    uint32_t platform;    /* enum package_platform */
    uint32_t file_count;
    uint32_t start_count;
    uint64_t total_size;
    uint32_t installed;   /* PACKAGE_INSTALLED_*, as this caller sees it */
    uint32_t store;       /* 1: from the OPOS Store (not yet possible) */
    char id[64];
    char name[32];
    char version[16];
    char publisher[64];
    char requires_opos[16];
    char installed_version[16];
    char reason[128];     /* why the check failed, or empty */
};
_Static_assert(sizeof(struct package_info) == 368,
               "struct package_info is ABI (opos.h)");

/* One Start > Programs entry from an installed package. */
struct package_start_entry {
    char label[PACKAGE_NAME_MAX + 1];
    char path[256];
};

/* Bracket calls made for a program; enter fails while another program
   installs packages or updates the system. */
bool package_call_enter(void);
void package_call_leave(void);
bool package_call_busy(void);

/* Reads and checks the package at path without installing it. account is
   the caller's (for info->installed) or PERMISSION_NO_ACCOUNT. */
enum package_result package_check(const char *path, size_t length,
                                  size_t account, struct package_info *info);
enum package_result package_install(const char *path, size_t length,
                                    size_t account, bool admin,
                                    uint32_t options,
                                    struct package_info *info);
enum package_result package_remove(const char *id, size_t length,
                                   size_t account, bool admin,
                                   uint32_t options);

/* At boot, after update_resume: finish interrupted work, then list the
   installed package folders for permissions.c. */
void package_resume(void);
void package_registry_load(void);

/* Start entries for account (and everyone). Returns how many were
   written. An account's own copy hides everyone's copy of the same id. */
size_t package_start_entries(size_t account, struct package_start_entry *out,
                             size_t capacity);

/*
 * Identity of a program being started (docs/packages.md, "Identity").
 * True when path (absolute, canonical) lies inside the folder of a
 * package installed for everyone or for account, and hash is the SHA-256
 * of one of that package's executable files as its record lists them.
 * Then id holds the package id and *store whether it came from the OPOS
 * Store. Anything else (a changed file, another spelling of the folder,
 * a damaged record) is false: the program is a loose program.
 */
bool package_identify(const char *path, size_t length, size_t account,
                      const uint8_t hash[32], char id[PACKAGE_ID_MAX + 1],
                      bool *store);

#endif
