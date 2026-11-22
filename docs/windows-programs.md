# Windows programs on OPOS

How unmodified Windows programs (`.exe`) can run on OPOS, what OPOS
needs for that, and in which order to build it. Status: plan, nothing
built yet. `docs/linux-programs.md` and `docs/linux-compat.md` describe
the Linux layer this plan builds on; `docs/packages.md` already has
`platform: windows` in the package format, hidden until this works.

## Approach: Wine, unchanged, as a Linux program

Two ways were considered.

1. **An own Windows layer in the kernel**: a PE loader plus our own
   `kernel32`, `user32`, `gdi32`, `ntdll` and the rest. Wine has spent
   30 years on this and still has gaps. Not realistic.
2. **Wine as a Linux program** (chosen). Wine is a Linux program that
   loads Windows programs and supplies the Windows libraries. OPOS
   already runs unmodified Linux programs. Everything Wine needs from
   Linux that OPOS lacks goes into OPOS's Linux layer, not into Wine.

Why 2:
- The project rule: third-party software runs unchanged. Wine is built
  from a pinned release with a checked SHA-256, like every other Linux
  package (`store/linux/<name>/`), and is not patched.
- Wine is LGPL 2.1 or later, so OPOS may redistribute it with its
  license and its source.
- Every gap filled for Wine also helps other Linux programs: Unix
  sockets, shared memory, a Wayland display server, ALSA sound.
- Wine's "new WoW64" mode (Wine 9 and newer, built with
  `--enable-archs=i386,x86_64`) runs 32-bit Windows programs in a 64-bit
  process. OPOS then needs no 32-bit Linux system calls, only a 32-bit
  code segment (step 1).
- Wine 9 and newer has a Wayland display driver (`winewayland.drv`).
  A Wayland server is much smaller than an X server, so OPOS needs no
  X11.

To check at the start of step 1: whether Wine builds with musl (Alpine
Linux ships a musl Wine, so it should), and which Wine release has the
most complete Wayland driver.

## What OPOS lacks today

From `docs/linux-compat.md` and `docs/linux-programs.md`:

1. **Address space.** Windows programs expect the low 4 GiB: 32-bit
   programs live there, and many load at fixed addresses such as
   `0x400000`. OPOS puts PIE programs at `0x04000000`, the dynamic
   linker at `0x10000000` and the stack below `0x80000000`, and
   `mmap` has a 1 GiB area. Wine reserves large `PROT_NONE` ranges at
   fixed addresses and expects `MAP_NORESERVE` to cost nothing; OPOS
   backs `PROT_NONE` pages with memory.
2. **`MAP_SHARED`.** Not implemented. Wine and wineserver share
   memory through `mmap` of the same file or memfd.
3. **Unix sockets.** `AF_UNIX`, `socketpair` and passing descriptors
   (`SCM_RIGHTS`) are missing. Every Wine process talks to `wineserver`
   that way. Wayland needs them too.
4. **Waiting on many descriptors.** `poll` and `epoll` must work on
   sockets, pipes and descriptors that wineserver hands out.
5. **Signals with full context.** Wine turns `SIGSEGV`, `SIGILL`,
   `SIGTRAP` and `SIGFPE` into Windows exceptions. It reads and changes
   the registers in `ucontext_t` and returns through `rt_sigreturn`.
   OPOS delivers `SA_SIGINFO` but has not been tested with changed
   contexts.
6. **Segments.** Wine uses `ARCH_SET_GS` (OPOS has only
   `ARCH_SET_FS`) and runs 32-bit code with a 32-bit user code
   segment in the GDT, as Linux has.
7. **Graphics.** No X11 or Wayland server.
8. **Sound.** No ALSA devices (`/dev/snd`). Wine's `winealsa.drv` uses
   them through alsa-lib.
9. **Package dependencies.** A Windows package needs the Wine package.
   `docs/packages.md` leaves `needs:` and `runtime` packages
   undesigned.

## Order of work

Each step ends with a check in QEMU, and in VMware by the user.

1. **Kernel groundwork, no windows.** Fill gaps 1-6: a Linux-like
   address space for Linux programs (programs and libraries placed
   high, the low 4 GiB left free, reservations without memory),
   `MAP_SHARED` for files and memfd, `AF_UNIX` stream and datagram
   sockets with `SCM_RIGHTS`, `poll`/`epoll` on all of them,
   `ARCH_SET_GS`, a 32-bit user code segment, and changed signal
   contexts. Each gets a small musl test program like the existing
   `musl-*-test`s.
   Check: a `store/linux/wine/` recipe builds Wine without X11 or
   Wayland; `wine cmd /c echo hello` prints in the Terminal; small
   32-bit and 64-bit console test programs (built here with MinGW)
   run, including one that catches an access violation.
2. **Console programs.** Windows console programs run in the OPOS
   Terminal; the Wine prefix (`C:` drive) lives in the account's
   AppData. Files can open an `.exe` with Wine.
   Check: `cmd.exe` takes typed commands; a MinGW-built program reads
   and writes files on `C:` and on the OPOS disk (`Z:`); a program
   that opens a window fails with a clear message, not a hang.
3. **Wayland display server.** In the desktop: a socket at
   `$XDG_RUNTIME_DIR/wayland-0` with `wl_compositor`, `wl_shm`,
   `wl_subcompositor`, `xdg_wm_base`, `wl_seat` (pointer and keyboard
   with an xkb keymap), `wl_output`, and later `wl_data_device` for the
   clipboard. Each `xdg_toplevel` becomes an OPOS window with OPOS
   decorations.
   Check: Wine's `notepad`, `winemine` and `regedit` draw, take the
   mouse and keyboard, resize and close. A GTK or SDL program using
   Wayland directly also works, which helps the Linux side.
4. **Sound.** ALSA PCM devices (`/dev/snd/controlC0`,
   `/dev/snd/pcmC0D0p`: hardware and software parameters, write,
   state ioctls) that feed the OPOS mixer (`docs/sound.md`).
   Check: a Windows program plays a WAV file through `winmm` and through
   `DirectSound`, recorded with the QEMU sound-capture recipe.
5. **Store and desktop.** Design `runtime` packages and `needs:`; Wine
   becomes a runtime package. `platform: windows` packages become
   visible when Wine is installed. Each Windows package gets its own
   prefix in its AppData, a Start menu entry and the normal
   permissions sheet. Running a user's own `.exe` or `.msi` from Files
   stays possible.
   Check: a redistributable Windows program from the Store (for example
   PuTTY, MIT) installs, starts from Start, and uninstalls cleanly.
6. **3D, in software.** OpenGL through Mesa's software renderer
   (llvmpipe) on Wayland `wl_shm`; Direct3D 9-11 through Wine's own
   WineD3D on top of it. Vulkan (Mesa lavapipe) and DXVK later.
   Check: a small Direct3D 9 sample draws. Expect low speed: real GPU
   drivers are a separate, much larger project.
7. **Games and Steam.** Steam and Proton need glibc, 32-bit Linux
   programs, Vulkan and Linux namespaces (`bwrap`). Out of scope until
   steps 1-6 are done; then a separate plan.

## Not planned

- Windows kernel drivers, anti-cheat and copy protection that needs
  them.
- Changing Wine or any Windows program to make it work on OPOS.
- Shipping Windows programs in the Store whose license does not allow
  it. Users can still run their own.
