#ifndef OPOS_HDA_H
#define OPOS_HDA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Intel High Definition Audio output (docs/sound.md). The controller plays
 * a ring of 48 kHz, 16-bit stereo samples; hda_poll, called by the 100 Hz
 * timer, writes new samples behind the playing position.
 */

#define HDA_SAMPLE_RATE 48000u
#define HDA_CHANNELS 2u

/* Finds and starts the first HDA controller with a usable output.
   Returns false when there is none (OPOS then has no sound). */
bool hda_initialize(void);
bool hda_ready(void);

/* From the timer interrupt: refills the ring. */
void hda_poll(void);

/*
 * Where samples come from: fills frames stereo frames (left, right
 * interleaved) at out. The default source writes silence. Called from the
 * timer interrupt, so it must be quick and must not sleep.
 */
typedef void (*hda_source)(int16_t *out, size_t frames);
void hda_set_source(hda_source source);

/* Frames played since start, for tests and the mixer. */
uint64_t hda_frames_played(void);

void hda_describe(void (*print)(const char *line));

#endif
