#!/bin/sh
# OpenTTD for OPOS: the open-source transport simulation game, with the
# OpenGFX graphics, OpenSFX sounds and OpenMSX music, so it runs without
# the original game's files. Licenses: OpenTTD, OpenGFX and OpenMSX
# GPL-2.0 and FluidSynth LGPL-2.1 (the package carries their source
# releases), OpenSFX CC BY-SA 3.0 (its license.txt is inside its tar),
# GeneralUser GS (its own free license, soundfont/LICENSE.txt), SDL and
# zlib zlib license, liblzma public domain, musl MIT. Built against SDL 2
# from store/linux/sdl2 (built first when OUT/sdl2 is missing); sound and
# music go through SDL's OPOS audio driver. OpenTTD's FluidSynth music
# driver renders the OpenMSX MIDI files with the GeneralUser GS SoundFont;
# the Start menu entry is openttd-start (start.c), which points OpenTTD at
# that SoundFont. The package asks for the network (capabilities: net), so
# multiplayer, the server list and the online content window work over
# OPOS's IPv4 sockets (OpenTTD skips IPv6 addresses it cannot open). No
# PNG, FreeType, ICU or curl: the game uses its sprite font, and without
# curl it downloads online content through its own TCP content protocol
# instead of HTTP, which OpenTTD does by itself. OpenTTD sets its sockets
# non-blocking with ioctl(FIONBIO), which needs OPOS 0.6.7. OpenTTD's
# source is not changed.
#
# OpenTTD is C++, so this recipe needs a musl cross toolchain with g++
# (musl-cross-make), not LINUX_CC: set MUSL_CROSS to its prefix, for
# example $HOME/musl-cross/bin/x86_64-linux-musl- (the default). It also
# needs cmake and a host C++ compiler for OpenTTD's build tools.
# Not built by default: add "openttd" to STORE_LINUX.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=15.3
# The package version: OpenTTD's, plus a fourth number for OPOS rebuilds.
PACKAGE_VERSION=15.3.0.1
OPENGFX=8.0
OPENSFX=1.0.3
OPENMSX=0.4.2
FLUIDSYNTH=2.6.1
# GeneralUser GS 2.0.3 has no release tags: pinned to a commit.
GENERALUSER=684543d5e5efaef08d02be50dcda8d552478fa60
ZLIB=1.3.1
XZ=5.4.6
CROSS=${MUSL_CROSS:-$HOME/musl-cross/bin/x86_64-linux-musl-}
if [ ! -x "${CROSS}g++" ]; then
    echo "store/linux/openttd: ${CROSS}g++ not found; set MUSL_CROSS to a" \
         "musl cross toolchain prefix (musl-cross-make)" >&2
    exit 1
fi
SOURCE=$(fetch "https://cdn.openttd.org/openttd-releases/$VERSION/openttd-$VERSION-source.tar.xz" \
    5ea21eea7d59c78a42071924ac18c6bc0116088f2e96b14cfee9369175973be7 \
    "$CACHE")
GFX=$(fetch "https://cdn.openttd.org/opengfx-releases/$OPENGFX/opengfx-$OPENGFX-all.zip" \
    43a0c1dabf39cb865394f3a6cc36d4da5c10ecfaaf55652043104806810903be \
    "$CACHE")
# OpenGFX 8.0 has no source tarball on cdn.openttd.org: its source is the
# release tag on GitHub (the archive is named 8.0.tar.gz there).
GFX_SOURCE=$(fetch "https://github.com/OpenTTD/OpenGFX/archive/refs/tags/$OPENGFX.tar.gz" \
    0c3575cf70fb3ccaa2c2a8d3525598d10534a1f4401fa70a98814ecab5287e56 \
    "$CACHE")
SFX=$(fetch "https://cdn.openttd.org/opensfx-releases/$OPENSFX/opensfx-$OPENSFX-all.zip" \
    e0a218b7dd9438e701503b0f84c25a97c1c11b7c2f025323fb19d6db16ef3759 \
    "$CACHE")
