#!/bin/sh
set -eu
qemu_args="${QEMU_ARGS:--accel tcg,thread=multi}"

make build/initramfs.tar build/install.tar
python3 - <<'PY'
from pathlib import Path

main = Path('kernel/main.c').read_text()
mount = '    /* An update cut short by a power loss is finished first. */'
launch = '    bool boot_shell = root_selection_failed || settings_take_boot_shell_once();'
assert main.count(mount) == 1 and main.count(launch) == 1
main = main.replace('void kmain(void) {',
                    'void update_root_install(void);\n'
                    'void update_root_step(void);\n'
                    'void kmain(void) {', 1)
main = main.replace(mount,
                    '    if (installer_available()) update_root_install();\n\n'
                    + mount, 1)
main = main.replace(launch,
                    '    if (!installer_available()) update_root_step();\n'
                    '    bool boot_shell = true;', 1)
Path('build/update-test-main.c').write_text(main)

update = Path('kernel/update.c').read_text()
header = '#include "vfs.h"\n'
assert update.count(header) == 1
update = update.replace(header, header +
    '\nstatic void update_root_checkpoint(const char *message) {\n'
    '    console_write_string(message);\n'
    '    for (volatile size_t i = 0; i < 100000000; i++)\n'
    '        __asm__ volatile ("pause");\n'
    '}\n', 1)
start = update.index('static enum update_result apply(bool copying) {')
end = update.index('\nenum update_result update_commit(', start)
apply = update[start:end]
needle = 'if (!block_end_batch() || !saved) return UPDATE_DISK_ERROR;'
assert apply.count(needle) == 3
parts = apply.split(needle)
apply = parts[0]
for name, rest in zip(('backup', 'menu', 'copying'), parts[1:]):
    apply += (needle + '\n'
              + f'        update_root_checkpoint("UPDATE-ROOT: {name} durable\\n");'
              + rest)
update = update[:start] + apply + update[end:]
prepared = '    return apply(false);\n'
assert update.count(prepared) == 1
update = update.replace(prepared,
    '    update_root_checkpoint("UPDATE-ROOT: prepared durable\\n");\n'
    + prepared, 1)
first = '                return false;\n            }\n        }\n    }\n    return true;\n}\n\nstatic bool write_state'
assert update.count(first) == 1
update = update.replace(first,
    '                return false;\n            }\n'
    '            if (pass == 0)\n'
    '                update_root_checkpoint("UPDATE-ROOT: first file copied\\n");\n'
    '        }\n    }\n    return true;\n}\n\nstatic bool write_state', 1)
resume = 'void update_resume(void) {\n'
assert update.count(resume) == 1
update = update.replace(resume,
                        resume + '    console_write_string("UPDATE-ROOT: resume entered\\n");\n',
                        1)
guard = 'if (!installed()) {\n        return;\n    }\n    char state[16];'
assert update.count(guard) == 1
update = update.replace(guard,
                        'if (!installed()) {\n'
                        '        console_write_string("UPDATE-ROOT: resume not installed\\n");\n'
                        '        return;\n    }\n    char state[16];', 1)
missing = 'if (!read_small(STAGE_STATE, (uint8_t *)state, sizeof(state) - 1,\n                    &state_length)) {\n        return;\n    }'
assert update.count(missing) == 1
update = update.replace(missing,
                        missing.replace('        return;',
                                        '        console_write_string("UPDATE-ROOT: state unreadable\\n");\n'
                                        '        return;'), 1)
Path('build/update-test-update.c').write_text(update)

makefile = Path('GNUmakefile').read_text()
assert makefile.count('kernel/main.c') == 2
assert makefile.count('kernel/update.c') == 2
makefile = makefile.replace('kernel/main.c',
                            'build/update-test-main.c tools/test_update_root_probe.c')
makefile = makefile.replace('kernel/update.c', 'build/update-test-update.c')
Path('build/update-test.mk').write_text(makefile)
PY

make -f build/update-test.mk KERNEL=build/update-new-kernel.elf \
    OPOS_VERSION=0.1.4 build/update-new-kernel.elf
rm -rf build/update-new-root
mkdir -p build/update-new-root
cp -a build/initramfs_root/. build/update-new-root/
printf 'new\n' > build/update-new-root/tests/update-generation.txt
tar --format=ustar -C build/update-new-root -cf \
    build/update-new-initramfs.tar bin etc lib share tests
rm -rf build/update-test-root
mkdir -p build/update-test-root
cp -a build/initramfs_root/. build/update-test-root/
cp build/update-new-kernel.elf build/update-test-root/tests/update-kernel.elf
cp build/update-new-initramfs.tar build/update-test-root/tests/update-initramfs.tar
tar --format=ustar -C build/update-test-root -cf \
    build/update-test-initramfs.tar bin etc lib share tests
make -f build/update-test.mk KERNEL=build/update-old-kernel.elf \
    OPOS_VERSION=0.1.3 build/update-old-kernel.elf
make -f build/update-test.mk KERNEL=build/update-old-kernel.elf \
    ISO=build/update-old.iso INITRAMFS=build/update-test-initramfs.tar \
    OPOS_VERSION=0.1.3 build/update-old.iso

