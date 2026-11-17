#!/bin/sh
# DOSBox Staging for OPOS: the DOS emulator for old PC games and programs.
# No DOS games are included; people mount their own folders in DOSBox.
# Licenses: DOSBox Staging GPL-2.0 or later (the package carries its source
# release, and its licenses/ folder for the code it bundles), FluidSynth
# LGPL-2.1, libmt32emu LGPL-2.1, alsa-lib LGPL-2.1, iir1 MIT, asio BSL-1.0,
# SDL and SDL_image zlib license, zlib and zlib-ng zlib license, libpng libpng
# license, SpeexDSP, libogg, Opus and opusfile BSD-3-Clause, musl MIT.
# Built against SDL 2 from store/linux/sdl2 (built first when OUT/sdl2 is
# missing); picture and sound go through SDL's OPOS drivers.
#
# OPOS has no OpenGL. DOSBox Staging loads OpenGL at run time (glad, through
# SDL) and, when SDL cannot make an OpenGL context, falls back to drawing
# with SDL's renderer ("texture" output). Its CMake still asks for an
# OpenGL library to link, so an empty stand-in libGL.a is passed; no OpenGL
# function is linked from it. (0.83.1 does not compile with
# OPT_OPENGL=OFF: a shared header includes glad/gl.h.)
# alsa-lib is only there because the Linux build asks for ALSA MIDI; OPOS
# has no ALSA devices, so DOSBox finds no ALSA MIDI ports. DOSBox Staging's
# source is not changed.
#
# DOSBox Staging is C++20, so this recipe needs a musl cross toolchain with
# g++ (musl-cross-make), not LINUX_CC: set MUSL_CROSS to its prefix, for
# example $HOME/musl-cross/bin/x86_64-linux-musl- (the default). It also
# needs cmake and pkg-config. Not built by default: add "dosbox-staging"
# to STORE_LINUX.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=0.83.1
# The package version: DOSBox Staging's, plus a fourth number for OPOS
# rebuilds.
PACKAGE_VERSION=0.83.1.1
ZLIB=1.3.1
ZLIB_NG=2.2.4
PNG=1.6.47
SDL_IMAGE=2.8.12
IIR=1.9.5
MUNT=2_8_4
FLUIDSYNTH=2.6.1
SPEEXDSP=1.2.1
OGG=1.3.5
OPUS=1.5.2
OPUSFILE=0.12
ALSA=1.2.13
ASIO=1-30-2
CROSS=${MUSL_CROSS:-$HOME/musl-cross/bin/x86_64-linux-musl-}
if [ ! -x "${CROSS}g++" ]; then
    echo "store/linux/dosbox-staging: ${CROSS}g++ not found; set MUSL_CROSS" \
         "to a musl cross toolchain prefix (musl-cross-make)" >&2
    exit 1
fi
SOURCE=$(fetch "https://github.com/dosbox-staging/dosbox-staging/archive/refs/tags/v$VERSION.tar.gz" \
    14c3703369e56125a425aea8ebb1a425823df58f9769f3e48b1b8ebbf2c6cf09 \
    "$CACHE")
ZLIB_SOURCE=$(fetch "https://zlib.net/fossils/zlib-$ZLIB.tar.gz" \
    9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 \
    "$CACHE")
ZLIB_NG_SOURCE=$(fetch "https://github.com/zlib-ng/zlib-ng/archive/refs/tags/$ZLIB_NG.tar.gz" \
    a73343c3093e5cdc50d9377997c3815b878fd110bf6511c2c7759f2afb90f5a3 \
    "$CACHE")
PNG_SOURCE=$(fetch "https://download.sourceforge.net/libpng/libpng-$PNG.tar.xz" \
    b213cb381fbb1175327bd708a77aab708a05adde7b471bc267bd15ac99893631 \
    "$CACHE")
SDL_IMAGE_SOURCE=$(fetch "https://github.com/libsdl-org/SDL_image/releases/download/release-$SDL_IMAGE/SDL2_image-$SDL_IMAGE.tar.gz" \
    393f5efb50536ec13ca4f4affb69cc9966d3c3f969e6c5e701faddf9f9785381 \
    "$CACHE")
