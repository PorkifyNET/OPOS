#!/usr/bin/env python3
"""Builds the files of an OPOS update release (run by `make release`).

    mkrelease.py --version 0.2.0 --key KEY.pem --output DIR [--iso ISO] \
        SOURCE=PATH ...

Each SOURCE file is installed at PATH on the system volume (for example
build/kernel.elf=boot/kernel.elf). DIR receives:

    opos-update.manifest   the signed list of files (format: kernel/update.h)
    opos-update.sig        ECDSA P-256 signature of the manifest (DER)
    <asset>                one file per manifest entry, named after PATH
    opos-release-notes.txt the --notes file (shown before installing)
    opos-<version>.iso     the install ISO, when --iso is given (not part
                           of the update; handy for new installs)

Upload all of them to a GitHub Release. OPOS downloads
releases/latest/download/opos-update.manifest, so the release must be
marked "latest". The key must match kernel/update_key.h, or installs
reject the update; this script checks that before signing.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_HEADER = os.path.join(ROOT, "kernel", "update_key.h")
VERSION_RE = re.compile(r"^[0-9]{1,9}(\.[0-9]{1,9}){0,3}$")
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


def fail(message):
    print("mkrelease: " + message, file=sys.stderr)
    sys.exit(1)


def kernel_public_key():
    text = open(KEY_HEADER, encoding="ascii").read()
    body = text[text.index("{") + 1:text.index("}")]
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))


def key_public_point(key):
    der = subprocess.run(
        ["openssl", "ec", "-in", key, "-pubout", "-outform", "DER"],
        check=True, capture_output=True).stdout
    return der[-65:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--iso")
    parser.add_argument("--notes", help="release notes text file (shown "
                        "before installing; not signed)")
    parser.add_argument("files", nargs="+", metavar="SOURCE=PATH")
    args = parser.parse_args()

    if not VERSION_RE.match(args.version):
        fail("version must look like 1.2.3, not %r" % args.version)
    if not os.path.isfile(args.key):
        fail("no signing key at %s (create one with tools/update-keygen.sh)"
             % args.key)
    if key_public_point(args.key) != kernel_public_key():
        fail("%s is not the key in kernel/update_key.h" % args.key)

    entries = []
    assets = set()
    for item in args.files:
        source, _, path = item.partition("=")
        asset = os.path.basename(path)
        if not path.startswith("boot/") or not NAME_RE.match(asset):
            fail("bad install path %r" % path)
        if asset in assets:
            fail("two files are named %s" % asset)
        assets.add(asset)
        data = open(source, "rb").read()
        entries.append((path, asset, source, data))
    # The kernel is installed last; keep it last in the list too.
    entries.sort(key=lambda e: e[0] == "boot/kernel.elf")

    lines = ["OPOS-UPDATE 1", "version " + args.version]
    for path, asset, _, data in entries:
        lines.append("file %s %d %s %s" % (
            hashlib.sha256(data).hexdigest(), len(data), asset, path))
    manifest = ("\n".join(lines) + "\n").encode("ascii")

    if os.path.isdir(args.output):
        shutil.rmtree(args.output)
    os.makedirs(args.output)
    manifest_path = os.path.join(args.output, "opos-update.manifest")
    signature_path = os.path.join(args.output, "opos-update.sig")
    with open(manifest_path, "wb") as f:
        f.write(manifest)
    subprocess.run(["openssl", "dgst", "-sha256", "-sign", args.key,
                    "-out", signature_path, manifest_path], check=True)
    for _, asset, source, _ in entries:
        shutil.copyfile(source, os.path.join(args.output, asset))
    if args.notes:
        notes = open(args.notes, "rb").read()
        if len(notes) > 4096:
            fail("%s is over 4096 bytes" % args.notes)
        with open(os.path.join(args.output, "opos-release-notes.txt"),
                  "wb") as f:
            f.write(notes)
    if args.iso:
        shutil.copyfile(args.iso, os.path.join(
            args.output, "opos-%s.iso" % args.version))

    print(manifest.decode("ascii"), end="")
    print("Release files for OPOS %s are in %s" % (args.version, args.output))


if __name__ == "__main__":
    main()