MSX=$(fetch "https://cdn.openttd.org/openmsx-releases/$OPENMSX/openmsx-$OPENMSX-all.zip" \
    5a4277a2e62d87f2952ea5020dc20fb2f6ffafdccf9913fbf35ad45ee30ec762 \
    "$CACHE")
MSX_SOURCE=$(fetch "https://cdn.openttd.org/openmsx-releases/$OPENMSX/openmsx-$OPENMSX-source.tar.xz" \
    0f5196f9045c9123ed9207930a73871811654e46e7de2d079be8f8ee501ba840 \
    "$CACHE")
FLUID_SOURCE=$(fetch "https://github.com/FluidSynth/fluidsynth/archive/refs/tags/v$FLUIDSYNTH.tar.gz" \
    3d258a3bf97cc20c59eeebfe62c2432fae88adda74d3ad098681c76e0ebf446b \
    "$CACHE")
SOUNDFONT=$(fetch "https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/$GENERALUSER/GeneralUser-GS.sf2" \
    9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe \
    "$CACHE")
SOUNDFONT_LICENSE=$(fetch "https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/$GENERALUSER/documentation/LICENSE.txt" \
    7b32efefdf95ce38a043799f0659853ddc00fbaa14d8c50f0aca16b9b8b405be \
    "$CACHE")
ZLIB_SOURCE=$(fetch "https://zlib.net/fossils/zlib-$ZLIB.tar.gz" \
    9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 \
    "$CACHE")
XZ_SOURCE=$(fetch "https://github.com/tukaani-project/xz/releases/download/v$XZ/xz-$XZ.tar.gz" \
    aeba3e03bf8140ddedf62a0a367158340520f6b384f75ca6045ccc6c0d43fd5c \
    "$CACHE")

SDL=$OUT/sdl2
if [ ! -f "$SDL/lib/libSDL2.a" ]; then
    SDL2_LIBRARY_ONLY=1 sh "$HERE/../sdl2/build.sh" "$OUT" "$CACHE"
fi

DIR=$(work openttd "$OUT")
DEPS=$DIR/deps
JOBS=$(nproc 2>/dev/null || echo 4)

# zlib and liblzma, static (title game and saves are LZMA-compressed).
tar -xzf "$ZLIB_SOURCE" -C "$DIR"
(cd "$DIR/zlib-$ZLIB" && CC="${CROSS}gcc" CFLAGS=-O2 \
    ./configure --static --prefix="$DEPS" && make -j"$JOBS" && make install) \
    > "$DIR/zlib.log" 2>&1 || { tail -20 "$DIR/zlib.log" >&2; exit 1; }
tar -xzf "$XZ_SOURCE" -C "$DIR"
(cd "$DIR/xz-$XZ" && CC="${CROSS}gcc" ./configure --host=x86_64-linux-musl \
    --prefix="$DEPS" --disable-shared --enable-static --disable-xz \
    --disable-xzdec --disable-lzmadec --disable-lzmainfo --disable-lzma-links \
    --disable-scripts --disable-doc --disable-nls --disable-threads \
    && make -j"$JOBS" && make install) \
    > "$DIR/xz.log" 2>&1 || { tail -20 "$DIR/xz.log" >&2; exit 1; }

SYSROOT=$(dirname "$(dirname "$CROSS")")/x86_64-linux-musl
cat > "$DIR/toolchain.cmake" <<TOOLCHAIN
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${CROSS}gcc)
set(CMAKE_CXX_COMPILER ${CROSS}g++)
set(CMAKE_FIND_ROOT_PATH $SYSROOT $DEPS $SDL)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
TOOLCHAIN

# FluidSynth, static, as a synthesizer only: OpenTTD mixes its output
# itself, so every audio and MIDI driver, file and network option is off.
tar -xzf "$FLUID_SOURCE" -C "$DIR"
FLUID_OFF=
for option in alsa aufile dbus dsound ipv6 jack ladspa libsndfile midishare \
    network opensles oboe openmp oss pipewire portaudio pulseaudio readline \
    sdl3 systemd wasapi waveout winmidi coreaudio coremidi framework dart kai \
    native-dls signalsmith; do
    FLUID_OFF="$FLUID_OFF -Denable-$option=off"
