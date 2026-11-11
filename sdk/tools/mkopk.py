#!/usr/bin/env python3
"""Builds and checks OPOS packages (.opk). See docs/packages.md.

    mkopk.py build PACKAGE.in [-o OUT]   write a package
    mkopk.py check FILE.opk              check a package, print a summary

A package is an uncompressed ustar archive. Its first member is PACKAGE,
a text file of "key: value" lines; every other member is a file below
files/ that PACKAGE lists with its SHA-256 and size.

PACKAGE.in is PACKAGE without hashes and sizes. Its file lines name the
file in the package, the file to copy (relative to PACKAGE.in), and
"exec" for an executable:

    format: 1
    id: org.example.paint
    name: Paint
    version: 1.2.0
    publisher: Example Ltd
    platform: opos
    requires-opos: 0.1.5
    file: files/paint build/paint exec
    file: files/brushes.dat data/brushes.dat
    start: files/paint Paint

OUT is a file name, or a directory that receives <id>-<version>.opk
(default: the current directory). The archive is the same for the same
input: times, owners and modes are fixed.

The checks here are the rules the OPOS installer applies. A package this
tool accepts must install; one it rejects must not. Only the Python
standard library is used, so this works from a copy of sdk/ alone.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

BLOCK = 512
PACKAGE_MAX = 16 * 1024
FILES_MAX = 512
PATH_MAX = 200
NAME_MAX = 31
PUBLISHER_MAX = 63
ID_MAX = 63
VERSION_MAX = 15

PLATFORMS = ("opos", "linux", "windows")
# Capabilities a program can ask for beyond the defaults
# (docs/app-sandbox.md). Bits match PERMISSION_CAP_* in the kernel.
CAPABILITIES = {
    "files.shared": 1 << 2,
    "net": 1 << 3,
    "clipboard.read": 1 << 4,
}
CAPS_ALL = 0x7F

ID_PATTERN = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)+$")
VERSION_PATTERN = re.compile(r"^[0-9]{1,5}(?:\.[0-9]{1,5}){0,3}$")
HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")
# Characters FAT does not allow in a name, plus space (lines are split on
# spaces) and backslash.
BAD_PATH_CHARACTERS = set('"*:<>?\\| ')
KEYS = ("format", "id", "name", "version", "publisher", "platform",
        "requires-opos", "capabilities", "file", "start")
SINGLE_KEYS = ("format", "id", "name", "version", "publisher", "platform",
               "requires-opos", "capabilities")
REQUIRED_KEYS = ("format", "id", "name", "version", "publisher", "platform")


class PackageError(Exception):
    pass


def fail(message):
    raise PackageError(message)


def printable(text):
    return all(0x20 <= ord(c) < 0x7F for c in text)


def check_path(path):
    """A file name inside the package: files/ followed by safe parts."""
    if len(path) > PATH_MAX:
        fail(f"{path[:40]}...: path longer than {PATH_MAX} characters")
    if not printable(path) or any(c in BAD_PATH_CHARACTERS for c in path):
        fail(f"{path}: path has a character OPOS cannot store")
    parts = path.split("/")
    if parts[0] != "files" or len(parts) < 2:
        fail(f"{path}: path must start with files/")
    for part in parts[1:]:
        if part in ("", ".", ".."):
            fail(f"{path}: empty, '.' or '..' path part")
        if part.endswith(".") or part.endswith(" "):
            fail(f"{path}: path part ends with '.'")


def check_version(text, what):
    if len(text) > VERSION_MAX or not VERSION_PATTERN.match(text):
        fail(f"{what}: '{text}' is not 1 to 4 numbers separated by dots")


def parse_capabilities(text):
    bits = 0
    names = [n for n in re.split(r"[ ,]+", text.strip()) if n]
    for name in names:
        if name not in CAPABILITIES:
            fail(f"capabilities: unknown capability '{name}'")
        bits |= CAPABILITIES[name]
    return bits


def parse_package(text, template):
    """Parses PACKAGE (template False) or PACKAGE.in (template True).

    Returns a dict: the single keys, "files" (list of dicts with path,
    exec and either sha256/size or source) and "start" (list of
    (path, label)).
    """
    if not printable(text.replace("\n", "")):
        fail("PACKAGE: only printable ASCII is allowed")
    package = {"files": [], "start": []}
    seen_paths = set()
    for number, line in enumerate(text.split("\n"), 1):
        if line.strip() == "":
            continue
        if line != line.strip():
            fail(f"PACKAGE line {number}: leading or trailing space")
        key, separator, value = line.partition(": ")
        if not separator or not value:
            fail(f"PACKAGE line {number}: expected 'key: value'")
        if key not in KEYS:
            fail(f"PACKAGE line {number}: unknown key '{key}' "
                 "(needs a newer OPOS)")
        if key in SINGLE_KEYS:
            if key in package:
                fail(f"PACKAGE line {number}: '{key}' given twice")
            package[key] = value
        elif key == "file":
            fields = value.split(" ")
            entry = {"path": fields[0], "exec": False}
            rest = fields[1:]
            if rest and rest[-1] == "exec":
                entry["exec"] = True
                rest = rest[:-1]
            if template:
                if len(rest) != 1:
                    fail(f"PACKAGE line {number}: expected "
                         "'file: files/NAME SOURCE [exec]'")
                entry["source"] = rest[0]
            else:
                if (len(rest) != 2 or not HASH_PATTERN.match(rest[0])
                        or not rest[1].isdigit()
                        or (len(rest[1]) > 1 and rest[1][0] == "0")):
                    fail(f"PACKAGE line {number}: expected "
                         "'file: files/NAME SHA256 SIZE [exec]'")
                entry["sha256"] = rest[0]
                entry["size"] = int(rest[1])
            check_path(entry["path"])
            folded = entry["path"].lower()
            if folded in seen_paths:
                fail(f"{entry['path']}: listed twice (names ignore case)")
            seen_paths.add(folded)
            package["files"].append(entry)
        else:  # start
            path, _, label = value.partition(" ")
            if not label or len(label) > NAME_MAX:
                fail(f"PACKAGE line {number}: start needs a label of 1 "
                     f"to {NAME_MAX} characters")
            package["start"].append((path, label))

    for key in REQUIRED_KEYS:
        if key not in package:
            fail(f"PACKAGE: '{key}' is missing")
    if package["format"] != "1":
        fail(f"format: {package['format']} is not supported "
             "(needs a newer OPOS)")
    if len(package["id"]) > ID_MAX or not ID_PATTERN.match(package["id"]):
        fail(f"id: '{package['id']}' must look like org.example.name "
             f"(a-z, 0-9, '-', '.', at most {ID_MAX})")
    if len(package["name"]) > NAME_MAX:
        fail(f"name: longer than {NAME_MAX} characters")
    if len(package["publisher"]) > PUBLISHER_MAX:
        fail(f"publisher: longer than {PUBLISHER_MAX} characters")
    check_version(package["version"], "version")
    if "requires-opos" in package:
        check_version(package["requires-opos"], "requires-opos")
    if package["platform"] not in PLATFORMS:
        fail(f"platform: '{package['platform']}' is not one of "
             + ", ".join(PLATFORMS))
    if package["platform"] == "opos":
        if "capabilities" in package:
            fail("capabilities: an opos package declares them with "
                 "OPOS_MANIFEST in each program, not here")
    else:
        package["capability_bits"] = parse_capabilities(
            package.get("capabilities", ""))
    if not package["files"]:
        fail("PACKAGE: no files")
    if len(package["files"]) > FILES_MAX:
        fail(f"PACKAGE: more than {FILES_MAX} files")
    # A file and a folder of the same name cannot both exist.
    for folded in seen_paths:
        parent = folded.rsplit("/", 1)[0]
        while parent != "files":
            if parent in seen_paths:
                fail(f"{parent}: both a file and a folder")
            parent = parent.rsplit("/", 1)[0]
    executables = {f["path"] for f in package["files"] if f["exec"]}
    for path, label in package["start"]:
        if path not in executables:
            fail(f"start: {path} is not a listed exec file")
    return package


def read_manifest_note(image):
    """Mirrors manifest_read() in kernel/manifest.c. Returns
    (capabilities, name, version) or None."""
    def u16(at):
        return struct.unpack_from("<H", image, at)[0]

    def u32(at):
        return struct.unpack_from("<I", image, at)[0]

    def u64(at):
        return struct.unpack_from("<Q", image, at)[0]

    def text(field, may_be_empty):
        end = field.find(b"\0")
        if end < 0 or (end == 0 and not may_be_empty):
            return None
        if any(b < 0x20 or b == 0x7F for b in field[:end]):
            return None
        return field[:end].decode("latin-1")

    size = len(image)
    if size < 64 or image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        return None
    table, entry_size, count = u64(32), u16(54), u16(56)
    if entry_size < 56 or table > size or entry_size * count > size - table:
        return None
    found = []
    for i in range(count):
        header = table + i * entry_size
        if u32(header) != 4:  # PT_NOTE
            continue
        start, length = u64(header + 8), u64(header + 32)
        if start > size or length > size - start:
            return None
        offset = 0
        while offset < length:
            if length - offset < 12:
                return None
            note = start + offset
            name_size, descriptor_size, kind = (
                u32(note), u32(note + 4), u32(note + 8))
            name_space = (name_size + 3) & ~3
            descriptor_space = (descriptor_size + 3) & ~3
            remaining = length - offset - 12
            if name_space > remaining or descriptor_space > remaining - name_space:
                return None
            name = image[note + 12:note + 12 + name_size]
            descriptor = image[note + 12 + name_space:
                               note + 12 + name_space + descriptor_size]
            if name == b"OPOS\0" and kind == 2:
                if (descriptor_size != 56 or u32(note + 12 + name_space) != 1
                        or found):
                    return None
                program_name = text(descriptor[8:40], False)
                version = text(descriptor[40:56], True)
                if program_name is None or version is None:
                    return None
                found.append((struct.unpack_from("<I", descriptor, 4)[0]
                              & CAPS_ALL, program_name, version))
            offset += 12 + name_space + descriptor_space
    return found[0] if len(found) == 1 else None


def check_executable(platform, path, data):
    if platform == "opos":
        if read_manifest_note(data) is None:
            fail(f"{path}: an opos program needs one valid OPOS_MANIFEST "
                 "note (see <opos.h>)")
    elif platform == "linux":
        if (len(data) < 20 or data[:4] != b"\x7fELF" or data[4] != 2
                or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 62):
            fail(f"{path}: not a 64-bit x86-64 ELF executable")
    elif data[:2] != b"MZ":
        fail(f"{path}: not a Windows executable")


# ------------------------------------------------------------ ustar

def ustar_header(name, size, mode, kind):
    if len(name) <= 100:
        prefix, short = "", name
    else:
        cut = name.rfind("/", 0, 156)
        if cut < 0 or len(name) - cut - 1 > 100:
            fail(f"{name}: path does not fit a ustar header")
        prefix, short = name[:cut], name[cut + 1:]
    header = bytearray(BLOCK)

    def put(offset, width, value):
        header[offset:offset + len(value)] = value

    put(0, 100, short.encode())
    put(100, 8, b"%07o\0" % mode)
    put(108, 8, b"0000000\0")
    put(116, 8, b"0000000\0")
    put(124, 12, b"%011o\0" % size)
    put(136, 12, b"00000000000\0")
    put(148, 8, b"        ")
    header[156] = ord(kind)
    put(257, 6, b"ustar\0")
    put(263, 2, b"00")
    put(345, 155, prefix.encode())
    checksum = sum(header)
    put(148, 8, b"%06o\0 " % checksum)
    return bytes(header)


def write_package(package_text, members, out):
    """members: list of (path, data, exec). PACKAGE goes first."""
    parts = []

    def add(name, data, mode, kind):
        parts.append(ustar_header(name, len(data), mode, kind))
        parts.append(data)
        parts.append(b"\0" * (-len(data) % BLOCK))

    add("PACKAGE", package_text.encode(), 0o644, "0")
    for path, data, executable in members:
        add(path, data, 0o755 if executable else 0o644, "0")
    parts.append(b"\0" * (2 * BLOCK))
    with open(out, "wb") as handle:
        handle.write(b"".join(parts))


def read_members(archive):
    """Strict ustar reader. Returns [(name, data)] in archive order."""
    members = []
    offset = 0
    while True:
        if len(archive) - offset < BLOCK:
            fail("archive: ends without the two empty end blocks")
        header = archive[offset:offset + BLOCK]
        if header == b"\0" * BLOCK:
            rest = archive[offset + BLOCK:offset + 2 * BLOCK]
            if rest != b"\0" * BLOCK:
                fail("archive: one empty block where two are needed")
            if any(archive[offset + 2 * BLOCK:]):
                fail("archive: data after the end blocks")
            return members
        if header[257:263] != b"ustar\0" or header[263:265] != b"00":
            fail("archive: not a POSIX ustar archive")
        stored = header[148:156].split(b"\0")[0].strip()
        expected = sum(header[:148]) + 8 * 32 + sum(header[156:])
        try:
            if int(stored, 8) != expected:
                fail("archive: header checksum is wrong")
            size = int(header[124:136].split(b"\0")[0].strip() or b"0", 8)
        except ValueError:
            fail("archive: header field is not octal")
        kind = chr(header[156])
        name = header[:100].split(b"\0")[0]
        prefix = header[345:500].split(b"\0")[0]
        try:
            name = ((prefix + b"/" if prefix else b"") + name).decode("ascii")
        except UnicodeDecodeError:
            fail("archive: member name is not ASCII")
        if kind not in ("0", "\0"):
            what = {"1": "hard link", "2": "symbolic link", "5": "folder"}
            fail(f"{name}: {what.get(kind, 'special file')} entries are "
                 "not allowed")
        start = offset + BLOCK
        if size > len(archive) - start:
            fail(f"{name}: archive is cut short")
        members.append((name, archive[start:start + size]))
        offset = start + size + (-size % BLOCK)


def check_archive(archive):
    """Checks a whole .opk. Returns the parsed PACKAGE."""
    members = read_members(archive)
    if not members or members[0][0] != "PACKAGE":
        fail("archive: PACKAGE must be the first member")
    if len(members[0][1]) > PACKAGE_MAX:
        fail(f"PACKAGE: larger than {PACKAGE_MAX} bytes")
    try:
        text = members[0][1].decode("ascii")
    except UnicodeDecodeError:
        fail("PACKAGE: only printable ASCII is allowed")
    package = parse_package(text, template=False)
    listed = {f["path"]: f for f in package["files"]}
    seen = set()
    for name, data in members[1:]:
        if name == "PACKAGE":
            fail("archive: PACKAGE appears twice")
        if name not in listed:
            fail(f"{name}: in the archive but not listed in PACKAGE")
        if name in seen:
            fail(f"{name}: appears twice in the archive")
        seen.add(name)
        entry = listed[name]
        if len(data) != entry["size"]:
            fail(f"{name}: size {len(data)}, PACKAGE says {entry['size']}")
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            fail(f"{name}: SHA-256 does not match PACKAGE")
        if entry["exec"]:
            check_executable(package["platform"], name, data)
    for path in listed:
        if path not in seen:
            fail(f"{path}: listed in PACKAGE but missing from the archive")
    return package


# ------------------------------------------------------------ commands

def build(template_path, out):
    with open(template_path, encoding="utf-8", errors="replace") as handle:
        template = handle.read()
    package = parse_package(template, template=True)
    base = os.path.dirname(os.path.abspath(template_path))
    members = []
    lines = []
    files = iter(package["files"])
    for line in template.split("\n"):
        if not line.startswith("file: "):
            lines.append(line)
            continue
        entry = next(files)
        source = os.path.join(base, entry["source"])
        try:
            with open(source, "rb") as handle:
                data = handle.read()
        except OSError as error:
            fail(f"{entry['path']}: cannot read {source}: {error.strerror}")
        if entry["exec"]:
            check_executable(package["platform"], entry["path"], data)
        digest = hashlib.sha256(data).hexdigest()
        lines.append(f"file: {entry['path']} {digest} {len(data)}"
                     + (" exec" if entry["exec"] else ""))
        members.append((entry["path"], data, entry["exec"]))
    text = "\n".join(lines).strip("\n") + "\n"
    if len(text) > PACKAGE_MAX:
        fail(f"PACKAGE: larger than {PACKAGE_MAX} bytes")
    if out is None or os.path.isdir(out):
        out = os.path.join(out or ".",
                           f"{package['id']}-{package['version']}.opk")
    write_package(text, members, out)
    with open(out, "rb") as handle:
        check_archive(handle.read())
    return out


def summary(package):
    lines = [f"{package['name']} {package['version']} ({package['id']})",
             f"publisher: {package['publisher']}",
             f"platform: {package['platform']}"]
    if "requires-opos" in package:
        lines.append(f"requires OPOS {package['requires-opos']} or newer")
    total = sum(f["size"] for f in package["files"])
    lines.append(f"{len(package['files'])} files, {total} bytes")
    for path, label in package["start"]:
        lines.append(f"Start menu: {label} ({path})")
    return "\n".join(lines)


def main(argv):
    parser = argparse.ArgumentParser(
        description="Build or check an OPOS package (.opk).")
    commands = parser.add_subparsers(dest="command", required=True)
    build_parser = commands.add_parser("build", help="write a package")
    build_parser.add_argument("template", help="PACKAGE.in file")
    build_parser.add_argument("-o", "--output",
                              help="output file or directory")
    check_parser = commands.add_parser("check", help="check a package")
    check_parser.add_argument("package", help=".opk file")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "build":
            print(build(arguments.template, arguments.output))
        else:
            with open(arguments.package, "rb") as handle:
                print(summary(check_archive(handle.read())))
    except PackageError as error:
        print(f"mkopk: {error}", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"mkopk: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
