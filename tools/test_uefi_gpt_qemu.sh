#!/bin/sh
set -eu

make build/initramfs.tar
python3 - <<'PY'
from pathlib import Path
main = Path('kernel/main.c').read_text()
mount = '    /* An update cut short by a power loss is finished first. */'
selection = '    bool root_selection_failed = false;'
assert main.count(mount) == 1 and main.count(selection) == 1
main = main.replace('void kmain(void) {',
                    'void uefi_gpt_install_probe(void);\n'
                    'void uefi_gpt_boot_probe(void);\n'
                    'void kmain(void) {', 1)
main = main.replace(mount, '    if (installer_available()) uefi_gpt_install_probe();\n' + mount, 1)
main = main.replace(selection, '    if (!installer_available()) uefi_gpt_boot_probe();\n' + selection, 1)
Path('build/uefi-gpt-main.c').write_text(main)
mk = Path('GNUmakefile').read_text()
assert mk.count('kernel/main.c') == 2
mk = mk.replace('kernel/main.c', 'build/uefi-gpt-main.c tools/test_uefi_gpt_probe.c')
mk = mk.replace('$(BUILD)/kernel.elf', '$(BUILD)/uefi-gpt-kernel.elf')
mk = mk.replace('$(BUILD)/opos.iso', '$(BUILD)/uefi-gpt.iso')
Path('build/uefi-gpt.mk').write_text(mk)
PY

python3 - <<'PY'
from pathlib import Path
import struct
import hashlib

# A minimal empty FAT16 volume, 64 MiB, suitable as an EFI System Partition.
sectors = 131072
start = 2048 * 512
spc, reserved, fats, roots = 4, 1, 2, 512
root_secs = roots * 32 // 512
spf = 128
data_secs = sectors - reserved - fats * spf - root_secs
clusters = data_secs // spc
b = bytearray(384 * 1024 * 1024)
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
boot[36] = 0x80
boot[38] = 0x29
boot[43:54] = b'OPOS ESP   '
boot[54:62] = b'FAT16   '
boot[510:512] = bytes((0x55, 0xaa))
fat = bytearray(spf * 512)
struct.pack_into('<HH', fat, 0, 0xfff8, 0xffff)
fat_start = start + reserved * 512
b[fat_start:fat_start + len(fat)] = fat
b[fat_start + fats * len(fat):fat_start + (fats + 1) * len(fat)] = fat
Path('build/uefi-gpt.img').write_bytes(b)
data_start = 479232 * 512
data_size = 65536 * 512
sentinel = bytes((i * 37 + 11) & 0xff for i in range(512))
sentinel_chunk = sentinel * 2048
with open('build/uefi-gpt.img', 'r+b') as disk:
    disk.seek(data_start)
    for _ in range(data_size // len(sentinel_chunk)):
        disk.write(sentinel_chunk)
with open('build/uefi-gpt.img', 'rb') as disk:
    disk.seek(data_start)
    remaining = data_size
    hasher = hashlib.sha256()
    while remaining:
        chunk = disk.read(min(1024 * 1024, remaining))
        hasher.update(chunk)
        remaining -= len(chunk)
Path('build/uefi-gpt-data.sha256').write_text(hasher.hexdigest())
PY

sgdisk --clear --new=1:2048:+64M --typecode=1:ef00 \
    --new=2:133120:+160M --typecode=2:0700 \
    --new=3:479232:+32M --typecode=3:0700 build/uefi-gpt.img
make -f build/uefi-gpt.mk ISO=build/uefi-gpt.iso \
    KERNEL=build/uefi-gpt-kernel.elf build/uefi-gpt.iso
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/uefi-gpt-vars.fd

log=build/uefi-gpt-install.log
: > "$log"
timeout 60 qemu-system-x86_64 -accel tcg,thread=multi -m 512 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=build/uefi-gpt-vars.fd \
    -cdrom build/uefi-gpt.iso -drive file=build/uefi-gpt.img,format=raw \
    -boot order=d -display none -serial "file:$log" -monitor none -no-reboot || true
grep -q 'UEFI-GPT: partition install PASS' "$log"
python3 - <<'PY'
from pathlib import Path
import hashlib

image = Path('build/uefi-gpt.img')
start, size = 479232 * 512, 65536 * 512
expected = Path('build/uefi-gpt-data.sha256').read_text()
hasher = hashlib.sha256()
with image.open('rb') as disk:
    disk.seek(start)
    remaining = size
    while remaining:
        chunk = disk.read(min(1024 * 1024, remaining))
        if not chunk:
            raise SystemExit('GPT data partition truncated')
        hasher.update(chunk)
        remaining -= len(chunk)
if hasher.hexdigest() != expected:
    raise SystemExit('GPT data partition changed during install')
print('UEFI-GPT: data partition preserved PASS')
PY

cp /usr/share/OVMF/OVMF_VARS_4M.fd build/uefi-gpt-installed-vars.fd
log=build/uefi-gpt-boot.log
: > "$log"
timeout 120 qemu-system-x86_64 -accel tcg,thread=multi -m 512 \
    -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
    -drive if=pflash,format=raw,file=build/uefi-gpt-installed-vars.fd \
    -drive file=build/uefi-gpt.img,format=raw -boot order=c \
    -display none -serial "file:$log" -monitor none -no-reboot || true
grep -q 'UEFI-GPT: installed root preflight PASS' "$log"
grep -q 'OPOS: verified disk system root selected' "$log"
cat "$log" | tail -n 16
