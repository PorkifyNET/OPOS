#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block.h"
#include "cmos.h"
#include "fat16.h"
#include "installer.h"
#include "recovery.h"
#include "vfs.h"

/*
 * A start counts as unfinished from the moment the boot volume is mounted
 * until recovery_start_finished(). The count is decimal text, so a person
 * can read or clear it from another system.
 */
#define ATTEMPTS_PATH "/disk/OPOS/BOOT-ATTEMPTS"
#define ATTEMPTS_PATH_LENGTH (sizeof(ATTEMPTS_PATH) - 1)

static enum recovery_reason current_reason = RECOVERY_NONE;
static uint32_t failed_starts;
static bool counted;

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static bool installed(void) {
    return !installer_available() && fat16_system_mounted();
}

static void write_count(uint32_t count) {
    char text[12];
    size_t length = 0;
    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + count % 10);
        count /= 10;
    } while (count != 0 && digit_count < sizeof(digits));
    while (digit_count != 0) {
        text[length++] = digits[--digit_count];
    }
    text[length++] = '\n';
    (void)vfs_write_file(ATTEMPTS_PATH, ATTEMPTS_PATH_LENGTH,
                         (const uint8_t *)text, length);
}

uint32_t recovery_count_start(void) {
    if (counted || !installed()) {
        return failed_starts;
    }
    counted = true;
    char text[12];
    size_t length = 0;
    uint32_t count = 0;
    if (vfs_read_file(ATTEMPTS_PATH, ATTEMPTS_PATH_LENGTH, (uint8_t *)text,
                      sizeof(text), &length)) {
        for (size_t i = 0; i < length && text[i] >= '0' && text[i] <= '9'
                 && count < 100000; i++) {
            count = count * 10 + (uint32_t)(text[i] - '0');
        }
    }
    failed_starts = count;
    write_count(count + 1);
    return failed_starts;
}

void recovery_start_finished(void) {
    if (!counted) {
        return;
    }
    counted = false;
    write_count(0);
}

void recovery_set_reason(enum recovery_reason reason) {
    current_reason = reason;
}

enum recovery_reason recovery_reason(void) {
    return current_reason;
}

uint32_t recovery_failed_starts(void) {
    return failed_starts;
}

/* ----------------------------------------------------- undo the update */

/*
 * Big files are only ever copied to side files; the boot files change by
 * renames, so a power cut never leaves a half-written kernel or initramfs
 * where Limine looks:
 *   1. old pair  -> boot/kernel.new, boot/initramfs.new   (copies)
 *   2. this pair -> boot/undo/                            (copies)
 *   3. UNDO-STATE "SWAPPING": .new files renamed over the boot pair,
 *      initramfs first, kernel last
 *   4. UNDO-STATE "SAVING": boot/undo/ renamed over boot/old/
 *   5. UNDO-STATE and the leftovers removed.
 * Until the kernel rename the newer kernel still starts, and finishes the
 * job from UNDO-STATE (recovery_undo_resume).
 */
#define KERNEL "/disk/boot/kernel.elf"
#define INITRAMFS "/disk/boot/initramfs.tar"
#define NEW_KERNEL "/disk/boot/kernel.new"
#define NEW_INITRAMFS "/disk/boot/initramfs.new"
#define OLD_KERNEL "/disk/boot/old/kernel.elf"
#define OLD_INITRAMFS "/disk/boot/old/initramfs.tar"
#define UNDO_DIRECTORY "/disk/boot/undo"
#define UNDO_KERNEL UNDO_DIRECTORY "/kernel.elf"
#define UNDO_INITRAMFS UNDO_DIRECTORY "/initramfs.tar"
#define UNDO_STATE "/disk/OPOS/UNDO-STATE"
/* kernel/update.c keeps a half-installed update's step here. */
#define UPDATE_STATE "/disk/OPOS/UPDATE/STATE.TXT"
#define COPY_CHUNK 65536

/* UNDO-STATE: "<step> <newer kernel hash> <older kernel hash>\n". The
   hashes tell which kernel a later start is running. */