vm_pid=
stop_vm() {
    if [ -n "$vm_pid" ]; then
        kill "$vm_pid" 2>/dev/null || true
        wait "$vm_pid" 2>/dev/null || true
        vm_pid=
    fi
}
crash_vm() {
    if [ -n "$vm_pid" ]; then
        kill -KILL "$vm_pid" 2>/dev/null || true
        wait "$vm_pid" 2>/dev/null || true
        vm_pid=
    fi
}
trap stop_vm EXIT

wait_for() {
    log=$1
    marker=$2
    seconds=0
    while [ "$seconds" -lt 180 ]; do
        if grep -q 'UPDATE-ROOT: FAIL' "$log"; then
            tail -n 35 "$log"
            exit 1
        fi
        if grep -q "$marker" "$log"; then
            grep "$marker" "$log" | tail -n 1
            return
        fi
        if ! kill -0 "$vm_pid" 2>/dev/null; then
            tail -n 35 "$log"
            exit 1
        fi
        sleep 1
        seconds=$((seconds + 1))
    done
    tail -n 35 "$log"
    exit 1
}

: > build/update-base.img
truncate -s 192M build/update-base.img
log=build/update-install.log
: > "$log"
qemu-system-x86_64 $qemu_args -m 256 -cdrom build/update-old.iso \
    -drive file=build/update-base.img,format=raw,if=ide,index=0 \
    -boot order=d -display none -serial "file:$log" \
    -monitor none -no-reboot &
vm_pid=$!
wait_for "$log" 'UPDATE-ROOT: base installed'
stop_vm

cp build/update-base.img build/update-success.img
log=build/update-success-old.log
: > "$log"
qemu-system-x86_64 $qemu_args -m 256 \
    -drive file=build/update-success.img,format=raw,if=ide,index=0 \
    -boot order=c -display none -serial "file:$log" \
    -monitor none -no-reboot &
vm_pid=$!
wait_for "$log" 'UPDATE-ROOT: commit complete PASS'
stop_vm

log=build/update-success-new.log
: > "$log"
qemu-system-x86_64 $qemu_args -m 256 \
    -drive file=build/update-success.img,format=raw,if=ide,index=0 \
    -boot order=c -display none -serial "file:$log" \
    -monitor none -no-reboot &
vm_pid=$!
wait_for "$log" 'UPDATE-ROOT: new generation and rollback pair PASS'
stop_vm

python3 tools/test_update_menu_order.py build/update-success.img previous
python3 tools/test_update_previous_qemu.py build/update-success.img \
    build/update-success-previous.log
python3 tools/test_update_menu_order.py build/update-success.img current

for stage in prepared backup menu copying first-file; do
    case "$stage" in
        prepared) marker='prepared durable'; next='backup durable' ;;
        backup) marker='backup durable'; next='menu durable' ;;
        menu) marker='menu durable'; next='copying durable' ;;
        copying) marker='copying durable'; next='first file copied' ;;
        first-file) marker='first file copied'; next='commit complete' ;;
    esac
    image="build/update-cut-$stage.img"
    cp build/update-base.img "$image"
    log="build/update-cut-$stage.log"
    : > "$log"
    qemu-system-x86_64 $qemu_args -m 256 \
        -drive "file=$image,format=raw,if=ide,index=0" \
        -boot order=c -display none -serial "file:$log" \
        -monitor none -no-reboot &
    vm_pid=$!
    wait_for "$log" "UPDATE-ROOT: $marker"
    crash_vm
    if grep -q "UPDATE-ROOT: $next" "$log"; then
        echo "Cut missed $stage checkpoint"
        exit 1
    fi

    if [ "$stage" = menu ] || [ "$stage" = copying ] \
        || [ "$stage" = first-file ]; then
        python3 tools/test_update_menu_order.py "$image" previous
        python3 tools/test_update_previous_qemu.py "$image" \
            "build/update-$stage-previous.log"
        grep -q 'OPOS: update installed' \
            "build/update-$stage-previous.log"
        python3 tools/test_update_menu_order.py "$image" current
    fi

    if [ "$stage" = prepared ] || [ "$stage" = backup ]; then
        log="build/update-resume-$stage-old.log"
        : > "$log"
        qemu-system-x86_64 $qemu_args -m 256 \
            -drive "file=$image,format=raw,if=ide,index=0" \
            -boot order=c -display none -serial "file:$log" \
            -monitor none -no-reboot &
        vm_pid=$!
        wait_for "$log" 'UPDATE-ROOT: old generation resumed PASS'
        grep -q 'OPOS: update installed' "$log"
        stop_vm
    fi

    log="build/update-resume-$stage.log"
    : > "$log"
    qemu-system-x86_64 $qemu_args -m 256 \
        -drive "file=$image,format=raw,if=ide,index=0" \
        -boot order=c -display none -serial "file:$log" \
        -monitor none -no-reboot &
    vm_pid=$!
    wait_for "$log" 'UPDATE-ROOT: new generation and rollback pair PASS'
    stop_vm
    if [ "$stage" = prepared ] || [ "$stage" = backup ]; then
        python3 tools/test_update_menu_order.py "$image" previous
        python3 tools/test_update_previous_qemu.py "$image" \
            "build/update-$stage-previous.log"
    fi
    echo "UPDATE-ROOT: $stage power-cut recovery PASS"
done
