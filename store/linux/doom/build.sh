#!/bin/sh
# Doom for OPOS: Chocolate Doom (the Doom engine, kept faithful to the
# original) with Freedoom: Phase 1, a complete free game for it.
# Licenses: Chocolate Doom GPL-2.0 (the package carries its source),
# Freedoom BSD-3-Clause, SDL and SDL_mixer zlib, musl MIT. Built against
# SDL 2 and SDL_mixer from store/linux/sdl2 and store/linux/sdl2_mixer
# (built first when OUT/sdl2 or OUT/sdl2_mixer is missing): sound effects
# and OPL music through OPOS sound. SDL_net is left out (no multiplayer).
# Not built by default: add "doom" to STORE_LINUX.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=3.1.1
FREEDOOM=0.13.0
SOURCE=$(fetch "https://github.com/chocolate-doom/chocolate-doom/archive/refs/tags/chocolate-doom-$VERSION.tar.gz" \
    1edcc41254bdc194beb0d33e267fae306556c4d24110a1d3d3f865717f25da23 \
    "$CACHE")
WADS=$(fetch "https://github.com/freedoom/freedoom/releases/download/v$FREEDOOM/freedoom-$FREEDOOM.zip" \
    3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59 \
    "$CACHE")

SDL=$OUT/sdl2
if [ ! -f "$SDL/lib/libSDL2.a" ]; then
    SDL2_LIBRARY_ONLY=1 sh "$HERE/../sdl2/build.sh" "$OUT" "$CACHE"
fi
MIXER=$OUT/sdl2_mixer
if [ ! -f "$MIXER/lib/libSDL2_mixer.a" ]; then
    sh "$HERE/../sdl2_mixer/build.sh" "$OUT" "$CACHE"
fi

DIR=$(work doom "$OUT")
tar -xzf "$SOURCE" -C "$DIR"
SRC=$DIR/chocolate-doom-chocolate-doom-$VERSION

# The configuration header that configure or CMake would write.
cat > "$DIR/config.h" <<CONFIG
#define PACKAGE_NAME "Chocolate Doom"
#define PACKAGE_TARNAME "chocolate-doom"
#define PACKAGE_VERSION "$VERSION"
#define PACKAGE_STRING "Chocolate Doom $VERSION"
#define PROGRAM_PREFIX "chocolate-"
#define HAVE_DIRENT_H 1
#define HAVE_DECL_STRCASECMP 1
#define HAVE_DECL_STRNCASECMP 1
CONFIG

# The game's sources, as src/CMakeLists.txt lists them for
# chocolate-doom; platform files guard themselves with #ifdef.
GAME="i_main.c i_system.c m_argv.c m_misc.c
    aes_prng.c d_event.c d_iwad.c d_loop.c d_mode.c deh_str.c gusconf.c
    i_cdmus.c i_endoom.c i_flmusic.c i_glob.c i_input.c i_joystick.c
    i_musicpack.c i_oplmusic.c i_pcsound.c i_sdlmusic.c i_sdlsound.c
    i_sound.c i_timer.c i_video.c i_videohr.c i_winmusic.c midifallback.c
    midifile.c mus2mid.c m_bbox.c m_cheat.c m_config.c m_controls.c
    m_fixed.c net_client.c net_common.c net_dedicated.c net_gui.c net_io.c
    net_loop.c net_packet.c net_petname.c net_query.c net_sdl.c
    net_server.c net_structrw.c sha1.c memio.c tables.c v_diskicon.c
    v_video.c w_checksum.c w_main.c w_wad.c w_file.c w_file_stdc.c
    w_file_posix.c w_file_win32.c w_merge.c z_zone.c
    deh_io.c deh_main.c deh_mapping.c deh_text.c"
FILES=
for f in $GAME; do
    FILES="$FILES $SRC/src/$f"
