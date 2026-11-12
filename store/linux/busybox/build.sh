#!/bin/sh
# BusyBox for the OPOS Store: one static program with the shell (ash),
# vi, less and the usual Unix tools, built with musl.
# License: GPL-2.0. The package carries the exact source release and the
# build configuration, so the program comes with its corresponding
# source. Not built by default: add "busybox" to STORE_LINUX.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
VERSION=1.36.1
SOURCE=$(fetch "https://busybox.net/downloads/busybox-$VERSION.tar.bz2" \
    b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314 \
    "$CACHE")
DIR=$(work busybox "$OUT")
tar -xjf "$SOURCE" -C "$DIR"
SRC=$DIR/busybox-$VERSION

# Linux UAPI headers (linux/, asm/, ...) next to musl's own; musl has none.
HEADERS=$DIR/kernel-headers
mkdir -p "$HEADERS"
for d in linux asm-generic mtd scsi sound; do
    if [ -d "/usr/include/$d" ]; then
        ln -s "/usr/include/$d" "$HEADERS/$d"
    fi
done
for d in /usr/include/x86_64-linux-gnu/asm /usr/include/asm; do
    if [ -d "$d" ]; then
        ln -s "$d" "$HEADERS/asm"
        break
    fi
done
test -d "$HEADERS/linux" || {
    echo "store/linux/busybox: Linux headers not found (install linux-libc-dev)" >&2
    exit 1
}

CC="$LINUX_CC -isystem $HEADERS"
make -C "$SRC" CC="$CC" HOSTCC=gcc defconfig >/dev/null
# set_config NAME VALUE ("n" turns it off).
set_config() {
    sed -i "/^CONFIG_$1=/d; /^# CONFIG_$1 is not set/d" "$SRC/.config"
    if [ "$2" = n ]; then
        echo "# CONFIG_$1 is not set" >> "$SRC/.config"
    else
        echo "CONFIG_$1=$2" >> "$SRC/.config"
    fi
}
set_config STATIC y
# Parts that need kernel or libc features musl or new headers lack.
set_config TC n
set_config FEATURE_TC_INGRESS n
set_config SELINUX n
set_config FEATURE_HAVE_RPC n
set_config FEATURE_MOUNT_NFS n
set_config FEATURE_INETD_RPC n
set_config PAM n
set_config FEATURE_SYSTEMD n
yes "" | make -C "$SRC" CC="$CC" HOSTCC=gcc oldconfig >/dev/null 2>&1
make -C "$SRC" CC="$CC" HOSTCC=gcc -j"$(nproc)" busybox \
    > "$DIR/build.log" 2>&1 || {
    tail -30 "$DIR/build.log" >&2
    exit 1
}

STAGE=$DIR/stage
mkdir -p "$STAGE"
strip -o "$STAGE/busybox" "$SRC/busybox_unstripped"
cp "$SRC/LICENSE" "$STAGE/LICENSE"
printf '\nThe busybox program in this package is linked with musl libc, whose\nlicense is in LICENSE.musl. Its source is busybox-%s.tar.bz2 in this\npackage, built with the configuration in busybox.config.\n' \
    "$VERSION" >> "$STAGE/LICENSE"
cp "$SOURCE" "$STAGE/busybox-$VERSION.tar.bz2"
cp "$SRC/.config" "$STAGE/busybox.config"
cat > "$STAGE/package.pkg" <<PKG
format: 1
id: net.busybox.busybox
name: BusyBox
version: $VERSION
publisher: BusyBox project (built for OPOS)
platform: linux
requires-opos: 0.1.5
file: files/busybox busybox exec
file: files/LICENSE LICENSE
file: files/LICENSE.musl LICENSE.musl
file: files/source/busybox-$VERSION.tar.bz2 busybox-$VERSION.tar.bz2
file: files/source/busybox.config busybox.config
start: files/busybox BusyBox
PKG
package "$STAGE" "$OUT"