#define STEP_SWAPPING "SWAPPING"
#define STEP_SAVING "SAVING"

static uint8_t chunk[COPY_CHUNK];
static uint64_t running_kernel_hash;
static bool running_kernel_known;

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME 0x100000001b3ULL

static uint64_t fnv_update(uint64_t hash, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        hash = (hash ^ data[i]) * FNV_PRIME;
    }
    return hash;
}

void recovery_set_running_kernel(const void *image, size_t size) {
    if (image == NULL || size == 0) {
        return;
    }
    running_kernel_hash = fnv_update(FNV_OFFSET, image, size);
    running_kernel_known = true;
}

static bool exists(const char *path) {
    size_t size;
    return vfs_file_size(path, text_length(path), &size);
}

static bool non_empty(const char *path) {
    size_t size;
    return vfs_file_size(path, text_length(path), &size) && size != 0;
}

static void remove_file(const char *path) {
    bool not_empty;
    (void)vfs_remove(path, text_length(path), false, &not_empty);
}

static bool make_directory(const char *path) {
    struct vfs_metadata metadata;
    size_t length = text_length(path);
    return vfs_create_directory(path, length)
        || (vfs_stat(path, length, &metadata)
            && metadata.type == VFS_NODE_DIRECTORY);
}

/* Renames from over to (replacing it), durably. */
static bool replace_file(const char *from, const char *to) {
    block_begin_batch();
    bool ok = vfs_rename(from, text_length(from), to, text_length(to))
        == VFS_RENAME_OK;
    return block_end_batch() && ok;
}

static bool file_hash(const char *path, uint64_t *hash) {
    size_t size;
    size_t length = text_length(path);
    if (!vfs_file_size(path, length, &size)) {
        return false;
    }
    uint64_t value = FNV_OFFSET;
    for (size_t offset = 0; offset < size; offset += COPY_CHUNK) {
        size_t want = size - offset < COPY_CHUNK ? size - offset : COPY_CHUNK;
        size_t got = 0;
        if (!vfs_read_at(path, length, offset, chunk, want, &got)
            || got != want) {
            return false;
        }
        value = fnv_update(value, chunk, want);
    }
    *hash = value;
    return true;
}

struct copy_report {
    recovery_progress progress;
    const char *step;
    unsigned from;
    unsigned span;
};

/* Copies and reads the copy back; the batch makes it durable. */
static bool copy_verified(const char *from, const char *to,
                          const struct copy_report *report) {
    size_t size;
    size_t from_length = text_length(from);
    size_t to_length = text_length(to);
    if (!vfs_file_size(from, from_length, &size)) {
        return false;
    }
    block_begin_batch();
    bool ok = vfs_create_file(to, to_length, true);
    uint64_t hash = FNV_OFFSET;
    for (size_t offset = 0; ok && offset < size; offset += COPY_CHUNK) {
        size_t want = size - offset < COPY_CHUNK ? size - offset : COPY_CHUNK;
        size_t got = 0;
        size_t written = 0;
        ok = vfs_read_at(from, from_length, offset, chunk, want, &got)
            && got == want
            && vfs_write_at(to, to_length, offset, chunk, want, &written)
            && written == want;
        hash = fnv_update(hash, chunk, want);
        if (report->progress != NULL && size != 0) {
            report->progress(report->step, report->from
                + (unsigned)((uint64_t)report->span * (offset + want)
                             / size));
        }
    }
    if (!block_end_batch()) {
        ok = false;
    }
    uint64_t copied;
    return ok && file_hash(to, &copied) && copied == hash;
}

static bool write_state(const char *step, uint64_t newer, uint64_t older) {
    char text[64];
    size_t length = 0;
    for (size_t i = 0; step[i] != '\0'; i++) {
        text[length++] = step[i];
    }
    const uint64_t values[2] = {newer, older};
    for (size_t v = 0; v < 2; v++) {
        text[length++] = ' ';
        for (int shift = 60; shift >= 0; shift -= 4) {
            text[length++] = "0123456789abcdef"[(values[v] >> shift) & 0xf];
        }
    }
    text[length++] = '\n';
    block_begin_batch();
    bool ok = vfs_write_file(UNDO_STATE, text_length(UNDO_STATE),
                             (const uint8_t *)text, length);
    return block_end_batch() && ok;
}

