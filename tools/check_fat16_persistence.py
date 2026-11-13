#!/usr/bin/env python3
"""Check the FAT16 test disk after tools/test_fat16_persistence.sh ran.

The checks find files by their short names, so they still hold when OPOS
adds long-file-name entries or moves directory entries around. They check
names, sizes, contents and the FAT itself: both FAT copies agree, every
chain ends, no cluster is in two chains, and no cluster is lost.

Usage: check_fat16_persistence.py DISK.img
"""
import struct
import sys

EOF_MIN = 0xFFF8
DELETED = 0xE5
ATTR_DIR = 0x10
ATTR_VOLUME = 0x08
ATTR_LFN = 0x0F
RING3_DATA = b"ring3-file-data"


class Fat16:
    def __init__(self, data):
        self.data = data
        (self.sector_size, self.cluster_sectors, self.reserved, self.fat_count,
         self.root_entries, total16, _media, self.fat_sectors) = struct.unpack_from(
            "<HBHBHHBH", data, 11)
        self.cluster_size = self.sector_size * self.cluster_sectors
        self.fat_start = self.reserved * self.sector_size
        self.root_start = self.fat_start + self.fat_count * self.fat_sectors * self.sector_size
        self.data_start = self.root_start + self.root_entries * 32
        total = total16 or struct.unpack_from("<I", data, 32)[0]
        data_bytes = total * self.sector_size - self.data_start
        self.cluster_count = data_bytes // self.cluster_size
        self.fat_bytes = self.fat_sectors * self.sector_size

    def fat(self, copy=0):
        start = self.fat_start + copy * self.fat_bytes
        return self.data[start:start + self.fat_bytes]

    def next_cluster(self, cluster):
        return struct.unpack_from("<H", self.data, self.fat_start + cluster * 2)[0]

    def chain(self, first):
        clusters = []
        cluster = first
        while 2 <= cluster < EOF_MIN:
            if cluster in clusters or cluster >= self.cluster_count + 2:
                raise AssertionError(f"bad chain from cluster {first}: {clusters + [cluster]}")
            clusters.append(cluster)
            cluster = self.next_cluster(cluster)
        if cluster < EOF_MIN:
            raise AssertionError(f"chain from cluster {first} ends in {cluster:#x}, not EOF")
        return clusters

    def cluster_bytes(self, cluster):
        offset = self.data_start + (cluster - 2) * self.cluster_size
        return self.data[offset:offset + self.cluster_size]

    def read(self, first, size):
        return b"".join(self.cluster_bytes(c) for c in self.chain(first))[:size]

    def entries(self, first_cluster):
        """Visible short entries of a directory; cluster 0 is the root."""
        if first_cluster == 0:
            raw = self.data[self.root_start:self.data_start]
        else:
            raw = b"".join(self.cluster_bytes(c) for c in self.chain(first_cluster))
        found = {}
        for offset in range(0, len(raw), 32):
            entry = raw[offset:offset + 32]
            if entry[0] == 0:
                break
            attr = entry[11]
            if entry[0] == DELETED or attr == ATTR_LFN or attr & ATTR_VOLUME:
                continue
            base = entry[0:8].decode("ascii").rstrip()
            ext = entry[8:11].decode("ascii").rstrip()
            name = f"{base}.{ext}" if ext else base
            cluster = struct.unpack_from("<H", entry, 26)[0]
            size = struct.unpack_from("<I", entry, 28)[0]
            if name in found:
                raise AssertionError(f"{name} appears twice in directory {first_cluster}")
            found[name] = (attr, cluster, size)
        return found


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def check_file(fs, entries, name, size, content):
    expect(name in entries, f"{name} is missing")
    attr, cluster, real_size = entries[name]
    expect(not attr & ATTR_DIR, f"{name} is a directory")
    expect(real_size == size, f"{name} is {real_size} bytes, expected {size}")
    if size:
        expect(fs.read(cluster, size) == content, f"{name} has the wrong contents")
        expected_clusters = (size + fs.cluster_size - 1) // fs.cluster_size
        expect(len(fs.chain(cluster)) == expected_clusters,
               f"{name} chain is not {expected_clusters} clusters")
    else:
        expect(cluster == 0, f"empty {name} still owns cluster {cluster}")


def check_dir(entries, name):
    expect(name in entries, f"{name} is missing")
    attr, cluster, _size = entries[name]
    expect(attr & ATTR_DIR, f"{name} is not a directory")
    return cluster


def check_dots(fs, cluster, parent):
    entries = fs.entries(cluster)
    expect(entries.get(".", (0, None))[1] == cluster, f"'.' of directory {cluster} is wrong")
    expect(entries.get("..", (0, None))[1] == parent, f"'..' of directory {cluster} is wrong")
    return entries


def main():
    with open(sys.argv[1], "rb") as image:
        fs = Fat16(image.read())

    expect(fs.fat(0) == fs.fat(1) if fs.fat_count > 1 else True, "the FAT copies differ")

    root = fs.entries(0)
    expect(set(root) == {"PERSIST.TXT", "DOCS", "CREATED.TXT", "TRUNC.TMP"},
           f"root holds {sorted(root)}")
    check_file(fs, root, "PERSIST.TXT", 513, b"b" * 513)
    check_file(fs, root, "CREATED.TXT", 100, b"a" * 100)
    check_file(fs, root, "TRUNC.TMP", 15, RING3_DATA)

    docs_cluster = check_dir(root, "DOCS")
    docs = check_dots(fs, docs_cluster, 0)
    growth = {f"F{i:02d}.TMP" for i in range(12)}
    expect(set(docs) == {".", "..", "USER.TXT", "AGAIN", "REUSE.TMP"} | growth,
           f"DOCS holds {sorted(docs)}")
    check_file(fs, docs, "USER.TXT", 15, RING3_DATA)
    check_file(fs, docs, "REUSE.TMP", 15, RING3_DATA)
    for name in growth:
        check_file(fs, docs, name, 0, b"")
    expect(len(fs.chain(docs_cluster)) * fs.cluster_size // 32 > 16,
           "DOCS did not grow past one 16-entry cluster")

    again_cluster = check_dir(docs, "AGAIN")
    again = check_dots(fs, again_cluster, docs_cluster)
    expect(set(again) == {".", ".."}, f"AGAIN holds {sorted(again)}")

    # Every allocated cluster belongs to exactly one chain.
    owners = {}
    heads = [root[n][1] for n in root if root[n][1]]
    heads += [docs[n][1] for n in docs if n not in (".", "..") and docs[n][1]]
    for head in heads:
        for cluster in fs.chain(head):
            expect(cluster not in owners, f"cluster {cluster} is in two chains")
            owners[cluster] = head
    for cluster in range(2, fs.cluster_count + 2):
        if fs.next_cluster(cluster) != 0:
            expect(cluster in owners, f"cluster {cluster} is allocated but in no file")

    print("FAT16 disk checks passed.")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(f"FAT16 disk check failed: {error}", file=sys.stderr)
        sys.exit(1)
