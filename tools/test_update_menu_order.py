"""Put the saved or current Limine entry first on a disposable FAT16 image."""

from pathlib import Path
import struct
import sys

image = Path(sys.argv[1])
choice = sys.argv[2]
assert choice in ("previous", "current")

with image.open("r+b") as disk:
    disk.seek(0x1BE + 8)
    partition = struct.unpack("<I", disk.read(4))[0] * 512
    disk.seek(partition + 11)
    sector = struct.unpack("<H", disk.read(2))[0]
    cluster_sectors = disk.read(1)[0]
    reserved = struct.unpack("<H", disk.read(2))[0]
    fats = disk.read(1)[0]
    root_count = struct.unpack("<H", disk.read(2))[0]
    disk.seek(partition + 22)
    fat_sectors = struct.unpack("<H", disk.read(2))[0]
    root = partition + (reserved + fats * fat_sectors) * sector
    data = root + ((root_count * 32 + sector - 1) // sector) * sector
    cluster_size = cluster_sectors * sector

    def entries(offset, size):
        disk.seek(offset)
        result = {}
        listing = disk.read(size)
        for at in range(0, len(listing), 32):
            entry = listing[at:at + 32]
            if not entry or entry[0] == 0:
                break
            if entry[0] == 0xE5 or entry[11] == 0x0F:
                continue
            name = entry[:8].decode("ascii").strip()
            ext = entry[8:11].decode("ascii").strip()
            result[name + ("." + ext if ext else "")] = (
                struct.unpack_from("<H", entry, 26)[0],
                struct.unpack_from("<I", entry, 28)[0],
            )
        return result

    def cluster_offset(cluster):
        assert cluster >= 2
        return data + (cluster - 2) * cluster_size

    boot_cluster, _ = entries(root, root_count * 32)["BOOT"]
    boot = entries(cluster_offset(boot_cluster), cluster_size)
    limine_cluster, _ = boot["LIMINE"]
    limine = entries(cluster_offset(limine_cluster), cluster_size)
    config = next(value for key, value in limine.items()
                  if key.startswith("LIMINE") and key.endswith(".CON"))
    config_cluster, size = config
    assert 0 < size <= cluster_size, "test menu spans multiple clusters"
    offset = cluster_offset(config_cluster)
    disk.seek(offset)
    text = disk.read(size)
    first = text.index(b"/OPOS")
    second = text.index(b"/OPOS", first + 5)
    prefix, a, b = text[:first], text[first:second], text[second:]
    assert b"boot():/boot/kernel.elf" in a + b
    assert b"boot():/boot/old/kernel.elf" in a + b
    want_previous = choice == "previous"
    if (b"/OPOS (previous version)" in a) != want_previous:
        text = prefix + b + a
        disk.seek(offset)
        disk.write(text)
    print(f"UPDATE-ROOT: {choice} menu entry selected for next boot")
