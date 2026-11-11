#!/usr/bin/env python3
"""Checks that kernel/package.c and sdk/tools/mkopk.py accept and reject
the same PACKAGE texts. Run by `make package-test`:

    test_package_parity.py build/test_package
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sdk", "tools"))
import mkopk  # noqa: E402

HASH = "0123456789abcdef" * 4
GOOD = f"""format: 1
id: org.example.paint
name: Paint
version: 1.2.0
publisher: Example Ltd
platform: opos
requires-opos: 0.1.5
file: files/paint {HASH} 100 exec
file: files/data/brushes.dat {HASH} 4096
start: files/paint Paint
"""
LINUX = GOOD.replace("platform: opos", "platform: linux")


def replace(old, new, base=GOOD):
    assert old in base, old
    return base.replace(old, new)


CASES = [
    GOOD,
    GOOD + "\n\n   \n",
    replace("start: files/paint Paint", "start: files/paint A Long Label"),
    replace("requires-opos: 0.1.5\n", ""),
    replace("start: files/paint Paint\n", ""),
    LINUX + "capabilities: net, clipboard.read files.shared\n",
    LINUX + "capabilities: camera\n",
    LINUX + "capabilities: , ,\n",
    GOOD + "capabilities: net\n",
    replace("platform: opos", "platform: windows"),
    replace("platform: opos", "platform: macos"),
    GOOD + "needs: x\n",
    GOOD + "origin: store\n",
    replace("format: 1", "format: 2"),
    replace("format: 1", "format: 01"),
    GOOD + "name: Other\n",
    GOOD + "nonsense\n",
    GOOD + "key:value\n",
    GOOD + "name:\n",
    GOOD + " name: x\n",
    GOOD + "name: x \n",
    replace("Example Ltd", "Exämple"),
    replace("Example Ltd", "Ex\tample"),
    replace("name: Paint", "name: " + "n" * 31),
    replace("name: Paint", "name: " + "n" * 32),
    replace("Example Ltd", "p" * 63),
    replace("Example Ltd", "p" * 64),
    replace("1.2.0", "1.2.3.4"),
    replace("1.2.0", "1.2.3.4.5"),
    replace("1.2.0", "99999.1"),
    replace("1.2.0", "123456"),
    replace("1.2.0", "1..2"),
    replace("1.2.0", "v1"),
    replace("0.1.5", "next"),
    replace("org.example.paint", "paint"),
    replace("org.example.paint", "Org.example.paint"),
    replace("org.example.paint", "org..paint"),
    replace("org.example.paint", "org.-x"),
    replace("org.example.paint", "org.x-"),
    replace("org.example.paint", "org.ex-ample.p4int"),
    replace("org.example.paint", "org.example.paint_2"),
    replace("org.example.paint", "o." + "a" * 61),
    replace("org.example.paint", "o." + "a" * 62),
    replace("files/data/brushes.dat", "files/a:b"),
    replace("files/data/brushes.dat", "files/a*b"),
    replace("files/data/brushes.dat", "files/../x"),
    replace("files/data/brushes.dat", "files/./x"),
    replace("files/data/brushes.dat", "files//x"),
    replace("files/data/brushes.dat", "files/"),
    replace("files/data/brushes.dat", "files"),
    replace("files/data/brushes.dat", "other/x"),
    replace("files/data/brushes.dat", "/files/x"),
    replace("files/data/brushes.dat", "files/name."),
    replace("files/data/brushes.dat", "files/" + "a" * 144),
    replace("files/data/brushes.dat", "files/" + "a" * 145),
    replace("files/data/brushes.dat", "files/PAINT"),
    replace("files/data/brushes.dat", "files/paint/x"),
    replace(f"{HASH} 4096", f"{HASH.upper()} 4096"),
    replace(f"{HASH} 4096", f"{HASH[:-1]} 4096"),
    replace(f"{HASH} 4096", f"{HASH} 04096"),
    replace(f"{HASH} 4096", f"{HASH} 0"),
    replace(f"{HASH} 4096", f"{HASH} 4294967295"),
    replace(f"{HASH} 4096", f"{HASH} 4294967296"),
    replace(f"{HASH} 4096", f"{HASH} 12345678901"),
    replace(f"{HASH} 4096", f"{HASH} -1"),
    replace(f"{HASH} 4096", f"{HASH} 4096 exec exec"),
    replace(f"{HASH} 4096", f"{HASH}  4096"),
    replace(f"{HASH} 4096", f"{HASH} 4096 data"),
    replace(f"{HASH} 4096", f"{HASH}"),
    replace("start: files/paint Paint", "start: files/data/brushes.dat B"),
    replace("start: files/paint Paint", "start: files/paint"),
    replace("start: files/paint Paint", "start: files/paint " + "L" * 32),
    replace("start: files/paint Paint", "start: files/nothing P"),
    GOOD + "start: files/paint P\n" * 15,
    GOOD + "start: files/paint P\n" * 16,
    "\n".join(l for l in GOOD.split("\n")
              if not l.startswith(("file:", "start:"))),
    GOOD + "".join(f"file: files/f{i} {HASH} 1\n" for i in range(510)),
    GOOD + "".join(f"file: files/f{i} {HASH} 1\n" for i in range(511)),
    GOOD + "\n" * (65536 - len(GOOD)),
    GOOD + "\n" * (65537 - len(GOOD)),
] + ["\n".join(l for l in GOOD.split("\n") if not l.startswith(key + ":"))
     for key in ("format", "id", "name", "version", "publisher", "platform")]


def python_result(text):
    try:
        mkopk.parse_package(text, template=False)
        return 0
    except mkopk.PackageError as error:
        return 4 if "needs a newer OPOS" in str(error) else 3


def main():
    cli = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "PACKAGE")
        for number, text in enumerate(CASES):
            data = text.encode("utf-8")
            with open(path, "wb") as handle:
                handle.write(data)
            output = subprocess.run([cli, path], capture_output=True,
                                    text=True, check=True).stdout
            kernel = int(output.split(" ", 1)[0])
            expected = python_result(data.decode("ascii", "replace")
                                     if any(b >= 0x80 for b in data)
                                     else text)
            if kernel != expected:
                failures += 1
                print(f"case {number}: kernel {kernel}, mkopk {expected}: "
                      f"{output.strip()}\n{text[:300]}")
    print(f"test_package_parity: {len(CASES)} cases, {failures} differ")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
