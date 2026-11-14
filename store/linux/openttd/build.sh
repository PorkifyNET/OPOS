#!/bin/sh
# OpenTTD for OPOS: the open-source transport simulation game, with the
# OpenGFX graphics set so it runs without the original game's files.
# Licenses: OpenTTD GPL-2.0 and OpenGFX GPL-2.0 (the package carries both
# source releases), SDL and zlib zlib license, liblzma public domain,
# musl MIT. Built against SDL 2 from store/linux/sdl2 (built first when
# OUT/sdl2 is missing). No sound set (OpenSFX) or music yet, no PNG,
# FreeType, ICU or curl: the game uses its sprite font and cannot
# download online content.
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
VERSION=14.1
OPENGFX=7.1
ZLIB=1.3.1
XZ=5.4.6
CROSS=${MUSL_CROSS:-$HOME/musl-cross/bin/x86_64-linux-musl-}
if [ ! -x "${CROSS}g++" ]; then
    echo "store/linux/openttd: ${CROSS}g++ not found; set MUSL_CROSS to a" \
         "musl cross toolchain prefix (musl-cross-make)" >&2
    exit 1
fi
SOURCE=$(fetch "https://cdn.openttd.org/openttd-releases/$VERSION/openttd-$VERSION-source.tar.xz" \
    2c14c8f01f44148c4f2c88c169a30abcdb002eb128a92b9adb76baa76b013494 \
    "$CACHE")
GFX=$(fetch "https://cdn.openttd.org/opengfx-releases/$OPENGFX/opengfx-$OPENGFX-all.zip" \
    928fcf34efd0719a3560cbab6821d71ce686b6315e8825360fba87a7a94d7846 \
    "$CACHE")
GFX_SOURCE=$(fetch "https://cdn.openttd.org/opengfx-releases/$OPENGFX/opengfx-$OPENGFX-source.tar.xz" \
    5322524ffd854cb65d1a4f156d84a3573c4156d08c3757aafd1b8fa3cda41f5a \
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

tar -xJf "$SOURCE" -C "$DIR"
SRC=$DIR/openttd-$VERSION

# OpenTTD's build tools (strgen, settingsgen) run on the build machine.
(env -u CC -u CXX cmake -S "$SRC" -B "$DIR/host" -DOPTION_TOOLS_ONLY=ON \
    -DCMAKE_BUILD_TYPE=Release \
    && cmake --build "$DIR/host" -j"$JOBS" --target tools) \
    > "$DIR/host.log" 2>&1 || { tail -30 "$DIR/host.log" >&2; exit 1; }

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

STAGE=$DIR/stage
mkdir -p "$STAGE"
"${CROSS}strip" -o "$STAGE/openttd" "$DIR/cross/openttd"
# baseset/ from the build holds the title game, openttd.grf and the
# no_sound/no_music sets the game needs to start.
for d in baseset lang ai game; do
    cp -r "$DIR/cross/$d" "$STAGE/"
done
python3 -I - "$GFX" "$STAGE/baseset" <<'PY'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
cp "$SRC/COPYING.md" "$STAGE/LICENSE"
SDL_VERSION=$(sed -n 's/^VERSION=//p' "$HERE/../sdl2/build.sh")
cat >> "$STAGE/LICENSE" <<NOTE

The openttd program in this package is OpenTTD $VERSION (GPL-2.0, above),
built for OPOS with SDL $SDL_VERSION (zlib license, see LICENSE.sdl),
zlib $ZLIB (LICENSE.zlib), liblzma from XZ Utils $XZ (public domain, see
LICENSE.liblzma) and musl libc (LICENSE.musl). Its source is
source/openttd-$VERSION-source.tar.xz in this package. baseset/ holds
OpenGFX $OPENGFX (GPL-2.0); its source is
source/opengfx-$OPENGFX-source.tar.xz.
NOTE
tar -xzOf "$CACHE/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION/LICENSE.txt" \
    > "$STAGE/LICENSE.sdl"
tar -xzOf "$ZLIB_SOURCE" "zlib-$ZLIB/LICENSE" > "$STAGE/LICENSE.zlib"
tar -xzOf "$XZ_SOURCE" "xz-$XZ/COPYING" > "$STAGE/LICENSE.liblzma"
mkdir -p "$STAGE/source"
cp "$SOURCE" "$GFX_SOURCE" "$STAGE/source/"

{
    cat <<PKG
format: 1
id: org.openttd.openttd
name: OpenTTD
version: $VERSION
publisher: OpenTTD and OpenGFX (built for OPOS)
platform: linux
requires-opos: 0.6.0
PKG
    (cd "$STAGE" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) |
    while read -r f; do
        if [ "$f" = openttd ]; then
            echo "file: files/$f $f exec"
        else
            echo "file: files/$f $f"
        fi
    done
    echo "file: files/LICENSE.musl LICENSE.musl"
    echo "start: files/openttd OpenTTD"
} > "$DIR/package.pkg"
mv "$DIR/package.pkg" "$STAGE/package.pkg"
package "$STAGE" "$OUT"
