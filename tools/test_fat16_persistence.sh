#!/bin/sh
set -eu

iso=${1:-build/opos.iso}
disk=${2:-build/fat16-test.img}
write_screen=build/fat16-write.ppm
read_screen=build/fat16-readback.ppm

type_text() {
    remaining=$1
    while [ -n "$remaining" ]; do
        character=$(printf '%.1s' "$remaining")
        case "$character" in
            ' ') key=spc ;;
            '.') key=dot ;;
            '/') key=slash ;;
            *) key=$character ;;
        esac
        printf 'sendkey %s 10\n' "$key"
        sleep 0.03
        remaining=${remaining#?}
    done
    printf 'sendkey ret 10\n'
}

run_boot() {
    first_command=$1
    second_command=$2
    third_command=$3
    fourth_command=$4
    fifth_command=$5
    sixth_command=$6
    screen=$7
    {
        sleep 8
        type_text "$first_command"
        if [ -n "$second_command" ]; then
            sleep 1
            type_text "$second_command"
        fi
        if [ -n "$third_command" ]; then
            sleep 1
            type_text "$third_command"
        fi
        if [ -n "$fourth_command" ]; then
            sleep 1
            type_text "$fourth_command"
        fi
        if [ -n "$fifth_command" ]; then
            sleep 1
            type_text "$fifth_command"
        fi
        if [ -n "$sixth_command" ]; then
            sleep 1
            type_text "$sixth_command"
        fi
        sleep 2
        printf 'screendump %s\n' "$screen"
        sleep 1
        printf 'quit\n'
    } | qemu-system-x86_64 -m 128 -cdrom "$iso" \
        -drive file="$disk",format=raw,if=ide,index=0 \
        -boot order=d -display none -serial none -monitor stdio -no-reboot
}

payload=
index=0
while [ "$index" -lt 600 ]; do
    payload="${payload}a"
    index=$((index + 1))
done

growth_payload=
index=0
while [ "$index" -lt 513 ]; do
    growth_payload="${growth_payload}b"
    index=$((index + 1))
done

run_boot 'disk mkdir docs' "disk write created.txt $payload" \
    "disk write persist.txt $growth_payload" 'run filetest' 'run filetest' \
    'proctest' "$write_screen"
test "$(dd if="$disk" bs=1 skip=131616 count=11 2>/dev/null)" = 'DOCS       '
test "$(od -An -tu1 -j 131627 -N 1 "$disk" | tr -d ' ')" = 16
test "$(od -An -tu2 -j 131642 -N 2 "$disk" | tr -d ' ')" = 3
test "$(dd if="$disk" bs=1 skip=131648 count=11 2>/dev/null)" = 'CREATED TXT'
test "$(od -An -tu4 -j 131676 -N 4 "$disk" | tr -d ' ')" = 100
test "$(dd if="$disk" bs=1 skip=131680 count=11 2>/dev/null)" = 'TRUNC   TMP'
test "$(od -An -tu2 -j 131706 -N 2 "$disk" | tr -d ' ')" = 5
test "$(od -An -tu4 -j 131708 -N 4 "$disk" | tr -d ' ')" = 15
test "$(od -An -tu1 -j 131712 -N 1 "$disk" | tr -d ' ')" = 229
test "$(od -An -tu4 -j 131612 -N 4 "$disk" | tr -d ' ')" = 513
test "$(od -An -tu2 -j 516 -N 2 "$disk" | tr -d ' ')" = 6
test "$(od -An -tu2 -j 518 -N 2 "$disk" | tr -d ' ')" = 10
test "$(od -An -tu2 -j 520 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 522 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 524 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 526 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 528 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 530 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 532 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 534 -N 2 "$disk" | tr -d ' ')" = 0
test "$(od -An -tu2 -j 66052 -N 2 "$disk" | tr -d ' ')" = 6
test "$(od -An -tu2 -j 66054 -N 2 "$disk" | tr -d ' ')" = 10
test "$(od -An -tu2 -j 66056 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 66058 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 66060 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 66062 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 66064 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 66066 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 66068 -N 2 "$disk" | tr -d ' ')" = 65535
test "$(od -An -tu2 -j 66070 -N 2 "$disk" | tr -d ' ')" = 0
test "$(dd if="$disk" bs=1 skip=148480 count=11 2>/dev/null)" = '.          '
test "$(dd if="$disk" bs=1 skip=148512 count=11 2>/dev/null)" = '..         '
test "$(od -An -tu2 -j 148538 -N 2 "$disk" | tr -d ' ')" = 0
test "$(dd if="$disk" bs=1 skip=148544 count=11 2>/dev/null)" = 'USER    TXT'
test "$(od -An -tu4 -j 148572 -N 4 "$disk" | tr -d ' ')" = 15
test "$(dd if="$disk" bs=1 skip=148576 count=11 2>/dev/null)" = 'AGAIN      '
test "$(od -An -tu1 -j 148587 -N 1 "$disk" | tr -d ' ')" = 16
test "$(od -An -tu2 -j 148602 -N 2 "$disk" | tr -d ' ')" = 8
test "$(dd if="$disk" bs=1 skip=148608 count=11 2>/dev/null)" = 'REUSE   TMP'
test "$(od -An -tu4 -j 148636 -N 4 "$disk" | tr -d ' ')" = 15
test "$(od -An -tu2 -j 148634 -N 2 "$disk" | tr -d ' ')" = 9
test "$(dd if="$disk" bs=1 skip=148640 count=11 2>/dev/null)" = 'F00     TMP'
test "$(dd if="$disk" bs=1 skip=148960 count=11 2>/dev/null)" = 'F10     TMP'
test "$(dd if="$disk" bs=1 skip=152064 count=11 2>/dev/null)" = 'F11     TMP'
test "$(od -An -tu1 -j 152096 -N 1 "$disk" | tr -d ' ')" = 229
test "$(dd if="$disk" bs=1 skip=148992 count=8 2>/dev/null)" = aaaaaaaa
test "$(dd if="$disk" bs=1 skip=149504 count=15 2>/dev/null)" = 'ring3-file-data'
test "$(dd if="$disk" bs=1 skip=147968 count=8 2>/dev/null)" = bbbbbbbb
test "$(dd if="$disk" bs=1 skip=150016 count=1 2>/dev/null)" = b
test "$(dd if="$disk" bs=1 skip=150528 count=15 2>/dev/null)" = 'ring3-file-data'
test "$(dd if="$disk" bs=1 skip=151040 count=11 2>/dev/null)" = '.          '
test "$(dd if="$disk" bs=1 skip=151072 count=11 2>/dev/null)" = '..         '
test "$(dd if="$disk" bs=1 skip=151552 count=15 2>/dev/null)" = 'ring3-file-data'

run_boot 'disk cat docs/user.txt' '' '' '' '' '' "$read_screen"
printf 'FAT16 persistence test passed.\n'