IIR_SOURCE=$(fetch "https://github.com/berndporr/iir1/archive/refs/tags/$IIR.tar.gz" \
    beb16142e08e5f68010c6e5014dea2276ea49b71a258439eff09c5ee3f781d88 \
    "$CACHE")
MUNT_SOURCE=$(fetch "https://github.com/munt/munt/archive/refs/tags/munt_$MUNT.tar.gz" \
    75f29ed4b56c830e71005ce2d3ffc0b6b19a89ca8e3119054fa0d315b8bcc384 \
    "$CACHE")
FLUID_SOURCE=$(fetch "https://github.com/FluidSynth/fluidsynth/archive/refs/tags/v$FLUIDSYNTH.tar.gz" \
    3d258a3bf97cc20c59eeebfe62c2432fae88adda74d3ad098681c76e0ebf446b \
    "$CACHE")
SPEEXDSP_SOURCE=$(fetch "https://downloads.xiph.org/releases/speex/speexdsp-$SPEEXDSP.tar.gz" \
    8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d \
    "$CACHE")
OGG_SOURCE=$(fetch "https://downloads.xiph.org/releases/ogg/libogg-$OGG.tar.xz" \
    c4d91be36fc8e54deae7575241e03f4211eb102afb3fc0775fbbc1b740016705 \
    "$CACHE")
OPUS_SOURCE=$(fetch "https://downloads.xiph.org/releases/opus/opus-$OPUS.tar.gz" \
    65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1 \
    "$CACHE")
OPUSFILE_SOURCE=$(fetch "https://downloads.xiph.org/releases/opus/opusfile-$OPUSFILE.tar.gz" \
    118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b \
    "$CACHE")
ALSA_SOURCE=$(fetch "https://www.alsa-project.org/files/pub/lib/alsa-lib-$ALSA.tar.bz2" \
    8c4ff37553cbe89618e187e4c779f71a9bb2a8b27b91f87ed40987cc9233d8f6 \
    "$CACHE")
ASIO_SOURCE=$(fetch "https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-$ASIO.tar.gz" \
    755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8 \
    "$CACHE")

SDL=$OUT/sdl2
if [ ! -f "$SDL/lib/libSDL2.a" ]; then
    SDL2_LIBRARY_ONLY=1 sh "$HERE/../sdl2/build.sh" "$OUT" "$CACHE"
fi

DIR=$(work dosbox-staging "$OUT")
# Source paths in the program read /build/..., not the builder's folders.
REAL_CROSS=$CROSS
CROSS=$(map_build_paths "$OUT" "$DIR/cross")
DEPS=$DIR/deps
JOBS=$(nproc 2>/dev/null || echo 4)
HOST=x86_64-linux-musl
mkdir -p "$DEPS/lib/pkgconfig" "$DIR/bin"
# Only the libraries built here, never the build machine's, and with the
# libraries each static library needs itself (--static).
export PKG_CONFIG_LIBDIR="$DEPS/lib/pkgconfig:$SDL/lib/pkgconfig"
export PKG_CONFIG_PATH=
cat > "$DIR/bin/pkg-config" <<'WRAP'
#!/bin/sh
exec pkg-config --static "$@"
WRAP
chmod +x "$DIR/bin/pkg-config"

# step NAME COMMANDS: runs COMMANDS in a subshell, logging to NAME.log.
step() {
    name=$1
    shift
    (set -e; eval "$*") > "$DIR/$name.log" 2>&1 || {
        tail -30 "$DIR/$name.log" >&2
        echo "store/linux/dosbox-staging: $name failed" >&2
        exit 1
    }
}
# autotools NAME: configure, build and install the unpacked NAME folder.
autotools() {
    folder=$1
    shift
    (cd "$DIR/$folder" && CC="${CROSS}gcc" CXX="${CROSS}g++" \
        CPPFLAGS="-I$DEPS/include" LDFLAGS="-L$DEPS/lib" \
        ./configure --host=$HOST --prefix="$DEPS" --disable-shared \
        --enable-static "$@" && make -j"$JOBS" && make install)
}

