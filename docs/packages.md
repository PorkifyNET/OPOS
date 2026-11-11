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
    platform: opos
    requires-opos: 0.1.5
    file: files/paint 3f2a...(SHA-256) 81920 exec
    file: files/brushes.dat 9c01... 4096
    start: files/paint Paint

- `id` is lowercase, reverse-domain, at most 63 characters of `a-z`,
  `0-9`, `.` and `-`. It names the install folder and the app storage,
  so it never changes for the life of a program.
- `version` uses the `VERSION` rules: up to four numbers.
- `platform` says what the executables are built for (see "Linux and
  Windows programs"): `opos`, `linux` or `windows`.
- Every file in `files/` is listed with its SHA-256 and size. A file in
  the archive that is not listed, a listed file that is missing, `..`,
  an absolute path or a link rejects the package.
- `exec` marks an executable. For `platform: opos`, each `exec` file
  must carry an OPOS manifest note whose capabilities are what the
  program may ask for. Other platforms cannot carry the note, so their
  `PACKAGE` holds a `capabilities:` line instead, and the kernel records
  it at install as the program's manifest. The package does not grant
  anything.
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

The store key is kept like the update key: the private half in WSL at
`~/.opos-signing/store-key.pem`, outside the repository, with an offline
backup. `tools/mkindex.py` refuses a key that does not match
`kernel/store_key.h`. Only the project owner signs an index. Other
people contribute packages by pull request to the package repository,
and the owner signs the index that includes them. If the key is lost,
a new one needs a system update that ships the new `store_key.h`. That
is easier than for the update key, which can only be replaced by
reinstalling.

Publisher keys (each publisher signs its own packages) are not in the
first version. They need key registration and revocation, which is a
lot of work for a store with one publisher. The index format leaves
room for a `signer:` field later.

### Packages from outside the store

A `.opk` opened from Files, a USB stick or a download installs only
after a warning. An administrator can install it for everyone, and
anyone can install it for their own account (see "Installing for one
account"):

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

Installing for everyone needs an administrator, like a system update,
because `/disk/Programs` is shared by every account.

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
Grants for the program are forgotten in the current account only.
Other accounts keep their data and their grants, because the person
removing the program cannot see them. If the program is installed
again, those accounts keep their earlier answers, which they can change
in the Security Center. Settings can clean up leftovers later.

### Installing for one account

Anyone can install a package for their own account without an
administrator. It goes to `<home>/Programs/<id>/`, and its record to
`<home>/AppData/Packages/<id>`, which programs cannot reach, like
`Permissions.dat`. Only that account sees it in Start, and it is
confined and checked exactly like a package installed for everyone.

- One id is installed once per account. If a package is installed for
  everyone, installing it for one account is refused. If an
  administrator later installs it for everyone, an account that already
  has its own copy keeps using that copy until it removes it.
- The kernel protects `<home>/Programs/<id>` like `/disk/Programs/<id>`:
  programs cannot write it, the account owner gets the same warning
  before changing it in Files.
- A person installs, updates and removes their own copies without a
  password. The warning for packages outside the Store still applies.

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

### Linux and Windows programs

The Store lists Linux and Windows programs as well as OPOS ones, but
only the ones this OPOS can run. Each index line carries `platform`,
and the kernel reports which platforms it supports.

- `opos`: always supported.
- `linux`: supported for statically linked programs today, through
  the Linux system call layer (`docs/linux-compat.md`). Dynamically
  linked ones need the library path above. Each Linux package is
  tested on OPOS before it goes into the index, because the layer does
  not cover every system call.
- `windows`: hidden until a Windows compatibility layer exists. The
  field is in the format now so that adding it later needs no format
  change.

Linux and Windows programs get the same identity, storage, grants and
prompts as OPOS programs. The compatibility layer is not trusted to
enforce anything (`docs/app-sandbox.md`).

A package may only contain software whose license allows it to be
redistributed, and the package repository keeps each package's license
next to it. Many Linux programs qualify. Most commercial Windows
programs do not, so a Windows section would mostly hold free software,
and other programs would be installed from their own installers.

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

A package whose `requires-opos` is newer than the running system is
still listed, with "Needs OPOS 0.2.0 or newer" in place of INSTALL and
a button that opens Settings > UPDATES. A package for a platform this
OPOS cannot run is not listed.

Settings > UPDATES gains a line for package updates. Nothing updates
by itself in the first version.

## Order of work

1. SDK: `make package` builds a `.opk` from a program and a
   `PACKAGE.in` file, filling in hashes and sizes. Host tool
   `tools/mkopk.py`, with tests for every reject case.
2. Kernel: package parser and checks, staging, the protected class,
   records and resume, for everyone and for one account. Tested with
   sideloaded packages in QEMU.
3. Identity: `store:<id>` grants and `<id>` storage, with a test that
   an update keeps both.
4. Store key, `tools/mkindex.py`, index checks, and `/bin/pkg` with
   install, update, remove and list.
5. Store window and the Settings > UPDATES line.
6. First real packages: move `sketch` and `hello` out of the image into
   the repository as examples.

Each step keeps ABI version 1. Loose disk programs keep working
throughout.

## Decisions

The project owner answered the first open questions on 2026-11-11:

- Installing for one account without an administrator: yes.
- Packages that need a newer OPOS: listed, saying which version they
  need.
- Removing a program does not remove grants in other accounts.
- Linux and Windows packages: listed when this OPOS can run them.
- The store key: the proposal in "Trust" stands until the owner decides
  otherwise.

## Open questions

- Should Linux packages come from an existing distribution's packages
  (converted to `.opk`), or be built for OPOS one by one?
- Does an account's own copy of a program count against a disk quota?
  OPOS has no quotas yet.

## Not covered

Paid programs, ratings and reviews, automatic updates, delta downloads,
publisher keys, a Windows compatibility layer, and packaging the system
itself.
