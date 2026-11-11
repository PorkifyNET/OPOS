# Packages and the OPOS Store (design draft)

Status: proposal. Nothing here is implemented. It builds on the
application sandbox (`docs/app-sandbox.md`), which is shipped in 0.1.4,
and reuses the update signing and download design (`docs/updates.md`).

## Goal

A person can find a program, install it, update it and remove it without
copying files by hand. Every installed program is confined by the
sandbox. The person can tell where a program came from, and a program
cannot change another program's files.

## Current state

- A disk program is an ELF executable copied anywhere below
  `/disk/Programs`. Start > Programs lists it. Nothing records where it
  came from or which version it is.
- `/disk/Programs` is writable, so one program (or the person, by
  mistake) can replace another program's executable.
- Grants are keyed by the executable's SHA-256, so every update asks
  for permissions again (`docs/app-sandbox.md`, "Identity of a program").
- App storage is `<home>/AppData/Programs/<16 hex>`, named from a hash
  of the executable path (`permission_identity_set_program` in
  `kernel/permissions.c`). Moving the executable loses its data.
- A program's manifest note holds a name, a version and requested
  capabilities (`kernel/manifest.c`). No program is signed.

## Design

### Package file

A package is one `.opk` file: an uncompressed ustar archive, the format
the kernel already reads for `initramfs.tar` (`kernel/initramfs.c`).
Compression is left for later, as for updates.

    PACKAGE            text, the package manifest (below)
    files/...          the program's files, installed as they are

`PACKAGE` holds one `key: value` per line:

    format: 1
    id: org.example.paint
    name: Paint
    version: 1.2.0
    publisher: Example Ltd
    requires-opos: 0.1.5
    file: files/paint 3f2a...(SHA-256) 81920 exec
    file: files/brushes.dat 9c01... 4096
    start: files/paint Paint

- `id` is lowercase, reverse-domain, at most 63 characters of `a-z`,
  `0-9`, `.` and `-`. It names the install folder and the app storage,
  so it never changes for the life of a program.
- `version` uses the `VERSION` rules: up to four numbers.
- Every file in `files/` is listed with its SHA-256 and size. A file in
  the archive that is not listed, a listed file that is missing, `..`,
  an absolute path or a link rejects the package.
- `exec` marks an executable. Each `exec` file must carry an OPOS
  manifest note whose capabilities are what the program may ask for.
  The package does not grant anything.
- `start` adds a Start > Programs entry. A package without one installs
  command-line tools only.
- A line with an unknown key makes the package "need a newer OPOS", as
  update manifests do.

### Trust

There is one store key, an ECDSA P-256 key like the update key but
separate from it. Its public half goes into the kernel
(`kernel/store_key.h`). Losing or leaking the update key must not affect
packages, and the other way round.

The store key signs the repository index, not each package:

    index          text: one line per package version
    index.sig      signature over index

An index line holds the id, version, name, publisher, `requires-opos`,
the package's download name, size and SHA-256, and a one-line
description. The chain is: store key, then index, then package hash,
then file hashes. The kernel checks all of it at install time, so
`/bin/pkg` (the downloader) is not trusted, just as `/bin/update` is not.

Publisher keys (each publisher signs its own packages) are not in the
first version. They need key registration and revocation, which is a
lot of work for a store with one publisher. The index format leaves
room for a `signer:` field later.

### Packages from outside the store

A `.opk` opened from Files, a USB stick or a download installs only
after a warning, and only for an administrator:

    "Paint" (org.example.paint) is not from the OPOS Store.
    OPOS cannot check who made it. Install anyway?

Such a package still goes through every file check above. It is
recorded as "sideloaded". Its grants stay keyed by executable hash,
so an update to it asks again. An index entry with the same id always
wins: a sideloaded package cannot be updated by a store package with a
different origin, or the other way round, without removing it first.

### Install location and protection

A package installs to `/disk/Programs/<id>/`, its files below that as
in `files/`. That folder is already the "program folder" of the sandbox,
so the program can read its own files without `files.shared`.

`/disk/Programs/<id>` for an installed package becomes a new protected
class, like `/disk/OPOS`: the kernel installer writes it, programs get
no write access, and an administrator changing it in Files gets the
existing warning and password prompt. Loose executables elsewhere in
`/disk/Programs` keep working as today.

The kernel keeps a record per package in `/disk/OPOS/Packages/<id>`:
id, version, origin (store or sideloaded), and the SHA-256 of each
installed file. Programs cannot read or write it.

