"""Boot a disposable image whose previous Limine entry is first."""

from pathlib import Path
import subprocess
import sys
import time

image = Path(sys.argv[1])
log = Path(sys.argv[2])
log.write_bytes(b"")
vm = subprocess.Popen([
    "qemu-system-x86_64", "-accel", "tcg,thread=multi", "-m", "256",
    "-drive", f"file={image},format=raw,if=ide,index=0",
    "-boot", "order=c", "-display", "none",
    "-serial", f"file:{log}",
    "-monitor", "none", "-no-reboot",
])
try:
    deadline = time.monotonic() + 240
    while time.monotonic() < deadline:
        output = log.read_text(errors="replace")
        if "UPDATE-ROOT: old generation resumed PASS" in output:
            print("UPDATE-ROOT: previous menu entry booted old generation PASS")
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
