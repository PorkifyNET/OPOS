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
    LINUX + "capabilities: notifications net\n",
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


sys.path.insert(0, os.path.join(ROOT, "tools"))
import mkindex  # noqa: E402

H1 = "00112233445566778899aabbccddeeff" * 2
H2 = "ffeeddccbbaa99887766554433221100" * 2
INDEX = f"""format: 1

entry: org.example.paint 1.2.0
package: {H1}
file: org.example.paint-1.2.0.opk 100 {H1}
name: Paint
description: Draws pictures é

entry: org.example.paint 1.10.0
package: {H2}
entry: org.example.zoo 1.0
signer: someone
package: {H1}
"""


def index_replace(old, new, base=INDEX):
    assert old in base, old
    return base.replace(old, new, 1)


ZOO = "entry: org.example.zoo 1.0\nsigner: someone\npackage: " + H1 + "\n"
INDEX_CASES = [
    INDEX,
    "format: 1\n",
    "format: 1\n\n\n",
    INDEX[:-1],
    "",
    "format: 2\n",
    "format: 1",
    "format:  1\n",
    " format: 1\n",
    index_replace("format: 1\n", "format: 1\r\n"),
    index_replace("name: Paint", "name: Pa\tint"),
    index_replace("name: Paint", "name: Pa\x7fint"),
    index_replace("name: Paint", "name:"),
    index_replace("name: Paint", "name: "),
    index_replace("name: Paint", "name:Paint"),
    index_replace("name: Paint", "Name: Paint"),
    index_replace("name: Paint", "-name: Paint"),
    index_replace("name: Paint", "na-me: Paint"),
    index_replace("name: Paint", "na me: Paint"),
    index_replace("name: Paint", "name:: Paint"),
    index_replace("name: Paint", "name: " + "x" * 1018),
    index_replace("name: Paint", "name: " + "x" * 1019),
    index_replace("name: Paint", "just text"),
    index_replace(f"package: {H2}\n", ""),
    index_replace(f"package: {H2}\n", f"package: {H2}\npackage: {H2}\n"),
    index_replace(f"package: {H2}", f"package: {H2.upper()}"),
    index_replace(f"package: {H2}", f"package: {H2[:-1]}"),
    index_replace(f"package: {H2}", f"package: {H2}0"),
    index_replace(f"package: {H2}", f"package:  {H2}"),
    "format: 1\npackage: " + H1 + "\n",
    INDEX.replace(ZOO, ""),
    INDEX + "entry: org.example.zoo 1.0\npackage: " + H1 + "\n",
    INDEX + "entry: org.example.zoo 0.9\npackage: " + H1 + "\n",
    INDEX + "entry: org.example.zoo 1.0.1\npackage: " + H1 + "\n",
    INDEX + "entry: org.example.aaa 1.0\npackage: " + H1 + "\n",
    INDEX + "entry: org.example.zoo.b 1.0\npackage: " + H1 + "\n",
    INDEX + "entry: org.example.zoo 1.0.0\npackage: " + H1 + "\n",
    index_replace("entry: org.example.zoo 1.0", "entry: org.example.zoo"),
    index_replace("entry: org.example.zoo 1.0", "entry: org.example.zoo 1.0 "),
    index_replace("entry: org.example.zoo 1.0", "entry: org.example.zoo  1.0"),
    index_replace("entry: org.example.zoo 1.0", "entry: Org.example.zoo 1.0"),
    index_replace("entry: org.example.zoo 1.0", "entry: zoo 1.0"),
    index_replace("entry: org.example.zoo 1.0", "entry: org.example.zoo 1.a"),
    index_replace("entry: org.example.zoo 1.0",
                  "entry: org.example.zoo 12345.12345.12345.1"),
    index_replace("entry: org.example.zoo 1.0",
                  "entry: org.example.zoo 1234.1234.1234.12"),
    index_replace("entry: org.example.zoo 1.0",
                  "entry: org.example.zoo 1.2.3.4.5"),
    index_replace("entry: org.example.zoo 1.0",
                  "entry: o" + "a" * 61 + ".b 1.0"),
    index_replace("entry: org.example.zoo 1.0",
                  "entry: o" + "a" * 60 + ".b 1.0"),
    "format: 1\n" + "\n" * (262144 - 10),
    "format: 1\n" + "\n" * (262145 - 10),
]
LOOKUPS = [("org.example.paint", "1.2.0"), ("org.example.paint", "1.10.0"),
           ("org.example.paint", "1.10"), ("org.example.zoo", "1.0"),
           ("org.example.pain", "1.2.0"), ("org.example.none", "1.0")]


def python_index(data, lookup):
    try:
        entries = mkindex.parse_index(data)
    except mkindex.UnsupportedIndex:
        return 4, "-"
    except mkindex.IndexError_:
        return 3, "-"
    if lookup is None:
        return 0, "-"
    if lookup not in entries:
        return 10, "-"
    return 0, entries[lookup]


def index_parity(cli, folder):
    failures = 0
    count = 0
    path = os.path.join(folder, "index")
    for number, text in enumerate(INDEX_CASES):
        data = text.encode("utf-8")
        with open(path, "wb") as handle:
            handle.write(data)
        for lookup in [None] + LOOKUPS:
            count += 1
            command = [cli, "--index", path] + (list(lookup) if lookup
                                                 else [])
            output = subprocess.run(command, capture_output=True, text=True,
                                    errors="replace",
                                    check=True).stdout
            kernel_code, kernel_hash = output.split(" ", 2)[:2]
            expected = python_index(data, lookup)
            if (int(kernel_code), kernel_hash) != expected:
                failures += 1
                print(f"index case {number} {lookup}: kernel {kernel_code} "
                      f"{kernel_hash}, mkindex {expected}: {output.strip()}"
                      f"\n{text[:300]!r}")
    return count, failures


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
        index_count, index_failures = index_parity(cli, folder)
    failures += index_failures
    print(f"test_package_parity: {len(CASES)} cases, {index_count} index "
          f"checks, {failures} differ")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
