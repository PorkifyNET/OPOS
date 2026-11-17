#!/bin/sh
# ScummVM for OPOS: the engine collection for classic point-and-click
# adventure and role-playing games. Games are not included (most are
# commercial); people add their own game folders in ScummVM, and the
# freeware games ScummVM may give away have their own Store packages
# (store/linux/scummvm-games). Licenses: ScummVM GPL-3.0 or later (the
# source release, 210 MB, is uploaded beside the package in the Store
# release, see SOURCE.txt), libmad GPL-2.0 (its source release is in the
# package), libogg, libvorbis and FLAC BSD-3-Clause, libpng
# libpng license, zlib zlib license, SDL zlib license, musl MIT. Built
# against SDL 2 from store/linux/sdl2 (built first when OUT/sdl2 is
# missing); sound goes through SDL's OPOS audio driver. Built with the
# stable engines ScummVM enables by default, its built-in MT-32 and OPL
# emulators, MP3, Ogg Vorbis and FLAC for compressed game speech and
# music, and no network or cloud support. The Start menu entry is
# scummvm-start (start.c), which points ScummVM at the package's data/
# folder. ScummVM's source is not changed.
#
# ScummVM is C++, so this recipe needs a musl cross toolchain with g++
# (musl-cross-make), not LINUX_CC: set MUSL_CROSS to its prefix, for
# example $HOME/musl-cross/bin/x86_64-linux-musl- (the default).
# Not built by default: add "scummvm" to STORE_LINUX.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=2026.4.0
# The package version: ScummVM's, plus a fourth number for OPOS rebuilds.
PACKAGE_VERSION=2026.4.0.1
ZLIB=1.3.1
PNG=1.6.47
MAD=0.15.1b
OGG=1.3.5
VORBIS=1.3.7
FLAC=1.4.3
CROSS=${MUSL_CROSS:-$HOME/musl-cross/bin/x86_64-linux-musl-}
if [ ! -x "${CROSS}g++" ]; then
    echo "store/linux/scummvm: ${CROSS}g++ not found; set MUSL_CROSS to a" \
         "musl cross toolchain prefix (musl-cross-make)" >&2
    exit 1
fi
SOURCE=$(fetch "https://downloads.scummvm.org/frs/scummvm/$VERSION/scummvm-$VERSION.tar.xz" \
    cd660b34104da7f85ed0a11b7c6c2320c629bb87ee297935711ebc1e09731f12 \
    "$CACHE")
ZLIB_SOURCE=$(fetch "https://zlib.net/fossils/zlib-$ZLIB.tar.gz" \
    9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 \
    "$CACHE")
PNG_SOURCE=$(fetch "https://download.sourceforge.net/libpng/libpng-$PNG.tar.xz" \
    b213cb381fbb1175327bd708a77aab708a05adde7b471bc267bd15ac99893631 \
    "$CACHE")
MAD_SOURCE=$(fetch "https://downloads.sourceforge.net/mad/libmad-$MAD.tar.gz" \
    bbfac3ed6bfbc2823d3775ebb931087371e142bb0e9bb1bee51a76a6e0078690 \
    "$CACHE")
OGG_SOURCE=$(fetch "https://downloads.xiph.org/releases/ogg/libogg-$OGG.tar.xz" \
    c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705 \
    "$CACHE")
VORBIS_SOURCE=$(fetch "https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS.tar.xz" \
    b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b \
    "$CACHE")
FLAC_SOURCE=$(fetch "https://downloads.xiph.org/releases/flac/flac-$FLAC.tar.xz" \
    6c58e69cd22348f441b861092b825e591d0b822e106de6eb0ee4d05d27205b70 \
    "$CACHE")

SDL=$OUT/sdl2
if [ ! -f "$SDL/lib/libSDL2.a" ]; then
    SDL2_LIBRARY_ONLY=1 sh "$HERE/../sdl2/build.sh" "$OUT" "$CACHE"
fi

DIR=$(work scummvm "$OUT")
# Source paths in the program read /build/..., not the builder's folders.
CROSS=$(map_build_paths "$OUT" "$DIR/cross")
DEPS=$DIR/deps
JOBS=$(nproc 2>/dev/null || echo 4)
# Only the libraries built here: never the build machine's.
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig"
export PKG_CONFIG_PATH=

tar -xzf "$ZLIB_SOURCE" -C "$DIR"
(cd "$DIR/zlib-$ZLIB" && CC="${CROSS}gcc" CFLAGS=-O2 \
    ./configure --static --prefix="$DEPS" && make -j"$JOBS" && make install) \
    > "$DIR/zlib.log" 2>&1 || { tail -20 "$DIR/zlib.log" >&2; exit 1; }
