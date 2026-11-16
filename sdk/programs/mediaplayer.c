/*
 * Media Player: plays music and videos.
 *
 * Music: WAV, MP3, FLAC and Ogg Vorbis. Video: MPEG-1 (.mpg, .mpeg)
 * with MPEG audio. Files open through the Files picker and go into a
 * playlist; every file picked stays open, because the picker lets a
 * program reopen only the file picked last.
 *
 * Decoders: dr_wav, dr_mp3 and dr_flac (David Reid, public domain or
 * MIT-0), stb_vorbis (Sean Barrett, public domain or MIT) and pl_mpeg
 * (Dominic Szablewski, MIT), unchanged in sdk/third_party.
 *
 * Sound: every decoder gives 16-bit samples at the file's own rate,
 * which are mixed down to stereo and resampled (linear) to OPOS's
 * 48 kHz. The sound queue is topped up from the event loop, and the
 * playing position is what OPOS has really played: written frames
 * minus those still queued. Videos follow that clock, so pictures stay
 * in step with the sound; a video without sound follows the system
 * clock.
 *
 * Mouse: the buttons, the position bar, the volume bar, the playlist
 * (double-click plays, the wheel scrolls). Keys: Space play/pause,
 * Left/Right 5 s back/forward (Ctrl: 30 s), Up/Down volume, M mute,
 * N next, P previous, S stop, O or Ctrl+O open, R repeat, H shuffle,
 * L playlist, F full screen (Esc leaves it), Enter plays the chosen
 * playlist entry, Delete removes it.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_gfx.h>

/* ------------------------------------------------- third-party decoders */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wshift-negative-value"

#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#define DR_WAV_NO_WCHAR
#include "../third_party/dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../third_party/dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#include "../third_party/dr_flac.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"

#define PL_MPEG_IMPLEMENTATION
#define PLM_NO_STDIO
#include "../third_party/pl_mpeg.h"

#pragma GCC diagnostic pop

OPOS_MANIFEST("Media Player", "1.0", 0);

/* ------------------------------------------------------------- layout */

#define BAR_H 84           /* position bar and buttons */
#define LIST_W 280         /* playlist panel */
#define ROW_H 22
#define BUTTON_H 32
#define MIN_W 560
#define MIN_H 360

#define UI_BACK OPOS_RGB(18, 20, 28)
#define UI_PANEL OPOS_RGB(30, 34, 46)
#define UI_BUTTON OPOS_RGB(48, 56, 76)
#define UI_HOVER OPOS_RGB(66, 76, 104)
#define UI_ACTIVE OPOS_RGB(70, 110, 190)
#define UI_TEXT OPOS_RGB(230, 236, 248)
#define UI_DIM OPOS_RGB(130, 140, 165)
#define UI_ACCENT OPOS_RGB(110, 170, 255)
#define UI_WARN OPOS_RGB(245, 205, 110)

/* -------------------------------------------------------------- items */

#define MAX_ITEMS 48     /* each keeps a file open; programs get 64 */
#define PATH_LEN 256
#define AUDIO_FILE_MAX (512u * 1024 * 1024)

enum kind {
    KIND_UNKNOWN,
    KIND_WAV,
    KIND_MP3,
    KIND_FLAC,
    KIND_VORBIS,
    KIND_MPEG, /* MPEG-1 program stream: video and/or MP2 audio */
};

static const char *const kind_names[] = {
    "?", "WAV", "MP3", "FLAC", "OGG VORBIS", "MPEG-1 VIDEO",
};

struct item {
    char path[PATH_LEN];
    char name[64];
    int fd;
    uint64_t size;
    enum kind kind;
    double duration; /* seconds; < 0 until known */
};

static struct item items[MAX_ITEMS];
static int item_count;
static int current = -1;  /* the item loaded in the player */
static int selected = -1; /* the playlist's chosen row */
static int list_scroll;

/* ------------------------------------------------------------- player */

enum repeat_mode { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

static bool playing;   /* started and not paused */
static bool loaded;    /* a source is open */
static bool at_end;    /* the decoder ran out; waiting for the queue */
static enum repeat_mode repeat_mode;
static bool shuffle;
static int volume = 80; /* 0..100 */
static bool muted;
static bool show_list = true;
static bool fullscreen;
static char status[96];

/* Audio sources. */
static uint8_t *file_data; /* the whole file, for the music decoders */
static size_t file_data_size;
static drwav wav;
static drmp3 mp3;
static drflac *flac;
static stb_vorbis *vorbis;
static uint32_t src_rate;
static uint32_t src_channels;
static uint64_t src_total; /* frames, 0 if unknown */

/* MPEG-1 (pl_mpeg) */
static plm_t *plm;
static bool plm_audio;   /* the stream has sound we play */
static bool plm_video;
static plm_frame_t *pending_frame; /* decoded, not yet due */
static uint32_t *video_pixels;     /* the frame on screen (0x00RRGGBB) */
static int video_w, video_h;
static bool video_shown;
static int64_t stream_fd = -1;
static uint64_t stream_size;
static uint64_t stream_pos;

/* Output (48 kHz stereo). */
#define SOUND_QUEUE 8192   /* frames OPOS may hold: about 0.17 s */
#define SOUND_LOW 6144     /* top up below this */
#define CHUNK 1024
static long sound = -1;    /* stream handle, or negative error */
static uint64_t written;   /* frames written since base_time */
static double base_time;   /* media time of the first written frame */
static long wall_start_ms; /* system-clock start (no sound / no audio) */
static double wall_base;

/* The resampler: source frames (stereo) with a fractional position. */
#define SRC_FRAMES 4096
static int16_t src_buf[(SRC_FRAMES + 1) * 2];
static int src_count;   /* frames in src_buf */
static double src_pos;  /* position in src_buf */
static bool src_done;   /* the decoder has no more */

/* Scope: the last output frames, as mono, for the picture. */
#define SCOPE 512
static int16_t scope[SCOPE];
static int scope_at;

/* ------------------------------------------------------------- canvas */

static struct opos_canvas canvas;
static int view_w, view_h;
static int mouse_x = -1, mouse_y = -1;
static bool dirty = true;
static enum { DRAG_NONE, DRAG_SEEK, DRAG_VOLUME } drag;
static double drag_time; /* where a seek drag points */
static long last_click_ms;
static int last_click_row = -1;

static uint64_t random_state = 0x9e3779b97f4a7c15ull;

static uint32_t random_below(uint32_t limit) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return limit == 0 ? 0 : (uint32_t)(random_state % limit);
}

static long now_ms(void) {
    struct opos_timespec time;
    if (opos_uptime(&time) != 0) {
        return 0;
    }
    return time.seconds * 1000 + time.nanoseconds / 1000000;
}