done
FILES="$FILES $SRC/src/doom/*.c $SRC/textscreen/*.c $SRC/pcsound/*.c $SRC/opl/*.c"
# textscreen/examples and opl/examples are not part of the game.
$LINUX_CC -O2 -DDISABLE_SDL2NET=1 \
    -I"$DIR" -I"$SRC/src" -I"$SRC/src/doom" -I"$SRC/textscreen" \
    -I"$SRC/pcsound" -I"$SRC/opl" -I"$MIXER/include/SDL2" \
    -I"$SDL/include/SDL2" -D_REENTRANT \
    -o "$DIR/chocolate-doom" $FILES \
    -L"$MIXER/lib" -lSDL2_mixer -L"$SDL/lib" -lSDL2 -lm -static > "$DIR/build.log" 2>&1 || {
    grep -E "error|undefined" "$DIR/build.log" | head -30 >&2
    exit 1
}

STAGE=$DIR/stage
mkdir -p "$STAGE"
strip -o "$STAGE/chocolate-doom" "$DIR/chocolate-doom"
python3 -I - "$WADS" "$STAGE" "$FREEDOOM" <<'PY'
import sys, zipfile
archive, stage, version = sys.argv[1:]
names = {"freedoom1.wad": "freedoom1.wad", "COPYING.txt": "COPYING.freedoom",
         "CREDITS.txt": "CREDITS.freedoom"}
with zipfile.ZipFile(archive) as z:
    for name, target in names.items():
        with z.open("freedoom-%s/%s" % (version, name)) as src, \
             open("%s/%s" % (stage, target), "wb") as dst:
            dst.write(src.read())
PY
cp "$SRC/COPYING.md" "$STAGE/LICENSE"
SDL_VERSION=$(sed -n 's/^VERSION=//p' "$HERE/../sdl2/build.sh")
MIXER_VERSION=$(sed -n 's/^VERSION=//p' "$HERE/../sdl2_mixer/build.sh")
cat >> "$STAGE/LICENSE" <<NOTE

The chocolate-doom program in this package is Chocolate Doom $VERSION
(GPL-2.0, above), built for OPOS with SDL $SDL_VERSION (zlib license, see
LICENSE.sdl), SDL_mixer $MIXER_VERSION (zlib license, see
LICENSE.sdl_mixer) and musl libc (LICENSE.musl). Its source is
chocolate-doom-$VERSION.tar.gz in this package. freedoom1.wad is
Freedoom: Phase 1 $FREEDOOM (BSD-3-Clause, see COPYING.freedoom and
CREDITS.freedoom).
NOTE
tar -xzOf "$CACHE/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION/LICENSE.txt" \
    > "$STAGE/LICENSE.sdl"
tar -xzOf "$CACHE/SDL2_mixer-$MIXER_VERSION.tar.gz" \
    "SDL2_mixer-$MIXER_VERSION/LICENSE.txt" > "$STAGE/LICENSE.sdl_mixer"
cp "$SOURCE" "$STAGE/chocolate-doom-$VERSION.tar.gz"
cat > "$STAGE/package.pkg" <<PKG
format: 1
id: org.chocolate-doom.freedoom
name: Freedoom
version: $FREEDOOM
publisher: Chocolate Doom and Freedoom (built for OPOS)
platform: linux
requires-opos: 0.1.5
file: files/chocolate-doom chocolate-doom exec
file: files/freedoom1.wad freedoom1.wad
file: files/LICENSE LICENSE
file: files/LICENSE.sdl LICENSE.sdl
file: files/LICENSE.sdl_mixer LICENSE.sdl_mixer
file: files/LICENSE.musl LICENSE.musl
file: files/COPYING.freedoom COPYING.freedoom
file: files/CREDITS.freedoom CREDITS.freedoom
file: files/source/chocolate-doom-$VERSION.tar.gz chocolate-doom-$VERSION.tar.gz
start: files/chocolate-doom Freedoom
PKG
package "$STAGE" "$OUT"
