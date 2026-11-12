# Sound on OPOS

OPOS has no sound. Doom and the SDL demos run silent. This plan adds
sound output in five steps, each its own pull request.

## Target hardware

Intel High Definition Audio (HDA): the sound controller of nearly every
PC since 2004, and what QEMU (`-device intel-hda -device hda-duplex` or
`hda-output`), VirtualBox (default "Intel HD Audio") and VMware
(`hdaudio`) emulate. AC'97 (older VirtualBox setups) and USB audio can
come later. Input (microphones) is out of scope.

## Design

- **One format.** The kernel plays 48 kHz, 16-bit, stereo. Programs give
  that format; SDL converts anything else itself.
- **Polled, no interrupts.** The controller plays a ring of buffers in
  memory over and over (a BDL with several entries). The 100 Hz timer
  reads how far it has played (the position in LPIB) and writes fresh
  samples behind it. With a 200 ms ring, refilling every 10 ms leaves
  plenty of margin, and no interrupt routing is needed (the same way
  the network and USB drivers already work).
- **A mixer in the kernel.** Each program that plays sound gets a stream
  with its own buffer. On each refill the kernel adds the streams
  together, clamps, and applies the master volume. When nothing plays,
  silence is written.
- **Codec setup by walking the widget graph.** HDA codecs differ between
  machines, so the driver does not hard-code nodes: it finds an output
  pin (line out, speaker or headphone, as the pin's configuration
  default says it is connected), follows connections back to a DAC
  through mixers and selectors, unmutes and sets gain along the path,
  and turns on EAPD where present (some laptops' amplifiers need it).
  Every connected output pin that reaches a DAC is driven, so
  headphones and speakers both play.

## Steps

1. **HDA driver and a test tone.** Find the controller (PCI class 04,
   subclass 03), reset it, talk to codecs through CORB/RIRB (with the
   immediate command registers as a fallback), walk the widget graph,
   and play one output stream from a ring of buffers. A kernel shell
   command `sound` reports what was found and `beep` plays a short tone.
   Tested in QEMU with the WAV audio backend
   (`-audiodev wav,id=a,path=out.wav`), which records exactly what the
   guest played: the test checks the tone's frequency in the file.
2. **Mixer and the sound call.** Per-program streams mixed into the
   ring, a master volume (Settings, and the volume keys), and an OPOS
   call for programs: open a stream, write samples (blocking while the
   stream's buffer is full), ask how much is queued, close. Added to
   `sdk/include/opos.h`, and through the same call for Linux programs.
   An OPOS demo program plays a WAV file.
3. **SDL audio driver.** `store/linux/sdl2/opos/SDL_oposaudio.c`: SDL's
   audio thread fills a buffer from the program's callback and writes it
   with the sound call. The SDL demos gain `testmultiaudio`.
4. **Doom with sound.** SDL_mixer (no codec libraries: Doom's effects are
   raw samples and its music is played by Chocolate Doom's own OPL
   emulator), then Chocolate Doom built with it: effects and music.
5. **Real hardware.** Run on real machines and VirtualBox/VMware; fix
   what codecs differ in. Interrupt-driven refill if polling turns out to
   glitch under load.

## As built in step 1

`kernel/hda.c` drives the first HDA controller (PCI class 04, subclass
03) at boot:
- **Controller.** Reset through GCTL, codecs found in STATESTS. Commands
  go through the CORB and answers come back in the RIRB, both 256
  entries and polled. The RIRB's response-flag enable is on (with
  interrupts off in INTCTL): QEMU's controller stops taking commands
  after RINTCNT answers until software clears that flag, which it only
  sets with the enable on.
- **Codec.** The first codec with an audio function group and a usable
  output is used. Every output-capable pin whose configuration is line
  out, speaker or headphones (and not "no connection") is followed back
  through mixers and selectors to an analog DAC. Each node on the way is
  powered, selected and unmuted at 0 dB; the pin's output (and headphone
  amplifier, for headphone pins) and EAPD are turned on; every DAC
  reached gets stream 1 and the format.
- **Stream.** The first output stream plays a ring of 8 buffers of
  4 KiB (about 170 ms) at 48 kHz, 16-bit stereo, with no interrupts. The
  timer (100 Hz) calls `hda_poll`, which reads LPIB, adds what was
  played, and fills the ring up to one buffer behind the play position
  from the source (`hda_set_source`; silence for now).
- **Shell.** `sound` shows the controller, codec and paths; `beep [HZ]`
  plays a 500 ms square wave (750 Hz by default) over the source.

Machines without HDA log "no sound output found"; nothing else changes.

