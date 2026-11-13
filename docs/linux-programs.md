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
   `/dev/random` and `/dev/urandom`; `/dev/tty` in step 6. Still
   missing: listing `/dev`, and `/proc` (BusyBox `ps`, `top` and `free`
   need it).
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
7. **Dynamic linking.** Dynamically linked musl programs work since
   step 6, with musl's own dynamic linker. glibc programs need far
   more (glibc's loader, `/proc`, and many more system calls).
8. **Graphics.** Linux GUI programs draw through X11 or Wayland, which
   OPOS does not have. SDL 2 programs work since step 7, with an SDL
   video driver that draws into an OPOS window. Still missing: OpenGL,
   sound, game controllers, and programs that use X11 or Wayland
   directly (GTK, Qt).

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
6. BusyBox and dynamic linking done (see "As built in step 6").
7. Done. Graphics through SDL 2 (see "As built in step 7").

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
| `/dev/tty` | the console, as standard input (step 6) | the console | 5:0 |

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

## As built in step 6

**BusyBox.** `store/linux/busybox/build.sh` builds BusyBox 1.36.1 static
with musl (the default configuration, without the few parts that need
glibc or kernel features: `tc`, NFS mounts, RPC, SELinux, PAM,
systemd). The binary is 1.2 MB. BusyBox is GPL-2.0, so the package
also carries the exact source release and the configuration used
(`source/` in the program folder, 3.7 MB in all). It is not in the
default `STORE_LINUX`; publishing it is a separate decision. To build
it with the rest of the Store, run `make store STORE_LINUX="lua sqlite busybox"`.

**What works.** Tested from the desktop Terminal:
- `busybox sh` (ash) with its line editing and history;
- pipes, redirections, background jobs (`&`, `$!`, `wait`, `kill`);
- Ctrl+C interrupting a command while the shell keeps running, and
  `trap`;
- `vi` editing (insert, arrows, `dd`, `:wq`);
- `less` paging and search;
- `ls -l`, `cat`, `seq`, `sort`, `awk`, `grep`, `wc`, `uptime`.

**Kernel fixes and additions it needed:**
- **Syscall return.** `interrupts.S` read the `bool` results of the
  syscall dispatch, signal delivery and return check as 64-bit values.
  Only the low byte is defined, so a normal return could be taken as
  "exit with the current `rax`". This ended a shell after a signal
  handler ran, and its behavior changed with unrelated code changes.
  The results are now zero-extended.
- **Console copies.** `dup` and `fcntl(F_DUPFD)` of descriptors 0-2 give
  an open `/dev/tty`, and `/dev/tty` itself opens. Before, ash could not
  keep its terminal descriptor and turned job control off.
- **`wait4`.** It accepts `WNOHANG`, `WUNTRACED`, `WCONTINUED` and a
  resource-usage pointer. Before, ash with job control got `EINVAL` and
  showed its prompt before commands ended.
- **`SIGCHLD`.** A parent gets it when a child ends.
- **`rt_sigsuspend` (130).** Waits with a temporary mask.
- **`kill` (62).** A process may signal itself and the processes it
  started.
- **`nanosleep`.** A signal interrupts it (Ctrl+C on `sleep`).
- **Process groups.** `getpgrp`, `getpgid`, `getsid`, `setpgid` and
  `setsid` treat every process as its own group and session.
- **`getppid` and `sysinfo`.**
- **Terminal.** After a resize, a full-screen scroll region stays full.
  Before, it kept the first size of 24 rows, so `less` scrolled inside
  the top rows.

**Still missing for BusyBox:**
- `/proc` (`ps`, `top`, `free`);
- `sendfile` (`cat` falls back to read and write);
- symbolic links (`readlink` now answers, but FAT has no links);
- file times (`ls -l` shows 1970);
- job control (Ctrl+Z, `fg`, `bg`).

A process killed by a signal is reported as exiting with 128 + the
signal number, not as signaled, so the shell prints `Done(143)` rather
than `Terminated`.

**Dynamic linking.** Programs linked against musl's shared libc run
with musl's own dynamic linker. The OPOS image carries musl's `libc.so`
as `/lib/ld-musl-x86_64.so.1` (0.7 MB, with musl's license in
`/share/licenses/musl.txt`). `make` copies it from `MUSL_LIBC_SO`, by
default the first of `build/musl-local/lib/x86_64-linux-musl/libc.so`
and the distribution's paths that exists; without one the image builds
without it. A package can carry its own shared libraries next to the
program and find them with an `$ORIGIN` run path
(`-Wl,-rpath,'$ORIGIN'`), or through `LD_LIBRARY_PATH`. musl's default
path is `/lib:/usr/local/lib:/usr/lib`.

Kernel changes:
- **Interpreter size.** The interpreter is read into a per-task buffer
  that grows like the program buffer, up to 4 MiB. Before, it was a
  fixed 64 KiB.
- **`MAP_FIXED`.** `mmap` places a mapping at the given address in the
  mmap area, replacing pages already there; `MAP_FIXED_NOREPLACE` fails
  with `EEXIST` instead. Without these flags an address is a hint.
  `MAP_DENYWRITE`, `MAP_EXECUTABLE`, `MAP_NORESERVE`, `MAP_POPULATE`,
  `MAP_NONBLOCK` and `MAP_STACK` are accepted and change nothing. musl
  maps each library's span and then each segment with `MAP_FIXED`.
- **`mprotect` on the program's segments.** The loader makes the main
  program's relocated data read-only (RELRO). A page may lose write or
  execute permission, or get back what its segment had; never more. A
  forked child maps the segments with their first permissions.
- **`readlink` and `readlinkat` (89, 267).** `/proc/self/exe` gives the
  program's path, which musl needs to expand `$ORIGIN`. Other existing
  paths are not links (`EINVAL`).
- **`membarrier` (324).** OPOS runs all threads on one CPU, so the
  barrier commands return at once. musl calls it in `dlopen`.
- **Faults.** A user page fault also logs the faulting address.

**Toolchain.** Dynamic linking needs the linker to find musl's
`libc.so`. Debian's musl package links `usr/lib/x86_64-linux-musl/libc.so`
to `/lib/x86_64-linux-musl/libc.so` with an absolute link, which an
unpacked copy in `build/musl-local` loses. Without it `-lc` finds
`libc.a`, and a "dynamic" program has libc linked in. It then needs
only the interpreter, but a shared library built this way carries its
own copy of libc, and its thread-local storage fails. A local specs file
must list `build/musl-local/lib/x86_64-linux-musl` before
`build/musl-local/usr/lib/x86_64-linux-musl` in `link_libgcc`.
`readelf -d program | grep NEEDED` shows `libc.so` when it is right.

**Tested** in QEMU, in a test package with three programs and one
library, each built with `gcc -specs build/musl-local.specs`:
- a program using only libc: `malloc`, `printf`, arguments and the
  environment;
- a program using its own `libgreet.so` through `$ORIGIN`. The library
  has a constructor, thread-local storage (`__tls_get_addr`) and a
  shared counter. The program calls it from the main thread and from a
  second thread, then `dlopen`s the library and calls it through
  `dlsym` (same symbol), and `dlopen`s a missing library (the error
  message). The output matches the same program on Linux line for line;
- from BusyBox `sh`: a program without a run path fails with musl's
  "Error loading shared library" message, and runs with
  `LD_LIBRARY_PATH`; `execve` and background jobs start dynamic
  programs.

The old test interpreter `/lib/opos-ld.so` and its libraries stay for
the kernel's own regression tests.

## As built in step 7

**SDL 2.** `store/linux/sdl2/build.sh` builds SDL 2.32.10 (pinned
release, SHA-256 checked) as a static library with musl, with a video
driver for OPOS windows: `store/linux/sdl2/opos/SDL_oposvideo.c`. The
script copies the driver next to SDL's dummy driver, whose sources
SDL's `configure` already compiles, and adds it to SDL's driver list,
where it comes first. Everything that needs a Linux device, server or
library OPOS lacks is off: X11, Wayland, KMS, OpenGL, Vulkan, sound
servers, udev, D-Bus. Sound, game controllers and haptics use SDL's
dummy drivers. The driver is zlib-licensed like SDL, so programs built
with it keep their own license.

**The driver.**
- One window per program, as OPOS gives a program one window. A second
  SDL window fails. The OPOS window opens when SDL first draws, after
  SDL has set the title.
- SDL draws in `SDL_PIXELFORMAT_RGB888`, OPOS's own pixel format, with
  its software renderer. Each frame is presented whole.
- Keys: presses and releases with SDL scan codes, from the new key-state
  events below; typed characters as text input. On a kernel without
  key-state events, each typed key becomes a press and a release.
- Mouse buttons, motion and the wheel.
- A resizable window follows the OPOS window's size. A fixed-size one
  keeps its size, shown at the top left.
- Closing the window sends SDL's quit event. After that the driver stops
  asking for events, because OPOS ends a program that waits again after
  its close event.
- The display modes offered for full screen are 1024x768, 800x600 and
  640x480. OPOS does not tell a program the screen size before it opens
  a window.

**Kernel additions:**
- **Key presses and releases.** The keyboard driver now keeps every key
  transition, not only typed characters. A program asks for them with
  `opos_window_options(OPOS_WINDOW_KEY_STATE)` (new call 0x4f500016) and
  gets `OPOS_EVENT_KEY_STATE` events with the PC scan code. A press
  comes before the character it types. A release goes to every window
  that got the press, so no key stays down when the focus moves. Other
  programs see no change.
- **`access("/proc")`.** `/proc` and `/proc/self` exist for `access`.
  SDL checks that before it reads `/proc/self/exe` to find the program's
  folder (`SDL_GetBasePath`).

**SDL Demos package** (`org.libsdl.sdl2-demos`, 5 MB, four Start menu
entries): SDL's `testsprite2`, `testdraw2`, `testrendercopyex` and
`checkkeys`, statically linked. Tested in QEMU from the Terminal:
- the sprites, drawing and rotation demos draw in their windows
  (testsprite2 at about 1100 frames per second);
- `checkkeys` reports presses, releases, Shift and text in typing order:
  Shift down, B down, text "B", B up, Shift up;
- closing a window with Super+Q ends each demo with status 0.

Mouse input was not tested: the QEMU monitor's `mouse_move` does not
move OPOS's absolute pointer.

**For SDL programs.** While `make store` runs, the recipe installs the
library into `build/store_opk/sdl2/`. A recipe for an SDL program can
build there with
`$LINUX_CC -I.../sdl2/include/SDL2 prog.c -L.../sdl2/lib -lSDL2 -static`.

## Doom

`store/linux/doom/build.sh` builds Chocolate Doom 3.1.1 (GPL-2.0) against
the SDL library above and packages it with Freedoom: Phase 1 0.13.0
(BSD-3-Clause), a complete free game for the Doom engine. The package is
`org.chocolate-doom.freedoom`, named Freedoom, about 33 MB, with one
Start menu entry. It carries the Chocolate Doom source, the licenses of
SDL and musl, and Freedoom's COPYING and CREDITS. Like the SDL demos it is
opt-in: add `doom` to `STORE_LINUX`. The recipe builds SDL itself when it
was not built yet (`SDL2_LIBRARY_ONLY=1`).

Chocolate Doom normally builds with autotools or CMake. The recipe writes
the few `config.h` lines they would and compiles the game's source list
with `$LINUX_CC` directly, without SDL_mixer and SDL_net. There is no
sound: OPOS has no sound driver yet.

Tested in QEMU: installed with `pkg install`, started from the Terminal,
it loads `freedoom1.wad` from its folder, opens a window titled
"Freedoom: Phase 1 - Chocolate Doom" and runs the title demo. The keys
work: the menus open, and in a game the player walks, turns, fires and
picks things up. Settings and saves go to
`$HOME/.local/share/chocolate-doom/` in its AppData folder. Closing the
window asks Doom to quit; Doom answers with its own "are you sure"
question, so Super+Q ends it instead.

What it needed from OPOS:
- **`mkdir` of a folder that exists.** `SDL_GetPrefPath` creates every
  folder of its path from the root down, as `mkdir -p` does. Linux
  answers EEXIST for a folder that exists even where the program may not
  write; OPOS answered EACCES, and SDL gave up. OPOS now answers EEXIST
  for an existing path the program may see (on the way to its own
  folders, or readable), and EACCES otherwise.
- **Faster large writes.** Installing the 33 MB package took about a
  quarter of an hour. Three changes in the FAT driver and the VFS:
  - Each read or write at an offset walked the file's cluster chain from
    the start, and so did finding its end before a write, so copying a
    file in chunks grew with the square of its size. The driver now
    remembers where it last walked a few chains to; any change to the FAT
    other than appending forgets that.
  - Allocating or freeing a chain wrote each FAT entry on its own: a read
    and a write of both FAT copies per cluster. Entries that share a FAT
    sector are now written together, and free clusters are looked for
    after the last allocation first.
  - Every sector written sent a cache flush to the disk. Each change to a
    file or folder (a VFS write, create, remove, rename, resize) now
    flushes once, when it is done. Batches nest
    (`block_begin_batch`/`block_end_batch`), so the package installer's
    own batches still decide when its steps are on the disk.

  Measured in QEMU (KVM under WSL 2, IDE disk): installing the Doom
  package went from about 15 minutes to about 6, removing it from 3–4
  minutes to under 40 seconds, and `dd` writing 1.6 MB in 4 KB blocks
  from 57 seconds to 19. Each disk command there still costs
  milliseconds of emulated port I/O. Writing several sectors per command
  would help most; it waits until the drivers that use DMA are checked
  for buffers that cross pages.

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

`run elf` and `proctest` pass in the kernel shell. `/tests/exec-test`
expects the argument `run elf` gives it, so run it through `run elf`, not
on its own. `run filetest` needs the files that
`tools/test_fat16_persistence.sh` writes first (`disk mkdir docs`, then
`created.txt` and `persist.txt`). `make persistence-test` runs all of
this on a fresh FAT16 disk and checks the disk image by file name.
