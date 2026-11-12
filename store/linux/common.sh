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

# package STAGE OUT: builds the .opk from STAGE/package.pkg into OUT.
package() {
    cp "$MUSL_LICENSE" "$1/LICENSE.musl"
    python3 "$MKOPK" build "$1/package.pkg" -o "$2"
}
