"""Boot the UEFI previous-version entry and require its probe marker."""

from pathlib import Path
import subprocess
import sys
import time

image, log, variables = map(Path, sys.argv[1:4])
log.write_bytes(b"")
vm = subprocess.Popen([
    "qemu-system-x86_64", "-accel", "tcg,thread=multi", "-m", "512",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,file={variables}",
    "-drive", f"file={image},format=raw", "-boot", "order=c",
    "-display", "none", "-serial", f"file:{log}",
    "-monitor", "none", "-no-reboot",
])
try:
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        output = log.read_text(errors="replace")
        if "UPDATE-ROOT: old generation resumed PASS" in output:
            print("UPDATE-ROOT: UEFI previous entry booted old generation PASS")
            break
        if "UPDATE-ROOT: FAIL" in output or "UPDATE-ROOT: new generation" in output:
            raise RuntimeError(output[-2500:])
        if vm.poll() is not None:
            raise RuntimeError(output[-2500:])
        time.sleep(0.2)
    else:
        raise RuntimeError(log.read_text(errors="replace")[-2500:])
finally:
    vm.terminate()
    try:
        vm.wait(timeout=5)
    except subprocess.TimeoutExpired:
        vm.kill()
        vm.wait()
