#!/bin/sh
# SQLite for the OPOS Store: the sqlite3 command-line shell, built static
# with musl. SQLite is in the public domain (LICENSE here); musl is MIT.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
VERSION=3.46.1
SOURCE=$(fetch \
    "https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip" \
    77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784 \
    "$CACHE")
DIR=$(work sqlite "$OUT")
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' \
    "$SOURCE" "$DIR"
SRC=$DIR/sqlite-amalgamation-3460100
# One thread, no loadable extensions: OPOS has no dlopen for them.
$LINUX_CC -Os -static -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 \
    "$SRC/shell.c" "$SRC/sqlite3.c" -o "$DIR/sqlite3"

STAGE=$DIR/stage
mkdir -p "$STAGE"
strip -o "$STAGE/sqlite3" "$DIR/sqlite3"
cp "$(dirname "$0")/LICENSE" "$STAGE/LICENSE"
cat > "$STAGE/package.pkg" <<EOF
format: 1
id: org.sqlite.sqlite3
name: SQLite
version: $VERSION
publisher: SQLite (built for OPOS)
platform: linux
requires-opos: 0.1.5
file: files/sqlite3 sqlite3 exec
file: files/LICENSE LICENSE
file: files/LICENSE.musl LICENSE.musl
start: files/sqlite3 SQLite
EOF
package "$STAGE" "$OUT"
