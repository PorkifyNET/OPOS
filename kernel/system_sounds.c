#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appearance.h"
#include "sound.h"
#include "system_sounds.h"
#include "timer.h"

/*
 * Each sound is a few notes: a sine with a softer second harmonic, a short
 * linear attack and an exponential decay. They are rendered into one
 * buffer when played (about 1 ms of work per 100 ms of sound), which the
 * mixer's system voice then plays. The kernel has no floating point, so
 * everything is fixed point.
 */

#define RATE 48000u
#define BUFFER_MS 1300u
#define BUFFER_FRAMES (RATE / 1000u * BUFFER_MS)
#define ATTACK_FRAMES 240u  /* 5 ms */
#define RELEASE_FRAMES 480u /* 10 ms fade at a note's end */
#define TABLE_BITS 10u
#define TABLE_SIZE (1u << TABLE_BITS)

struct note {
    uint16_t frequency;   /* Hz */
    uint16_t start;       /* ms from the sound's start */
    uint16_t length;      /* ms */
    uint16_t half_life;   /* ms for the level to halve after the attack */
    uint16_t level;       /* peak, in 1/1000 of full scale */
};

struct sound_shape {
    const char *name;
    const struct note *notes;
    size_t count;
};

/* Major-chord arpeggios up for hello and down for goodbye, a two-note
   chime for notifications, a falling minor third for warnings. */
static const struct note startup_notes[] = {
    {523, 0, 700, 180, 260},   /* C5 */
    {659, 110, 700, 180, 260}, /* E5 */
    {784, 220, 700, 180, 260}, /* G5 */
    {1047, 330, 900, 260, 280}, /* C6 */
};
static const struct note shutdown_notes[] = {
    {784, 0, 600, 160, 260},   /* G5 */
    {659, 140, 600, 160, 260}, /* E5 */
    {523, 280, 900, 260, 280}, /* C5 */
};
static const struct note logout_notes[] = {
    {659, 0, 500, 140, 260},   /* E5 */
    {523, 120, 700, 200, 260}, /* C5 */
};
static const struct note notification_notes[] = {
    {880, 0, 500, 120, 240},   /* A5 */
    {1319, 90, 700, 180, 240}, /* E6 */
};
static const struct note warning_notes[] = {
    {494, 0, 300, 80, 300},    /* B4 */
    {392, 150, 500, 110, 320}, /* G4 */
};
static const struct note volume_notes[] = {
    {1600, 0, 45, 12, 220},
};

#define SHAPE(name, notes) {name, notes, sizeof(notes) / sizeof(notes[0])}
static const struct sound_shape shapes[SYSTEM_SOUND_COUNT] = {
    [SYSTEM_SOUND_STARTUP] = SHAPE("startup", startup_notes),
    [SYSTEM_SOUND_SHUTDOWN] = SHAPE("shutdown", shutdown_notes),
    [SYSTEM_SOUND_LOGOUT] = SHAPE("logout", logout_notes),
    [SYSTEM_SOUND_NOTIFICATION] = SHAPE("notification", notification_notes),
    [SYSTEM_SOUND_WARNING] = SHAPE("warning", warning_notes),
    [SYSTEM_SOUND_VOLUME] = SHAPE("volume", volume_notes),
};

static int16_t sine[TABLE_SIZE];
static bool sine_ready;
static int32_t accumulator[BUFFER_FRAMES];
static int16_t buffer[BUFFER_FRAMES];

/* Bhaskara I's approximation, within 0.2% of sin over a half wave. */
static void sine_build(void) {
    const int64_t half = TABLE_SIZE / 2;
    for (int64_t i = 0; i < (int64_t)TABLE_SIZE; i++) {
        int64_t p = i < half ? i : i - half;
        int64_t product = p * (half - p);
        int64_t value = 32767 * 16 * product / (5 * half * half - 4 * product);
        sine[i] = (int16_t)(i < half ? value : -value);
    }
    sine_ready = true;
}

static void render_note(const struct note *note) {
    uint32_t start = RATE / 1000u * note->start;
    uint32_t length = RATE / 1000u * note->length;
    if (start >= BUFFER_FRAMES) {
        return;
    }
    if (length > BUFFER_FRAMES - start) {
        length = BUFFER_FRAMES - start;
    }
    uint32_t step = (uint32_t)(((uint64_t)note->frequency << 32) / RATE);
    uint32_t phase = 0;
    /* Envelope in Q30; each frame after the attack multiplies it by
       1 - ln 2 / half-life frames. */
    const int64_t one = 1ll << 30;
    uint64_t half_life = (uint64_t)RATE / 1000u * note->half_life;
    int64_t decay = one - (int64_t)(744261117ull / (half_life ? half_life : 1));
    int64_t envelope = one;
    int64_t peak = (int64_t)note->level * 32767 / 1000;
    for (uint32_t i = 0; i < length; i++) {
        int64_t level;
        if (i < ATTACK_FRAMES) {
            level = one * i / ATTACK_FRAMES;
        } else {
            envelope = envelope * decay >> 30;
            level = envelope;
        }
        if (length - i < RELEASE_FRAMES) {
            level = level * (length - i) / RELEASE_FRAMES;
        }
        int32_t tone = sine[phase >> (32 - TABLE_BITS)] * 4
                     + sine[(phase * 2) >> (32 - TABLE_BITS)];
        /* tone peaks near 5 * 32767; scale to the note's level. */
        accumulator[start + i] +=
            (int32_t)((int64_t)tone * peak / (5 * 32767) * level >> 30);
        phase += step;
    }
}

static bool play(enum system_sound sound) {
    if (sound >= SYSTEM_SOUND_COUNT || !sound_output_ready()) {
        return false;
    }
    /* The voice may be reading the buffer: stop it before rendering. */
    sound_voice_stop();
    if (!sine_ready) {
        sine_build();
    }
    const struct sound_shape *shape = &shapes[sound];
    uint32_t frames = 0;
    for (size_t i = 0; i < shape->count; i++) {
        uint32_t end = RATE / 1000u
                     * (uint32_t)(shape->notes[i].start + shape->notes[i].length);
        frames = end > frames ? end : frames;
    }
    frames = frames < BUFFER_FRAMES ? frames : BUFFER_FRAMES;
    for (uint32_t i = 0; i < frames; i++) {
        accumulator[i] = 0;
    }
    for (size_t i = 0; i < shape->count; i++) {
        render_note(&shape->notes[i]);
    }
    for (uint32_t i = 0; i < frames; i++) {
        int32_t value = accumulator[i];
        buffer[i] = (int16_t)(value > 32767 ? 32767
                              : value < -32768 ? -32768 : value);
    }
    return sound_voice_play(buffer, frames);
}

bool system_sound_play(enum system_sound sound) {
    return appearance_system_sounds() && play(sound);
}

bool system_sound_play_always(enum system_sound sound) {
    return play(sound);
}

void system_sound_wait(uint32_t milliseconds) {
    /* The timer runs at 100 Hz and drives the mixer. */
    uint64_t end = timer_ticks() + (milliseconds + 9) / 10;
    while (sound_voice_playing() && timer_ticks() < end) {
        __asm__ volatile ("pause");
    }
}

const char *system_sound_name(enum system_sound sound) {
    return sound < SYSTEM_SOUND_COUNT ? shapes[sound].name : NULL;
}