**Test.** `make sound-test` boots the ISO in QEMU with `intel-hda` and
`hda-output`, records the guest's sound with the monitor's `wavcapture`,
plays `beep` and `beep 1500`, and `tools/check_tones.py` finds both
tones in the recording. Also checked by hand: `hda-duplex` (line out,
the line-in pin is left alone) and `ich9-intel-hda` with `hda-micro`
(found as speaker). Not yet tried on real hardware, VirtualBox or VMware
(step 5).

## As built in step 2

`kernel/sound.c` is the mixer. `sound_initialize` makes it the HDA
driver's source, so the timer interrupt mixes every 10 ms:
- **Streams.** Up to 8 at a time, across all programs. A stream is a ring
  of 48 kHz, 16-bit stereo frames whose size the program picks when it
  opens it (rounded up to a power of two, 1024 to 32768 frames: 21 ms to
  0.68 s), so a game can keep its latency low and a music player can
  queue more. The program only moves the write index and the mixer only
  moves the read index, so writing needs no lock. Closing a stream while
  another thread of the program copies into it marks the stream closing;
  the copy frees it when it ends.
- **Mixing.** The streams are summed in 32 bits, multiplied by the master
  volume on a squared curve (volume² / 10000, so 50 sounds about half as
  loud as 100), and clamped to 16 bits. A stream that runs dry just adds
  nothing.
- **Calls.** `0x4f500040` open(capacity) returns a handle, or -19 without
  sound hardware, -24 when all 8 streams are in use. `0x4f500041`
  write(handle, frames, count) queues every frame, waiting a timer tick
  at a time while the stream is full; the program's memory is checked
  again on each pass. `0x4f500042` queued(handle) and `0x4f500043`
  close(handle). Handles belong to the process that opened them, and its
  streams are closed when it ends. The SDK wraps them as
  `opos_sound_open`, `opos_sound_write`, `opos_sound_queued` and
  `opos_sound_close`. The Linux-side wrapper comes with the SDL driver in
  step 3: musl programs can already make these calls with `syscall()`.
- **Volume.** 0 to 100, 70 by default. Kept in `OPOS.CFG` (settings file
  version 3 adds a volume byte: the volume plus one, 0 meaning "not set";
  versions 1 and 2 are still read) and in CMOS register 0x4b. The volume
  keys (scancodes E0 30 up, E0 2E down, E0 20 mute) change it in steps of
  5 and show a popup over the clock for two seconds. Mute is not saved.
  The shell has `volume [0-100|mute|unmute]`, and `sound` also prints the
  volume and how many streams are open. A volume slider in Settings is
  left for later.
- **Demo.** `/bin/play` (`sdk/examples/play.c`) plays a 16-bit PCM WAV
  file, mono or stereo, at any rate (converted to 48 kHz by linear
  interpolation). With no file it plays 440 Hz and then 880 Hz.

**Test.** `make sound-test` now also runs `/bin/play` and checks for
440 Hz and 880 Hz after the two beeps. By hand: at volume 70 the tones
peak at 5879 (12000 × 0.49) and at volume 30 at 1079 (12000 × 0.09), and
`play` prints "no sound output" on a machine without HDA.

## As built in step 3

`store/linux/sdl2/opos/SDL_oposaudio.c` is an SDL 2 audio driver. The
SDL recipe copies it next to SDL's dummy audio driver (whose sources
`configure` compiles anyway), puts it first in SDL's driver list and
builds SDL with `SDL_AUDIO_DRIVER_OPOS`:
- **Format.** The device always runs at 48 kHz, 16-bit stereo, OPOS's
  stream format; SDL converts from whatever the program asks for. The
  program's buffer size (`samples`) is kept, up to 16384 frames.
- **Playing.** Each SDL audio device opens one OPOS stream twice the
  size of SDL's buffer (at least 1024 frames). SDL's audio thread fills
  its buffer from the program's callback and `PlayDevice` writes it with
  the sound call, which waits while the stream is full. That pacing
  replaces `WaitDevice`, so the delay stays near two buffers.
- **No sound hardware.** The driver checks for it when SDL starts its
  audio (by opening and closing a stream). Without it, SDL has no audio
  driver, as before this step, and programs run silent.
- The driver makes the system calls itself, like the video driver, so
  SDL needs no OPOS headers. The SDL Demos package gains `testmultiaudio`
  ("SDL Sound" in Start), which plays SDL's `sample.wav` twice.

**Test.** In QEMU with `intel-hda`, `testmultiaudio` from the installed
package prints "Using audio driver: opos" and exits with status 0. The
recording (monitor `wavcapture`) holds two plays of 10.81 s and 10.94 s
(the file is 10.96 s; the quiet ends fall under the detector's
threshold) that correlate at 0.998 with each other, so playback is
steady.

## Not planned yet

Recording, more than one sound card at a time, sample-rate conversion in
the kernel, surround (more than two channels).