static void set_status(const char *text) {
    strncpy(status, text, sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
    dirty = true;
}

/* -------------------------------------------------------------- files */

static int64_t file_seek(int fd, int64_t offset, int whence) {
    return opos_syscall3(8, fd, offset, whence);
}

/* Reads up to length bytes (several calls: OPOS gives at most 4 KiB a
   call). Returns the bytes read, or a negative error. */
static int64_t file_read(int fd, void *buffer, size_t length) {
    size_t done = 0;
    while (done < length) {
        long got = opos_read(fd, (uint8_t *)buffer + done,
                             length - done > 65536 ? 65536 : length - done);
        if (got < 0) {
            return done != 0 ? (int64_t)done : got;
        }
        if (got == 0) {
            break;
        }
        done += (size_t)got;
    }
    return (int64_t)done;
}

static bool ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text);
    size_t b = strlen(suffix);
    if (b > a) {
        return false;
    }
    for (size_t i = 0; i < b; i++) {
        char c = text[a - b + i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != suffix[i]) {
            return false;
        }
    }
    return true;
}

/* What a file holds: by its first bytes, else by its name. */
static enum kind probe(int fd, const char *path) {
    uint8_t head[16] = {0};
    if (file_seek(fd, 0, 0) == 0) {
        (void)file_read(fd, head, sizeof(head));
    }
    if (memcmp(head, "RIFF", 4) == 0 && memcmp(head + 8, "WAVE", 4) == 0) {
        return KIND_WAV;
    }
    if (memcmp(head, "RF64", 4) == 0 || memcmp(head, "riff", 4) == 0) {
        return KIND_WAV;
    }
    if (memcmp(head, "fLaC", 4) == 0) {
        return KIND_FLAC;
    }
    if (memcmp(head, "OggS", 4) == 0) {
        return KIND_VORBIS;
    }
    if (head[0] == 0 && head[1] == 0 && head[2] == 1
        && (head[3] == 0xba || head[3] == 0xb3)) {
        return KIND_MPEG;
    }
    if (memcmp(head, "ID3", 3) == 0) {
        return KIND_MP3;
    }
    if (head[0] == 0xff && (head[1] & 0xe0) == 0xe0) {
        return KIND_MP3;
    }
    if (ends_with(path, ".mp3")) {
        return KIND_MP3;
    }
    if (ends_with(path, ".wav")) {
        return KIND_WAV;
    }
    if (ends_with(path, ".flac")) {
        return KIND_FLAC;
    }
    if (ends_with(path, ".ogg") || ends_with(path, ".oga")) {
        return KIND_VORBIS;
    }
    if (ends_with(path, ".mpg") || ends_with(path, ".mpeg")) {
        return KIND_MPEG;
    }
    return KIND_UNKNOWN;
}

/* The file name without its folders, in capitals for the font. */
static void display_name(const char *path, char *name, size_t capacity) {
    const char *slash = strrchr(path, '/');
    const char *base = slash != NULL ? slash + 1 : path;
    size_t length = 0;
    for (; base[length] != '\0' && length + 1 < capacity; length++) {
        char c = base[length];
        name[length] = c >= 32 && c < 127 ? c : '?';
    }
    name[length] = '\0';
}

/* ------------------------------------------------------------- sound */

static void sound_reset(double media_time) {
    if (sound >= 0) {
        opos_sound_close(sound);
    }
    sound = opos_sound_open(SOUND_QUEUE);
    written = 0;
    base_time = media_time;
    wall_base = media_time;
    wall_start_ms = now_ms();
    src_count = 0;
    src_pos = 0;
    src_done = false;
    at_end = false;
}

/* Whether the clock comes from the sound OPOS plays. */
static bool clock_from_sound(void) {
    if (sound < 0 || !loaded) {
        return false;
    }
    return items[current].kind != KIND_MPEG || plm_audio;
}

/* Where playback is, in seconds. */
static double media_time(void) {
    if (!loaded) {
        return 0;
    }
    if (clock_from_sound()) {
        long queued = opos_sound_queued(sound);
        if (queued < 0) {
            queued = 0;
        }
        uint64_t played = written > (uint64_t)queued ? written - queued : 0;
        return base_time + (double)played / OPOS_SOUND_RATE;
    }
    if (!playing) {
        return wall_base;
    }
    return wall_base + (double)(now_ms() - wall_start_ms) / 1000.0;
}

/* Converts n frames of `channels` interleaved samples to stereo. */
static void to_stereo(int16_t *out, const int16_t *in, size_t frames,
                      uint32_t channels) {
    for (size_t i = 0; i < frames; i++) {
        if (channels == 1) {
            out[2 * i] = out[2 * i + 1] = in[i];
        } else {
            out[2 * i] = in[i * channels];
            out[2 * i + 1] = in[i * channels + 1];
        }
    }
}

static int16_t clamp16(float value) {
    if (value > 32767.0f) {
        return 32767;
    }
    if (value < -32768.0f) {
        return -32768;
    }
    return (int16_t)value;
}

/* Fills out with up to max stereo frames from the open source. */
static size_t source_read(int16_t *out, size_t max) {
    static int16_t raw[SRC_FRAMES * 8];
    size_t got = 0;
    uint32_t channels = src_channels == 0 ? 2 : src_channels;
    size_t room = sizeof(raw) / sizeof(raw[0]) / (channels > 8 ? 8 : channels);
    if (max > room) {
        max = room;
    }
    if (channels > 8) {
        return 0;
    }
    switch (items[current].kind) {
    case KIND_WAV:
        got = (size_t)drwav_read_pcm_frames_s16(&wav, max, raw);
        break;
    case KIND_MP3:
        got = (size_t)drmp3_read_pcm_frames_s16(&mp3, max, raw);
        break;
    case KIND_FLAC:
        got = (size_t)drflac_read_pcm_frames_s16(flac, max, raw);
        break;
    case KIND_VORBIS: {
        int frames = stb_vorbis_get_samples_short_interleaved(
            vorbis, (int)channels, raw, (int)(max * channels));
        got = frames > 0 ? (size_t)frames : 0;
        break;
    }
    case KIND_MPEG: {
        if (!plm_audio) {
            return 0;
        }
        plm_samples_t *samples = plm_decode_audio(plm);
        if (samples == NULL) {
            return 0;
        }
        size_t count = samples->count;
        for (size_t i = 0; i < count * 2; i++) {
            out[i] = clamp16(samples->interleaved[i] * 32767.0f);
        }
        return count;
    }
    default:
        return 0;
    }
    to_stereo(out, raw, got, channels);
    return got;
}

