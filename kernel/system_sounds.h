#ifndef OPOS_SYSTEM_SOUNDS_H
#define OPOS_SYSTEM_SOUNDS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * System sounds (docs/notifications.md, step 1): short chimes made from
 * sine tones when they are played, so no sound files are needed. They play
 * through the mixer's system voice at the master volume, and only while
 * the account's SYSTEM SOUNDS setting is on.
 */

enum system_sound {
    SYSTEM_SOUND_STARTUP,
    SYSTEM_SOUND_SHUTDOWN,
    SYSTEM_SOUND_LOGOUT,
    SYSTEM_SOUND_NOTIFICATION,
    SYSTEM_SOUND_WARNING,
    SYSTEM_SOUND_VOLUME,
    SYSTEM_SOUND_COUNT
};

/* Starts sound (replacing one playing). False when it does not play: no
   sound card, system sounds off, or an unknown sound. */
bool system_sound_play(enum system_sound sound);

/* Same, ignoring the SYSTEM SOUNDS setting (kernel shell "chime"). */
bool system_sound_play_always(enum system_sound sound);

/* Waits until the sound playing ends, at most milliseconds (before the
   machine shuts down or restarts). */
void system_sound_wait(uint32_t milliseconds);

/* Names for the shell: "startup", "shutdown", ... NULL past the end. */
const char *system_sound_name(enum system_sound sound);

#endif
