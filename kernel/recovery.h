#ifndef OPOS_RECOVERY_H
#define OPOS_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * OPOS Recovery (docs/recovery.md): why it opened, the count of starts
 * that never finished (/disk/OPOS/BOOT-ATTEMPTS), and undoing the last
 * update.
 */

enum recovery_reason {
    RECOVERY_NONE,
    /* "OPOS Recovery" in the boot menu (opos.recovery). */
    RECOVERY_REQUESTED,
    /* RECOVERY_FAILED_STARTS_LIMIT starts in a row did not finish. */
    RECOVERY_FAILED_STARTS,
    /* The disk system root failed verification or its pin. */
    RECOVERY_SYSTEM_FILES,
};

/* Starts that did not finish before Recovery opens by itself. */
#define RECOVERY_FAILED_STARTS_LIMIT 3u

/* Counts this start on an installed system (after the boot volume is
   mounted). Returns how many earlier starts did not finish. */
uint32_t recovery_count_start(void);

/* This start finished: the desktop ran for a while after a login, or
   OPOS is restarting or shutting down on request. */
void recovery_start_finished(void);

void recovery_set_reason(enum recovery_reason reason);
enum recovery_reason recovery_reason(void);
/* The failed-start count recovery_count_start returned. */
uint32_t recovery_failed_starts(void);

/* Progress of a long Recovery job: what it does, 0-100. */
typedef void (*recovery_progress)(const char *step, unsigned percent);

/* The kernel file Limine started (to tell which version is running). */
void recovery_set_running_kernel(const void *image, size_t size);

/* Whether /disk/boot/old holds an earlier kernel and initramfs to go back
   to, with no update half-installed. */
bool recovery_undo_available(void);

/*
 * Swaps the current kernel/initramfs pair with the one in /disk/boot/old,
 * so the next start runs the version from before the last update (and
 * "OPOS (previous version)" then starts the newer one). The newer pair is
 * kept in /disk/boot/undo until the swap is complete, and the step is
 * recorded in /disk/OPOS/UNDO-STATE so a power cut can be finished at the
 * next start. On failure sets *error to a short reason.
 */
bool recovery_undo_update(recovery_progress progress, const char **error);

enum recovery_resume {
    RECOVERY_RESUME_NONE,
    /* An interrupted undo was finished; this start can carry on. */
    RECOVERY_RESUME_DONE,
    /* It was finished, but the running kernel is the one that was
       replaced: restart. */
    RECOVERY_RESUME_RESTART,
};

/* Finishes an undo cut short by a power loss (after the disk mounts,
   before the system root is chosen). */
enum recovery_resume recovery_undo_resume(void);

/* ------------------------------------------------------- rescue files */

struct recovery_rescue_result {
    size_t copied;
    /* Files that could not be read (skipped). */
    size_t unreadable;
    uint64_t bytes;
    /* Writing failed (drive full or removed): copying stopped there. */
    bool stopped;
    /* Where the copies went, e.g. "/disk2/OPOS-RESCUE-2026-11-16". */
    char folder[48];
};

/*
 * Copies the home folders (/disk/Users/<name>) of the `count` accounts in
 * `names` to a new OPOS-RESCUE-<date> folder on extra volume `volume`
 * (1..FAT16_MAX_VOLUMES-1, mounted at /disk<volume+1>). Unreadable files
 * are skipped and counted; a failed write stops the copy. Returns false
 * (with *error) only when nothing could start.
 */
bool recovery_rescue_files(const char *const *names, size_t count,
                           size_t volume, recovery_progress progress,
                           struct recovery_rescue_result *result,
                           const char **error);

/* ------------------------------------------- settings, next start */

/* Machine settings, every account's personal settings and desktop look
   (DESKTOP.CFG: background, theme, taskbar) back to their defaults.
   Accounts, passwords, files and programs stay. */
bool recovery_reset_settings(void);

/* "Restart into Recovery": the next start opens it (one time). */
bool recovery_request_next_start(void);
/* Whether that was asked for; clears it. Installed systems only. */
bool recovery_take_requested(void);

/* ------------------------------------------------------ start-up log */

/*
 * Everything the kernel writes to the serial port is also kept in memory
 * (the first RECOVERY_LOG_CAPACITY bytes of this start) and, on installed
 * systems, saved to /disk/OPOS/LOG-THIS.TXT; the previous start's log is
 * moved to LOG-LAST.TXT. Recovery shows both.
 */
#define RECOVERY_LOG_CAPACITY (64u * 1024u)
void recovery_log_put(char character);
/* Rotates the files and starts saving (after the boot volume mounts). */
void recovery_log_open(void);
/* Writes what was logged since the last call (cheap when nothing was). */
void recovery_log_flush(void);
/* This start's log so far. */
const char *recovery_log_text(size_t *length);
/* The previous start's log, read into `buffer`; false when there is none. */
bool recovery_log_previous(char *buffer, size_t capacity, size_t *length);

#endif