SYSROOT=$(dirname "$(dirname "$REAL_CROSS")")/$HOST
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
set(PKG_CONFIG_EXECUTABLE $DIR/bin/pkg-config)
TOOLCHAIN
CMAKE_COMMON="-DCMAKE_TOOLCHAIN_FILE=$DIR/toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX=$DEPS -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH='$DEPS;$SDL' -DCMAKE_POSITION_INDEPENDENT_CODE=OFF"
# cmake_lib NAME SOURCE_FOLDER OPTIONS...: build and install a library.
cmake_lib() {
    name=$1
    folder=$2
    shift 2
    step "$name" cmake -S "$DIR/$folder" -B "$DIR/build-$name" \
        "$CMAKE_COMMON" "$*" \
        "&& cmake --build $DIR/build-$name -j$JOBS" \
        "&& cmake --install $DIR/build-$name"
}

for archive in "$ZLIB_SOURCE" "$ZLIB_NG_SOURCE" "$SDL_IMAGE_SOURCE" \
    "$IIR_SOURCE" "$MUNT_SOURCE" "$FLUID_SOURCE" "$SPEEXDSP_SOURCE" \
    "$OPUS_SOURCE" "$OPUSFILE_SOURCE"; do
    tar -xzf "$archive" -C "$DIR"
done
tar -xJf "$PNG_SOURCE" -C "$DIR"
tar -xJf "$OGG_SOURCE" -C "$DIR"
tar -xjf "$ALSA_SOURCE" -C "$DIR"

step zlib "cd $DIR/zlib-$ZLIB && CC=${CROSS}gcc CFLAGS=-O2 ./configure --static --prefix=$DEPS && make -j$JOBS && make install"
cmake_lib zlib-ng "zlib-ng-$ZLIB_NG" -DZLIB_COMPAT=OFF -DZLIB_ENABLE_TESTS=OFF \
    -DZLIBNG_ENABLE_TESTS=OFF -DWITH_GTEST=OFF
step libpng autotools "libpng-$PNG" --disable-tools
cmake_lib sdl2_image "SDL2_image-$SDL_IMAGE" -DSDL2_DIR=$SDL/lib/cmake/SDL2 \
    -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_TESTS=OFF -DSDL2IMAGE_VENDORED=OFF \
    -DSDL2IMAGE_DEPS_SHARED=OFF -DSDL2IMAGE_BACKEND_STB=ON \
    -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JPG=OFF -DSDL2IMAGE_JXL=OFF \
    -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF
# DOSBox Staging includes SDL.h through SDL_image's include folder, as on a
# Linux system where both share include/SDL2.
cp "$SDL/include/SDL2/"*.h "$DEPS/include/SDL2/"
cmake_lib iir "iir1-$IIR" -DIIR1_BUILD_TESTING=OFF -DIIR1_BUILD_DEMO=OFF \
    -DIIR1_INSTALL_STATIC=ON
# iir1 always builds a shared library too, and its CMake package offers
# that first. A static-only package file replaces it.
rm -rf "$DEPS/lib/cmake/iir" "$DEPS"/lib/libiir.so*
mkdir -p "$DEPS/lib/cmake/iir"
cat > "$DEPS/lib/cmake/iir/iirConfig.cmake" <<IIRCONFIG
add_library(iir::iir_static STATIC IMPORTED)
set_target_properties(iir::iir_static PROPERTIES
    IMPORTED_LOCATION "$DEPS/lib/libiir_static.a"
    INTERFACE_INCLUDE_DIRECTORIES "$DEPS/include")
IIRCONFIG
cmake_lib mt32emu "munt-munt_$MUNT/mt32emu" -Dlibmt32emu_SHARED=OFF \
    -Dmt32emu_BUILD_TESTING=OFF
FLUID_OFF=
for option in alsa aufile dbus dsound ipv6 jack ladspa libsndfile midishare \
    network opensles oboe openmp oss pipewire portaudio pulseaudio readline \
    sdl2 sdl3 systemd wasapi waveout winmidi coreaudio coremidi framework \
    dart kai native-dls signalsmith libinstpatch; do
    FLUID_OFF="$FLUID_OFF -Denable-$option=off"
