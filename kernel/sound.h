#ifndef OPOS_SOUND_H
#define OPOS_SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The sound mixer (docs/sound.md, step 2). Programs open streams of
 * 48 kHz, 16-bit stereo samples; the timer mixes every stream, applies
 * the master volume and hands the result to the HDA driver.
 */

#define SOUND_STREAM_MIN_FRAMES 1024u
#define SOUND_STREAM_MAX_FRAMES 32768u
#define SOUND_VOLUME_DEFAULT 70u

/* Finds the sound card (HDA, else AudioPCI 97) and connects the mixer to
   it. Logs "no sound output found" without one. */
void sound_initialize(void);

/* Whether a sound card plays. */
bool sound_output_ready(void);

/* From the timer interrupt: refills the card's ring. */
void sound_poll(void);

/* Plays a square wave of frequency Hz (20-20000) for milliseconds over
   everything else (kernel shell "beep"). False without sound or for a
   frequency out of range. */
bool sound_beep(uint32_t frequency, uint32_t milliseconds);

/* What the drivers found (kernel shell "sound"). */
void sound_describe(void (*print)(const char *line));

/* Opens a stream for owner (a process id) holding about capacity frames
   (rounded up to a power of two, 1024 to 32768). Returns a handle (1 or
   more) or a negative error: -19 without sound hardware, -24 when every
   stream is in use, -12 without memory, -22 for a bad capacity. */
int64_t sound_open(uint64_t owner, uint32_t capacity);

/* Queues up to frames frames from samples (left, right interleaved).
   Returns how many were queued (fewer when the stream is full), or -9 for
   a handle owner does not have. Never blocks. */
int64_t sound_queue(uint64_t owner, int64_t handle, const int16_t *samples,
                    uint32_t frames);

/* Frames queued and not played yet, the stream's capacity, or -9. */
int64_t sound_queued(uint64_t owner, int64_t handle);
int64_t sound_capacity(uint64_t owner, int64_t handle);

/* Closes a stream; what is still queued is dropped. */
int64_t sound_close(uint64_t owner, int64_t handle);

/* Closes every stream of owner (a process ended). */
void sound_process_exited(uint64_t owner);

/* Master volume, 0-100, kept in the settings. Muting keeps the volume. */
uint32_t sound_volume(void);
void sound_set_volume(uint32_t volume);
bool sound_muted(void);
void sound_set_muted(bool muted);

/* Streams open now (for "sound" in the shell). */
size_t sound_stream_count(void);

#endif
