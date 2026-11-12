/*
  SDL audio driver for OPOS.
  Copyright (C) 2026 the OPOS authors

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

/*
 * OPOS mixes sound streams of 48 kHz, 16-bit stereo frames in the kernel
 * (docs/sound.md). This driver opens one stream per SDL audio device
 * through the sound system calls in sdk/include/opos.h (the same numbers,
 * written out here so SDL needs no OPOS headers). The device always runs
 * at that format; SDL converts whatever the program asked for.
 *
 * Writing to a stream waits while it is full, so PlayDevice paces SDL's
 * audio thread by itself and WaitDevice has nothing to do. The stream
 * holds two of SDL's buffers (at least 1024 frames), which keeps the
 * delay near what the program chose with its buffer size.
 */
#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_OPOS

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"

#define OPOS_SYS_SOUND_OPEN 0x4f500040L
#define OPOS_SYS_SOUND_WRITE 0x4f500041L
#define OPOS_SYS_SOUND_CLOSE 0x4f500043L
#define OPOS_SOUND_RATE 48000
#define OPOS_SOUND_MAX_FRAMES 32768

struct SDL_PrivateAudioData
{
    long stream;
    Uint8 *mixbuf;
};

static long opos_audio_call(long number, long a, long b, long c)
{
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return result;
}

static int OPOSAUDIO_OpenDevice(SDL_AudioDevice *_this, const char *devname)
{
    struct SDL_PrivateAudioData *data;
    Uint32 capacity;

    (void)devname;
    _this->spec.freq = OPOS_SOUND_RATE;
    _this->spec.format = AUDIO_S16LSB;
    _this->spec.channels = 2;
    if (_this->spec.samples > OPOS_SOUND_MAX_FRAMES / 2) {
        _this->spec.samples = OPOS_SOUND_MAX_FRAMES / 2;
    }
    SDL_CalculateAudioSpec(&_this->spec);

    data = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    _this->hidden = data;
    data->mixbuf = (Uint8 *)SDL_malloc(_this->spec.size);
    if (!data->mixbuf) {
        return SDL_OutOfMemory();
    }
    SDL_memset(data->mixbuf, _this->spec.silence, _this->spec.size);

    capacity = (Uint32)_this->spec.samples * 2;
    data->stream = opos_audio_call(OPOS_SYS_SOUND_OPEN, (long)capacity, 0, 0);
    if (data->stream < 0) {
        return SDL_SetError("OPOS: cannot open a sound stream (%ld)",
                            data->stream);
    }
    return 0;
}

static void OPOSAUDIO_PlayDevice(SDL_AudioDevice *_this)
{
    struct SDL_PrivateAudioData *data = _this->hidden;
    long frames = (long)(_this->spec.size / 4);
    long written = opos_audio_call(OPOS_SYS_SOUND_WRITE, data->stream,
                                   (long)data->mixbuf, frames);
    if (written < 0) {
        SDL_OpenedAudioDeviceDisconnected(_this);
    }
}

static Uint8 *OPOSAUDIO_GetDeviceBuf(SDL_AudioDevice *_this)
{
    return _this->hidden->mixbuf;
}

static void OPOSAUDIO_CloseDevice(SDL_AudioDevice *_this)
{
    struct SDL_PrivateAudioData *data = _this->hidden;
    if (data->stream > 0) {
        opos_audio_call(OPOS_SYS_SOUND_CLOSE, data->stream, 0, 0);
    }
    SDL_free(data->mixbuf);
    SDL_free(data);
}

static SDL_bool OPOSAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    /* Only with sound hardware (and on OPOS at all): otherwise SDL moves
       on to the dummy driver and the program runs silent. */
    long stream = opos_audio_call(OPOS_SYS_SOUND_OPEN, 1024, 0, 0);
    if (stream < 0) {
        return SDL_FALSE;
    }
    opos_audio_call(OPOS_SYS_SOUND_CLOSE, stream, 0, 0);

    impl->OpenDevice = OPOSAUDIO_OpenDevice;
    impl->PlayDevice = OPOSAUDIO_PlayDevice;
    impl->GetDeviceBuf = OPOSAUDIO_GetDeviceBuf;
    impl->CloseDevice = OPOSAUDIO_CloseDevice;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    impl->SupportsNonPow2Samples = SDL_TRUE;
    return SDL_TRUE;
}

AudioBootStrap OPOSAUDIO_bootstrap = {
    "opos", "OPOS sound", OPOSAUDIO_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_OPOS */
