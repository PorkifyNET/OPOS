"""Set the default OPOS entry in the GPT system partition's Limine menu."""

from pathlib import Path
import struct
import sys

image = Path(sys.argv[1])
choice = sys.argv[2]
assert choice in ("previous", "current")

with image.open("r+b") as disk:
    # Partition 2 is the OPOS system volume; it holds the active UEFI menu.
    disk.seek(512 + 72)
    entries_lba = struct.unpack("<Q", disk.read(8))[0]
    entry_count, entry_size = struct.unpack("<II", disk.read(8))
    assert entry_count >= 2 and entry_size >= 128
    disk.seek(entries_lba * 512 + entry_size + 32)
    volume_start = struct.unpack("<Q", disk.read(8))[0] * 512

    disk.seek(volume_start + 11)
    sector = struct.unpack("<H", disk.read(2))[0]
    cluster_sectors = disk.read(1)[0]
    reserved = struct.unpack("<H", disk.read(2))[0]
    fats = disk.read(1)[0]
    root_count = struct.unpack("<H", disk.read(2))[0]
    disk.seek(volume_start + 22)
    fat_sectors = struct.unpack("<H", disk.read(2))[0]
    root_start = volume_start + (reserved + fats * fat_sectors) * sector
    data_start = root_start + root_count * 32
    cluster_size = cluster_sectors * sector

    def directory(cluster):
        if cluster == 0:
            disk.seek(root_start)
            base, raw = root_start, disk.read(root_count * 32)
        else:
            base = data_start + (cluster - 2) * cluster_size
            disk.seek(base)
            raw = disk.read(cluster_size)
        result = {}
        for at in range(0, len(raw), 32):
            entry = raw[at:at + 32]
            if not entry or entry[0] == 0:
                break
            if entry[0] == 0xE5 or entry[11] == 0x0F:
                continue
            name = entry[:8].decode("ascii").strip()
            ext = entry[8:11].decode("ascii").strip()
            result[name + ("." + ext if ext else "")] = (
                entry[11], struct.unpack_from("<H", entry, 26)[0],
                struct.unpack_from("<I", entry, 28)[0], base + at,
            )
        return result

    boot = directory(0)["BOOT"][1]
    limine = directory(boot)["LIMINE"][1]
    files = directory(limine)
    matches = [value for name, value in files.items()
               if name.startswith("LIMINE") and name.endswith(".CON")]
    if not matches:
        raise SystemExit(f"Limine directory has no config: {files!r}")
    _, cluster, size, entry_offset = matches[0]
    offset = data_start + (cluster - 2) * cluster_size
    disk.seek(offset)
    text = disk.read(size)

    positions = []
    at = 0
    for line in text.splitlines(keepends=True):
        if line.startswith(b"/OPOS"):
            positions.append(at)
        at += len(line)
    assert len(positions) == 2, f"expected two OPOS entries: {positions!r}"
    first, second = positions
    prefix, entry_a, entry_b = text[:first], text[first:second], text[second:]
    if entry_a.startswith(b"/OPOS (previous version)"):
        previous, current = entry_a, entry_b
    else:
        current, previous = entry_a, entry_b
    assert b"boot():/boot/kernel.elf" in current + previous
    assert b"boot():/boot/old/kernel.elf" in current + previous
    if choice == "previous":
        desired, default = previous + current, 1
    else:
        desired, default = current + previous, 1
    output = prefix + desired
    output = output.replace(b"timeout: 3", b"timeout: 9", 1)
    separator = b"\r\n\r\n" if b"\r\n\r\n" in output else b"\n\n"
    default_line = b"default_entry: " + str(default).encode("ascii")
    output = output.replace(separator,
                            separator[:len(separator) // 2]
                            + default_line + separator, 1)
    assert len(output) <= cluster_size, "menu exceeds its allocated cluster"
    disk.seek(offset)
    disk.write(output)
    disk.seek(entry_offset + 28)
    disk.write(struct.pack("<I", len(output)))
    print(f"UPDATE-ROOT: UEFI {choice} default entry set")
