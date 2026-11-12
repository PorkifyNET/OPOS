#!/bin/sh
# SDL_mixer 2 for OPOS, as a static library for SDL programs (no package
# of its own). Only the core mixer and WAVE files: every codec that needs
# a library (MIDI, MOD, Ogg, FLAC, MP3, Opus, WavPack, GME) and the
# external music command are off. Doom needs no more: its effects are
# raw samples and Chocolate Doom plays its music with its own OPL
# emulator through SDL_mixer.
# License: zlib. Built against SDL 2 from store/linux/sdl2 (built first
# when OUT/sdl2 is missing). Installed into OUT/sdl2_mixer:
#   $LINUX_CC -I OUT/sdl2_mixer/include/SDL2 -I OUT/sdl2/include/SDL2 \
#       prog.c -L OUT/sdl2_mixer/lib -lSDL2_mixer -L OUT/sdl2/lib -lSDL2
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=2.8.1
SOURCE=$(fetch "https://github.com/libsdl-org/SDL_mixer/releases/download/release-$VERSION/SDL2_mixer-$VERSION.tar.gz" \
    cb760211b056bfe44f4a1e180cc7cb201137e4d1572f2002cc1be728efd22660 \
    "$CACHE")

SDL=$OUT/sdl2
if [ ! -f "$SDL/lib/libSDL2.a" ]; then
    SDL2_LIBRARY_ONLY=1 sh "$HERE/../sdl2/build.sh" "$OUT" "$CACHE"
fi

DIR=$(work sdl2_mixer "$OUT")
tar -xzf "$SOURCE" -C "$DIR"
SRC=$DIR/SDL2_mixer-$VERSION
PREFIX=$OUT/sdl2_mixer
rm -rf "$PREFIX"
(cd "$SRC" && CC="$LINUX_CC" CFLAGS=-O2 LDFLAGS=-static \
    SDL2_CONFIG="$SDL/bin/sdl2-config" PKG_CONFIG_PATH="$SDL/lib/pkgconfig" \
    ./configure --prefix="$PREFIX" --disable-shared --enable-static \
    --disable-sdltest --with-sdl-prefix="$SDL" \
    --disable-music-cmd --enable-music-wave \
    --disable-music-mod --disable-music-mod-modplug \
    --disable-music-mod-xmp --disable-music-midi \
    --disable-music-midi-timidity --disable-music-midi-native \
    --disable-music-midi-fluidsynth --disable-music-gme \
    --disable-music-ogg --disable-music-flac --disable-music-mp3 \
    --disable-music-opus --disable-music-wavpack) \
    > "$DIR/configure.log" 2>&1 || {
    tail -30 "$DIR/configure.log" >&2
    exit 1
}
make -C "$SRC" -j"$(nproc)" > "$DIR/build.log" 2>&1 \
    && make -C "$SRC" install >> "$DIR/build.log" 2>&1 || {
    tail -30 "$DIR/build.log" >&2
    exit 1
}
