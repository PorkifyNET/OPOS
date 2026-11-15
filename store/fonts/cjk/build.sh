#!/bin/sh
# Chinese, Japanese, Korean fonts, for the OPOS Store: Droid Sans
# Fallback, a TrueType font with Han characters, kana and Hangul. It
# installs to /disk/Programs/net.porkify.opos.fonts-cjk/, where
# /share/fonts/fallback.txt finds it, so every program's text can show
# these scripts. License: Apache-2.0 (the NOTICE and the license text
# are in the package).
#
#   sh store/fonts/cjk/build.sh OUT CACHE     (MKOPK in the environment)
set -eu
: "${MKOPK:?MKOPK is not set}"
OUT=$1
CACHE=$2
VERSION=1.0
BASE=https://raw.githubusercontent.com/aosp-mirror/platform_frameworks_base/android10-release/data/fonts

# fetch URL SHA256 NAME: prints the path of the checked download.
fetch() {
    file=$CACHE/$3
    mkdir -p "$CACHE"
    if [ ! -f "$file" ]; then
        curl -fsSL -o "$file.part" "$1"
        mv "$file.part" "$file"
    fi
    if ! echo "$2  $file" | sha256sum -c --quiet - >/dev/null 2>&1; then
        echo "store/fonts: $file does not match its SHA-256; delete it" \
             "to download it again" >&2
        exit 1
    fi
    echo "$file"
}

FONT=$(fetch "$BASE/DroidSansFallbackFull.ttf" \
    2392015530438bafc48edfc4aee6d9de2387f627a6134d8ab3dfcc99d21c8240 \
    DroidSansFallbackFull.ttf)
NOTICE=$(fetch "$BASE/NOTICE" \
    38751245389e1e23f73e6f5384b5cbe7fa972cc4410c5adc9c04b082a0b9561a \
    droid-fonts-NOTICE)
LICENSE=$(fetch https://www.apache.org/licenses/LICENSE-2.0.txt \
    cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30 \
    apache-2.0.txt)

STAGE=$OUT/work-fonts-cjk
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$FONT" "$STAGE/DroidSansFallbackFull.ttf"
cp "$NOTICE" "$STAGE/NOTICE"
cp "$LICENSE" "$STAGE/LICENSE"
cat > "$STAGE/package.pkg" <<EOF
format: 1
id: net.porkify.opos.fonts-cjk
name: Chinese, Japanese, Korean fonts
version: $VERSION
publisher: The Android Open Source Project (packaged for OPOS)
platform: opos
requires-opos: 0.6.2
file: files/DroidSansFallbackFull.ttf DroidSansFallbackFull.ttf
file: files/NOTICE NOTICE
file: files/LICENSE LICENSE
EOF
python3 "$MKOPK" build "$STAGE/package.pkg" -o "$OUT"
