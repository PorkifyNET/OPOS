#!/usr/bin/env python3
"""Tests for mkopk.py: a good package builds and checks, and every reject
rule in docs/packages.md rejects. Run: python3 sdk/tools/test_mkopk.py"""
import hashlib
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkopk  # noqa: E402


def elf(notes=b"", machine=62):
    """A minimal ELF64 image with one PT_NOTE segment holding notes."""
    header = bytearray(64)
    header[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", header, 18, machine)
    struct.pack_into("<Q", header, 32, 64)        # program header table
    struct.pack_into("<HH", header, 54, 56, 1)    # entry size, count
    program = bytearray(56)
    struct.pack_into("<I", program, 0, 4)         # PT_NOTE
    struct.pack_into("<Q", program, 8, 120)       # offset
    struct.pack_into("<Q", program, 32, len(notes))
    return bytes(header + program + notes)


def note(kind, descriptor, owner=b"OPOS\0"):
    pad = lambda b: b + b"\0" * (-len(b) % 4)  # noqa: E731
    return (struct.pack("<III", len(owner), len(descriptor), kind)
            + pad(owner) + pad(descriptor))


def manifest(name=b"Paint", version=b"1.0", caps=0, fmt=1):
    return note(2, struct.pack("<II", fmt, caps)
                + name.ljust(32, b"\0") + version.ljust(16, b"\0"))


GOOD_PROGRAM = elf(note(1, struct.pack("<I", 1)) + manifest())

TEMPLATE = """format: 1
id: org.example.paint
name: Paint
version: 1.2.0
publisher: Example Ltd
platform: opos
requires-opos: 0.1.5
file: files/paint paint.elf exec
file: files/data/brushes.dat brushes.dat
start: files/paint Paint
"""


class Fixture(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.dir = self.folder.name
        self.write("paint.elf", GOOD_PROGRAM)
        self.write("brushes.dat", b"brushes\n" * 100)

    def tearDown(self):
        self.folder.cleanup()

    def write(self, name, data):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as handle:
            handle.write(data)
        return path

    def build(self, template=TEMPLATE):
        path = self.write("PACKAGE.in", template.encode())
        return mkopk.build(path, self.dir)

    def build_rejects(self, template, fragment):
        with self.assertRaises(mkopk.PackageError) as caught:
            self.build(template)
        self.assertIn(fragment, str(caught.exception))

    def archive(self, package_text, members):
        out = os.path.join(self.dir, "hand.opk")
        mkopk.write_package(package_text, members, out)
        with open(out, "rb") as handle:
            return handle.read()

    def check_rejects(self, archive, fragment):
        with self.assertRaises(mkopk.PackageError) as caught:
            mkopk.check_archive(archive)
        self.assertIn(fragment, str(caught.exception))


def package_line(path, data, executable=False):
    return (f"file: {path} {hashlib.sha256(data).hexdigest()} {len(data)}"
            + (" exec" if executable else ""))


HEAD = """format: 1
id: org.example.paint
name: Paint
version: 1.2.0
publisher: Example Ltd
platform: opos
"""


class BuildTests(Fixture):
    def test_good_package_builds_and_checks(self):
        out = self.build()
        self.assertTrue(out.endswith("org.example.paint-1.2.0.opk"))
        with open(out, "rb") as handle:
            data = handle.read()
        package = mkopk.check_archive(data)
        self.assertEqual(package["id"], "org.example.paint")
        self.assertEqual([f["path"] for f in package["files"]],
                         ["files/paint", "files/data/brushes.dat"])
        self.assertEqual(package["start"], [("files/paint", "Paint")])
        self.assertIn("requires OPOS 0.1.5", mkopk.summary(package))

    def test_build_is_reproducible(self):
        with open(self.build(), "rb") as handle:
            first = handle.read()
        with open(self.build(), "rb") as handle:
            self.assertEqual(first, handle.read())

    def test_package_member_is_first(self):
        with open(self.build(), "rb") as handle:
            members = mkopk.read_members(handle.read())
        self.assertEqual(members[0][0], "PACKAGE")
        self.assertNotIn("paint.elf", members[0][1].decode())

    def test_long_path_uses_ustar_prefix(self):
        long_path = "files/" + "/".join(["folder"] * 18) + "/x.dat"
        template = TEMPLATE.replace("files/data/brushes.dat", long_path)
        with open(self.build(template), "rb") as handle:
            members = mkopk.read_members(handle.read())
        self.assertIn(long_path, [name for name, _ in members])

    def test_linux_package(self):
        self.write("tool", elf(machine=62))
        self.build(HEAD.replace("opos", "linux")
                   + "capabilities: net, clipboard.read\n"
                   + "file: files/tool tool exec\n")

    def test_missing_source(self):
        self.build_rejects(TEMPLATE.replace("paint.elf", "nothing"),
                           "cannot read")


class RejectTests(Fixture):
    """Each rule in PACKAGE parsing."""
    def test_unknown_key_needs_newer_opos(self):
        self.build_rejects(TEMPLATE + "needs: org.example.runtime\n",
                           "needs a newer OPOS")

    def test_unknown_format(self):
        self.build_rejects(TEMPLATE.replace("format: 1", "format: 2"),
                           "needs a newer OPOS")

    def test_required_keys(self):
        for key in ("format", "id", "name", "version", "publisher",
                    "platform"):
            lines = [l for l in TEMPLATE.split("\n")
                     if not l.startswith(key + ":")]
            self.build_rejects("\n".join(lines), f"'{key}' is missing")

    def test_key_twice(self):
        self.build_rejects(TEMPLATE + "name: Other\n", "given twice")

    def test_bad_line(self):
        self.build_rejects(TEMPLATE + "nonsense\n", "expected 'key: value'")
        self.build_rejects(TEMPLATE + " name: x\n", "leading or trailing")

    def test_bad_ids(self):
        for bad in ("paint", "Org.example.paint", "org..paint", "org.-x",
                    "org.example.paint_2", "org.example." + "a" * 60):
            self.build_rejects(TEMPLATE.replace("org.example.paint", bad),
                               "id:")

    def test_bad_versions(self):
        for bad in ("1.2.3.4.5", "1.x", "v1", "1..2", "123456"):
            self.build_rejects(TEMPLATE.replace("1.2.0", bad), "version")
        self.build_rejects(TEMPLATE.replace("version: 1.2.0", "version:"),
                           "expected 'key: value'")
        self.build_rejects(TEMPLATE.replace("0.1.5", "next"),
                           "requires-opos")

    def test_long_name_and_publisher(self):
        self.build_rejects(TEMPLATE.replace("name: Paint", "name: " + "n" * 32),
                           "name:")
        self.build_rejects(TEMPLATE.replace("Example Ltd", "p" * 64),
                           "publisher:")

    def test_non_ascii(self):
        self.build_rejects(TEMPLATE.replace("Example Ltd", "Exämple"),
                           "printable ASCII")

    def test_unknown_platform(self):
        self.build_rejects(TEMPLATE.replace("platform: opos",
                                            "platform: macos"), "platform:")

    def test_capabilities_line(self):
        self.build_rejects(TEMPLATE + "capabilities: net\n", "OPOS_MANIFEST")
        self.write("tool", elf())
        self.build_rejects(HEAD.replace("opos", "linux")
                           + "capabilities: camera\nfile: files/tool tool\n",
                           "unknown capability")

    def test_bad_paths(self):
        for bad in ("paint", "/files/paint", "files/../paint", "files/./a",
                    "files//a", "files/", "other/paint", "files/a:b",
                    "files/a\\b", "files/a*", "files/name.",
                    "files/" + "a" * 200):
            self.build_rejects(TEMPLATE.replace("files/data/brushes.dat",
                                                bad), "path")

    def test_duplicate_paths_ignore_case(self):
        self.build_rejects(TEMPLATE + "file: files/PAINT paint.elf\n",
                           "listed twice")

    def test_file_and_folder_same_name(self):
        self.build_rejects(TEMPLATE + "file: files/data brushes.dat\n",
                           "both a file and a folder")

    def test_no_files(self):
        lines = [l for l in TEMPLATE.split("\n")
                 if not l.startswith(("file:", "start:"))]
        self.build_rejects("\n".join(lines), "no files")

    def test_too_many_files(self):
        extra = "".join(f"file: files/f{i} brushes.dat\n" for i in range(512))
        self.build_rejects(TEMPLATE + extra, "more than 512")

    def test_start_must_name_exec(self):
        self.build_rejects(TEMPLATE + "start: files/data/brushes.dat B\n",
                           "not a listed exec")
        self.build_rejects(TEMPLATE + "start: files/paint\n", "label")
        self.build_rejects(TEMPLATE + "start: files/paint " + "L" * 32 + "\n",
                           "label")

    def test_bad_file_line(self):
        self.build_rejects(TEMPLATE + "file: files/x\n", "SOURCE")

    def test_too_many_starts(self):
        self.build_rejects(TEMPLATE + "start: files/paint P\n" * 16,
                           "more than 16 start")

    def test_size_limits(self):
        digest = "0" * 64
        for size in ("4294967296", "00", "01", "12345678901"):
            text = HEAD + f"file: files/b.dat {digest} {size}\n"
            self.check_rejects(self.archive(text, []), "SHA256 SIZE")


class ExecutableTests(Fixture):
    """Executables must match the platform; opos ones need a manifest."""
    def reject_program(self, data, fragment="OPOS_MANIFEST"):
        self.write("paint.elf", data)
        self.build_rejects(TEMPLATE, fragment)

    def test_opos_without_manifest(self):
        self.reject_program(elf(note(1, struct.pack("<I", 1))))

    def test_opos_two_manifests(self):
        self.reject_program(elf(manifest() + manifest()))

    def test_opos_bad_manifests(self):
        self.reject_program(elf(manifest(name=b"")))
        self.reject_program(elf(manifest(name=b"Pa\x01nt")))
        self.reject_program(elf(manifest(fmt=2)))
        self.reject_program(elf(manifest(name=b"n" * 32)))
        self.reject_program(elf(manifest(version=b"v" * 16)))
        self.reject_program(elf(note(2, b"\0" * 52)))

    def test_opos_bad_bounds(self):
        good = elf(manifest())
        self.reject_program(good[:-8])           # note runs past the file
        self.reject_program(b"MZ" + good[2:])     # not ELF
        self.reject_program(good[:64])            # table past the end

    def test_manifest_values(self):
        data = elf(manifest(caps=0xFFFFFFFF, version=b""))
        self.assertEqual(mkopk.read_manifest_note(data), (0x7F, "Paint", ""))

    def test_linux_needs_x86_64_elf(self):
        self.write("tool", elf(machine=183))      # AArch64
        self.build_rejects(HEAD.replace("opos", "linux")
                           + "file: files/tool tool exec\n", "x86-64")

    def test_windows_needs_mz(self):
        self.write("tool.exe", b"PE but not MZ")
        self.build_rejects(HEAD.replace("opos", "windows")
                           + "file: files/tool.exe tool.exe exec\n",
                           "not a Windows")
        self.write("tool.exe", b"MZ" + b"\0" * 62)
        self.build(HEAD.replace("opos", "windows")
                   + "file: files/tool.exe tool.exe exec\n")


class ArchiveTests(Fixture):
    """Archives built by hand, as a hostile or broken tool would."""
    def setUp(self):
        super().setUp()
        self.data = b"brushes"
        self.text = HEAD + package_line("files/b.dat", self.data) + "\n"

    def test_hand_built_good(self):
        mkopk.check_archive(self.archive(self.text,
                                         [("files/b.dat", self.data, False)]))

    def test_package_not_first(self):
        archive = bytearray(self.archive(self.text,
                                         [("files/b.dat", self.data, False)]))
        first, second = archive[:1024], archive[1024:2048]
        self.check_rejects(bytes(second + first + archive[2048:]),
                           "must be the first")

    def test_unlisted_member(self):
        self.check_rejects(self.archive(self.text, [
            ("files/b.dat", self.data, False),
            ("files/extra", b"x", False)]), "not listed")

    def test_missing_member(self):
        self.check_rejects(self.archive(self.text, []), "missing from")

    def test_member_twice(self):
        self.check_rejects(self.archive(self.text, [
            ("files/b.dat", self.data, False),
            ("files/b.dat", self.data, False)]), "twice")

    def test_package_twice(self):
        self.check_rejects(self.archive(self.text, [
            ("files/b.dat", self.data, False),
            ("PACKAGE", b"x", False)]), "PACKAGE appears twice")

    def test_wrong_hash_and_size(self):
        self.check_rejects(self.archive(self.text,
                                        [("files/b.dat", b"brushez", False)]),
                           "SHA-256")
        self.check_rejects(self.archive(self.text,
                                        [("files/b.dat", b"brush", False)]),
                           "size")

    def test_bad_hash_field(self):
        text = HEAD + "file: files/b.dat " + "A" * 64 + " 7\n"
        self.check_rejects(self.archive(text,
                                        [("files/b.dat", self.data, False)]),
                           "SHA256 SIZE")

    def test_large_package_member(self):
        text = self.text + "\n" * (mkopk.PACKAGE_MAX + 1)
        self.check_rejects(self.archive(text,
                                        [("files/b.dat", self.data, False)]),
                           "larger than")

    def replace_kind(self, kind, fragment):
        archive = bytearray(self.archive(self.text,
                                         [("files/b.dat", self.data, False)]))
        header = 1024  # PACKAGE header + one data block
        archive[header + 156] = ord(kind)
        archive[header + 148:header + 156] = b"        "
        archive[header + 148:header + 156] = (
            b"%06o\0 " % sum(archive[header:header + 512]))
        self.check_rejects(bytes(archive), fragment)

    def test_links_and_folders(self):
        self.replace_kind("2", "symbolic link")
        self.replace_kind("1", "hard link")
        self.replace_kind("5", "folder")
        self.replace_kind("3", "special file")

    def test_bad_checksum(self):
        archive = bytearray(self.archive(self.text,
                                         [("files/b.dat", self.data, False)]))
        archive[0] ^= 1
        self.check_rejects(bytes(archive), "checksum")

    def test_not_ustar(self):
        archive = bytearray(self.archive(self.text,
                                         [("files/b.dat", self.data, False)]))
        archive[257:263] = b"ustar "
        self.check_rejects(bytes(archive), "not a POSIX ustar")

    def test_truncated(self):
        archive = self.archive(self.text, [("files/b.dat", self.data, False)])
        self.check_rejects(archive[:-1024], "end blocks")
        self.check_rejects(archive[:1024 + 100], "end blocks")
        self.check_rejects(archive[:-512], "one empty block")

    def test_trailing_data(self):
        archive = self.archive(self.text, [("files/b.dat", self.data, False)])
        self.check_rejects(archive + b"x", "after the end")


if __name__ == "__main__":
    unittest.main()
