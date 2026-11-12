#!/bin/sh
# Plays tones through the HDA driver in QEMU and checks what came out.
#   tools/test_sound.sh [ISO]
# QEMU records the guest's sound with the monitor's wavcapture (it works
# with the "none" audio backend), the kernel shell plays "beep" (750 Hz)
# and "beep 1500" (the driver), then /bin/play plays 440 and 880 Hz through
# the mixer; tools/check_tones.py finds all four tones in the file.
# BOOT_WAIT (default 40) is how long to wait for the desktop.
set -eu
iso=${1:-build/opos.iso}
wav=build/sound-test.wav
boot_wait=${BOOT_WAIT:-40}
accel="-accel tcg"
[ -w /dev/kvm ] && accel="-accel kvm -cpu host"
rm -f "$wav"

type_text() {
    remaining=$1
    while [ -n "$remaining" ]; do
        character=$(printf '%.1s' "$remaining")
        case "$character" in
            ' ') key=spc ;;
            '/') key=slash ;;
            *) key=$character ;;
        esac
        printf 'sendkey %s 10\n' "$key"
        sleep 0.03
        remaining=${remaining#?}
    done
    printf 'sendkey ret 10\n'
}

{
    sleep "$boot_wait"
    # Super+Enter focuses the terminal; "shell" goes to the kernel shell.
    printf 'sendkey meta_l-ret 10\n'
    sleep 1
    type_text shell
    sleep 2
    printf 'wavcapture %s a0\n' "$wav"
    sleep 2
    type_text beep
    sleep 3
    type_text 'beep 1500'
    sleep 3
    type_text 'run /bin/play'
    sleep 4
    printf 'stopcapture 0\n'
    sleep 1
    printf 'quit\n'
} | timeout 300 qemu-system-x86_64 $accel -m 512M -cdrom "$iso" \
    -audiodev none,id=a0 -device intel-hda -device hda-output,audiodev=a0 \
    -display none -serial none -monitor stdio -no-reboot >/dev/null 2>&1 \
    || true

python3 tools/check_tones.py "$wav" 750 1500 440 880