static bool parse_hex(const char *text, uint64_t *value) {
    uint64_t result = 0;
    for (size_t i = 0; i < 16; i++) {
        char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9') {
            digit = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = (unsigned)(c - 'a' + 10);
        } else {
            return false;
        }
        result = result << 4 | digit;
    }
    *value = result;
    return true;
}

static bool word_is(const char *text, size_t length, const char *word) {
    size_t i = 0;
    while (i < length && word[i] != '\0' && text[i] == word[i]) {
        i++;
    }
    return i == length && word[i] == '\0';
}

/* Reads UNDO-STATE: true for SWAPPING or SAVING with both hashes. */
static bool read_state(bool *saving, uint64_t *newer, uint64_t *older) {
    char text[64];
    size_t length = 0;
    if (!vfs_read_file(UNDO_STATE, text_length(UNDO_STATE), (uint8_t *)text,
                       sizeof(text) - 1, &length)) {
        return false;
    }
    text[length] = '\0';
    size_t step_length = 0;
    while (step_length < length && text[step_length] != ' ') {
        step_length++;
    }
    bool swapping = word_is(text, step_length, STEP_SWAPPING);
    bool saving_step = word_is(text, step_length, STEP_SAVING);
    if ((!swapping && !saving_step) || length < step_length + 34
        || text[step_length + 17] != ' '
        || !parse_hex(text + step_length + 1, newer)
        || !parse_hex(text + step_length + 18, older)) {
        return false;
    }
    *saving = saving_step;
    return true;
}

/* Everything an undo leaves behind, the state first. */
static void remove_undo_files(void) {
    block_begin_batch();
    remove_file(UNDO_STATE);
    remove_file(NEW_KERNEL);
    remove_file(NEW_INITRAMFS);
    remove_file(UNDO_KERNEL);
    remove_file(UNDO_INITRAMFS);
    bool not_empty;
    (void)vfs_remove(UNDO_DIRECTORY, text_length(UNDO_DIRECTORY), true,
                     &not_empty);
    (void)block_end_batch();
}

/* Step 3: whichever .new file is still there goes into place. */
static bool swap_in(void) {
    return (!exists(NEW_INITRAMFS) || replace_file(NEW_INITRAMFS, INITRAMFS))
        && (!exists(NEW_KERNEL) || replace_file(NEW_KERNEL, KERNEL));
}

/* Step 4: the newer pair becomes "OPOS (previous version)". */
static bool save_newer(void) {
    return make_directory("/disk/boot/old")
        && (!exists(UNDO_INITRAMFS)
            || replace_file(UNDO_INITRAMFS, OLD_INITRAMFS))
        && (!exists(UNDO_KERNEL) || replace_file(UNDO_KERNEL, OLD_KERNEL));
}

bool recovery_undo_available(void) {
    return installed() && non_empty(OLD_KERNEL) && non_empty(OLD_INITRAMFS)
        && !exists(UPDATE_STATE) && !exists(UNDO_STATE);
}

static bool undo_fail(const char **error, const char *reason) {
    if (error != NULL) {
        *error = reason;
    }
    return false;
}

static void report_step(recovery_progress progress, const char *step,
                        unsigned percent) {
    if (progress != NULL) {
        progress(step, percent);
    }
}

