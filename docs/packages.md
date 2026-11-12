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
  of the executable path (`permission_confine_disk_program` in
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

As built in step 1 (`sdk/tools/mkopk.py`, the reference for the
installer's checks):

- `PACKAGE` is the first member, at most 64 KB, printable ASCII, one
  `key: value` per line. That lets the installer read it before any
  file data arrives. (Step 1 said 16 KB, which cannot hold 512 file
  lines; step 2 raised it.)
- Only regular-file members. Folders are implied by paths; folder, link
  and special entries reject the package. So does data after the two
  end blocks, a bad header checksum, or a member listed twice.
- Paths have no spaces and none of `" * : < > ? \ |`, are at most 150
  characters, and no part ends in `.`. Names are compared ignoring case,
  as FAT does. A name cannot be both a file and a folder. (Step 1 said
  200. A program can only open paths shorter than 256 characters, and
  the longest install folder, `/disk/Users/<20>/Programs/<63>/`, takes
  107 of them.)
- At most 512 files of at most 4 GiB - 1 each (the FAT limit), and at
  most 16 `start` lines. `name` and `start` labels are 1 to 31
  characters, `publisher` at most 63, versions at most 15.
- `format`, `id`, `name`, `version`, `publisher` and `platform` are
  required; each single key appears once.
- An `opos` executable needs exactly one valid manifest note, checked as
  `manifest_read()` does. A `linux` one must be an x86-64 ELF64, a
  `windows` one must start with `MZ`. `capabilities:` takes
  `files.shared`, `net` and `clipboard.read`, and is refused in an
  `opos` package.
- The archive is reproducible: times, owners and modes are fixed.

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

### As built in step 2

Packages from outside the Store install and remove, for everyone or for
one account. The Store, the index and its key come in step 4.

- Kernel: `kernel/package.c` checks `PACKAGE` text and ustar headers
  (the same rules as `mkopk.py`; `make package-test` compares the two on
  86 cases). `kernel/package_install.c` installs, removes, resumes and
  lists start entries.
- System calls `0x4f500030`-`32` (`opos_package_check`,
  `opos_package_install`, `opos_package_remove` in `<opos.h>`). They
  take a file path rather than streamed data: the kernel reads the
  archive itself, with the caller's right to read it checked first.
  Confined (disk) programs cannot call them. A package from outside the
  Store installs only with `OPOS_PACKAGE_CONFIRMED`, which the caller
  passes after the person agreed.
- `/bin/pkg info|install|remove|list` shows what a package is, warns
  that it is not from the Store, and asks for YES (`-y` skips it).
  `--everyone` installs for every account; `--ignoreversion` reinstalls.
  It is not in Start > Programs, like `update`.
- Installing reads `PACKAGE` alone first, so refusals (wrong platform,
  newer OPOS needed, conflicts, version not newer, not confirmed) cost no
  copying. Then the archive is read again and every member is copied to
  `/disk/OPOS/PKGSTAGE/NEW` while its SHA-256 is computed, and the
  refusals are checked again on that read. What is installed is what was
  checked, even if the archive changed in between.
- Then `STATE.TXT` names the target folder and record, and the work only
  rolls forward: the old folder to `PKGSTAGE/OLD`, `NEW` to the target,
  the record written, `OLD` and the staging folder removed. Removing is
  the same without `NEW`. At boot, `package_resume` finishes it. A
  `STATE.TXT` naming anything but a package folder and its record is
  refused, and installs stop until the staging folder is removed.
- Records are `origin: sideloaded` followed by the package's `PACKAGE`
  text, so they also hold each file's hash.
- Protection (`kernel/permissions.c`): the installer lists every
  installed folder; permissions.c matches a path's folder by device and
  inode, so 8.3 aliases do not get around it. Programs may only read
  inside a package folder. Files treats one installed for everyone like
  system files (administrator, warning and password) and an account's
  own copy as read-only: it changes only through the installer.
  `/disk/Programs` and `<home>/Programs` cannot be moved or removed, and
  `<home>/AppData/Packages` is read-only, readable only by the account's
  own unconfined programs.
- A program in `<home>/Programs/<folder>` reads its own folder like one
  in `/disk/Programs/<folder>`.
- An account that already has its own copy can still update it after an
  administrator installed the same id for everyone.
- Start > Programs lists every `start` entry of the account's own and
  everyone's packages. Built-in and loose program names win over package
  labels, so a package cannot appear as a built-in.
- Packages and system updates refuse to run at the same time.
- Tested in QEMU (KVM, installed BIOS disk): install for one account and
  everyone, update, `NOT_NEWER`, a damaged member, a Windows package, a
  package needing OPOS 9.0, removal, the account/everyone conflict, the
  YES prompt, `pkg list`, Start > Programs launching a packaged program,
  boot resume of an interrupted install and removal, and a forged
  `STATE.TXT`.

Not done yet: a window for the warning (Files cannot open a `.opk`
yet).

### As built in step 3

- `package_identify` (`kernel/package_install.c`) runs when a disk
  program starts. It looks for an executable inside
  `/disk/Programs/<id>/` or `<home>/Programs/<id>/` whose package has a
  record. The executable's SHA-256 must match a file the record marks
  `exec`. The id folder may use any case, because FAT ignores case. An
  8.3 alias, a changed file or a damaged record makes the program a
  loose program.
- A matched program gets app storage `<home>/AppData/Programs/<id>`
  (`permission_confine_disk_program_named`). A Store package's grants
  are keyed by `permission_store_key`, the SHA-256 of `store:<id>`, so
  `Permissions.dat` keeps its format. A sideloaded package keeps the
  executable's hash as its key.
- Removal forgets the program's grants in the removing account: the
  Store key, or each executable's hash for a sideloaded package. The new
  `OPOS_PACKAGE_REMOVE_DATA` option also deletes that account's
  `AppData/Programs/<id>`. `pkg remove` asks before deleting the data,
  and `--delete-data` deletes it without asking. If the account still
  has the other copy of the id, its own or everyone's, the removal keeps
  both grants and data, because that copy uses them.
- Host tests: `tools/test_permission_grants.c` checks identity with a
  stubbed `package_identify`, and `tools/test_app_permissions.c` checks
  storage names.
- QEMU test on an installed disk, with two versions of one test package:
  - Install for one account. The storage counter survives the update
    from 1.0 to 1.1.
  - Plain removal and reinstall keep the data. `--delete-data` clears it.
  - Install for everyone uses the same `<id>` folder.
  - An uppercase spelling of the id folder gets the same identity.
- Store packages cannot be installed until step 4. So far only the host
  test covers grants surviving an update.
- Step 2 sideloaded installs used the hash-named folder for app storage.
  They now use the `<id>` folder, so data from before this change is not
  carried over. Step 2 was never released.

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

1. Done. SDK: `make package PKG=x.pkg` builds a `.opk` from a program
   and a `.pkg` file, filling in hashes and sizes. Host tool
   `sdk/tools/mkopk.py` (`build` and `check`), tests in
   `sdk/tools/test_mkopk.py` (`make package-test`), example
   `sdk/examples/pickfile.pkg`.
2. Done. Kernel: package parser and checks, staging, the protected class,
   records and resume, for everyone and for one account. Tested with
   sideloaded packages in QEMU.
3. Done. Identity: `store:<id>` grants and `<id>` storage. The host test
   shows that an update keeps both. QEMU shows that storage survives an
   update for sideloaded packages.
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
