#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmos.h"
#include "hda.h"
#include "memory.h"
#include "sound.h"

/*
 * The sound mixer. Each stream is a ring of frames (left and right 16-bit
 * samples) that one program fills through the sound calls and the timer
 * interrupt drains (hda_poll calls sound_mix). The program only moves
 * write, the mixer only moves read, so a stream needs no lock while it is
 * open; opening and closing change the table with interrupts off. A stream
 * closed while another thread of its program copies into it is marked
 * closing and freed when that copy ends.
 */

#define SOUND_STREAMS 8u

struct sound_stream {
    bool used;
    bool closing;
    uint32_t writers;
    uint64_t owner;
    int64_t handle;
    uint32_t mask;            /* capacity - 1; capacity is a power of 2 */
    volatile uint32_t read;   /* frames, counted modulo 2^32 */
    volatile uint32_t write;
    int16_t *samples;
    uint64_t physical;
    uint32_t pages;
};

static struct sound_stream streams[SOUND_STREAMS];
static int64_t next_handle = 1;
static volatile uint32_t volume = SOUND_VOLUME_DEFAULT;
static volatile bool muted;
/* Mixed in 32 bits before clamping; one HDA buffer at most. */
static int32_t mix[1024 * 2];

static uint64_t interrupts_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupts_restore(uint64_t flags) {
    if ((flags & 0x200u) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static struct sound_stream *stream_of(uint64_t owner, int64_t handle) {
    for (size_t i = 0; i < SOUND_STREAMS; i++) {
        if (streams[i].used && !streams[i].closing
            && streams[i].owner == owner && streams[i].handle == handle) {
            return &streams[i];
        }
    }
    return NULL;
}

/* From the timer interrupt, through hda_poll. */
static void sound_mix(int16_t *out, size_t frames) {
    while (frames != 0) {
        size_t chunk = frames < 1024 ? frames : 1024;
        for (size_t i = 0; i < chunk * 2; i++) {
            mix[i] = 0;
        }
        for (size_t s = 0; s < SOUND_STREAMS; s++) {
            struct sound_stream *stream = &streams[s];
            if (!stream->used || stream->closing) {
                continue;
            }
            uint32_t available = stream->write - stream->read;
            uint32_t take = available < chunk ? available : (uint32_t)chunk;
            uint32_t read = stream->read;
            for (uint32_t i = 0; i < take; i++) {
                uint32_t at = ((read + i) & stream->mask) * 2;
                mix[2 * i] += stream->samples[at];
                mix[2 * i + 1] += stream->samples[at + 1];
            }
            stream->read = read + take;
        }
        /* Volume on a squared curve: half way sounds about half as loud. */
        int32_t gain = muted ? 0 : (int32_t)(volume * volume);
        for (size_t i = 0; i < chunk * 2; i++) {
            int32_t value = (int32_t)((int64_t)mix[i] * gain / 10000);
            out[i] = (int16_t)(value > 32767 ? 32767
                               : value < -32768 ? -32768 : value);
        }
        out += chunk * 2;
        frames -= chunk;
    }
}

void sound_initialize(void) {
    uint32_t saved = settings_sound_volume();
    volume = saved <= 100 ? saved : SOUND_VOLUME_DEFAULT;
    if (hda_ready()) {
        hda_set_source(sound_mix);
    }
}

int64_t sound_open(uint64_t owner, uint32_t capacity) {
    if (!hda_ready()) {
        return -19;
    }
    if (capacity == 0 || capacity > SOUND_STREAM_MAX_FRAMES) {
        return -22;
    }
    uint32_t frames = SOUND_STREAM_MIN_FRAMES;
    while (frames < capacity) {
        frames *= 2;
    }
    uint32_t pages = frames * 4 / 4096;
    uint64_t physical = physical_pages_alloc_contiguous(pages);
    if (physical == 0) {
        return -12;
    }
    int16_t *samples = physical_page_to_hhdm(physical);
    uint64_t flags = interrupts_save();
    struct sound_stream *free_stream = NULL;
    for (size_t i = 0; i < SOUND_STREAMS && free_stream == NULL; i++) {
        if (!streams[i].used) {
            free_stream = &streams[i];
        }
    }
    int64_t handle = -24;
    if (free_stream != NULL) {
        handle = next_handle++;
        *free_stream = (struct sound_stream){
            .used = true, .owner = owner, .handle = handle,
            .mask = frames - 1, .samples = samples,
            .physical = physical, .pages = pages,
        };
    }
    interrupts_restore(flags);
    if (free_stream == NULL) {
        for (uint32_t i = 0; i < pages; i++) {
            (void)physical_page_free(physical + (uint64_t)i * 4096);
        }
    }
    return handle;
}

static void release(struct sound_stream *stream);

int64_t sound_queue(uint64_t owner, int64_t handle, const int16_t *samples,
                    uint32_t frames) {
    uint64_t flags = interrupts_save();
    struct sound_stream *stream = stream_of(owner, handle);
    if (stream != NULL) {
        stream->writers++;
    }
    interrupts_restore(flags);
    if (stream == NULL) {
        return -9;
    }
    uint32_t write = stream->write;
    uint32_t space = stream->mask + 1 - (write - stream->read);
    uint32_t count = frames < space ? frames : space;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = ((write + i) & stream->mask) * 2;
        stream->samples[at] = samples[2 * i];
        stream->samples[at + 1] = samples[2 * i + 1];
    }
    __asm__ volatile ("" : : : "memory");
    stream->write = write + count;
    flags = interrupts_save();
    bool free_now = --stream->writers == 0 && stream->closing;
    interrupts_restore(flags);
    if (free_now) {
        release(stream);
    }
    return count;
}

int64_t sound_queued(uint64_t owner, int64_t handle) {
    struct sound_stream *stream = stream_of(owner, handle);
    return stream == NULL ? -9 : (int64_t)(stream->write - stream->read);
}

int64_t sound_capacity(uint64_t owner, int64_t handle) {
    struct sound_stream *stream = stream_of(owner, handle);
    return stream == NULL ? -9 : (int64_t)stream->mask + 1;
}

static void release(struct sound_stream *stream) {
    uint64_t flags = interrupts_save();
    uint64_t physical = stream->physical;
    uint32_t pages = stream->pages;
    *stream = (struct sound_stream){0};
    interrupts_restore(flags);
    for (uint32_t i = 0; i < pages; i++) {
        (void)physical_page_free(physical + (uint64_t)i * 4096);
    }
}

/* Frees stream now, or marks it closing while a copy is under way. */
static void close_stream(struct sound_stream *stream) {
    uint64_t flags = interrupts_save();
    stream->closing = true;
    bool free_now = stream->writers == 0;
    interrupts_restore(flags);
    if (free_now) {
        release(stream);
    }
}

int64_t sound_close(uint64_t owner, int64_t handle) {
    struct sound_stream *stream = stream_of(owner, handle);
    if (stream == NULL) {
        return -9;
    }
    close_stream(stream);
    return 0;
}

void sound_process_exited(uint64_t owner) {
    for (size_t i = 0; i < SOUND_STREAMS; i++) {
        if (streams[i].used && !streams[i].closing
            && streams[i].owner == owner) {
            close_stream(&streams[i]);
        }
    }
}

uint32_t sound_volume(void) {
    return volume;
}

void sound_set_volume(uint32_t new_volume) {
    if (new_volume > 100) {
        new_volume = 100;
    }
    volume = new_volume;
    settings_set_sound_volume((uint8_t)new_volume);
}

bool sound_muted(void) {
    return muted;
}

void sound_set_muted(bool new_muted) {
    muted = new_muted;
}

size_t sound_stream_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < SOUND_STREAMS; i++) {
        count += streams[i].used && !streams[i].closing ? 1 : 0;
    }
    return count;
}