bool recovery_undo_update(recovery_progress progress, const char **error) {
    if (!recovery_undo_available()) {
        return undo_fail(error, "NO EARLIER VERSION IS KEPT ON THIS COMPUTER");
    }
    report_step(progress, "CHECKING THE TWO VERSIONS", 0);
    uint64_t newer;
    uint64_t older;
    if (!file_hash(KERNEL, &newer) || !file_hash(OLD_KERNEL, &older)) {
        return undo_fail(error, "COULD NOT READ THE SYSTEM FILES");
    }
    if (newer == older) {
        return undo_fail(error, "THE EARLIER VERSION IS THE SAME AS THIS ONE");
    }
    const struct copy_report copies[4] = {
        {progress, "PREPARING THE PREVIOUS VERSION", 2, 42},
        {progress, "PREPARING THE PREVIOUS VERSION", 44, 4},
        {progress, "SAVING THIS VERSION", 48, 42},
        {progress, "SAVING THIS VERSION", 90, 4},
    };
    if (!copy_verified(OLD_INITRAMFS, NEW_INITRAMFS, &copies[0])
        || !copy_verified(OLD_KERNEL, NEW_KERNEL, &copies[1])
        || !make_directory(UNDO_DIRECTORY)
        || !copy_verified(INITRAMFS, UNDO_INITRAMFS, &copies[2])
        || !copy_verified(KERNEL, UNDO_KERNEL, &copies[3])) {
        remove_undo_files();
        return undo_fail(error, "COPYING FAILED (IS THE DISK FULL?)."
                                " NOTHING CHANGED");
    }
    report_step(progress, "SWITCHING VERSIONS", 95);
    if (!write_state(STEP_SWAPPING, newer, older)) {
        remove_undo_files();
        return undo_fail(error, "COULD NOT WRITE TO THE DISK. NOTHING CHANGED");
    }
    if (!swap_in()) {
        return undo_fail(error, "SWITCHING FAILED. RESTART: OPOS FINISHES IT"
                                " WHEN IT STARTS");
    }
    if (!write_state(STEP_SAVING, newer, older) || !save_newer()) {
        return undo_fail(error, "THE PREVIOUS VERSION IS BACK, BUT THE NEWER"
                                " ONE COULD NOT BE KEPT");
    }
    remove_undo_files();
    report_step(progress, "DONE", 100);
    return true;
}

enum recovery_resume recovery_undo_resume(void) {
    if (!installed()) {
        return RECOVERY_RESUME_NONE;
    }
    if (!exists(UNDO_STATE)) {
        /* An undo cut short while copying: nothing switched yet. */
        if (exists(NEW_KERNEL) || exists(NEW_INITRAMFS)
            || exists(UNDO_KERNEL) || exists(UNDO_INITRAMFS)) {
            remove_undo_files();
        }
        return RECOVERY_RESUME_NONE;
    }
    bool saving;
    uint64_t newer;
    uint64_t older;
    bool known = read_state(&saving, &newer, &older) && running_kernel_known;
    if (!known
        || (running_kernel_hash != newer && running_kernel_hash != older)) {
        /* Left by a start of another version: nothing to finish. */
        remove_undo_files();
        return RECOVERY_RESUME_NONE;
    }
    enum recovery_resume result = RECOVERY_RESUME_DONE;
    if (!saving) {
        bool restart = running_kernel_hash == newer;
        if (!swap_in() || !write_state(STEP_SAVING, newer, older)) {
            return RECOVERY_RESUME_NONE;
        }
        if (restart) {
            result = RECOVERY_RESUME_RESTART;
        }
    }
    if (save_newer()) {
        remove_undo_files();
    }
    return result;
}

/* ------------------------------------------------------- rescue files */

#define RESCUE_PATH 512
#define RESCUE_DEPTH 24
#define USERS_ROOT "/disk/Users"

struct rescue_walk {
    char source[RESCUE_PATH];
    size_t source_length;
    char target[RESCUE_PATH];
    size_t target_length;
    size_t index[RESCUE_DEPTH];
    /* Path lengths to go back to when a level ends. */
    size_t source_mark[RESCUE_DEPTH];
    size_t target_mark[RESCUE_DEPTH];
};

static struct rescue_walk walk;
static char rescue_step[96];

static bool path_append(char *path, size_t *length, const char *part,
                        size_t part_length) {
    if (*length + 1 + part_length >= RESCUE_PATH) {
        return false;
    }
    path[(*length)++] = '/';
    for (size_t i = 0; i < part_length; i++) {
        path[(*length)++] = part[i];
    }
    path[*length] = '\0';
    return true;
}

