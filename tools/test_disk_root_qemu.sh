#!/bin/sh
set -eu

make build/initramfs.tar
python3 - <<'PY'
from pathlib import Path

main = Path('kernel/main.c').read_text()
mount = '    /* An update cut short by a power loss is finished first. */'
launch = '    bool boot_shell = root_selection_failed || settings_take_boot_shell_once();'
assert main.count(mount) == 1 and main.count(launch) == 1
main = main.replace('void kmain(void) {',
                    'void disk_root_smoke_prepare(void);\n'
                    'void disk_root_smoke_launch(void);\n'
                    'void disk_root_smoke_unstage(void);\n'
                    'void kmain(void) {', 1)
main = main.replace(mount, '    if (installer_available()) disk_root_smoke_prepare();\n\n' + mount, 1)
main = main.replace('    bool root_selection_failed = false;',
                    '    if (!installer_available()) disk_root_smoke_unstage();\n'
                    '    bool root_selection_failed = false;', 1)
main = main.replace(launch, '    if (installer_available()) disk_root_smoke_launch();\n'
                    '    bool boot_shell = true;', 1)
Path('build/root-smoke-main.c').write_text(main)

makefile = Path('GNUmakefile').read_text()
assert makefile.count('kernel/main.c') == 2
makefile = makefile.replace('kernel/main.c',
                            'build/root-smoke-main.c tools/test_disk_root_probe.c')
makefile = makefile.replace('$(BUILD)/kernel.elf',
                            '$(BUILD)/root-smoke-kernel.elf')
makefile = makefile.replace('$(BUILD)/opos.iso',
                            '$(BUILD)/root-smoke.iso')
Path('build/root-smoke.mk').write_text(makefile)
PY

: > build/root-smoke.img
truncate -s 96M build/root-smoke.img
make -f build/root-smoke.mk build/root-smoke.iso

log=build/root-smoke-serial.log
: > "$log"
qemu-system-x86_64 -m 256 -cdrom build/root-smoke.iso \
    -drive file=build/root-smoke.img,format=raw,if=ide,index=0 \
    -boot order=d -display none -serial "file:$log" \
    -monitor none -no-reboot &
vm_pid=$!
trap 'kill "$vm_pid" 2>/dev/null || true; wait "$vm_pid" 2>/dev/null || true' EXIT

seconds=0
while [ "$seconds" -lt 120 ]; do
    if grep -q 'ROOT-SMOKE: FAIL' "$log"; then
        tail -n 35 "$log"
        exit 1
    fi
    if grep -q 'ROOT-SMOKE: program launch PASS' "$log"; then
        grep 'ROOT-SMOKE:' "$log"
        break
    fi
    if ! kill -0 "$vm_pid" 2>/dev/null; then
        tail -n 35 "$log"
        exit 1
    fi
    sleep 1
    seconds=$((seconds + 1))
done
if ! grep -q 'ROOT-SMOKE: program launch PASS' "$log"; then
    tail -n 35 "$log"
    exit 1
fi
kill "$vm_pid" 2>/dev/null || true
wait "$vm_pid" 2>/dev/null || true

log=build/root-installed-serial.log
: > "$log"
qemu-system-x86_64 -m 256 \
    -drive file=build/root-smoke.img,format=raw,if=ide,index=0 \
    -boot order=c -display none -serial "file:$log" \
    -monitor none -no-reboot &
vm_pid=$!
seconds=0
while [ "$seconds" -lt 120 ]; do
    if grep -q 'disk system root verification failed' "$log"; then
        tail -n 35 "$log"
        exit 1
    fi
    if grep -q 'verified disk system root selected' "$log"; then
        grep -q 'ROOT-SMOKE: ready marker removed before boot' "$log"
        grep -q 'OPOS: staging disk system root' "$log"
        grep 'ROOT-SMOKE: ready marker removed before boot\|OPOS: staging disk system root\|verified disk system root selected' "$log"
        exit 0
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