tar -xJf "$PNG_SOURCE" -C "$DIR"
(cd "$DIR/libpng-$PNG" && CC="${CROSS}gcc" CPPFLAGS="-I$DEPS/include" \
    LDFLAGS="-L$DEPS/lib" ./configure --host=x86_64-linux-musl \
    --prefix="$DEPS" --disable-shared --enable-static --disable-tools \
    && make -j"$JOBS" && make install) \
    > "$DIR/png.log" 2>&1 || { tail -20 "$DIR/png.log" >&2; exit 1; }
# autotools NAME FOLDER OPTIONS...: a static library into DEPS. HOST may
# name another triple for configure scripts too old to know musl (libmad);
# the compiler is the musl one either way. MAKE_CFLAGS, when set, replaces
# the CFLAGS configure chose (libmad's include -fforce-mem, which GCC
# dropped long ago).
autotools() {
    name=$1
    folder=$2
    shift 2
    (cd "$DIR/$folder" && CC="${CROSS}gcc" CPPFLAGS="-I$DEPS/include" \
        LDFLAGS="-L$DEPS/lib" ./configure --host="${HOST:-x86_64-linux-musl}" \
        --prefix="$DEPS" --disable-shared --enable-static "$@" \
        && make -j"$JOBS" ${MAKE_CFLAGS:+CFLAGS="$MAKE_CFLAGS"} \
        && make install) \
        > "$DIR/$name.log" 2>&1 || { tail -20 "$DIR/$name.log" >&2; exit 1; }
}
tar -xzf "$MAD_SOURCE" -C "$DIR"
tar -xJf "$OGG_SOURCE" -C "$DIR"
tar -xJf "$VORBIS_SOURCE" -C "$DIR"
tar -xJf "$FLAC_SOURCE" -C "$DIR"
HOST=x86_64-pc-linux-gnu
MAKE_CFLAGS="-O2 -fomit-frame-pointer"
autotools mad "libmad-$MAD" --enable-fpm=64bit
HOST=
MAKE_CFLAGS=
autotools ogg "libogg-$OGG"
autotools vorbis "libvorbis-$VORBIS" --disable-examples --disable-docs
autotools flac "flac-$FLAC" --disable-programs --disable-examples \
    --disable-cpplibs --disable-doxygen-docs --disable-xmms-plugin

tar -xJf "$SOURCE" -C "$DIR"
SRC=$DIR/scummvm-$VERSION
# configure finds x86_64-linux-musl-g++ and sdl2-config on PATH.
PATH="$(dirname "$CROSS"):$SDL/bin:$PATH"
# --enable-static only picks static libraries; LDFLAGS=-static makes the
# program itself static (no libstdc++.so or libc.so needed on OPOS).
(cd "$SRC" && LDFLAGS=-static ./configure --host=x86_64-linux-musl --prefix=/ \
    --enable-release --enable-static \
    --with-sdl-prefix="$SDL" \
    --with-zlib-prefix="$DEPS" --with-png-prefix="$DEPS" \
    --with-mad-prefix="$DEPS" --with-ogg-prefix="$DEPS" \
    --with-vorbis-prefix="$DEPS" --with-flac-prefix="$DEPS" \
    --disable-alsa --disable-seq-midi --disable-sndio --disable-timidity \
    --disable-fluidsynth --disable-fluidlite --disable-cloud --disable-libcurl \
    --disable-sdlnet --disable-tts --disable-discord --disable-taskbar \
    --disable-system-dialogs --disable-opengl-game --disable-tremor \
    --disable-mpeg2 --disable-a52 --disable-jpeg --disable-gif \
    --disable-freetype2 --disable-fribidi --disable-theoradec \
    --disable-faad --disable-vpx --disable-mikmod --disable-openmpt \
    --disable-readline --disable-libunity --disable-gtk \
    && make -j"$JOBS" \
    && make install DESTDIR="$DIR/install") \
    > "$DIR/build.log" 2>&1 || {
    grep -E "error|undefined|ERROR" "$DIR/build.log" | head -30 >&2
    tail -20 "$DIR/build.log" >&2
    exit 1
}
if "${CROSS}readelf" -d "$DIR/install/bin/scummvm" | grep -q NEEDED; then
    echo "store/linux/scummvm: scummvm is not linked statically" >&2
    exit 1
