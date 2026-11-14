#!/usr/bin/env python3
"""Builds and signs the OPOS Store index (docs/packages.md, "Index").

    mkindex.py --output DIR [--key KEY] [--unsigned]
               [--catalog FILE] OPK_OR_FOLDER...

Every .opk named (or found in a named folder) is checked with
sdk/tools/mkopk.py, copied to DIR as <id>-<version>.opk, and listed in
DIR/index. Unless --unsigned, DIR/index.sig is the ECDSA P-256 signature
of the index made with KEY (default ~/.opos-signing/store-key.pem). The
key must match kernel/store_key.h, or installs would refuse the index.

The repository release then holds index, index.sig and the .opk files
(docs/packages.md, "Repository").

--catalog names the Store catalog (store/catalog.txt): for each package
id, a "[id]" line and then category, license, featured, summary and
description lines for the Store window.

The index format, which kernel/package.c checks the same way
(parse_index below is its twin; tools/test_package_parity.py compares
them):

    format: 1
    entry: <id> <version>
    package: <SHA-256 of the PACKAGE member>
    file: <download name> <size> <SHA-256 of the .opk>
    name: ...
    publisher: ...
    platform: opos
    requires-opos: 0.1.4
    category: games
    license: GPL-2.0
    featured: yes
    summary: ...
    description: ...

The kernel reads only "format", "entry" and "package". The other lines
are for /bin/pkg and the Store window.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk", "tools"))
import mkopk  # noqa: E402

KEY_HEADER = os.path.join(ROOT, "kernel", "store_key.h")
INDEX_MAX = 262144
LINE_MAX = 1024
KEY_PATTERN = re.compile(r"^[a-z][a-z-]*$")
HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")


class IndexError_(Exception):
    pass


class UnsupportedIndex(IndexError_):
    pass


def version_key(version):
    parts = [int(part) for part in version.split(".")]
    return parts + [0] * (4 - len(parts))


def parse_index(data):
    """Checks an index (bytes). Returns {(id, version): package hash}.

    Raises UnsupportedIndex for a newer format and IndexError_ otherwise.
    """
    if len(data) > INDEX_MAX:
        raise IndexError_("index: larger than 262144 bytes")
    if not data.endswith(b"\n"):
        raise IndexError_("index: must end with a line break")
    if not data.startswith(b"format: 1\n"):
        if data.startswith(b"format: "):
            raise UnsupportedIndex("index: a newer index format")
        raise IndexError_("index: must start with 'format: 1'")
    entries = {}
    current = None
    previous = None
    has_package = False
    for raw in data[len(b"format: 1\n"):-1].split(b"\n") if len(
            data) > len(b"format: 1\n") else []:
        if any(b < 0x20 or b == 0x7F for b in raw):
            raise IndexError_("index: a control character in a line")
        if len(raw) > LINE_MAX:
            raise IndexError_("index: a line longer than 1024 bytes")
        if not raw:
            continue
        key, separator, value = raw.partition(b": ")
        if (not separator or not value
                or not KEY_PATTERN.match(key.decode("latin-1"))):
            raise IndexError_(f"index: {raw[:40]!r} is not 'key: value'")
        if key == b"entry":
            if current is not None and not has_package:
                raise IndexError_(f"index: entry {current} has no package "
                                  "line")
            text = value.decode("latin-1")
            ident, space, version = text.partition(" ")
            if (not space or not mkopk.ID_PATTERN.match(ident)
                    or len(ident) > 63
                    or len(version) > 15
                    or not mkopk.VERSION_PATTERN.match(version)):
                raise IndexError_(f"index: {text!r} needs an id and a "
                                  "version")
            here = (ident.encode("ascii"), version_key(version))
            if previous is not None and here <= previous:
                raise IndexError_(f"index: {text!r} is out of order or "
                                  "listed twice")
            previous = here
            current = (ident, version)
            has_package = False
        elif key == b"package":
            if current is None:
                raise IndexError_("index: a package line before any entry")
            if has_package:
                raise IndexError_(f"index: entry {current} has two package "
                                  "lines")
            text = value.decode("latin-1")
            if not HASH_PATTERN.match(text):
                raise IndexError_("index: a package line needs 64 lowercase "
                                  "hex digits")
            entries[current] = text
            has_package = True
    if current is not None and not has_package:
        raise IndexError_(f"index: entry {current} has no package line")
    return entries


def kernel_public_key():
    text = open(KEY_HEADER, encoding="ascii").read()
    body = text[text.index("{") + 1:text.index("}")]
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))


def key_public_point(key):
    der = subprocess.run(
        ["openssl", "ec", "-in", key, "-pubout", "-outform", "DER"],
        check=True, capture_output=True).stdout
    return der[-65:]


def one_line(text):
    return " ".join(text.split())


CATALOG_CATEGORIES = {"games", "internet", "office", "graphics",
                      "sound-video", "programming", "system", "accessories"}
CATALOG_LIMITS = {"category": 16, "license": 40, "featured": 3,
                  "summary": 90, "description": 900}


def read_catalog(path):
    """Reads the Store catalog: {id: {key: value}}."""
    catalog = {}
    if path is None:
        return catalog
    current = None
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.rstrip("\n")
            where = f"mkindex: {path}:{number}"
            if not line.strip() or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                ident = line[1:-1].strip()
                if not mkopk.ID_PATTERN.match(ident) or ident in catalog:
                    raise SystemExit(f"{where}: [{ident}] is not a new "
                                     "package id")
                current = catalog[ident] = {}
                continue
            key, separator, text = line.partition(":")
            key, text = key.strip(), one_line(text)
            if current is None or not separator:
                raise SystemExit(f"{where}: needs '[id]' and then "
                                 "'key: value' lines")
            if key not in CATALOG_LIMITS or key in current:
                raise SystemExit(f"{where}: {key!r} is not a catalog key "
                                 "or is given twice")
            if not text or len(text) > CATALOG_LIMITS[key]:
                raise SystemExit(f"{where}: {key} must be 1-"
                                 f"{CATALOG_LIMITS[key]} characters")
            if not text.isascii() or "\t" in text:
                raise SystemExit(f"{where}: {key} must be ASCII (the OPOS "
                                 "font has no other characters)")
            if key == "category" and text not in CATALOG_CATEGORIES:
                raise SystemExit(f"{where}: category must be one of "
                                 + ", ".join(sorted(CATALOG_CATEGORIES)))
            if key == "featured" and text != "yes":
                raise SystemExit(f"{where}: featured can only be 'yes'")
            current[key] = text
    return catalog


def asks(package, data):
    """What the package's programs may ask for, for the Store window: the
    capability names from each exec file's OPOS manifest note (or the
    PACKAGE capabilities line for other platforms), or "none"."""
    bits = 0
    if package["platform"] == "opos":
        exec_paths = {f["path"] for f in package["files"] if f["exec"]}
        for name, member in mkopk.read_members(data)[1:]:
            if name in exec_paths:
                bits |= mkopk.read_manifest_note(member)[0]
    else:
        bits = package.get("capability_bits", 0)
    names = [name for name, bit in sorted(mkopk.CAPABILITIES.items(),
                                          key=lambda item: item[1])
             if bits & bit]
    return " ".join(names) if names else "none"


def collect(paths):
    found = []
    for path in paths:
        if os.path.isdir(path):
            found += sorted(os.path.join(path, name)
                            for name in os.listdir(path)
                            if name.endswith(".opk"))
        else:
            found.append(path)
    return found


def build(paths, output, catalog):
    packages = []
    for path in collect(paths):
        with open(path, "rb") as handle:
            data = handle.read()
        try:
            package = mkopk.check_archive(data)
        except mkopk.PackageError as error:
            raise SystemExit(f"mkindex: {path}: {error}")
        package_text = mkopk.read_members(data)[0][1]
        packages.append((package, data, package_text))
    packages.sort(key=lambda item: (item[0]["id"].encode("ascii"),
                                    version_key(item[0]["version"])))
    lines = ["format: 1"]
    seen = set()
    os.makedirs(output, exist_ok=True)
    for package, data, package_text in packages:
        ident, version = package["id"], package["version"]
        if (ident, version) in seen:
            raise SystemExit(f"mkindex: {ident} {version} is given twice")
        seen.add((ident, version))
        name = f"{ident}-{version}.opk"
        with open(os.path.join(output, name), "wb") as handle:
            handle.write(data)
        lines += [
            "",
            f"entry: {ident} {version}",
            f"package: {hashlib.sha256(package_text).hexdigest()}",
            f"file: {name} {len(data)} {hashlib.sha256(data).hexdigest()}",
            f"name: {package['name']}",
            f"publisher: {package['publisher']}",
            f"platform: {package['platform']}",
            f"asks: {asks(package, data)}",
        ]
        if package.get("requires-opos"):
            lines.append(f"requires-opos: {package['requires-opos']}")
        about = catalog.get(ident, {})
        for key in ("category", "license", "featured", "summary",
                    "description"):
            if about.get(key):
                lines.append(f"{key}: {about[key]}")
    data = ("\n".join(lines) + "\n").encode("utf-8")
    parse_index(data)
    path = os.path.join(output, "index")
    with open(path, "wb") as handle:
        handle.write(data)
    return path, len(packages)


def main(argv):
    parser = argparse.ArgumentParser(
        description="Build and sign the OPOS Store index.")
    parser.add_argument("paths", nargs="+", help=".opk files or folders")
    parser.add_argument("--output", required=True)
    parser.add_argument("--key", default=os.path.expanduser(
        "~/.opos-signing/store-key.pem"))
    parser.add_argument("--unsigned", action="store_true",
                        help="write no index.sig (for tests)")
    parser.add_argument("--catalog")
    args = parser.parse_args(argv)
    if not args.unsigned:
        if not os.path.isfile(args.key):
            raise SystemExit(f"mkindex: no signing key at {args.key} "
                             "(create one with tools/store-keygen.sh)")
        if key_public_point(args.key) != kernel_public_key():
            raise SystemExit("mkindex: the key does not match "
                             "kernel/store_key.h")
    index, count = build(args.paths, args.output,
                         read_catalog(args.catalog))
    if not args.unsigned:
        signature = os.path.join(args.output, "index.sig")
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", args.key,
                        "-out", signature, index], check=True)
    print(f"mkindex: {count} packages in {index}"
          + ("" if args.unsigned else ", signed"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
