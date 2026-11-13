#!/bin/sh
# Write files to a fresh FAT16 test disk from the kernel shell, run the
# ring-3 file test twice and the process test, then check the disk image
# with tools/check_fat16_persistence.py and read a file back in a second
# boot. Usage: tools/test_fat16_persistence.sh [ISO] [DISK]
# The disk must come fresh from build/mkfat16_test (make persistence-test).
# Set BOOT_WAIT to wait longer for the desktop on a slow machine.
set -eu

iso=${1:-build/opos.iso}
disk=${2:-build/fat16-test.img}
write_log=build/fat16-write.log
read_log=build/fat16-readback.log
write_screen=build/fat16-write.ppm
read_screen=build/fat16-readback.ppm
boot_wait=${BOOT_WAIT:-40}
here=$(dirname "$0")

repeat() {
    text=
    count=0
    while [ "$count" -lt "$2" ]; do
        text="$text$1"
        count=$((count + 1))
    done
    printf '%s' "$text"
}

fail() {
    printf 'FAT16 persistence test failed: %s\n' "$1" >&2
    exit 1
}

# -boot d starts the ISO; the test disk alone is not bootable.
sh "$here/qemu_cmds.sh" "$iso" "$write_log" -d "$disk" -q '-boot d' -k \
    -b "$boot_wait" -w 4 -s "$write_screen" \
    'disk mkdir docs' \
    "disk write created.txt $(repeat a 600)" \
    "disk write persist.txt $(repeat b 513)" \
    'run filetest' 'run filetest' 'proctest'

passes=$(grep -c 'ring-3 file syscalls passed' "$write_log" || true)
[ "$passes" -eq 2 ] || fail "filetest passed $passes of 2 times (see $write_log)"
grep -q 'Process control block test passed' "$write_log" ||
    fail "proctest did not pass (see $write_log)"

python3 "$here/check_fat16_persistence.py" "$disk" || fail 'disk contents'

sh "$here/qemu_cmds.sh" "$iso" "$read_log" -d "$disk" -q '-boot d' -k \
    -b "$boot_wait" -s "$read_screen" 'disk cat docs/user.txt'
grep -q 'ring3-file-data' "$read_log" ||
    fail "docs/user.txt did not read back (see $read_log)"

printf 'FAT16 persistence test passed.\n'