### Identity

This is option 3 of "Identity of a program" in `docs/app-sandbox.md`.

- When a store program starts, the kernel finds the package that owns
  the executable path and compares the executable's SHA-256 with the
  record. On a match, the program's identity is the package id.
  Grants are keyed by `store:<id>`, so an update keeps them.
- On a mismatch (someone changed the file), the identity falls back to
  the executable hash, as for any loose program. The program is then a
  new, unknown program.
- App storage for a package is `<home>/AppData/Programs/<id>`, so an
  update or a move inside the package keeps the program's data.
- Sideloaded packages keep hash identity for grants but use the
  `<id>` storage folder.

The hash check at start costs a read of the executable, which the
grants lookup already does today.

### Install, update and remove

Installing needs an administrator, like a system update, because
`/disk/Programs` is shared by every account.

1. `/bin/pkg` downloads the index and signature, then the package, and
   passes them to the kernel through new admin-only syscalls, modeled on
   the update syscalls (begin, file, write, commit, cancel).
2. The kernel checks the index signature, the package hash in the index,
   `requires-opos`, and every file entry, then unpacks into
   `/disk/OPOS/Packages/staging/<id>`.
3. It flushes the staged files, writes `STATE.TXT`, renames any old
   folder to `staging/<id>.old`, renames the new one into
   `/disk/Programs/<id>`, writes the record, and then deletes the old
   folder. A power cut during these steps is finished at the next start,
   like `update_resume()`.

An update is the same steps with a higher version. The index never
offers a lower one; `--ignoreversion` exists only for development.

Removing deletes the folder and the record. The person is asked whether
to delete the program's data in their own `AppData/Programs/<id>`.
Other accounts' data is left, and Settings can clean it up later.
Grants for the program are forgotten in the current account.

### Dependencies

None in the first version. A package carries everything it needs in its
own folder. SDK programs are statically linked already. A Linux program
ships its shared libraries next to it and is started through the
existing dynamic interpreter. The interpreter has no per-program library
path today (`LD_LIBRARY_PATH` or an `$ORIGIN` run path), so Linux
packages need that first.

Shared runtimes (for example one copy of a large library) can come later
as packages of kind `runtime`, which other packages name in a
`needs:` line. This is not designed here.

### Repository

The index, signature and packages are assets of a GitHub Release in a
separate repository, for example `PorkifyNET/opos-packages`, at
`releases/latest/download/index`. `tools/mkindex.py` builds and signs
the index from a folder of `.opk` files, like `tools/mkrelease.py`.
Sources follow the updater order: `--source`, then media
(`opos-packages/` on a CD or USB stick), then
`/disk/OPOS/PACKAGE-SOURCE.TXT`, then GitHub. Media let a VM install
packages without a network.

### Store window

A built-in window lists the index: name, publisher, version, size and
description, with INSTALL, UPDATE and REMOVE buttons, and an "Installed"
view. It runs `/bin/pkg` with a `--gui` output mode, as Settings >
UPDATES runs `/bin/update`. It shows what each program may ask for,
taken from the manifest notes, before it is installed.

Settings > UPDATES gains a line for package updates. Nothing updates
by itself in the first version.

## Order of work

1. SDK: `make package` builds a `.opk` from a program and a
   `PACKAGE.in` file, filling in hashes and sizes. Host tool
   `tools/mkopk.py`, with tests for every reject case.
2. Kernel: package parser and checks, staging, the protected class,
   records and resume. Tested with sideloaded packages in QEMU.
3. Identity: `store:<id>` grants and `<id>` storage, with a test that
   an update keeps both.
4. Store key, `tools/mkindex.py`, index checks, and `/bin/pkg` with
   install, update, remove and list.
5. Store window and the Settings > UPDATES line.
6. First real packages: move `sketch` and `hello` out of the image into
   the repository as examples.

Each step keeps ABI version 1. Loose disk programs keep working
throughout.

## Open questions

- Should a person without administrator rights be able to install a
  program for their own account only, in `<home>/Programs`?
- Should the Store show packages that `requires-opos` a newer system,
  with a note to update OPOS first?
- Where does the store key live, and who besides the project owner may
  sign an index?
- Does removing a program also remove its grants in other accounts,
  which the kernel can reach but the person removing it cannot see?

## Not covered

Paid programs, ratings and reviews, automatic updates, delta downloads,
publisher keys, Windows programs (they need a compatibility layer
first), and packaging the system itself.