done
# The default SoundFont path is kept in the library; name the usual Linux
# one, not the build folder.
cmake_lib fluidsynth "fluidsynth-$FLUIDSYNTH" $FLUID_OFF \
    -DDEFAULT_SOUNDFONT=/usr/share/soundfonts/default.sf2
step speexdsp autotools "speexdsp-$SPEEXDSP" --disable-examples
step ogg autotools "libogg-$OGG"
step opus autotools "opus-$OPUS" --disable-doc --disable-extra-programs
step opusfile autotools "opusfile-$OPUSFILE" --disable-http --disable-examples \
    --disable-doc
# alsa-lib keeps its configuration folder's path in the library; it names
# the usual Linux folders (OPOS has neither), and the install goes to a
# staging root first because configure's folders are outside DEPS.
step alsa "cd $DIR/alsa-lib-$ALSA && CC=${CROSS}gcc ./configure --host=$HOST \
    --prefix=$DEPS --disable-shared --enable-static \
    --with-configdir=/usr/share/alsa --with-plugindir=/usr/lib/alsa-lib \
    --disable-python --disable-ucm --disable-topology --disable-alisp \
    --disable-old-symbols --without-debug \
    && make -j$JOBS && make install DESTDIR=$DIR/alsa-root \
    && cp -r $DIR/alsa-root$DEPS/. $DEPS/"
# asio (serial port and modem emulation) is headers only.
tar -xzf "$ASIO_SOURCE" -C "$DIR"
cp -r "$DIR/asio-asio-$ASIO/asio/include/"* "$DEPS/include/"
# The OpenGL stand-in (see the top of this file).
${CROSS}ar rc "$DEPS/lib/libGL.a"

tar -xzf "$SOURCE" -C "$DIR"
SRC=$DIR/dosbox-staging-$VERSION
# DEPS/include stands in for /usr/include: some DOSBox targets include
# library headers (Iir.h) without linking the library themselves.
# LIBATOMIC: its CMake looks for libatomic.so.1, which a static link
# cannot use; the static libatomic.a is the same library. -L: its decoder
# library links opusfile, opus and ogg by name only. The install prefix:
# DOSBox keeps its data folder as a resource fallback, and the program is
# not installed, so it names /usr/local rather than the build folder.
step dosbox cmake -S "$SRC" -B "$DIR/build" "$CMAKE_COMMON" \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DSDL2_DIR=$SDL/lib/cmake/SDL2 -DUSE_SYSTEM_LIBS=ON \
    -DOPT_TESTS=OFF -DOPT_MANYMOUSE=OFF \
    -DOpenGL_GL_PREFERENCE=LEGACY -DOPENGL_gl_LIBRARY=$DEPS/lib/libGL.a \
    -DOPENGL_INCLUDE_DIR=$SDL/include \
    "'-DCMAKE_C_FLAGS=-isystem $DEPS/include'" \
    "'-DCMAKE_CXX_FLAGS=-isystem $DEPS/include'" \
    -DLIBATOMIC=$SYSROOT/lib/libatomic.a \
    "'-DCMAKE_EXE_LINKER_FLAGS=-static -L$DEPS/lib'" \
    "&& cmake --build $DIR/build -j$JOBS"

STAGE=$DIR/stage
mkdir -p "$STAGE"
"${CROSS}strip" -o "$STAGE/dosbox" "$DIR/build/dosbox"
# DOSBox Staging looks for resources/ next to the program.
cp -r "$DIR/build/resources" "$STAGE/resources"
cp -r "$SRC/licenses" "$STAGE/licenses"
cp "$SRC/LICENSE" "$STAGE/LICENSE"
SDL_VERSION=$(sed -n 's/^VERSION=//p' "$HERE/../sdl2/build.sh")
cat >> "$STAGE/LICENSE" <<NOTE

