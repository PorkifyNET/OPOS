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
