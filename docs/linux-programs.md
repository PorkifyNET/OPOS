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

1. **Devices.** Done in step 3: `/dev/null`, `/dev/zero`, `/dev/full`,
   `/dev/random` and `/dev/urandom`. Still missing: `/dev/tty` (opening
   it fails with `ENXIO`, as with no controlling terminal), listing
   `/dev`, `/proc`.
2. **Identity.** Done in step 3: one non-root uid per account, and
   generated `/etc/passwd` and `/etc/group`.
3. **Limits.** Raised in step 4 (see there). Still bounded: one
   argument page (64 arguments, 64 environment strings, 2048 bytes of
   strings), a 64 MiB `mmap` area, every page allocated at once (no
   demand paging), no `MAP_SHARED` or `MAP_FIXED`.
4. **Terminal.** Done in step 5 (see there). Still missing: `SIGWINCH`
   on resize, mouse reporting, Unicode beyond ASCII, function keys,
   job control (Ctrl+Z, process groups).
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
3. Done. `/dev` devices and a non-root identity. See "As built in
   step 3".
4. Done. Higher limits: descriptors, open files, executable size,
   stack, heap, `mremap`, `getrlimit`. See "As built in step 4".
5. Done. Terminal: raw mode, escape sequences in the desktop Terminal,
   Ctrl+C and Ctrl+D. See "As built in step 5".
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

## As built in step 3

**Devices.** Opening one of these paths gives a kernel-made file, not
a file on disk. No folder permission is needed, as on Linux.

| Path | Read | Write | `stat` |
| --- | --- | --- | --- |
| `/dev/null` | end of file | accepted, discarded | character device 1:3 |
| `/dev/zero` | zeros | accepted, discarded | 1:5 |
| `/dev/full` | zeros | `ENOSPC` | 1:7 |
| `/dev/random`, `/dev/urandom` | random bytes, as `getrandom` | accepted, discarded | 1:8, 1:9 |
| `/dev/tty` | open fails with `ENXIO` | | 5:0 |

`stat`, `access` and `lseek` (always 0) work on them.

**Identity.** A process of an account has uid and gid
`1000 + account index`. A process without an account (one started from
the kernel shell) is root, uid 0.
- `getuid`, `geteuid`, `getgid`, `getegid`, `getresuid` and `getresgid`
  return that id.
- The `setuid` family succeeds only for the ids the process already has;
  any other id gives `EPERM`.
- `stat` reports every file as owned by the calling process, because
  FAT has no owners.

**`/etc/passwd` and `/etc/group`.** These are made for the reading
process. They contain `root` and the process's own account: its name,
uid, gid, `HOME` (its storage folder when confined) and `/bin/sh`. Other
accounts are not listed. Both files are read-only (`EACCES`).
`getpwuid`, `getpwnam` and `getgrgid` work.

**QEMU test.** A static musl test program, installed as a Linux
package, passes 22 checks:
- each device's reads, writes, `fstat` and `stat`;
- `freopen` of `stderr` to `/dev/null`;
- uid 1000;
- `getpwuid`, whose home equals `HOME` and whose name equals `USER`;
- `setuid(0)` fails with `EPERM`;
- `HOME` is owned by the process's uid.

SQLite runs without errors as a non-root user. No unimplemented system
call was logged.

## As built in step 4

| Limit | Before | Now |
| --- | --- | --- |
| Descriptors per process | 11 | 64 (`RLIMIT_NOFILE`) |
| Open files system wide | 32 | 128 |
| Executable file | 1 MiB | 16 MiB |
| Loaded pages per image | 512 (2 MiB) | 4096 (16 MiB) |
| Main stack | 128 KiB | 512 KiB (`RLIMIT_STACK`) |
| `brk` heap | 512 KiB | 32 MiB |
| Arguments, environment strings | 16, 16, 1024 bytes | 64, 64, 2048 bytes |
| `mmap` area | 64 MiB | 64 MiB (unchanged) |

- **Executable buffer.** Each task's buffer is allocated at 1 MiB and
  grows in 1 MiB steps, only when a larger program is started. A
  replaced buffer's pages are freed. The contiguous allocator carves
  each buffer from never-used memory, so a very large program can fail
  with `ENOMEM` once that memory is used up.
- **`mremap` (25).** Shrinks in place, or grows in place when the pages
  after the mapping are free. Otherwise, with `MREMAP_MAYMOVE`, it moves
  the pages to a free run by remapping them, without copying.
  `MREMAP_FIXED` gives `EINVAL`. musl's `realloc` of large blocks uses it.
- **`getrlimit` (97) and `prlimit64` (302).** These work for the calling
  process. `RLIMIT_NOFILE` and `RLIMIT_STACK` report the limits above;
  the other resources are unlimited. A lower limit is accepted but not
  enforced. Raising a limit gives `EPERM`.
