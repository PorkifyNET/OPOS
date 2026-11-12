#!/bin/sh
# Lua for the OPOS Store: the lua interpreter, built static with musl.
# License: MIT (taken from the source's lua.h), plus musl's.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
VERSION=5.4.7
SOURCE=$(fetch "https://www.lua.org/ftp/lua-$VERSION.tar.gz" \
    9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30 \
    "$CACHE")
DIR=$(work lua "$OUT")
tar -xzf "$SOURCE" -C "$DIR"
make -C "$DIR/lua-$VERSION/src" CC="$LINUX_CC -std=gnu99" \
    CFLAGS="-Os -Wall -DLUA_COMPAT_5_3 -DLUA_USE_LINUX" \
    MYLDFLAGS=-static MYLIBS= lua >/dev/null

STAGE=$DIR/stage
mkdir -p "$STAGE"
strip -o "$STAGE/lua" "$DIR/lua-$VERSION/src/lua"
sed -n '/^\* Copyright (C) 1994-/,/^\*\*\*\*/p' \
    "$DIR/lua-$VERSION/src/lua.h" | sed -e '/^\*\{4,\}/d' \
    -e 's/^\* \{0,1\}//' > "$STAGE/LICENSE"
printf '\nThe lua program in this package is linked with musl libc, whose\nlicense is in LICENSE.musl.\n' >> "$STAGE/LICENSE"
test -s "$STAGE/LICENSE"
cat > "$STAGE/package.pkg" <<EOF
format: 1
id: org.lua.lua
name: Lua
version: $VERSION
publisher: Lua.org, PUC-Rio (built for OPOS)
platform: linux
requires-opos: 0.1.5
file: files/lua lua exec
file: files/LICENSE LICENSE
file: files/LICENSE.musl LICENSE.musl
start: files/lua Lua
EOF
package "$STAGE" "$OUT"