fi
for feature in "MAD" "Ogg" "Vorbis" "FLAC >= 1.1.3"; do
    if ! grep -qx "Checking for $feature\.\.\. yes" "$DIR/build.log"; then
        echo "store/linux/scummvm: built without $feature" >&2
        exit 1
    fi
done

STAGE=$DIR/stage
mkdir -p "$STAGE"
"${CROSS}strip" -o "$STAGE/scummvm" "$DIR/install/bin/scummvm"
$LINUX_CC -O2 -Wall -o "$DIR/scummvm-start" "$HERE/start.c" -static
strip -o "$STAGE/scummvm-start" "$DIR/scummvm-start"
# Themes, translations and engine data files.
cp -r "$DIR/install/share/scummvm" "$STAGE/data"
cp "$SRC/COPYING" "$STAGE/LICENSE"
SDL_VERSION=$(sed -n 's/^VERSION=//p' "$HERE/../sdl2/build.sh")
cat >> "$STAGE/LICENSE" <<NOTE

The scummvm program in this package is ScummVM $VERSION (GPL-3.0 or
later, above), unchanged, built for OPOS with libmad $MAD (GPL-2.0,
LICENSE.libmad), libogg $OGG, libvorbis $VORBIS and FLAC $FLAC
(BSD-3-Clause, LICENSE.ogg, LICENSE.vorbis, LICENSE.flac), libpng $PNG
(LICENSE.libpng), zlib $ZLIB (LICENSE.zlib), SDL $SDL_VERSION (zlib
license, see LICENSE.sdl) and musl libc (LICENSE.musl). ScummVM's other
licenses and credits are in doc/. Where to get the sources: see
SOURCE.txt. The OPOS build recipe is store/linux/scummvm in the OPOS
repository. scummvm-start (OPOS, see start.c there) runs scummvm with the
data/ folder of this package.
NOTE
SOURCE_SUM=$(sha256sum "$SOURCE" | cut -d' ' -f1)
cat > "$STAGE/SOURCE.txt" <<NOTE
The source of ScummVM $VERSION, from which the scummvm program in this
package was built unchanged, is too big to carry in every install
(about 210 MB). It is published next to this package, as the file

    scummvm-$VERSION.tar.xz
    SHA-256 $SOURCE_SUM

in the OPOS Store release this package was downloaded from:

    https://github.com/PorkifyNET/opos-packages/releases/tag/Store

It is the same file as ScummVM's own source release at
https://downloads.scummvm.org/frs/scummvm/$VERSION/. The source of
libmad $MAD is source/libmad-$MAD.tar.gz in this package.
NOTE
mkdir -p "$STAGE/doc"
cp "$DIR/install/share/doc/scummvm/"* "$STAGE/doc/" 2>/dev/null || true
cp "$SRC/LICENSES/"* "$STAGE/doc/" 2>/dev/null || true
tar -xzOf "$CACHE/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION/LICENSE.txt" \
    > "$STAGE/LICENSE.sdl"
tar -xzOf "$ZLIB_SOURCE" "zlib-$ZLIB/LICENSE" > "$STAGE/LICENSE.zlib"
tar -xJOf "$PNG_SOURCE" "libpng-$PNG/LICENSE" > "$STAGE/LICENSE.libpng"
cp "$DIR/libmad-$MAD/COPYING" "$STAGE/LICENSE.libmad"
cp "$DIR/libogg-$OGG/COPYING" "$STAGE/LICENSE.ogg"
cp "$DIR/libvorbis-$VORBIS/COPYING" "$STAGE/LICENSE.vorbis"
cp "$DIR/flac-$FLAC/COPYING.Xiph" "$STAGE/LICENSE.flac"
mkdir -p "$STAGE/source"
cp "$MAD_SOURCE" "$STAGE/source/"
# The ScummVM source goes into the Store release beside the packages
# (make store copies OUT/source/ into build/store/).
mkdir -p "$OUT/source"
cp "$SOURCE" "$OUT/source/"

{
    cat <<PKG
format: 1
id: org.scummvm.scummvm
name: ScummVM
version: $PACKAGE_VERSION
publisher: The ScummVM Team (built for OPOS)
platform: linux
requires-opos: 0.6.7
PKG
    (cd "$STAGE" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) |
    while read -r f; do
        case $f in
        scummvm|scummvm-start) echo "file: files/$f $f exec" ;;
        *) echo "file: files/$f $f" ;;
        esac
    done
    echo "file: files/LICENSE.musl LICENSE.musl"
    echo "start: files/scummvm-start ScummVM"
} > "$DIR/package.pkg"
mv "$DIR/package.pkg" "$STAGE/package.pkg"
package "$STAGE" "$OUT"