The dosbox program in this package is DOSBox Staging $VERSION (GPL-2.0 or
later, above; the code it bundles is listed in licenses/), unchanged,
built for OPOS with FluidSynth $FLUIDSYNTH and libmt32emu (munt $MUNT)
(LGPL-2.1, see LICENSE.fluidsynth and LICENSE.mt32emu), alsa-lib $ALSA
(LGPL-2.1, LICENSE.alsa), iir1 $IIR (MIT, LICENSE.iir1), asio $ASIO
(Boost Software License 1.0, LICENSE.asio), SDL $SDL_VERSION
and SDL_image $SDL_IMAGE (zlib license, LICENSE.sdl and LICENSE.sdl_image),
zlib $ZLIB and zlib-ng $ZLIB_NG (zlib license, LICENSE.zlib and
LICENSE.zlib-ng), libpng $PNG (LICENSE.libpng), SpeexDSP $SPEEXDSP,
libogg $OGG, Opus $OPUS and opusfile $OPUSFILE (BSD-3-Clause,
LICENSE.speexdsp, LICENSE.ogg, LICENSE.opus, LICENSE.opusfile) and musl
libc (LICENSE.musl). The sources of DOSBox Staging and of the LGPL
libraries are in source/ in this package; the OPOS build recipe is
store/linux/dosbox-staging in the OPOS repository.
NOTE
tar -xzOf "$CACHE/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION/LICENSE.txt" \
    > "$STAGE/LICENSE.sdl"
cp "$DIR/SDL2_image-$SDL_IMAGE/LICENSE.txt" "$STAGE/LICENSE.sdl_image"
cp "$DIR/fluidsynth-$FLUIDSYNTH/LICENSE" "$STAGE/LICENSE.fluidsynth"
cp "$DIR/munt-munt_$MUNT/mt32emu/COPYING.LESSER.txt" "$STAGE/LICENSE.mt32emu"
cp "$DIR/alsa-lib-$ALSA/COPYING" "$STAGE/LICENSE.alsa"
cp "$DIR/iir1-$IIR/COPYING" "$STAGE/LICENSE.iir1"
cp "$DIR/asio-asio-$ASIO/asio/LICENSE_1_0.txt" "$STAGE/LICENSE.asio"
cp "$DIR/zlib-$ZLIB/LICENSE" "$STAGE/LICENSE.zlib"
cp "$DIR/zlib-ng-$ZLIB_NG/LICENSE.md" "$STAGE/LICENSE.zlib-ng"
cp "$DIR/libpng-$PNG/LICENSE" "$STAGE/LICENSE.libpng"
cp "$DIR/speexdsp-$SPEEXDSP/COPYING" "$STAGE/LICENSE.speexdsp"
cp "$DIR/libogg-$OGG/COPYING" "$STAGE/LICENSE.ogg"
cp "$DIR/opus-$OPUS/COPYING" "$STAGE/LICENSE.opus"
cp "$DIR/opusfile-$OPUSFILE/COPYING" "$STAGE/LICENSE.opusfile"
mkdir -p "$STAGE/source"
cp "$SOURCE" "$STAGE/source/dosbox-staging-$VERSION.tar.gz"
cp "$FLUID_SOURCE" "$STAGE/source/fluidsynth-$FLUIDSYNTH.tar.gz"
cp "$MUNT_SOURCE" "$STAGE/source/munt-$MUNT.tar.gz"
cp "$ALSA_SOURCE" "$STAGE/source/"

{
    cat <<PKG
format: 1
id: org.dosbox-staging.dosbox-staging
name: DOSBox Staging
version: $PACKAGE_VERSION
publisher: The DOSBox Staging Team (built for OPOS)
platform: linux
requires-opos: 0.6.7
PKG
    (cd "$STAGE" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) |
    while read -r f; do
        case $f in
        dosbox) echo "file: files/$f $f exec" ;;
        *) echo "file: files/$f $f" ;;
        esac
    done
    echo "file: files/LICENSE.musl LICENSE.musl"
    echo "start: files/dosbox DOSBox Staging"
} > "$DIR/package.pkg"
mv "$DIR/package.pkg" "$STAGE/package.pkg"
package "$STAGE" "$OUT"
