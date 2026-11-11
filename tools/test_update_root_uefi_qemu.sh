#!/bin/sh
set -eu
qemu_args='-accel tcg,thread=multi'

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

probe = Path('tools/test_update_root_probe.c').read_text()
install = 'bool installed = installer_install(0, NULL, &error);'
replacement = '''struct installer_layout layout;
    bool installed = installer_layout_load(0, &layout, &error);
    if (installed) {
        layout.target = 1;
        installed = installer_install_partition(
            &layout, INSTALLER_BOOT_UEFI_DEFAULT, NULL, &error);
    }'''
assert probe.count(install) == 1
probe = probe.replace(install, replacement)
Path('build/update-test-uefi-probe.c').write_text(probe)

update = Path('kernel/update.c').read_text()
header = '#include "vfs.h"\n'
assert update.count(header) == 1
update = update.replace(header, header +
    '\nstatic void update_root_checkpoint(const char *message) {\n'
    '    console_write_string(message);\n'
    '    for (volatile size_t i = 0; i < 10000000; i++)\n'
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
Path('build/update-test-update.c').write_text(update)

makefile = Path('GNUmakefile').read_text()
assert makefile.count('kernel/main.c') == 2
assert makefile.count('kernel/update.c') == 2
makefile = makefile.replace('kernel/main.c',
                            'build/update-test-main.c build/update-test-uefi-probe.c')
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
    while [ "$seconds" -lt 240 ]; do
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

python3 - <<'PY'
from pathlib import Path
import struct

sectors = 131072
start = 2048 * 512
spc, reserved, fats, roots, spf = 4, 1, 2, 512, 128
b = bytearray(256 * 1024 * 1024)
boot = memoryview(b)[start:start + 512]
boot[0:3] = bytes((0xeb, 0x3c, 0x90))
boot[3:11] = b'MSDOS5.0'
struct.pack_into('<H', boot, 11, 512)
boot[13] = spc
struct.pack_into('<H', boot, 14, reserved)
boot[16] = fats
struct.pack_into('<H', boot, 17, roots)
struct.pack_into('<I', boot, 32, sectors)
boot[21] = 0xf8
struct.pack_into('<H', boot, 22, spf)
boot[38] = 0x29
boot[43:54] = b'OPOS ESP   '
boot[54:62] = b'FAT16   '
boot[510:512] = bytes((0x55, 0xaa))
fat = bytearray(spf * 512)
struct.pack_into('<HH', fat, 0, 0xfff8, 0xffff)
fat_start = start + reserved * 512
b[fat_start:fat_start + len(fat)] = fat
b[fat_start + len(fat):fat_start + 2 * len(fat)] = fat
Path('build/update-uefi.img').write_bytes(b)
PY
sgdisk --clear --new=1:2048:+64M --typecode=1:ef00 \
    --new=2:133120:+160M --typecode=2:0700 build/update-uefi.img
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/update-uefi-vars.fd
ovmf_code=/usr/share/OVMF/OVMF_CODE_4M.fd
ovmf_vars=build/update-uefi-vars.fd
qemu_ovmf() {
    exec qemu-system-x86_64 -accel tcg,thread=multi \
        -drive "if=pflash,format=raw,readonly=on,file=$ovmf_code" \
        -drive "if=pflash,format=raw,file=$ovmf_vars" "$@"
}

log=build/update-uefi-install.log
: > "$log"
qemu_ovmf -m 512 -cdrom build/update-old.iso \
    -drive file=build/update-uefi.img,format=raw -boot order=d \
    -display none -serial "file:$log" -monitor none -no-reboot &
vm_pid=$!
wait_for "$log" 'UPDATE-ROOT: base installed'
stop_vm
cp build/update-uefi.img build/update-uefi-base.img

log=build/update-uefi-commit.log
: > "$log"
qemu_ovmf -m 512 -drive file=build/update-uefi.img,format=raw \
    -boot order=c -display none -serial "file:$log" -monitor none -no-reboot &
vm_pid=$!
wait_for "$log" 'UPDATE-ROOT: commit complete PASS'
stop_vm

log=build/update-uefi-current.log
: > "$log"
qemu_ovmf -m 512 -drive file=build/update-uefi.img,format=raw \
    -boot order=c -display none -serial "file:$log" -monitor none -no-reboot &
vm_pid=$!
wait_for "$log" 'UPDATE-ROOT: new generation and rollback pair PASS'
stop_vm

python3 tools/test_update_menu_order_uefi.py build/update-uefi.img previous
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/update-uefi-prev-vars.fd
python3 tools/test_update_previous_uefi_qemu.py build/update-uefi.img \
    build/update-uefi-previous.log build/update-uefi-prev-vars.fd
echo 'UPDATE-ROOT: UEFI/GPT previous-version rollback PASS'

for stage in prepared backup menu copying first-file; do
    case "$stage" in
        prepared) marker='prepared durable'; next='backup durable' ;;
        backup) marker='backup durable'; next='menu durable' ;;
        menu) marker='menu durable'; next='copying durable' ;;
        copying) marker='copying durable'; next='first file copied' ;;
        first-file) marker='first file copied'; next='commit complete' ;;
    esac
    image="build/update-uefi-cut-$stage.img"
    cp build/update-uefi-base.img "$image"
    ovmf_vars="build/update-uefi-cut-$stage-vars.fd"
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$ovmf_vars"
    log="build/update-uefi-cut-$stage.log"
    : > "$log"
    qemu_ovmf -m 512 -drive "file=$image,format=raw" -boot order=c \
        -display none -serial "file:$log" -monitor none -no-reboot &
    vm_pid=$!
    wait_for "$log" "UPDATE-ROOT: $marker"
    crash_vm
    if grep -q "UPDATE-ROOT: $next" "$log"; then
        echo "Cut missed UEFI/GPT $stage checkpoint"
        exit 1
    fi

    if [ "$stage" = menu ] || [ "$stage" = copying ] \
        || [ "$stage" = first-file ]; then
        python3 tools/test_update_menu_order_uefi.py "$image" previous
        cp /usr/share/OVMF/OVMF_VARS_4M.fd "$ovmf_vars"
        python3 tools/test_update_previous_uefi_qemu.py "$image" \
            "build/update-uefi-cut-$stage-previous.log" "$ovmf_vars"
        python3 tools/test_update_menu_order_uefi.py "$image" current
    fi

    # For menu and later cuts, the previous-entry boot above resumes the
    # interrupted update itself. Prepared and backup cuts have no previous
    # entry yet, so resume them directly through the old entry here.
    if [ "$stage" = prepared ] || [ "$stage" = backup ]; then
        log="build/update-uefi-resume-$stage-old.log"
        : > "$log"
        qemu_ovmf -m 512 -drive "file=$image,format=raw" -boot order=c \
            -display none -serial "file:$log" -monitor none -no-reboot &
        vm_pid=$!
        wait_for "$log" 'UPDATE-ROOT: old generation resumed PASS'
        stop_vm
    fi

    log="build/update-uefi-resume-$stage-new.log"
    : > "$log"
    qemu_ovmf -m 512 -drive "file=$image,format=raw" -boot order=c \
        -display none -serial "file:$log" -monitor none -no-reboot &
    vm_pid=$!
    wait_for "$log" 'UPDATE-ROOT: new generation and rollback pair PASS'
    stop_vm
    echo "UPDATE-ROOT: UEFI/GPT $stage power-cut recovery PASS"
done
