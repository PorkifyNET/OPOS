# Linux programs on OPOS

How unmodified Linux programs get onto OPOS through the Store, what
already works, and what is missing. `docs/linux-compat.md` describes the
system call layer itself; `docs/packages.md` the package format, which
already accepts `platform: linux`.

## What was tried

Two real programs, built from their release sources as static x86-64
binaries with musl (`gcc -specs musl.specs -Os -static`), packaged as
`platform: linux` `.opk` files and installed with `pkg install`:

- **Lua 5.4.7** (294 KB): a script that fills a 100 000-entry table,
  formats strings, reads the clock and date, writes and reads back a
  2000-line file in `$HOME`, prints 3000 characters at once, and uses
  `pcall` and coroutines. The interactive prompt evaluates typed lines.
- **SQLite 3.46.1** (908 KB, the `sqlite3` shell): creates a database in
  its storage folder, inserts 5003 rows with a recursive query, reads
  them back, and keeps its rollback journal. The prompt works.

Both now run completely. The first attempt failed in seven places, each
one a small gap that many programs share.

## What was fixed

| Symptom | Cause | Fix |
| --- | --- | --- |
| `cannot read stdin: Function not implemented` | musl reads through `readv` | `readv`: one read per buffer, stopping after the first bytes from a terminal, pipe or socket |
| A 2000-line file was empty; long output vanished | `writev` refused more than 256 bytes | `writev`: one write per buffer with `write`'s limits; larger console and file writes are short instead of `EINVAL` |
| Console reads of more than 128 bytes failed | `EINVAL` instead of a short read | Returns at most one line, as a terminal does |
| The Lua prompt waited for end of file | `isatty()` was false: no `ioctl` | `ioctl` `TCGETS`, `TIOCGWINSZ` and `TCSETS*` on the console; `ENOTTY` elsewhere |
| `cannot find home directory` | Empty environment | `HOME`, `USER`, `LOGNAME`, `PATH=/bin`, `TERM=dumb` for programs started from the desktop |
| `os.clock()` was -1 | No CPU-time clocks | `CLOCK_PROCESS_CPUTIME_ID` and the other monotonic clock ids read the monotonic clock |
| `unable to open database file` | `access`, `O_NOFOLLOW`, and `lstat` of `/disk/Users` refused | `access`/`faccessat`; `O_NOFOLLOW`, `O_SYNC`, `O_DSYNC`, `O_NOATIME` accepted; folders on the way to the program's own folders may be stat'ed |

Also added because SQLite and most databases need them: `pread64`,
`pwrite64`, `fsync`, `fdatasync`, `fcntl` record locks (`F_GETLK`,
`F_SETLK`, `F_SETLKW`), and `chmod`/`chown` and their variants, which
succeed without effect because FAT keeps no owners or modes.

Every system call OPOS lacks is now written to the serial log once per
process: `OPOS: Linux syscall 25 not implemented (lua)`. Running a
program and reading the log is the quickest way to see what it needs.

### HOME

A confined program's `HOME` is its own storage folder,
`<home>/AppData/Programs/<id>`, created at start. That is the one place
it may write, so dotfiles (`~/.sqliterc`, `~/.luarc`) and relative
`~/` paths work without asking for anything. The account's real home is
not visible to it, as before. Programs that are not confined (built-in
ones) get the account's home.

Programs started from the kernel shell keep an empty environment: its
self-tests check the exact initial stack.

### Folders on the way

A confined program may `stat` (not list) `/`, `/disk`, `/disk/Users`,
`/disk/Users/<name>` and the other folders that lead to its own storage
and program folder. Linux programs check each part of a path, for
example to resolve links. The names are already in the program's own
paths, so nothing new is revealed.

## What is missing

In the order the next programs are likely to hit them.

1. **Devices.** No `/dev`: `/dev/null`, `/dev/zero`, `/dev/urandom`
   and `/dev/tty` are opened by many programs (SQLite falls back
   quietly when `/dev/urandom` is missing).
2. **Identity.** Every Linux program sees uid 0 (root) and no
   `/etc/passwd`. Programs behave differently as root (SQLite calls
   `fchown`). OPOS should report one non-root uid per account and a
   generated `passwd` line.
3. **Limits.** 11 file descriptors per process and 32 open files system
   wide; a 1 MiB executable; a 4 MiB `mmap` area; 16 arguments and 16
   environment strings; no `mremap` (Lua's `realloc` falls back to
   copying). Larger programs (BusyBox, Python) will hit these first.
4. **Terminal.** Input is always one line with echo, and the desktop
   Terminal does not interpret escape sequences. Full-screen programs
   (`vi`, `less`, `top`) need raw mode, cursor movement and colors.
   Ctrl+C should send `SIGINT`; Ctrl+D should end input.
