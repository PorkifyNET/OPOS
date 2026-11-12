#!/bin/sh
# SDL2 for OPOS: the SDL 2 library with a video driver for OPOS windows
# (opos/SDL_oposvideo.c) and an audio driver for OPOS sound
# (opos/SDL_oposaudio.c), and a package of SDL's own demo programs to
# show them working. Programs are linked statically with musl.
# License: zlib (SDL and the OPOS driver). Not built by default: add
# "sdl2" to STORE_LINUX. With SDL2_LIBRARY_ONLY=1 it only builds and
# installs the library (no package).
#
# Besides the package, the library is installed into OUT/sdl2 (include/,
# lib/libSDL2.a, lib/libSDL2main.a, lib/libSDL2_test.a) so recipes for SDL
# programs can build against it:
#   $LINUX_CC -I OUT/sdl2/include/SDL2 prog.c -L OUT/sdl2/lib -lSDL2 -static
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=2.32.10
SOURCE=$(fetch "https://github.com/libsdl-org/SDL/releases/download/release-$VERSION/SDL2-$VERSION.tar.gz" \
    5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 \
    "$CACHE")
DIR=$(work sdl2 "$OUT")
tar -xzf "$SOURCE" -C "$DIR"
SRC=$DIR/SDL2-$VERSION

# The OPOS driver goes next to the dummy driver, whose sources configure
# already compiles (--enable-video-dummy), and SDL tries it first.
cp "$HERE/opos/SDL_oposvideo.c" "$SRC/src/video/dummy/"
sed -i 's|^#ifdef SDL_VIDEO_DRIVER_DUMMY$|#ifdef SDL_VIDEO_DRIVER_OPOS\n    \&OPOS_bootstrap,\n#endif\n&|' \
    "$SRC/src/video/SDL_video.c"
sed -i 's|^extern VideoBootStrap DUMMY_bootstrap;$|extern VideoBootStrap OPOS_bootstrap;\n&|' \
    "$SRC/src/video/SDL_sysvideo.h"
grep -q OPOS_bootstrap "$SRC/src/video/SDL_video.c" \
    && grep -q OPOS_bootstrap "$SRC/src/video/SDL_sysvideo.h" || {
    echo "store/linux/sdl2: could not add the OPOS driver" >&2
    exit 1
}
# The audio driver likewise goes next to the dummy audio driver.
cp "$HERE/opos/SDL_oposaudio.c" "$SRC/src/audio/dummy/"
sed -i 's|^#ifdef SDL_AUDIO_DRIVER_DUMMY$|#ifdef SDL_AUDIO_DRIVER_OPOS\n    \&OPOSAUDIO_bootstrap,\n#endif\n&|' \
    "$SRC/src/audio/SDL_audio.c"
sed -i 's|^extern AudioBootStrap DUMMYAUDIO_bootstrap;$|extern AudioBootStrap OPOSAUDIO_bootstrap;\n&|' \
    "$SRC/src/audio/SDL_sysaudio.h"
grep -q OPOSAUDIO_bootstrap "$SRC/src/audio/SDL_audio.c" \
    && grep -q OPOSAUDIO_bootstrap "$SRC/src/audio/SDL_sysaudio.h" || {
    echo "store/linux/sdl2: could not add the OPOS audio driver" >&2
    exit 1
}

