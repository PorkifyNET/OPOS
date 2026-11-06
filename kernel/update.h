#ifndef OPOS_UPDATE_H
#define OPOS_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * System updates. /bin/update downloads a release (see tools/mkrelease.py)
 * and hands it to the kernel, which checks it and installs it:
 *
 *   update_begin    checks the manifest's signature (ECDSA P-256 with the
 *                   key in update_key.h) and version, and works out which
 *                   files differ from the installed ones;
 *   update_write    receives a changed file in order into the staging
 *                   folder /disk/OPOS/UPDATE and checks its SHA-256;
 *   update_commit   installs the staged files.
 *
 * Manifest (the signed text, LF line ends):
 *
 *   OPOS-UPDATE 1
 *   version 0.2.0
 *   file <sha256 hex> <size> <asset name> <path on the system volume>
 *   ...
 *
 * Paths are under boot/ (not boot/limine/ or boot/old/). A line this
 * kernel does not know makes the update UNSUPPORTED, so later formats can
 * require a newer updater.
 *
 * Installing: the current boot/kernel.elf and boot/initramfs.tar are
 * copied to boot/old/ and limine.conf gets an "OPOS (previous version)"
 * entry for them, then the staged files are copied over the installed
 * ones (the kernel last). STATE.TXT in the staging folder records the
 * step, so update_resume can finish after a power cut on the next start;
 * until then the previous-version entry still boots.
 */

enum update_result {
    UPDATE_OK = 0,
    UPDATE_NOT_INSTALLED = 1,  /* live media, or no system volume */
    UPDATE_NOT_ADMIN = 2,
    UPDATE_BAD_SIGNATURE = 3,
    UPDATE_BAD_MANIFEST = 4,
    UPDATE_UNSUPPORTED = 5,    /* needs a newer updater */
    UPDATE_NOT_NEWER = 6,
    UPDATE_NO_SESSION = 7,     /* update_begin has not succeeded */
    UPDATE_BAD_FILE = 8,       /* wrong size or SHA-256 */
    UPDATE_INCOMPLETE = 9,     /* a changed file was not fully sent */
    UPDATE_DISK_ERROR = 10,
    UPDATE_INVALID = 11,       /* bad index, offset or argument */
    UPDATE_BUSY = 12,          /* another program's call is running */
};

#define UPDATE_MAX_FILES 32
#define UPDATE_MANIFEST_MAX 8192
#define UPDATE_SIGNATURE_MAX 128
#define UPDATE_ASSET_MAX 64
#define UPDATE_PATH_MAX 96

struct update_file_info {
    uint64_t size;
    uint32_t needed; /* differs from the installed file */
    uint32_t reserved;
    char asset[UPDATE_ASSET_MAX];
    char path[UPDATE_PATH_MAX];
};

/* The running version, e.g. "0.1.0" (the VERSION file). */
const char *update_current_version(void);

/*
 * Bracket every call made for a program (user.c). Long file work then
 * sleeps a tick between chunks so the desktop keeps running, and a
 * second program's call is refused (enter returns false: UPDATE_BUSY)
 * until leave.
 */
bool update_call_enter(void);
void update_call_leave(void);

/* On success *file_count is the manifest's file count. */
enum update_result update_begin(bool admin, const uint8_t *manifest,
                                size_t manifest_length,
                                const uint8_t *signature,
                                size_t signature_length,
                                size_t *file_count);
enum update_result update_file(size_t index, struct update_file_info *info);
/* Data must arrive in order, starting at offset 0 for each file. */
enum update_result update_write(bool admin, size_t index, uint64_t offset,
                                const uint8_t *data, size_t length);
enum update_result update_commit(bool admin);
void update_cancel(void);

/* Called once at boot after the system volume is mounted: finishes an
   update a power cut interrupted. */
void update_resume(void);

#endif