- **Kernel memory.** The kernel's static memory grows from about 10 MiB
  to 14 MiB, mostly per-process page lists. Each process's stack now
  takes 512 KiB at start.

**QEMU test.** A static musl test program with 3 MiB of data, a 3.1 MB
executable installed as a Linux package, passes 15 checks:
- its data loads;
- `RLIMIT_NOFILE` reports 64;
- 61 opens succeed, ending at fd 63, and the next open gives `EMFILE`;
- 400 KiB of stack;
- 8 MiB of `brk`;
- `mremap` grows with a move and keeps the data, shrinks, and fails a
  growth that cannot move;
- `realloc` grows to 16 MiB;
- a `fork` and `execv` with 61 arguments.

SQLite, Lua and the browser still run.

## As built in step 5

**Line discipline (kernel).** The console has one set of termios
settings, shared like one tty. `TCGETS`/`TCSETS*` read and change it, and
the desktop restores the defaults when a program from the Terminal ends.
- **Line mode (`ICANON`):** Backspace and Ctrl+U edit, and Enter ends
  the line. Ctrl+D ends input: `read` returns what was typed, or 0.
- **Raw mode:** `VMIN` and `VTIME` decide how long `read` waits. Bytes
  arrive as typed. `ICRNL` maps Enter (CR) to LF.
- **`ECHO`:** shows typed text.
- **`ISIG`:** Ctrl+C sends `SIGINT` and the waiting `read` returns
  `EINTR`.
- **`poll` and `select`:** report standard input readable when keys are
  waiting.
- **Other ioctls:** `FIONREAD`, `TIOCGPGRP`/`TIOCSPGRP`, `TIOCGSID`,
  `TCFLSH`, `TCSBRK` and `TIOCSWINSZ` are accepted.
- **`TIOCGWINSZ`:** reports the Terminal window's size.
- **`TERM`:** set to `xterm`.

**Keys.** While a program without its own window runs from the
Terminal, every key goes to it, as a Linux terminal sends it:

| Key | Sent |
| --- | --- |
| Enter | CR |
| Backspace | DEL |
| Arrows | `ESC [ A`–`D` (`ESC O A`–`D` in application cursor mode) |
| Home, End | `ESC [ H`, `ESC [ F` |
| Delete | `ESC [ 3 ~` |
| Page Up, Page Down | `ESC [ 5 ~`, `ESC [ 6 ~` (alternate screen only; otherwise they scroll the Terminal) |
| Esc, Tab | themselves |
| Ctrl+letter | control character |

Ctrl+V pastes the clipboard. Ctrl+C sends `SIGINT` to the program and
the processes it forked. A second Ctrl+C ends them if the first came
less than 2 seconds earlier, or was never delivered because the
program makes no system calls. A program with a window (the browser,
Sketch) leaves the prompt free, as before.

**Terminal emulator (desktop).** A VT100/xterm subset:
- **Screen:** the last rows of the scrollback.
- **Cursor:** movement and positioning (`CUU`…`CUP`, `HVP`, `VPA`,
  `CHA`), save and restore.
- **Erasing:** `ED`, `EL` and `ECH`, keeping the background color.
- **Editing:** insert and delete lines and characters.
- **Scrolling:** scroll regions (`DECSTBM`), `SU`/`SD`, index and
  reverse index.
- **Colors:** SGR with 16 colors, bold, reverse, and the first 16 of 256
  colors.
- **Modes:** the alternate screen (`?1049`, `?1047`, `?47`), cursor
  hiding (`?25`), application cursor keys (`?1`).
- **Replies:** cursor position reports (`DSR 6`) and device attributes.
- **Ignored:** window titles (OSC) and character set selection.
- **Text:** tabs every 8 columns, and auto-wrap at the right edge.
- **UTF-8:** each character outside ASCII is drawn as `?`.

The screen has as many rows as the window shows and up to 96 columns.
Leaving the alternate screen restores the text that was there.

**QEMU test.** A static musl test program checks:
- `isatty`, the window size and `TERM`;
- colors, reverse and background, carriage return, tabs, `EL`, and
  UTF-8 drawn as `?`;
- a full-screen box in the alternate screen with a full-width
  reverse-video status line, raw keys (Up arrives as `1b 5b 41`), and
  `q` quitting back to the old text;
- Ctrl+D ending input after two lines;
- a `SIGINT` handler catching Ctrl+C;
- a busy loop ended by the second Ctrl+C;
- `poll` on standard input reporting a typed key.

The Lua and SQLite prompts work and Ctrl+D leaves them. The browser
started from the Terminal leaves the prompt usable.

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