PREFIX=$OUT/sdl2
rm -rf "$PREFIX"
# Everything that needs a Linux device, server or library OPOS lacks is
# off: X11, Wayland, KMS, OpenGL, Vulkan, sound servers, udev, D-Bus.
# Audio goes through the OPOS driver (without sound hardware SDL finds
# no audio driver, as before); joysticks and haptics use SDL's dummy
# drivers.
(cd "$SRC" && CC="$LINUX_CC" \
    CFLAGS="-O2 -DSDL_VIDEO_DRIVER_OPOS=1 -DSDL_AUDIO_DRIVER_OPOS=1" \
    LDFLAGS=-static ./configure --prefix="$PREFIX" \
    --disable-shared --enable-static \
    --enable-video-dummy --disable-video-offscreen \
    --disable-video-x11 --disable-video-wayland --disable-video-kmsdrm \
    --disable-video-vivante --disable-video-rpi --disable-video-directfb \
    --disable-video-opengl --disable-video-opengles \
    --disable-video-opengles1 --disable-video-opengles2 \
    --disable-video-vulkan \
    --disable-oss --disable-alsa --disable-jack --disable-esd \
    --disable-pipewire --disable-pulseaudio --disable-arts --disable-nas \
    --disable-sndio --disable-fusionsound --disable-libsamplerate \
    --disable-diskaudio --enable-dummyaudio \
    --disable-hidapi --disable-libudev --disable-dbus --disable-ime \
    --disable-ibus --disable-fcitx --disable-rpath) \
    > "$DIR/configure.log" 2>&1 || {
    tail -30 "$DIR/configure.log" >&2
    exit 1
}
make -C "$SRC" -j"$(nproc)" > "$DIR/build.log" 2>&1 \
    && make -C "$SRC" install >> "$DIR/build.log" 2>&1 || {
    tail -30 "$DIR/build.log" >&2
    exit 1
}

# SDL2_LIBRARY_ONLY=1 stops here: recipes of SDL programs use it when
# OUT/sdl2 is not built yet.
if [ "${SDL2_LIBRARY_ONLY:-}" = 1 ]; then
    exit 0
fi

# SDL's demos (zlib, from test/). Resources are found next to the program
# through SDL_GetBasePath.
STAGE=$DIR/stage
mkdir -p "$STAGE"
LIBS="-L$PREFIX/lib -lSDL2_test -lSDL2 -lm -static"
CFLAGS="-O2 -I$PREFIX/include/SDL2"
for demo in testsprite2 testdraw2 testrendercopyex checkkeys \
        testmultiaudio; do
    extra=
    case $demo in
        testsprite2|testrendercopyex|testmultiaudio)
            extra="$SRC/test/testutils.c" ;;
    esac
    $LINUX_CC $CFLAGS -o "$STAGE/$demo" "$SRC/test/$demo.c" $extra $LIBS \
        >> "$DIR/build.log" 2>&1 || {
        tail -30 "$DIR/build.log" >&2
        exit 1
    }
    strip "$STAGE/$demo"
done
cp "$SRC/test/icon.bmp" "$SRC/test/sample.bmp" "$SRC/test/sample.wav" \
    "$STAGE/"
cp "$SRC/LICENSE.txt" "$STAGE/LICENSE"
printf '\nThe programs in this package are SDL %s test programs, linked with\nSDL and an SDL video driver for OPOS (zlib license, above), and with\nmusl libc, whose license is in LICENSE.musl.\n' \
    "$VERSION" >> "$STAGE/LICENSE"
cat > "$STAGE/package.pkg" <<PKG
format: 1
id: org.libsdl.sdl2-demos
name: SDL2 Demos
version: $VERSION
publisher: SDL project (built for OPOS)
platform: linux
requires-opos: 0.1.5
file: files/testsprite2 testsprite2 exec
file: files/testdraw2 testdraw2 exec
file: files/testrendercopyex testrendercopyex exec
file: files/checkkeys checkkeys exec
file: files/testmultiaudio testmultiaudio exec
file: files/icon.bmp icon.bmp
file: files/sample.bmp sample.bmp
file: files/sample.wav sample.wav
file: files/LICENSE LICENSE
file: files/LICENSE.musl LICENSE.musl
start: files/testsprite2 SDL Sprites
start: files/testdraw2 SDL Drawing
start: files/testrendercopyex SDL Rotation
start: files/checkkeys SDL Keys
start: files/testmultiaudio SDL Sound
PKG
package "$STAGE" "$OUT"
