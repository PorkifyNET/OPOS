/*
 * Plays a WAV file through the sound calls:
 *   play FILE.wav     16-bit PCM, mono or stereo, any sample rate
 *   play              two test tones (440 Hz, then 880 Hz)
 * Other rates are converted to 48 kHz by linear interpolation, mono is
 * played on both sides.
 */
#include <stdbool.h>

#include <opos.h>

OPOS_MANIFEST("Play", "1.0", 0);

#define CHUNK_FRAMES 2048

static int16_t out[CHUNK_FRAMES * 2];
static uint8_t in[CHUNK_FRAMES * 4];

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
        | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static bool same(const uint8_t *p, const char *text) {
    for (int i = 0; i < 4; i++) {
        if (p[i] != (uint8_t)text[i]) {
            return false;
        }
    }
    return true;
}

static long read_all(int fd, void *buffer, size_t length) {
    size_t done = 0;
    while (done < length) {
        long got = opos_read(fd, (uint8_t *)buffer + done, length - done);
        if (got <= 0) {
            return got < 0 ? got : (long)done;
        }
        done += (size_t)got;
    }
    return (long)done;
}

static bool write_frames(long stream, uint32_t count) {
    return opos_sound_write(stream, out, count) == (long)count;
}

/* Waits until everything queued was played, then closes. */
static void finish(long stream) {
    while (opos_sound_queued(stream) > 0) {
        opos_sleep_ms(20);
    }
    opos_sound_close(stream);
}

/* A sine wave by a rotating vector (no maths library needed). */
static bool tone(long stream, double frequency, uint32_t milliseconds) {
    double angle = 2.0 * 3.14159265358979323846 * frequency / OPOS_SOUND_RATE;
    /* cos and sin of angle by their series; angle is small. */
    double a2 = angle * angle;
    double c = 1.0 - a2 / 2.0 + a2 * a2 / 24.0 - a2 * a2 * a2 / 720.0;
    double s = angle - angle * a2 / 6.0 + angle * a2 * a2 / 120.0
        - angle * a2 * a2 * a2 / 5040.0;
    double x = 1.0, y = 0.0;
    uint32_t total = OPOS_SOUND_RATE / 1000 * milliseconds;
    while (total != 0) {
        uint32_t count = total < CHUNK_FRAMES ? total : CHUNK_FRAMES;
        for (uint32_t i = 0; i < count; i++) {
            int16_t value = (int16_t)(y * 12000.0);
            out[2 * i] = value;
            out[2 * i + 1] = value;
            double nx = x * c - y * s;
            y = x * s + y * c;
            x = nx;
        }
        /* Keep the vector's length at 1 against rounding. */
        double length = x * x + y * y;
        x *= (3.0 - length) / 2.0;
        y *= (3.0 - length) / 2.0;
        if (!write_frames(stream, count)) {
            return false;
        }
        total -= count;
    }
    return true;
}

static int play_file(long stream, const char *path) {
    int fd = (int)opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        opos_print("play: cannot open the file\n");
        return 1;
    }
    uint8_t header[12];
    if (read_all(fd, header, 12) != 12 || !same(header, "RIFF")
        || !same(header + 8, "WAVE")) {
        opos_print("play: not a WAV file\n");
        return 1;
    }
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0, format = 0;
    uint32_t data_size = 0;
    for (;;) {
        uint8_t chunk[8];
        if (read_all(fd, chunk, 8) != 8) {
            opos_print("play: no sound data in the file\n");
            return 1;
        }
        uint32_t size = le32(chunk + 4);
        if (same(chunk, "fmt ")) {
            uint8_t fmt[16];
            if (size < 16 || read_all(fd, fmt, 16) != 16) {
                opos_print("play: bad format chunk\n");
                return 1;
            }
            format = le16(fmt);
            channels = le16(fmt + 2);
            rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            size -= 16;
        } else if (same(chunk, "data")) {
            data_size = size;
            break;
        }
        /* Skip the rest of the chunk (chunks are padded to even sizes). */
        size += size & 1;
        while (size != 0) {
            uint32_t step = size < sizeof(in) ? size : sizeof(in);
            if (read_all(fd, in, step) != (long)step) {
                opos_print("play: the file is cut short\n");
                return 1;
            }
            size -= step;
        }
    }
    if (format != 1 || bits != 16 || channels < 1 || channels > 2
        || rate < 4000 || rate > 192000) {
        opos_print("play: only 16-bit PCM WAV, mono or stereo\n");
        return 1;
    }
    /* Source position in 16.16 fixed point, one output frame at a time. */
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / OPOS_SOUND_RATE);
    uint64_t position = 0;
    uint32_t frame_bytes = channels * 2u;
    uint32_t frames_left = data_size / frame_bytes;
    uint32_t have = 0;    /* source frames in "in" */
    uint64_t first = 0;   /* source index of in[0] */
    uint32_t produced = 0;
    for (;;) {
        uint64_t index = position >> 16;
        if (index + 1 >= first + have) {
            /* Keep the last frame for interpolation, read more. */
            if (have != 0 && index >= first + have - 1) {
                for (uint32_t b = 0; b < frame_bytes; b++) {
                    in[b] = in[(have - 1) * frame_bytes + b];
                }
                first += have - 1;
                have = 1;
            }
            uint32_t room = sizeof(in) / frame_bytes - have;
            uint32_t want = frames_left < room ? frames_left : room;
            if (want != 0) {
                long got = read_all(fd, in + have * frame_bytes,
                                    want * frame_bytes);
                if (got <= 0) {
                    frames_left = 0;
                } else {
                    have += (uint32_t)got / frame_bytes;
                    frames_left -= (uint32_t)got / frame_bytes;
                }
            }
            if (index >= first + have) {
                break; /* the end */
            }
        }
        uint32_t at = (uint32_t)(index - first);
        uint32_t next = at + 1 < have ? at + 1 : at;
        int32_t fraction = (int32_t)(position & 0xffff);
        for (uint32_t c = 0; c < 2; c++) {
            uint32_t source = channels == 2 ? c : 0;
            int32_t a = (int16_t)le16(in + at * frame_bytes + source * 2);
            int32_t b = (int16_t)le16(in + next * frame_bytes + source * 2);
            out[2 * produced + c] =
                (int16_t)(a + (((b - a) * fraction) >> 16));
        }
        produced++;
        position += step;
        if (produced == CHUNK_FRAMES) {
            if (!write_frames(stream, produced)) {
                return 1;
            }
            produced = 0;
        }
    }
    if (produced != 0 && !write_frames(stream, produced)) {
        return 1;
    }
    opos_close(fd);
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    long stream = opos_sound_open(8192);
    if (stream < 0) {
        opos_print(stream == -OPOS_ENODEV ? "play: no sound output\n"
                                          : "play: cannot open a stream\n");
        return 1;
    }
    int status = 0;
    if (argc < 2) {
        opos_print("Playing 440 Hz, then 880 Hz.\n");
        if (!tone(stream, 440.0, 500) || !tone(stream, 880.0, 500)) {
            status = 1;
        }
    } else {
        status = play_file(stream, argv[1]);
    }
    finish(stream);
    return status;
}