/* Makes at least 2 frames available after src_pos, or sets src_done. */
static void source_refill(void) {
    while (src_pos + 1 >= src_count && !src_done) {
        /* Keep the last frame: interpolation needs it. */
        int keep = 0;
        if (src_count > 0) {
            int last = src_count - 1;
            src_buf[0] = src_buf[2 * last];
            src_buf[1] = src_buf[2 * last + 1];
            src_pos -= last;
            keep = 1;
        } else {
            src_pos = 0;
        }
        size_t got = source_read(src_buf + 2 * keep,
                                 SRC_FRAMES - (size_t)keep);
        src_count = keep + (int)got;
        if (got == 0) {
            src_done = true;
        }
    }
}

/* Makes up to count output frames; returns how many. */
static size_t resample(int16_t *out, size_t count) {
    double step = (double)src_rate / OPOS_SOUND_RATE;
    int level = muted ? 0 : volume * volume; /* of 10000 */
    size_t made = 0;
    while (made < count) {
        source_refill();
        if (src_pos + 1 >= src_count) {
            break; /* the end */
        }
        int index = (int)src_pos;
        double fraction = src_pos - index;
        for (int c = 0; c < 2; c++) {
            double a = src_buf[2 * index + c];
            double b = src_buf[2 * index + 2 + c];
            double value = (a + (b - a) * fraction) * level / 10000.0;
            out[2 * made + c] = clamp16((float)value);
        }
        scope[scope_at] = (int16_t)(((int)src_buf[2 * index]
                                     + src_buf[2 * index + 1]) / 2);
        scope_at = (scope_at + 1) % SCOPE;
        made++;
        src_pos += step;
    }
    return made;
}

/* Tops up the sound queue; false once the source has ended. */
static void pump_audio(void) {
    if (!playing || !loaded || at_end) {
        return;
    }
    if (items[current].kind == KIND_MPEG && !plm_audio) {
        return;
    }
    if (sound < 0) {
        /* No sound output: decode nothing, follow the clock. */
        return;
    }
    static int16_t out[CHUNK * 2];
    for (int rounds = 0; rounds < 8; rounds++) {
        long queued = opos_sound_queued(sound);
        if (queued < 0 || queued >= SOUND_LOW) {
            return;
        }
        size_t made = resample(out, CHUNK);
        if (made != 0) {
            opos_sound_write(sound, out, (uint32_t)made);
            written += made;
        }
        if (made < CHUNK) {
            at_end = true;
            return;
        }
    }
}

/* ------------------------------------------------------------- video */

static void stream_load(plm_buffer_t *buffer, void *user) {
    (void)user;
    static uint8_t chunk[65536];
    int64_t got = stream_fd >= 0
        ? file_read((int)stream_fd, chunk, sizeof(chunk)) : -1;
    if (got <= 0) {
        buffer->has_ended = TRUE;
        return;
    }
    stream_pos += (uint64_t)got;
    plm_buffer_write(buffer, chunk, (size_t)got);
}

static void stream_seek(plm_buffer_t *buffer, size_t offset, void *user) {
    (void)buffer;
    (void)user;
    if (stream_fd >= 0 && file_seek((int)stream_fd, (int64_t)offset, 0) >= 0) {
        stream_pos = offset;
    }
}

static size_t stream_tell(plm_buffer_t *buffer, void *user) {
    (void)buffer;
    (void)user;
    return (size_t)stream_pos;
}

static void video_show(plm_frame_t *frame) {
    if (video_pixels == NULL) {
        return;
    }
    plm_frame_to_bgra(frame, (uint8_t *)video_pixels, video_w * 4);
    video_shown = true;
    dirty = true;
}

/* Shows the frame that is due at the current time. */
static void pump_video(void) {
    if (!playing || !loaded || items[current].kind != KIND_MPEG
        || !plm_video) {
        return;
    }
    double now = media_time();
    plm_frame_t *due = NULL;
    for (int i = 0; i < 30; i++) {
        if (pending_frame == NULL) {
            pending_frame = plm_decode_video(plm);
            if (pending_frame == NULL) {
                break;
            }
        }
        if (pending_frame->time > now) {
            break;
        }
        due = pending_frame;
        pending_frame = NULL;
        if (i < 29) {
            /* Show only the newest due frame; pl_mpeg reuses the
               previous one's memory, so convert before decoding on. */
            plm_frame_t *peek = plm_decode_video(plm);
            if (peek == NULL || peek->time > now) {
                video_show(due);
                due = NULL;
                pending_frame = peek;
                break;
            }
            pending_frame = peek;
        }
    }
    if (due != NULL) {
        video_show(due);
    }
}

/* -------------------------------------------------------------- load */

static void unload(void) {
    if (!loaded) {
        return;
    }
    switch (items[current].kind) {
    case KIND_WAV:
        drwav_uninit(&wav);
        break;
    case KIND_MP3:
        drmp3_uninit(&mp3);
        break;
    case KIND_FLAC:
        drflac_close(flac);
        flac = NULL;
        break;
    case KIND_VORBIS:
        stb_vorbis_close(vorbis);
        vorbis = NULL;
        break;
    case KIND_MPEG:
        plm_destroy(plm);
        plm = NULL;
        break;
    default:
        break;
    }
    if (file_data != NULL) {
        opos_free(file_data, file_data_size);
        file_data = NULL;
    }
    if (video_pixels != NULL) {
        opos_free(video_pixels, (size_t)video_w * video_h * 4);
        video_pixels = NULL;
    }
    pending_frame = NULL;
    video_shown = false;
    stream_fd = -1;
    loaded = false;
    playing = false;
}

static bool load_whole(struct item *item) {
    if (item->size == 0 || item->size > AUDIO_FILE_MAX) {
        set_status(item->size == 0 ? "THE FILE IS EMPTY"
                                   : "THE FILE IS TOO LARGE (OVER 512 MB)");
        return false;
    }
    file_data_size = (size_t)item->size;
    file_data = opos_alloc(file_data_size);
    if (file_data == NULL) {
        set_status("NOT ENOUGH MEMORY FOR THIS FILE");
        return false;
    }
    if (file_seek(item->fd, 0, 0) != 0
        || file_read(item->fd, file_data, file_data_size)
               != (int64_t)file_data_size) {
        set_status("THE FILE COULD NOT BE READ");
        opos_free(file_data, file_data_size);
        file_data = NULL;
        return false;
    }
    return true;
}

