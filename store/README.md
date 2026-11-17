# OPOS Store packages

The packages OPOS publishes in its Store, built from this repository.
`make store` builds each package listed in `STORE_PACKAGES` in
`GNUmakefile`, and each Linux program in `STORE_LINUX` with
`linux/<name>/build.sh` (docs/linux-programs.md), and writes
`build/store/`:

- one `<id>-<version>.opk` per package,
- `index`, listing them with what `catalog.txt` says of each
  (category, license, summary, description),
- `index.sig`, signed with the store key
  (`STORE_KEY`, default `~/.opos-signing/store-key.pem`).

Linux programs are downloaded from their pinned source releases (each
checked against its SHA-256) and built with a static musl compiler:
`build/musl-local.specs` when present, otherwise `musl-gcc`
(`apt install musl-tools`). Override with `LINUX_CC=...` and
`MUSL_LICENSE=...`.

Font packages are listed in `STORE_FONTS` (default `cjk`): each
`fonts/<name>/build.sh` downloads its pinned font files and packages
them. `fonts/cjk` is "Chinese, Japanese, Korean fonts", Droid Sans
Fallback (Apache-2.0, 4 MB). It installs no program: OPOS finds the
font through `/share/fonts/fallback.txt` (`data/fonts/fallback.txt`).

`linux/busybox/build.sh` is not in the default `STORE_LINUX`. BusyBox
is GPL-2.0, and its package carries its source release and build
configuration. Add it with `STORE_LINUX="lua sqlite busybox"`. It needs
the Linux headers (`apt install linux-libc-dev`) and `bzip2`.

`linux/sdl2/build.sh` is not in the default `STORE_LINUX` either. It
builds SDL 2 with the OPOS video driver in `linux/sdl2/opos/` and
packages four of SDL's demo programs (SDL Demos). It also installs the
library into `build/store_opk/sdl2/` while `make store` runs, for later
recipes of SDL programs. Add it with `STORE_LINUX="lua sqlite sdl2"`.

`linux/doom/build.sh` (opt-in) builds Chocolate Doom 3.1.1 against that
SDL (building SDL first if `sdl2` is not listed before it) and packages
it with Freedoom: Phase 1 as Freedoom, 33 MB. Chocolate Doom is GPL-2.0,
so the package carries its source release. Add it with
`STORE_LINUX="lua sqlite sdl2 doom"`.

`linux/openttd/build.sh` (opt-in) builds OpenTTD 14.1 against that SDL
and packages it with the OpenGFX 7.1 graphics, OpenSFX 1.0.3 sounds
(CC BY-SA 3.0) and OpenMSX 0.4.2 music. Music is MIDI, played by
FluidSynth 2.6.1 (LGPL-2.1, linked into OpenTTD) with the GeneralUser GS
SoundFont; the Start menu runs `openttd-start` (`linux/openttd/start.c`),
which passes OpenTTD that SoundFont. About 100 MB. OpenTTD is
C++, so it needs a musl cross toolchain with `g++` (musl-cross-make)
instead of `LINUX_CC`: set `MUSL_CROSS` to its prefix (default
`~/musl-cross/bin/x86_64-linux-musl-`). It also needs `cmake` and a host
C++ compiler. The package carries the source releases of OpenTTD,
OpenGFX, OpenMSX (GPL-2.0) and FluidSynth (LGPL-2.1). It needs OPOS 0.6.0. Add it with
`STORE_LINUX="lua sqlite sdl2 openttd"`.

`linux/scummvm/build.sh` (opt-in) builds ScummVM 2026.4.0 against that
SDL with zlib, libpng, libmad (MP3), Ogg Vorbis and FLAC for game
speech and music, its default stable engines and no network support.
The program is about 85 MB, so it needs an OPOS kernel with the raised
program size limits (the release after 0.6.6). The Start menu runs `scummvm-start`
(`linux/scummvm/start.c`), which points ScummVM at the package's
`data/` folder (themes, translations, engine data files). No games are
included. It needs `MUSL_CROSS` like OpenTTD. ScummVM's source release
(GPL-3.0 or later) is 210 MB, so it is not inside the package: the
recipe puts `scummvm-<version>.tar.xz` in `build/store/` beside the
packages, and it must be uploaded to the Store release with them (the
package's `SOURCE.txt` points there). libmad's source is in the package.

`linux/scummvm-games/build.sh` (opt-in) packages the two freeware games
the ScummVM team gives away, unchanged and each with its readme as
`LICENSE`: Beneath a Steel Sky (`org.scummvm.bass`) and Flight of the
Amazon Queen (`org.scummvm.fotaq`), both CD versions with speech. Their
Start menu entry (`linux/scummvm-games/game-start.c`) runs the ScummVM
package's `scummvm-start` on the game folder, so ScummVM must be
installed too.

`linux/dosbox-staging/build.sh` (opt-in) builds DOSBox Staging 0.83.1
against that SDL, with its libraries (SDL_image, iir1, libmt32emu,
FluidSynth, SpeexDSP, Opus, zlib-ng, libpng, alsa-lib, asio) built
static first. OPOS has no OpenGL, so DOSBox falls back to drawing with
SDL's renderer; an empty `libGL.a` stand-in satisfies its CMake check.
It needs the OPOS kernel after 0.6.6 too (`SA_SIGINFO` handlers, a
44 MB program). The recipe needs `MUSL_CROSS`, `cmake` and
`pkg-config`, and carries the sources of DOSBox Staging and its LGPL
libraries. Add all three with
`STORE_LINUX="lua sqlite sdl2 scummvm scummvm-games dosbox-staging"`.

Upload everything in `build/store/` as the assets of a new release of
`PorkifyNET/opos-packages`. OPOS reads
`releases/latest/download/index` from there. See `docs/packages.md`.

To test without publishing, copy `build/store/` to an OPOS disk and
write its path (for example `/disk/store/`) into
`/disk/OPOS/PACKAGE-SOURCE.TXT`, or put it on a CD or USB stick as
`opos-packages/`.

To publish a new version of a package, raise `version` in its `.pkg`
file and in its `OPOS_MANIFEST`, then run `make store` again. The
index keeps only the packages of this run: a program that is published
but not built this time (Freedoom and OpenTTD are opt-in) drops out of
the Store. To keep such packages without rebuilding them, put their
published `.opk` files in a folder and name it in `STORE_PREBUILT`, for
example:

    make store STORE_PREBUILT=~/store-prebuilt

`~/store-prebuilt` then holds `org.chocolate-doom.freedoom-0.13.0.opk`
and `org.openttd.openttd-14.1.0.2.opk`, downloaded from the current
release. They are checked and listed as they are, with what
`catalog.txt` says of them. Upload only the files that changed: the
index, its signature and any new `.opk` files.
