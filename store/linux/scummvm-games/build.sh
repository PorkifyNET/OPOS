#!/bin/sh
# Freeware games for ScummVM, one Store package each, as the ScummVM team
# gives them away on downloads.scummvm.org:
#  - Beneath a Steel Sky (CD version with speech), (C) Revolution Software
#  - Flight of the Amazon Queen (CD version, speech as MP3), (C) John
#    Passfield and Steve Stamatiadis
# Both readmes allow free redistribution, also as part of a larger
# distribution, when the readme and copyright notices stay intact; the
# packages carry the game files unchanged, with the readme as LICENSE.
# Each package's Start menu entry (game-start.c) runs the ScummVM package
# (store/linux/scummvm) on its game folder, so ScummVM must be installed
# too. Not built by default: add "scummvm-games" to STORE_LINUX.
. "$(dirname "$0")/../common.sh"
OUT=$1
CACHE=$2
HERE=$(cd "$(dirname "$0")" && pwd)
BASS=$(fetch "https://downloads.scummvm.org/frs/extras/Beneath%20a%20Steel%20Sky/bass-cd-1.2.zip" \
    53209b9400eab6fd7fa71518b2f357c8de75cfeaa5ba57024575ab79cc974593 \
    "$CACHE")
FOTAQ=$(fetch "https://downloads.scummvm.org/frs/extras/Flight%20of%20the%20Amazon%20Queen/FOTAQ_Talkie-1.1.zip" \
    a25cdd5e003a0a5e402af99b218cc7ea81ad032cb36b8c05df3bd1167038d8a8 \
    "$CACHE")

# game ID NAME VERSION GAME_ID ZIP README_IN_ZIP PUBLISHER
game() {
    id=$1 name=$2 version=$3 game_id=$4 zip=$5 readme=$6 publisher=$7
    DIR=$(work "$game_id" "$OUT")
    STAGE=$DIR/stage
    mkdir -p "$STAGE/game"
    # Unpack the game files flat into game/ (the archives hold one folder
    # or none).
    python3 -I - "$zip" "$STAGE/game" <<'PY'
import os, sys, zipfile
archive, target = sys.argv[1:]
with zipfile.ZipFile(archive) as z:
    for info in z.infolist():
        if info.is_dir():
            continue
        name = os.path.basename(info.filename)
        if not name or name.startswith("."):
            raise SystemExit("unexpected entry: " + info.filename)
        with open(os.path.join(target, name), "wb") as out:
            out.write(z.read(info))
PY
    cp "$STAGE/game/$readme" "$STAGE/LICENSE"
    $LINUX_CC -O2 -Wall -DGAME_ID="\"$game_id\"" -o "$DIR/start" \
        "$HERE/game-start.c" -static
    strip -o "$STAGE/$game_id-start" "$DIR/start"
    {
        cat <<PKG
format: 1
id: $id
name: $name
version: $version
publisher: $publisher
platform: linux
requires-opos: 0.6.7
PKG
        (cd "$STAGE" && find . -type f | sed 's|^\./||' | LC_ALL=C sort) |
        while read -r f; do
            case $f in
            "$game_id-start") echo "file: files/$f $f exec" ;;
            *) echo "file: files/$f $f" ;;
            esac
        done
        echo "file: files/LICENSE.musl LICENSE.musl"
        echo "start: files/$game_id-start $name"
    } > "$DIR/package.pkg"
    mv "$DIR/package.pkg" "$STAGE/package.pkg"
    package "$STAGE" "$OUT"
}

game org.scummvm.bass "Beneath a Steel Sky" 1.2.0.1 sky "$BASS" readme.txt \
    "Revolution Software (freeware, via ScummVM)"
game org.scummvm.fotaq "Flight of the Amazon Queen" 1.1.0.1 queen "$FOTAQ" \
    readme.txt "John Passfield and Steve Stamatiadis (freeware, via ScummVM)"