/* Opens items[index] for playing from the start; false with a status. */
static bool load(int index) {
    unload();
    current = index;
    struct item *item = &items[index];
    src_total = 0;
    plm_audio = plm_video = false;
    bool ok = false;
    if (item->kind != KIND_MPEG && item->kind != KIND_UNKNOWN
        && !load_whole(item)) {
        return false;
    }
    switch (item->kind) {
    case KIND_WAV:
        ok = drwav_init_memory(&wav, file_data, file_data_size, NULL);
        if (ok) {
            src_rate = wav.sampleRate;
            src_channels = wav.channels;
            src_total = wav.totalPCMFrameCount;
        }
        break;
    case KIND_MP3:
        ok = drmp3_init_memory(&mp3, file_data, file_data_size, NULL);
        if (ok) {
            /* A seek table makes jumps in long files fast. */
            static drmp3_seek_point points[256];
            drmp3_uint32 count = 256;
            if (drmp3_calculate_seek_points(&mp3, &count, points)) {
                drmp3_bind_seek_table(&mp3, count, points);
            }
            src_rate = mp3.sampleRate;
            src_channels = mp3.channels;
            src_total = drmp3_get_pcm_frame_count(&mp3);
            drmp3_seek_to_pcm_frame(&mp3, 0);
        }
        break;
    case KIND_FLAC:
        flac = drflac_open_memory(file_data, file_data_size, NULL);
        ok = flac != NULL;
        if (ok) {
            src_rate = flac->sampleRate;
            src_channels = flac->channels;
            src_total = flac->totalPCMFrameCount;
        }
        break;
    case KIND_VORBIS: {
        int error = 0;
        vorbis = stb_vorbis_open_memory(file_data, (int)file_data_size,
                                        &error, NULL);
        ok = vorbis != NULL;
        if (ok) {
            stb_vorbis_info info = stb_vorbis_get_info(vorbis);
            src_rate = info.sample_rate;
            src_channels = (uint32_t)info.channels;
            src_total = stb_vorbis_stream_length_in_samples(vorbis);
        }
        break;
    }
    case KIND_MPEG: {
        stream_fd = item->fd;
        stream_size = item->size;
        stream_pos = 0;
        if (file_seek(item->fd, 0, 0) != 0) {
            break;
        }
        plm_buffer_t *buffer = plm_buffer_create_with_callbacks(
            stream_load, stream_seek, stream_tell, (size_t)stream_size,
            NULL);
        plm = buffer != NULL ? plm_create_with_buffer(buffer, TRUE) : NULL;
        if (plm == NULL || !plm_has_headers(plm)) {
            if (plm != NULL) {
                plm_destroy(plm);
                plm = NULL;
            }
            break;
        }
        plm_video = plm_get_num_video_streams(plm) > 0;
        plm_audio = plm_get_num_audio_streams(plm) > 0;
        plm_set_video_enabled(plm, plm_video);
        plm_set_audio_enabled(plm, plm_audio);
        plm_set_loop(plm, FALSE);
        src_rate = plm_audio ? (uint32_t)plm_get_samplerate(plm) : 0;
        src_channels = 2;
        double duration = plm_get_duration(plm);
        if (duration > 0) {
            item->duration = duration;
        }
        if (plm_video) {
            video_w = plm_get_width(plm);
            video_h = plm_get_height(plm);
            if (video_w <= 0 || video_h <= 0 || video_w > 4096
                || video_h > 4096) {
                break;
            }
            video_pixels = opos_alloc((size_t)video_w * video_h * 4);
            if (video_pixels == NULL) {
                set_status("NOT ENOUGH MEMORY FOR THIS VIDEO");
                plm_destroy(plm);
                plm = NULL;
                return false;
            }
        }
        ok = plm_video || plm_audio;
        break;
    }
    default:
        break;
    }
    if (!ok || (item->kind != KIND_MPEG
                && (src_rate < 1000 || src_rate > 384000
                    || src_channels == 0 || src_channels > 8))) {
        if (ok) {
            loaded = true; /* so unload closes the decoder */
            unload();
        } else if (file_data != NULL) {
            opos_free(file_data, file_data_size);
            file_data = NULL;
        }
        set_status(item->kind == KIND_UNKNOWN
                   ? "NOT A FILE MEDIA PLAYER KNOWS (WAV, MP3, FLAC, OGG, MPG)"
                   : "THIS FILE IS DAMAGED OR USES A FORM NOT SUPPORTED");
        return false;
    }
    if (item->kind != KIND_MPEG && src_total != 0) {
        item->duration = (double)src_total / src_rate;
    }
    loaded = true;
    sound_reset(0);
    char title[32] = "Media Player: ";
    size_t length = strlen(title);
    for (size_t i = 0; item->name[i] != '\0' && length + 1 < sizeof(title);
         i++) {
        title[length++] = item->name[i];
    }
    title[length] = '\0';
    opos_window_title(title);
    status[0] = '\0';
    if (sound < 0 && (item->kind != KIND_MPEG || plm_audio)) {
        set_status("NO SOUND OUTPUT: PLAYING WITHOUT SOUND");
    }
    dirty = true;
    return true;
}

/* Jumps to seconds into the loaded file. */
static void seek_to(double seconds) {
    if (!loaded) {
        return;
    }
    double duration = items[current].duration;
    if (seconds < 0) {
        seconds = 0;
    }
    if (duration > 0 && seconds > duration - 0.05) {
        seconds = duration - 0.05 > 0 ? duration - 0.05 : 0;
    }
    uint64_t frame = (uint64_t)(seconds * src_rate);
    switch (items[current].kind) {
    case KIND_WAV:
        drwav_seek_to_pcm_frame(&wav, frame);
        break;
    case KIND_MP3:
        drmp3_seek_to_pcm_frame(&mp3, frame);
        break;
    case KIND_FLAC:
        drflac_seek_to_pcm_frame(flac, frame);
        break;
    case KIND_VORBIS:
        stb_vorbis_seek(vorbis, (unsigned int)frame);
        break;
    case KIND_MPEG:
        pending_frame = NULL;
        plm_seek(plm, seconds, FALSE);
        seconds = plm_get_time(plm);
        if (plm_video) {
            plm_frame_t *frame_now = plm_decode_video(plm);
            if (frame_now != NULL) {
                video_show(frame_now);
                seconds = frame_now->time;
            }
        }
        break;
    default:
        break;
    }
    sound_reset(seconds);
    dirty = true;
}

/* --------------------------------------------------------- transport */

static void play_index(int index) {
    if (index < 0 || index >= item_count) {
        return;
    }
    if (load(index)) {
        playing = true;
        wall_start_ms = now_ms();
        selected = index;
    }
    dirty = true;
}

static void pause_toggle(void) {
    if (!loaded) {
        if (item_count > 0) {
            play_index(selected >= 0 ? selected : 0);
        }
        return;
    }
    if (playing) {
        /* The clock stops where the sound stops: drop what is queued
           and go back to that point. */
        double at = media_time();
        playing = false;
        seek_to(at);
    } else {
        playing = true;
        wall_start_ms = now_ms();
    }
    dirty = true;
}