static void set_text(char *buffer, size_t capacity, const char *a,
                     const char *b) {
    size_t length = 0;
    for (size_t i = 0; a[i] != '\0' && length + 1 < capacity; i++) {
        buffer[length++] = a[i];
    }
    for (size_t i = 0; b != NULL && b[i] != '\0' && length + 1 < capacity;
         i++) {
        buffer[length++] = b[i];
    }
    buffer[length] = '\0';
}

/* Copies one file; false when writing failed (a reading failure counts
   the file as unreadable and returns true). */
static bool rescue_copy_file(const char *from, size_t from_length,
                             const char *to, size_t to_length, size_t size,
                             struct recovery_rescue_result *result) {
    if (!vfs_create_file(to, to_length, true)) {
        return false;
    }
    for (size_t offset = 0; offset < size; offset += COPY_CHUNK) {
        size_t want = size - offset < COPY_CHUNK ? size - offset : COPY_CHUNK;
        size_t got = 0;
        if (!vfs_read_at(from, from_length, offset, chunk, want, &got)
            || got != want) {
            remove_file(to);
            result->unreadable++;
            return true;
        }
        size_t written = 0;
        if (!vfs_write_at(to, to_length, offset, chunk, want, &written)
            || written != want) {
            remove_file(to); /* no half file that looks complete */
            return false;
        }
    }
    result->copied++;
    result->bytes += size;
    return true;
}

static bool is_dot(const struct vfs_directory_entry *entry) {
    return (entry->name_length == 1 && entry->name[0] == '.')
        || (entry->name_length == 2 && entry->name[0] == '.'
            && entry->name[1] == '.');
}

/* Ends the current entry: back to the folder's own paths, next index. */
static void rescue_next(size_t depth) {
    walk.source[walk.source_length] = '\0';
    walk.target[walk.target_length] = '\0';
    walk.index[depth]++;
}

/*
 * Walks walk.source without recursion. With copy false it only adds up
 * *total_bytes; with copy true it copies into walk.target, reporting
 * done/total. Returns false when a write failed.
 */
static bool rescue_walk(bool copy, uint64_t *total_bytes, uint64_t total,
                        uint64_t *done, recovery_progress progress,
                        struct recovery_rescue_result *result) {
    size_t depth = 0;
    walk.index[0] = 0;
    for (;;) {
        size_t count = 0;
        if (!vfs_directory_count(walk.source, walk.source_length, &count)) {
            count = 0;
            if (copy && walk.index[depth] == 0) {
                result->unreadable++;
            }
        }
        if (walk.index[depth] >= count) {
            if (depth == 0) {
                return true;
            }
            depth--;
            walk.source_length = walk.source_mark[depth];
            walk.target_length = walk.target_mark[depth];
            rescue_next(depth);
            continue;
        }
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(walk.source, walk.source_length,
                                    walk.index[depth], &entry)
            || is_dot(&entry)) {
            walk.index[depth]++;
            continue;
        }
        size_t source_length = walk.source_length;
        size_t target_length = walk.target_length;
        if (!path_append(walk.source, &source_length, entry.name,
                         entry.name_length)
            || !path_append(walk.target, &target_length, entry.name,
                            entry.name_length)) {
            if (copy) {
                result->unreadable++;
            }
            rescue_next(depth);
            continue;
        }
        if (entry.type == VFS_NODE_DIRECTORY) {
            if (depth + 1 >= RESCUE_DEPTH) {
                if (copy) {
                    result->unreadable++;
                }
                rescue_next(depth);
                continue;
            }
            if (copy && !make_directory(walk.target)) {
                return false;
            }
            walk.source_mark[depth] = walk.source_length;
            walk.target_mark[depth] = walk.target_length;
            depth++;
            walk.index[depth] = 0;
            walk.source_length = source_length;
            walk.target_length = target_length;
            continue;
        }
        if (copy) {
            set_text(rescue_step, sizeof(rescue_step), "COPYING ",
                     walk.source + sizeof(USERS_ROOT));
            if (progress != NULL) {
                progress(rescue_step, total == 0 ? 0
                    : (unsigned)(*done * 100 / total));
            }
            if (!rescue_copy_file(walk.source, source_length, walk.target,
                                  target_length, entry.size, result)) {
                return false;
            }
            *done += entry.size;
        } else {
            *total_bytes += entry.size;
        }
        rescue_next(depth);
    }
}

