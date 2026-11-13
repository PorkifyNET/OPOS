#!/bin/sh
# Boot an OPOS ISO in QEMU, type shell commands through the monitor, and
# capture the serial log. Usage:
#   tools/qemu_cmds.sh ISO LOG [-d DISK.img] [-q "QEMU ARGS"] [-k] [-w SECONDS] [-s FILE] [-b SECONDS] "cmd1" "cmd2" ...
# -k enables KVM when /dev/kvm is usable (stricter, closer to VMware).
# -w sets seconds to wait after each command (default 3).
# -q appends raw QEMU arguments, for example an NVMe disk:
#   -q "-drive file=d.img,format=raw,if=none,id=n0 -device nvme,serial=n0,drive=n0"
# -s FILE saves a monitor screendump (PPM) after the last command.
# -b sets seconds to wait for the desktop before typing (default 6).
set -eu
iso=$1; log=$2; shift 2
disk=""; extra=""; accel="-accel tcg"; wait_after=3; screendump=""; boot_wait=6
while [ $# -gt 0 ]; do
    case "$1" in
        -d) disk="-drive file=$2,format=raw,if=ide"; shift 2 ;;
        -q) extra="$extra $2"; shift 2 ;;
        -k) [ -w /dev/kvm ] && accel="-accel kvm -cpu host"; shift ;;
        -w) wait_after=$2; shift 2 ;;
        -s) screendump=$2; shift 2 ;;
        -b) boot_wait=$2; shift 2 ;;
        *) break ;;
    esac
done
type_text() {
    remaining=$1
    while [ -n "$remaining" ]; do
        c=$(printf '%.1s' "$remaining")
        case "$c" in
            ' ') key=spc ;; '.') key=dot ;; '/') key=slash ;; '-') key=minus ;;
            '_') key=shift-minus ;; ':') key=shift-semicolon ;; '=') key=equal ;;
            ',') key=comma ;; ';') key=semicolon ;; "'") key=apostrophe ;;
            '`') key=grave_accent ;; '[') key=bracket_left ;; ']') key=bracket_right ;;
            '\') key=backslash ;;
            '!') key=shift-1 ;; '@') key=shift-2 ;; '#') key=shift-3 ;; '$') key=shift-4 ;;
            '%') key=shift-5 ;; '^') key=shift-6 ;; '&') key=shift-7 ;; '*') key=shift-8 ;;
            '(') key=shift-9 ;; ')') key=shift-0 ;; '+') key=shift-equal ;;
            '{') key=shift-bracket_left ;; '}') key=shift-bracket_right ;;
            '|') key=shift-backslash ;; '"') key=shift-apostrophe ;;
            '~') key=shift-grave_accent ;; '<') key=shift-comma ;;
            '>') key=shift-dot ;; '?') key=shift-slash ;;
            [A-Z]) key=shift-$(printf '%s' "$c" | tr 'A-Z' 'a-z') ;;
            *) key=$c ;;
        esac
        printf 'sendkey %s 10\n' "$key"
        sleep 0.03
        remaining=${remaining#?}
    done
    printf 'sendkey ret 10\n'
}
{
    sleep "$boot_wait"
    # The desktop opens on boot; its terminal command "shell" returns to the
    # kernel shell (in the shell itself it is an unknown command). Super+Enter
    # first focuses the terminal (live media opens the installer instead).
    printf 'sendkey meta_l-ret 10\n'
    sleep 1
    type_text shell
    sleep 2
    for cmd in "$@"; do
        type_text "$cmd"
        sleep "$wait_after"
    done
    if [ -n "$screendump" ]; then
        printf 'screendump %s\n' "$screendump"
        sleep 1
    fi
    printf 'quit\n'
} | timeout 300 qemu-system-x86_64 $accel -m 512M -cdrom "$iso" $disk $extra \
    -display none -serial "file:$log" -monitor stdio -no-reboot -no-shutdown >/dev/null 2>&1 || true