static void stop(void) {
    if (loaded) {
        seek_to(0);
    }
    playing = false;
    if (sound >= 0) {
        opos_sound_close(sound);
        sound = -1;
    }
    dirty = true;
}

static int next_index(int step) {
    if (item_count == 0) {
        return -1;
    }
    if (shuffle && item_count > 1) {
        int pick;
        do {
            pick = (int)random_below((uint32_t)item_count);
        } while (pick == current);
        return pick;
    }
    int index = (current < 0 ? 0 : current) + step;
    if (index >= item_count) {
        return repeat_mode == REPEAT_ALL || (step > 0 && current < 0) ? 0
                                                                      : -1;
    }
    if (index < 0) {
        return repeat_mode == REPEAT_ALL ? item_count - 1 : 0;
    }
    return index;
}

static void go_next(int step) {
    if (step < 0 && loaded && media_time() > 3.0) {
        seek_to(0); /* Previous restarts a song that is under way. */
        return;
    }
    int index = next_index(step);
    if (index < 0) {
        stop();
        return;
    }
    bool was_playing = playing || !loaded;
    play_index(index);
    if (!was_playing) {
        pause_toggle();
    }
}

/* At the end of a file: what comes next. */
static void finished(void) {
    if (repeat_mode == REPEAT_ONE) {
        seek_to(0);
        return;
    }
    int index = next_index(1);
    if (index < 0) {
        stop();
        return;
    }
    play_index(index);
}

static void check_end(void) {
    if (!playing || !loaded) {
        return;
    }
    double duration = items[current].duration;
    bool ended = false;
    if (items[current].kind == KIND_MPEG) {
        bool video_done = !plm_video
            || (pending_frame == NULL && plm_has_ended(plm));
        bool audio_done = !plm_audio || sound < 0
            || (at_end && opos_sound_queued(sound) <= 0);
        ended = video_done && audio_done
            && (plm_has_ended(plm) || (duration > 0
                                       && media_time() >= duration));
    } else if (sound >= 0) {
        ended = at_end && opos_sound_queued(sound) <= 0;
    } else {
        ended = duration > 0 && media_time() >= duration;
    }
    if (ended) {
        finished();
    }
}

/* ------------------------------------------------------------ playlist */

static void add_file(const char *path, bool start) {
    if (item_count == MAX_ITEMS) {
        set_status("THE PLAYLIST IS FULL (48 FILES)");
        return;
    }
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        set_status("THE FILE COULD NOT BE OPENED");
        return;
    }
    struct item *item = &items[item_count];
    memset(item, 0, sizeof(*item));
    strncpy(item->path, path, PATH_LEN - 1);
    display_name(path, item->name, sizeof(item->name));
    item->fd = (int)fd;
    int64_t size = file_seek((int)fd, 0, 2);
    item->size = size > 0 ? (uint64_t)size : 0;
    item->kind = probe((int)fd, path);
    item->duration = -1;
    item_count++;
    if (item->kind == KIND_UNKNOWN) {
        set_status("NOT A FILE MEDIA PLAYER KNOWS (WAV, MP3, FLAC, OGG, MPG)");
    }
    if (start || !loaded) {
        play_index(item_count - 1);
    }
    dirty = true;
}

static void open_file(void) {
    char path[PATH_LEN];
    long length = opos_pick_file(path, sizeof(path) - 1);
    if (length <= 0) {
        dirty = true;
        return;
    }
    path[length < PATH_LEN ? length : PATH_LEN - 1] = '\0';
    add_file(path, !playing);
}

static void remove_item(int index) {
    if (index < 0 || index >= item_count) {
        return;
    }
    if (index == current) {
        unload();
        if (sound >= 0) {
            opos_sound_close(sound);
            sound = -1;
        }
        current = -1;
        opos_window_title("Media Player");
    }
    opos_close(items[index].fd);
    for (int i = index; i + 1 < item_count; i++) {
        items[i] = items[i + 1];
    }
    item_count--;
    if (current > index) {
        current--;
    }
    if (selected >= item_count) {
        selected = item_count - 1;
    }
    dirty = true;
}

/* ------------------------------------------------------------- layout */

struct rect {
    int x, y, w, h;
};

