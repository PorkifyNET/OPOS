# Shared by store/linux/*/build.sh (sourced, POSIX sh).
#
# Each build script is run by "make store" as
#   sh store/linux/<name>/build.sh OUT CACHE
# with these in the environment:
#   LINUX_CC     the musl C compiler command (see GNUmakefile)
#   MUSL_LICENSE musl's copyright file, shipped in every package
#   MKOPK        sdk/tools/mkopk.py
# It downloads its pinned source into CACHE (once), checks its SHA-256,
# builds a static program, and writes <id>-<version>.opk into OUT.

set -eu

: "${LINUX_CC:?LINUX_CC is not set}"
: "${MUSL_LICENSE:?MUSL_LICENSE is not set}"
: "${MKOPK:?MKOPK is not set}"

# fetch URL SHA256 CACHE: prints the path of the checked download.
fetch() {
    url=$1
    sum=$2
    cache=$3
    file=$cache/$(basename "$url")
    mkdir -p "$cache"
    if [ ! -f "$file" ]; then
        curl -fsSL -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    if ! echo "$sum  $file" | sha256sum -c --quiet - >/dev/null 2>&1; then
        echo "store/linux: $file does not match its SHA-256; delete it" \
             "to download it again" >&2
        exit 1
    fi
    echo "$file"
}

# work NAME OUT: a fresh build folder for NAME under OUT.
work() {
    dir=$2/work-$1
    rm -rf "$dir"
    mkdir -p "$dir"
    echo "$dir"
}

# map_build_paths OUT TOOLS: prints a CROSS prefix for musl cross tools
# that record source paths under OUT as /build (and the tools' own folder
# as /musl-cross), so __FILE__ strings in the programs do not carry the
# builder's folders or user name. The tools sit in the new folder TOOLS:
# the compilers are small scripts, everything else links to the real tool.
map_build_paths() {
    out=$(cd "$1" && pwd)
    real=${MUSL_CROSS:-$HOME/musl-cross/bin/x86_64-linux-musl-}
    root=$(dirname "$(dirname "$real")")
    prefix=$(basename "$real")
    mkdir -p "$2"
    tools=$(cd "$2" && pwd)
    for tool in "$real"*; do
        ln -s "$tool" "$tools/$(basename "$tool")"
    done
    for tool in gcc g++ cc c++; do
        [ -e "$real$tool" ] || continue
        rm -f "$tools/$prefix$tool"
        printf '#!/bin/sh\nexec "%s" "-ffile-prefix-map=%s=/build" "-ffile-prefix-map=%s=/musl-cross" "$@"\n' \
            "$real$tool" "$out" "$root" > "$tools/$prefix$tool"
        chmod +x "$tools/$prefix$tool"
    done
    echo "$tools/$prefix"
}

# package STAGE OUT: builds the .opk from STAGE/package.pkg into OUT.
package() {
    cp "$MUSL_LICENSE" "$1/LICENSE.musl"
    python3 "$MKOPK" build "$1/package.pkg" -o "$2"
}
