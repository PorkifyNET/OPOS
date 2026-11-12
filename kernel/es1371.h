#ifndef OPOS_ES1371_H
#define OPOS_ES1371_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Ensoniq/Creative AudioPCI 97 (ES1371, CT5880, EV1938) output
 * (docs/sound.md): VMware's default sound card for many guest types. Like
 * the HDA driver it plays a ring of 48 kHz, 16-bit stereo samples that
 * es1371_poll, called by the 100 Hz timer, refills from a source.
 */

/* Finds and starts the first such card. False when there is none. */
bool es1371_initialize(void);
bool es1371_ready(void);
void es1371_poll(void);
void es1371_set_source(void (*source)(int16_t *out, size_t frames));
void es1371_describe(void (*print)(const char *line));

#endif