static bool inside(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

enum button {
    B_OPEN,
    B_PREV,
    B_PLAY,
    B_STOP,
    B_NEXT,
    B_REPEAT,
    B_SHUFFLE,
    B_LIST,
    B_FULL,
    B_COUNT,
};

static bool list_visible(void) {
    return show_list && !fullscreen && view_w >= MIN_W + LIST_W / 2;
}

static struct rect stage_rect(void) {
    int w = view_w - (list_visible() ? LIST_W : 0);
    int h = fullscreen ? view_h : view_h - BAR_H;
    return (struct rect){0, 0, w, h > 0 ? h : 0};
}

static struct rect list_rect(void) {
    return (struct rect){view_w - LIST_W, 0, LIST_W, view_h - BAR_H};
}

static struct rect seek_rect(void) {
    return (struct rect){96, view_h - BAR_H + 12, view_w - 192, 12};
}

static struct rect button_rect(enum button b) {
    /* Text buttons fit their longest label ("REPEAT ALL": 10 characters
       of 12 pixels) with 8 pixels on each side. */
    static const int widths[B_COUNT] = {68, 44, 52, 44, 44, 136, 136, 112,
                                        64};
    int x = 12;
    for (int i = 0; i < (int)b; i++) {
        x += widths[i] + 6;
        if (i == B_OPEN || i == B_NEXT) {
            x += 10;
        }
    }
    return (struct rect){x, view_h - BUTTON_H - 10, widths[b], BUTTON_H};
}

static struct rect volume_rect(void) {
    int w = 120;
    return (struct rect){view_w - w - 16, view_h - BUTTON_H / 2 - 13, w, 8};
}

/* -------------------------------------------------------------- draw */

static void format_time(double seconds, char *out) {
    if (seconds < 0) {
        strcpy(out, "--:--");
        return;
    }
    long total = (long)seconds;
    long hours = total / 3600;
    long minutes = total / 60 % 60;
    long secs = total % 60;
    if (hours > 0) {
        snprintf(out, 16, "%ld:%02ld:%02ld", hours, minutes, secs);
    } else {
        snprintf(out, 16, "%ld:%02ld", minutes, secs);
    }
}

static void fill_triangle(int x, int y, int size, bool right, uint32_t c) {
    for (int row = 0; row < size; row++) {
        int half = row < size / 2 ? row : size - 1 - row;
        int width = half * 2 + 1;
        if (right) {
            opos_fill_rect(&canvas, x, y + row, width, 1, c);
        } else {
            opos_fill_rect(&canvas, x + size - width, y + row, width, 1, c);
        }
    }
}

static void draw_icon(enum button b, struct rect r, uint32_t c) {
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    switch (b) {
    case B_PLAY:
        if (playing) {
            opos_fill_rect(&canvas, cx - 7, cy - 8, 5, 16, c);
            opos_fill_rect(&canvas, cx + 2, cy - 8, 5, 16, c);
        } else {
            fill_triangle(cx - 6, cy - 9, 19, true, c);
        }
        break;
    case B_STOP:
        opos_fill_rect(&canvas, cx - 7, cy - 7, 14, 14, c);
        break;
    case B_PREV:
        opos_fill_rect(&canvas, cx - 10, cy - 7, 3, 15, c);
        fill_triangle(cx - 9, cy - 7, 15, false, c);
        fill_triangle(cx - 1, cy - 7, 15, false, c);
        break;
    case B_NEXT:
        fill_triangle(cx - 9, cy - 7, 15, true, c);
        fill_triangle(cx - 1, cy - 7, 15, true, c);
        opos_fill_rect(&canvas, cx + 8, cy - 7, 3, 15, c);
        break;
    default: {
        const char *text = b == B_OPEN ? "OPEN"
            : b == B_REPEAT ? (repeat_mode == REPEAT_OFF ? "REPEAT OFF"
                               : repeat_mode == REPEAT_ALL ? "REPEAT ALL"
                                                           : "REPEAT ONE")
            : b == B_SHUFFLE ? (shuffle ? "SHUFFLE ON" : "SHUFFLE")
            : b == B_LIST ? "PLAYLIST" : "FULL";
        opos_draw_text(&canvas, r.x + (r.w - opos_text_width(text, 2)) / 2,
                       cy - opos_char_height(2) / 2, text, c, 2);
        break;
    }
    }
}

static void draw_button(enum button b) {
    struct rect r = button_rect(b);
    bool on = (b == B_REPEAT && repeat_mode != REPEAT_OFF)
        || (b == B_SHUFFLE && shuffle) || (b == B_LIST && show_list);
    uint32_t back = inside(r, mouse_x, mouse_y) ? UI_HOVER
        : on ? UI_ACTIVE : UI_BUTTON;
    opos_fill_rect(&canvas, r.x, r.y, r.w, r.h, back);
    draw_icon(b, r, UI_TEXT);
}

/* Scales the video frame into r, keeping its shape. */
static void draw_video(struct rect r) {
    if (video_pixels == NULL || !video_shown || r.w <= 0 || r.h <= 0) {
        return;
    }
    int w = r.w;
    int h = (int)((int64_t)w * video_h / video_w);
    if (h > r.h) {
        h = r.h;
        w = (int)((int64_t)h * video_w / video_h);
    }
    int x0 = r.x + (r.w - w) / 2;
    int y0 = r.y + (r.h - h) / 2;
    uint32_t step_x = (uint32_t)(((uint64_t)video_w << 16) / (uint32_t)w);
    uint32_t step_y = (uint32_t)(((uint64_t)video_h << 16) / (uint32_t)h);
    for (int y = 0; y < h; y++) {
        int py = y0 + y;
        if (py < 0 || py >= canvas.height) {
            continue;
        }
        const uint32_t *source = video_pixels
            + (size_t)((y * step_y) >> 16) * video_w;
        uint32_t *row = canvas.pixels + (size_t)py * canvas.stride;
        uint32_t sx = 0;
        for (int x = 0; x < w; x++, sx += step_x) {
            int px = x0 + x;
            if (px >= 0 && px < canvas.width) {
                row[px] = source[sx >> 16] & 0x00ffffff;
            }
        }
    }
}

static void draw_text_fit(int x, int y, const char *text, int max_w,
                          uint32_t c, int scale) {
    size_t fit = (size_t)(max_w / opos_char_width(scale));
    size_t length = strlen(text);
    if (length <= fit) {
        opos_draw_text(&canvas, x, y, text, c, scale);
        return;
    }
    if (fit < 4) {
        return;
    }
    x = opos_draw_text_n(&canvas, x, y, text, fit - 3, c, scale);
    opos_draw_text(&canvas, x, y, "...", c, scale);
}

static void draw_now_playing(struct rect r) {
    int pad = 28;
    int y = r.y + pad;
    if (!loaded && item_count == 0) {
        opos_draw_text(&canvas, r.x + pad, y, "MEDIA PLAYER", UI_TEXT, 3);
        y += 48;
        opos_draw_text(&canvas, r.x + pad, y,
                       "PRESS OPEN (OR O) TO PLAY MUSIC OR A VIDEO.",
                       UI_DIM, 2);
        y += 28;
        opos_draw_text(&canvas, r.x + pad, y,
                       "MUSIC: WAV, MP3, FLAC, OGG.  VIDEO: MPG (MPEG-1).",
                       UI_DIM, 2);
        return;
    }
    if (current < 0) {
        opos_draw_text(&canvas, r.x + pad, y, "NOTHING PLAYING", UI_DIM, 2);
        return;
    }
    struct item *item = &items[current];
    draw_text_fit(r.x + pad, y, item->name, r.w - 2 * pad, UI_TEXT, 3);
    y += 40;
    char line[96];
    if (loaded) {
        snprintf(line, sizeof(line), "%s  %u HZ  %s", kind_names[item->kind],
                 (unsigned)src_rate,
                 src_channels == 1 ? "MONO" : "STEREO");
        if (item->kind == KIND_MPEG) {
            snprintf(line, sizeof(line), "%s  %dx%d  %s",
                     kind_names[item->kind], video_w, video_h,
                     plm_audio ? "WITH SOUND" : "NO SOUND");
        }
        draw_text_fit(r.x + pad, y, line, r.w - 2 * pad, UI_DIM, 2);
    }
    /* The scope: the last sound played, as a line. */
    int sy = y + 40;
    int sh = r.y + r.h - sy - pad;
    if (sh < 40 || item->kind == KIND_MPEG) {
        return;
    }
    int sw = r.w - 2 * pad;
    int mid = sy + sh / 2;
    opos_fill_rect(&canvas, r.x + pad, mid, sw, 1, UI_PANEL);
    int previous = mid;
    for (int i = 0; i < sw; i++) {
        int index = (scope_at + i * SCOPE / sw) % SCOPE;
        int value = playing ? scope[index] : 0;
        int py = mid - value * (sh / 2) / 32768;
        int top = py < previous ? py : previous;
        int bottom = py < previous ? previous : py;
        opos_fill_rect(&canvas, r.x + pad + i, top, 1, bottom - top + 1,
                       UI_ACCENT);
        previous = py;
    }
}

static void draw_list(struct rect r) {
    opos_fill_rect(&canvas, r.x, r.y, r.w, r.h, UI_PANEL);
    opos_draw_text(&canvas, r.x + 12, r.y + 10, "PLAYLIST", UI_DIM, 2);
    int rows = (r.h - 40) / ROW_H;
    if (rows < 1) {
        return;
    }
    if (list_scroll > item_count - rows) {
        list_scroll = item_count - rows;
    }
    if (list_scroll < 0) {
        list_scroll = 0;
    }
    for (int row = 0; row < rows; row++) {
        int index = list_scroll + row;
        if (index >= item_count) {
            break;
        }
        int y = r.y + 36 + row * ROW_H;
        struct rect line = {r.x + 4, y, r.w - 8, ROW_H};
        if (index == selected) {
            opos_fill_rect(&canvas, line.x, line.y, line.w, line.h,
                           UI_BUTTON);
        } else if (inside(line, mouse_x, mouse_y)) {
            opos_fill_rect(&canvas, line.x, line.y, line.w, line.h,
                           UI_BACK);
        }
        uint32_t c = index == current ? UI_ACCENT
            : items[index].kind == KIND_UNKNOWN ? UI_DIM : UI_TEXT;
        char time[16];
        format_time(items[index].duration, time);
        int tw = opos_text_width(time, 1) * 2;
        draw_text_fit(line.x + 8, y + 3, items[index].name,
                      line.w - 24 - tw, c, 2);
        opos_draw_text(&canvas, line.x + line.w - tw - 8, y + 3, time,
                       UI_DIM, 2);
    }
    if (item_count == 0) {
        opos_draw_text(&canvas, r.x + 12, r.y + 40, "EMPTY", UI_DIM, 2);
    }
}

static void draw_bar(void) {
    int y = view_h - BAR_H;
    opos_fill_rect(&canvas, 0, y, view_w, BAR_H, UI_PANEL);
    double position = drag == DRAG_SEEK ? drag_time : media_time();
    double duration = loaded ? items[current].duration : -1;
    char left[16];
    char right[16];
    format_time(loaded ? position : -1, left);
    format_time(duration, right);
    struct rect seek = seek_rect();
    opos_draw_text(&canvas, 12, seek.y - 3, left, UI_TEXT, 2);
    opos_draw_text(&canvas, seek.x + seek.w + 12, seek.y - 3, right,
                   UI_TEXT, 2);
    opos_fill_rect(&canvas, seek.x, seek.y + 3, seek.w, 6, UI_BUTTON);
    if (duration > 0) {
        double share = position / duration;
        if (share > 1) {
            share = 1;
        }
        int done = (int)(seek.w * share);
        opos_fill_rect(&canvas, seek.x, seek.y + 3, done, 6, UI_ACCENT);
        opos_fill_rect(&canvas, seek.x + done - 3, seek.y, 7, 12, UI_TEXT);
    }
    for (int b = 0; b < B_COUNT; b++) {
        if (button_rect((enum button)b).x + button_rect((enum button)b).w
            < volume_rect().x - 60) {
            draw_button((enum button)b);
        }
    }
    struct rect volume_bar = volume_rect();
    opos_draw_text(&canvas, volume_bar.x - 48, volume_bar.y - 5,
                   muted ? "MUTE" : "VOL", muted ? UI_WARN : UI_DIM, 2);
    opos_fill_rect(&canvas, volume_bar.x, volume_bar.y, volume_bar.w,
                   volume_bar.h, UI_BUTTON);
    int level = volume_bar.w * volume / 100;
    opos_fill_rect(&canvas, volume_bar.x, volume_bar.y, level, volume_bar.h,
                   muted ? UI_DIM : UI_ACCENT);
    opos_fill_rect(&canvas, volume_bar.x + level - 3, volume_bar.y - 3, 7,
                   volume_bar.h + 6, UI_TEXT);
}

static void draw(void) {
    if (canvas.pixels == NULL) {
        return;
    }
    opos_fill_rect(&canvas, 0, 0, view_w, view_h,
                   fullscreen ? 0 : UI_BACK);
    struct rect stage = stage_rect();
    bool video = loaded && items[current].kind == KIND_MPEG && plm_video;
    if (video) {
        opos_fill_rect(&canvas, stage.x, stage.y, stage.w, stage.h, 0);
        draw_video(stage);
    } else {
        draw_now_playing(stage);
    }
    if (!fullscreen) {
        if (list_visible()) {
            draw_list(list_rect());
        }
        draw_bar();
        if (status[0] != '\0') {
            int w = opos_text_width(status, 2) + 24;
            int x = stage.x + (stage.w - w) / 2;
            if (x < 8) {
                x = 8;
            }
            opos_fill_rect(&canvas, x, stage.y + stage.h - 44, w, 32,
                           UI_PANEL);
            opos_draw_text(&canvas, x + 12, stage.y + stage.h - 36, status,
                           UI_WARN, 2);
        }
    }
    opos_canvas_present_area(&canvas, view_w, view_h);
    dirty = false;
}

/* -------------------------------------------------------------- input */

static void set_fullscreen(bool on) {
    fullscreen = on;
    opos_window_options(on ? OPOS_WINDOW_FULLSCREEN | OPOS_WINDOW_QUIET : 0);
    dirty = true;
}

static void set_volume(int value) {
    volume = value < 0 ? 0 : value > 100 ? 100 : value;
    muted = false;
    dirty = true;
}

static double seek_time_at(int x) {
    struct rect seek = seek_rect();
    double share = (double)(x - seek.x) / (seek.w > 0 ? seek.w : 1);
    if (share < 0) {
        share = 0;
    }
    if (share > 1) {
        share = 1;
    }
    double duration = loaded ? items[current].duration : 0;
    return share * (duration > 0 ? duration : 0);
}

static void press_button(enum button b) {
    switch (b) {
    case B_OPEN:
        open_file();
        break;
    case B_PREV:
        go_next(-1);
        break;
    case B_PLAY:
        pause_toggle();
        break;
    case B_STOP:
        stop();
        break;
    case B_NEXT:
        go_next(1);
        break;
    case B_REPEAT:
        repeat_mode = (enum repeat_mode)((repeat_mode + 1) % 3);
        break;
    case B_SHUFFLE:
        shuffle = !shuffle;
        break;
    case B_LIST:
        show_list = !show_list;
        break;
    case B_FULL:
        set_fullscreen(true);
        break;
    default:
        break;
    }
    dirty = true;
}

static void mouse_down(const struct opos_event *event) {
    int x = event->x;
    int y = event->y;
    if (fullscreen) {
        long now = now_ms();
        if (now - last_click_ms < 400) {
            set_fullscreen(false);
        } else {
            pause_toggle();
        }
        last_click_ms = now;
        return;
    }
    if (event->button != OPOS_BUTTON_LEFT) {
        return;
    }
    struct rect seek = seek_rect();
    struct rect seek_area = {seek.x - 6, seek.y - 8, seek.w + 12,
                             seek.h + 16};
    if (inside(seek_area, x, y) && loaded && items[current].duration > 0) {
        drag = DRAG_SEEK;
        drag_time = seek_time_at(x);
        dirty = true;
        return;
    }
    struct rect volume_bar = volume_rect();
    struct rect volume_area = {volume_bar.x - 6, volume_bar.y - 10,
                               volume_bar.w + 12, volume_bar.h + 20};
    if (inside(volume_area, x, y)) {
        drag = DRAG_VOLUME;
        set_volume((x - volume_bar.x) * 100 / volume_bar.w);
        return;
    }
    for (int b = 0; b < B_COUNT; b++) {
        if (inside(button_rect((enum button)b), x, y)) {
            press_button((enum button)b);
            return;
        }
    }
    if (list_visible() && inside(list_rect(), x, y)) {
        int row = (y - list_rect().y - 36) / ROW_H;
        int index = list_scroll + row;
        if (y >= list_rect().y + 36 && index >= 0 && index < item_count) {
            long now = now_ms();
            if (index == last_click_row && now - last_click_ms < 400) {
                play_index(index);
            }
            selected = index;
            last_click_row = index;
            last_click_ms = now;
            dirty = true;
        }
        return;
    }
    struct rect stage = stage_rect();
    if (inside(stage, x, y) && loaded && items[current].kind == KIND_MPEG) {
        long now = now_ms();
        if (now - last_click_ms < 400 && last_click_row == -2) {
            set_fullscreen(true);
        }
        last_click_row = -2;
        last_click_ms = now;
    }
}

static void mouse_move(const struct opos_event *event) {
    bool hover_changed = event->y >= view_h - BAR_H || list_visible();
    mouse_x = event->x;
    mouse_y = event->y;
    if (drag == DRAG_SEEK) {
        drag_time = seek_time_at(event->x);
        dirty = true;
    } else if (drag == DRAG_VOLUME) {
        struct rect volume_bar = volume_rect();
        set_volume((event->x - volume_bar.x) * 100 / volume_bar.w);
    } else if (hover_changed) {
        dirty = true;
    }
}

static void mouse_up(void) {
    if (drag == DRAG_SEEK) {
        seek_to(drag_time);
    }
    drag = DRAG_NONE;
    dirty = true;
}

static void wheel(const struct opos_event *event) {
    int notches = OPOS_WHEEL_NOTCHES(*event);
    if (list_visible() && inside(list_rect(), event->x, event->y)) {
        list_scroll -= notches * 3;
    } else {
        set_volume(volume + notches * 5);
    }
    dirty = true;
}

static void key(const struct opos_event *event) {
    uint32_t k = event->key;
    bool ctrl = (event->buttons & OPOS_MOD_CTRL) != 0;
    switch (k) {
    case ' ':
        pause_toggle();
        break;
    case OPOS_KEY_LEFT:
    case OPOS_KEY_RIGHT: {
        double step = ctrl ? 30 : 5;
        seek_to(media_time() + (k == OPOS_KEY_LEFT ? -step : step));
        break;
    }
    case OPOS_KEY_UP:
        if (selected > 0 && list_visible() && ctrl) {
            selected--;
        } else {
            set_volume(volume + 5);
        }
        break;
    case OPOS_KEY_DOWN:
        set_volume(volume - 5);
        break;
    case OPOS_KEY_PAGE_UP:
        if (selected > 0) {
            selected--;
        }
        break;
    case OPOS_KEY_PAGE_DOWN:
        if (selected + 1 < item_count) {
            selected++;
        }
        break;
    case OPOS_KEY_HOME:
        seek_to(0);
        break;
    case '\n':
        play_index(selected);
        break;
    case OPOS_KEY_DELETE:
        remove_item(selected);
        break;
    case OPOS_KEY_ESCAPE:
        if (fullscreen) {
            set_fullscreen(false);
        } else {
            status[0] = '\0';
        }
        break;
    case 'm':
    case 'M':
        muted = !muted;
        break;
    case 'n':
    case 'N':
        go_next(1);
        break;
    case 'p':
    case 'P':
        go_next(-1);
        break;
    case 's':
    case 'S':
        stop();
        break;
    case 'o':
    case 'O':
        open_file();
        break;
    case 'r':
    case 'R':
        repeat_mode = (enum repeat_mode)((repeat_mode + 1) % 3);
        break;
    case 'h':
    case 'H':
        shuffle = !shuffle;
        break;
    case 'l':
    case 'L':
        show_list = !show_list;
        break;
    case 'f':
    case 'F':
        set_fullscreen(!fullscreen);
        break;
    default:
        break;
    }
    dirty = true;
}

/* -------------------------------------------------------------- main */

int main(int argc, char **argv, char **envp) {
    (void)envp;
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    random_state ^= (uint64_t)high << 32 | low;
    if (opos_window_create("Media Player", 960, 600) < 0) {
        opos_print("mediaplayer: no window; run it from the desktop\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        add_file(argv[i], i == 1);
    }
    struct opos_event event;
    long last_draw = 0;
    for (;;) {
        long timeout = playing ? 10 : -1;
        long got = opos_window_event(&event, timeout);
        while (got > 0) {
            switch (event.type) {
            case OPOS_EVENT_RESIZE: {
                view_w = (int)event.width;
                view_h = (int)event.height;
                if (canvas.pixels == NULL || canvas.width < view_w
                    || canvas.height < view_h) {
                    opos_canvas_destroy(&canvas);
                    canvas = opos_canvas_create(view_w, view_h);
                    if (canvas.pixels == NULL) {
                        opos_print("mediaplayer: out of memory\n");
                        return 1;
                    }
                }
                dirty = true;
                break;
            }
            case OPOS_EVENT_MOUSE_DOWN:
                mouse_x = event.x;
                mouse_y = event.y;
                mouse_down(&event);
                break;
            case OPOS_EVENT_MOUSE_MOVE:
                mouse_move(&event);
                break;
            case OPOS_EVENT_MOUSE_UP:
                mouse_up();
                break;
            case OPOS_EVENT_WHEEL:
                wheel(&event);
                break;
            case OPOS_EVENT_KEY:
                key(&event);
                break;
            case OPOS_EVENT_CLOSE:
                unload();
                return 0;
            default:
                break;
            }
            got = opos_window_event(&event, 0);
        }
        pump_audio();
        pump_video();
        check_end();
        pump_audio();
        long now = now_ms();
        /* The time and the scope move while playing: redraw 10 times a
           second, or for each new video frame. */
        if (playing && now - last_draw >= 100) {
            dirty = true;
        }
        if (dirty) {
            draw();
            last_draw = now;
        }
    }
}