done
(cmake -S "$DIR/fluidsynth-$FLUIDSYNTH" -B "$DIR/fluidsynth" \
    -DCMAKE_TOOLCHAIN_FILE="$DIR/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=off \
    -DCMAKE_INSTALL_PREFIX="$DEPS" -DCMAKE_INSTALL_LIBDIR=lib $FLUID_OFF \
    && cmake --build "$DIR/fluidsynth" -j"$JOBS" --target libfluidsynth) \
    > "$DIR/fluidsynth.log" 2>&1 || { tail -30 "$DIR/fluidsynth.log" >&2; exit 1; }
# Only the library was built, so install it by hand (cmake --install would
# also want the fluidsynth command-line program).
mkdir -p "$DEPS/include/fluidsynth"
cp "$DIR/fluidsynth/src/libfluidsynth.a" "$DEPS/lib/"
cp "$DIR/fluidsynth-$FLUIDSYNTH/include/fluidsynth/"*.h "$DEPS/include/fluidsynth/"
cp "$DIR/fluidsynth/include/fluidsynth/"*.h "$DEPS/include/fluidsynth/"
cp "$DIR/fluidsynth/include/fluidsynth.h" "$DEPS/include/"

tar -xJf "$SOURCE" -C "$DIR"
SRC=$DIR/openttd-$VERSION

# OpenTTD's build tools (strgen, settingsgen) run on the build machine.
(env -u CC -u CXX cmake -S "$SRC" -B "$DIR/host" -DOPTION_TOOLS_ONLY=ON \
    -DCMAKE_BUILD_TYPE=Release \
    && cmake --build "$DIR/host" -j"$JOBS" --target tools) \
    > "$DIR/host.log" 2>&1 || { tail -30 "$DIR/host.log" >&2; exit 1; }

(cmake -S "$SRC" -B "$DIR/cross" \
    -DCMAKE_TOOLCHAIN_FILE="$DIR/toolchain.cmake" \
    -DHOST_BINARY_DIR="$DIR/host" \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPTION_DEDICATED=OFF \
    -DOPTION_USE_ASSERTS=OFF \
    -DSDL2_DIR="$SDL/lib/cmake/SDL2" \
    -DCMAKE_PREFIX_PATH="$DEPS;$SDL" \
    -DCMAKE_EXE_LINKER_FLAGS=-static \
    && cmake --build "$DIR/cross" -j"$JOBS") \
    > "$DIR/build.log" 2>&1 || {
    grep -E "error|undefined" "$DIR/build.log" | head -30 >&2
    exit 1
}
if ! grep -q "Fluidsynth found" "$DIR/build.log"; then
    echo "store/linux/openttd: OpenTTD was built without FluidSynth" >&2
    exit 1
fi

STAGE=$DIR/stage
mkdir -p "$STAGE/soundfont"
"${CROSS}strip" -o "$STAGE/openttd" "$DIR/cross/openttd"
$LINUX_CC -O2 -Wall -o "$DIR/openttd-start" "$HERE/start.c" -static
strip -o "$STAGE/openttd-start" "$DIR/openttd-start"
# baseset/ from the build holds the title game, openttd.grf and the
# no_sound/no_music sets the game needs to start.
for d in baseset lang ai game; do
    cp -r "$DIR/cross/$d" "$STAGE/"
done
# Graphics and sounds stay as .tar files, which OpenTTD reads in place.
# Music cannot: OpenTTD hands each song's file path to the MIDI player, so
# the OpenMSX tar is unpacked into baseset/openmsx-$OPENMSX/, as OpenTTD's
# own content download does.
python3 -I - "$STAGE/baseset" "$GFX" "$SFX" "$MSX" <<'PY'
import io, sys, tarfile, zipfile
baseset, graphics, sounds, music = sys.argv[1:]
for archive in (graphics, sounds):
    zipfile.ZipFile(archive).extractall(baseset)
with zipfile.ZipFile(music) as z:
    (name,) = [n for n in z.namelist() if n.endswith(".tar")]
    with tarfile.open(fileobj=io.BytesIO(z.read(name))) as tar:
        for member in tar.getmembers():
            if not (member.isfile() or member.isdir()) or member.name.startswith("/") \
                    or ".." in member.name.split("/"):
                raise SystemExit("unexpected entry in the OpenMSX tar: " + member.name)
        tar.extractall(baseset)
PY
if [ ! -f "$STAGE/baseset/openmsx-$OPENMSX/openmsx.obm" ]; then
    echo "store/linux/openttd: OpenMSX was not unpacked" >&2
    exit 1
fi
cp "$SOUNDFONT" "$STAGE/soundfont/GeneralUser-GS.sf2"
cp "$SOUNDFONT_LICENSE" "$STAGE/soundfont/LICENSE.txt"
cp "$SRC/COPYING.md" "$STAGE/LICENSE"
SDL_VERSION=$(sed -n 's/^VERSION=//p' "$HERE/../sdl2/build.sh")
cat >> "$STAGE/LICENSE" <<NOTE

The openttd program in this package is OpenTTD $VERSION (GPL-2.0, above),
unchanged, built for OPOS with FluidSynth $FLUIDSYNTH (LGPL-2.1, see
LICENSE.fluidsynth), SDL $SDL_VERSION (zlib license, see LICENSE.sdl),
zlib $ZLIB (LICENSE.zlib), liblzma from XZ Utils $XZ (public domain, see
LICENSE.liblzma) and musl libc (LICENSE.musl). Its sources are
source/openttd-$VERSION-source.tar.xz and
source/fluidsynth-$FLUIDSYNTH.tar.gz in this package; the OPOS build
recipe is store/linux/openttd in the OPOS repository. openttd-start
(OPOS, see start.c there) runs openttd with the SoundFont below.

baseset/ holds OpenGFX $OPENGFX (GPL-2.0; source
source/opengfx-$OPENGFX.tar.gz), OpenSFX $OPENSFX (CC BY-SA 3.0)
and OpenMSX $OPENMSX (GPL-2.0; source
source/openmsx-$OPENMSX-source.tar.xz). Each set's license.txt and
readme.txt with its credits are inside its .tar file, or for OpenMSX in
baseset/openmsx-$OPENMSX/.

soundfont/GeneralUser-GS.sf2 is GeneralUser GS by S. Christian Collins,
free to use and redistribute under its own license
(soundfont/LICENSE.txt). More at https://www.schristiancollins.com
NOTE
tar -xzOf "$CACHE/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION/LICENSE.txt" \
    > "$STAGE/LICENSE.sdl"
tar -xzOf "$FLUID_SOURCE" "fluidsynth-$FLUIDSYNTH/LICENSE" > "$STAGE/LICENSE.fluidsynth"
tar -xzOf "$ZLIB_SOURCE" "zlib-$ZLIB/LICENSE" > "$STAGE/LICENSE.zlib"
tar -xzOf "$XZ_SOURCE" "xz-$XZ/COPYING" > "$STAGE/LICENSE.liblzma"
mkdir -p "$STAGE/source"
cp "$SOURCE" "$MSX_SOURCE" "$STAGE/source/"
cp "$GFX_SOURCE" "$STAGE/source/opengfx-$OPENGFX.tar.gz"
cp "$FLUID_SOURCE" "$STAGE/source/fluidsynth-$FLUIDSYNTH.tar.gz"

{
    cat <<PKG
format: 1
id: org.openttd.openttd
name: OpenTTD
version: $PACKAGE_VERSION
publisher: OpenTTD, OpenGFX, OpenSFX, OpenMSX (built for OPOS)
platform: linux
capabilities: net
requires-opos: 0.6.7
PKG
    (cd "$STAGE" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) |
    while read -r f; do
        case $f in
        openttd|openttd-start) echo "file: files/$f $f exec" ;;
        *) echo "file: files/$f $f" ;;
        esac
    done
    echo "file: files/LICENSE.musl LICENSE.musl"
    echo "start: files/openttd-start OpenTTD"
} > "$DIR/package.pkg"
mv "$DIR/package.pkg" "$STAGE/package.pkg"
package "$STAGE" "$OUT"
