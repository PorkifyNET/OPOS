#ifndef OPOS_RECOVERY_H
#define OPOS_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * OPOS Recovery (docs/recovery.md): why it opened, and the count of
 * starts that never finished, kept in /disk/OPOS/BOOT-ATTEMPTS.
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

#endif