5. **Signals between processes.** `kill` and `tgkill` to other
   processes, process groups, `SIGCHLD`.
6. **Locks.** Record locks are granted at once and not tracked: two
   processes writing one SQLite database could corrupt it. Fine while
   each program has its own storage; needed before shared files.
7. **Dynamic linking.** Static musl programs work. Dynamically linked
   ones need a library search path (`$ORIGIN`, `LD_LIBRARY_PATH`) and
   more than four libraries; glibc programs need far more.
8. **Graphics.** Linux GUI programs draw through X11 or Wayland. A
   first step could be a small SDL2 backend that draws into an OPOS
   window, which covers many games and simple programs.

## Linux packages in the Store

The format already supports them, and the window shows "MAY ASK FOR"
from the `capabilities:` line of a Linux package's `PACKAGE`.

Proposal:
- Lua and SQLite went into the Store first: their licenses (MIT and
  public domain) allow redistribution, and they are tested above.
- Each Linux package is built from a pinned source release with a
  checked SHA-256, by a script in `store/linux/<name>/` that
  `make store` runs, so anyone can rebuild what the Store ships.
- Each package carries its license and musl's, as `docs/packages.md`
  requires.

## Order of work

1. Done. Real programs: Lua and SQLite run as Linux packages; the fixes
   above.
2. Done. Lua and SQLite in the Store, built by `make store` from pinned
   sources with their licenses. See "As built in step 2".
3. `/dev` devices and a non-root identity.
4. Higher limits: descriptors, open files, executable size, `mmap`,
   `mremap`.
5. Terminal: raw mode, escape sequences in the desktop Terminal,
   Ctrl+C and Ctrl+D.
6. A larger program as the next test (BusyBox), then dynamic linking.
7. Graphics through an SDL2 backend.

## As built in step 2

**Build scripts.** `store/linux/<name>/build.sh` builds one Linux
package. `make store` runs those named in `STORE_LINUX` (default
`lua sqlite`) after the OPOS packages, so the signed index lists all of
them. Each script:
- downloads its pinned source release into `STORE_SOURCES` (default
  `build/store_sources`) once, and stops when its SHA-256 differs;
- builds one static program with `LINUX_CC`;
- packages the stripped program with its license and musl's
  (`LICENSE`, `LICENSE.musl`) via `mkopk.py`.

`store/linux/common.sh` holds the shared steps.

| Package | Source | License |
| --- | --- | --- |
| `org.lua.lua` 5.4.7 | `lua-5.4.7.tar.gz` from lua.org | MIT, taken from the source's `lua.h` |
| `org.sqlite.sqlite3` 3.46.1 | `sqlite-amalgamation-3460100.zip` from sqlite.org | Public domain (`store/linux/sqlite/LICENSE`) |

Both need `requires-opos: 0.1.5`, the first version with the system
calls from step 1. An older OPOS lists them as needing an update.

**Toolchain.** `LINUX_CC` is `gcc -specs build/musl-local.specs` when
that file exists (musl unpacked in `build/musl-local`), and otherwise
`musl-gcc` from the distribution (`apt install musl-tools`).
`MUSL_LICENSE` is musl's copyright file from the same place. Both can be
set on the `make` command line. Fetching a pinned toolchain is not done
yet.

**QEMU test.** `build/store/` on the test disk as the package source:
the Store window lists Hello, Sketch, Lua and SQLite. INSTALL installs
Lua and SQLite, each "MAY ASK FOR: NOTHING BEYOND ITS OWN FILES".
`lua -e "print(6*7, _VERSION)"` prints `42 Lua 5.4`, and
`sqlite3 -version` prints `3.46.1 2024-08-13 ...`.

## Tests

QEMU on an installed disk, programs installed with `pkg install` as
`platform: linux` packages:
- `lua t.lua` prints every line of the script, including the 2000-line
  file in `$HOME` and a 600-character line.
- The Lua prompt answers `print(2^10)` with `1024.0`.
- `sqlite3 t.db` with `.read t.sql` prints `5003|5003`, `alpha`, `beta`,
  `gamma` and `3.46.1`; a later run reads `count(*)` = 5003 back.
- The built-in `musl-test`, `musl-signal-test`, `musl-pthread-test` and
  `musl-net-test` pass from the desktop and the kernel shell.

`run elf`, `/tests/exec-test`, `filetest` and `proctest` exit with
status 1 in the kernel shell. They do the same on `main` before these
changes, so the cause is older; it is recorded in `agents/agent-notes.md`.