static size_t append_number(char *text, size_t at, unsigned value,
                            unsigned digits) {
    for (unsigned i = digits; i > 0; i--) {
        text[at + i - 1] = (char)('0' + value % 10);
        value /= 10;
    }
    return at + digits;
}

bool recovery_rescue_files(const char *const *names, size_t count,
                           size_t volume, recovery_progress progress,
                           struct recovery_rescue_result *result,
                           const char **error) {
    *result = (struct recovery_rescue_result){0};
    if (count == 0) {
        return undo_fail(error, "NO ACCOUNT IS CHOSEN");
    }
    struct fat16_volume_info info;
    if (volume == 0 || volume >= FAT16_MAX_VOLUMES || volume > 8
        || !fat16_volume_info(volume, &info)) {
        return undo_fail(error, "THE DRIVE IS GONE. PLUG IT IN AGAIN");
    }
    /* "/disk2/OPOS-RESCUE-2026-11-16", then "-2" ... "-9" when taken. */
    char folder[48] = "/disk0/OPOS-RESCUE-";
    folder[5] = (char)('1' + volume);
    size_t at = text_length(folder);
    struct rtc_time now;
    if (!rtc_read(&now)) {
        now = (struct rtc_time){2000, 1, 1, 0, 0, 0};
    }
    at = append_number(folder, at, now.year, 4);
    folder[at++] = '-';
    at = append_number(folder, at, now.month, 2);
    folder[at++] = '-';
    at = append_number(folder, at, now.day, 2);
    folder[at] = '\0';
    size_t base = at;
    struct vfs_metadata metadata;
    for (char suffix = '2'; vfs_stat(folder, text_length(folder), &metadata);
         suffix++) {
        if (suffix > '9') {
            return undo_fail(error, "THE DRIVE ALREADY HAS 9 RESCUE FOLDERS"
                                    " FROM TODAY");
        }
        folder[base] = '-';
        folder[base + 1] = suffix;
        folder[base + 2] = '\0';
    }
    if (!make_directory(folder)) {
        return undo_fail(error, "CANNOT WRITE TO THE DRIVE");
    }
    set_text(result->folder, sizeof(result->folder), folder, NULL);

    if (progress != NULL) {
        progress("COUNTING FILES", 0);
    }
    uint64_t total = 0;
    uint64_t done = 0;
    for (size_t i = 0; i < count; i++) {
        set_text(walk.source, sizeof(walk.source), USERS_ROOT "/", names[i]);
        walk.source_length = text_length(walk.source);
        walk.target[0] = '\0';
        walk.target_length = 0;
        if (vfs_stat(walk.source, walk.source_length, &metadata)) {
            (void)rescue_walk(false, &total, 0, &done, NULL, result);
        }
    }
    for (size_t i = 0; i < count; i++) {
        set_text(walk.source, sizeof(walk.source), USERS_ROOT "/", names[i]);
        walk.source_length = text_length(walk.source);
        if (!vfs_stat(walk.source, walk.source_length, &metadata)) {
            continue; /* no home folder */
        }
        set_text(walk.target, sizeof(walk.target), folder, "/");
        size_t used = text_length(walk.target);
        set_text(walk.target + used, sizeof(walk.target) - used, names[i],
                 NULL);
        walk.target_length = text_length(walk.target);
        if (!make_directory(walk.target)
            || !rescue_walk(true, NULL, total, &done, progress, result)) {
            result->stopped = true;
            break;
        }
    }
    if (progress != NULL) {
        progress("DONE", 100);
    }
    return true;
}
