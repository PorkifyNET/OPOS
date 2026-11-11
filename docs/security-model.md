# Security model

This page describes how OPOS separates programs from the kernel and from
each other's files, what was checked in the November 2026 review, and what
is still missing. It is a description of the current code, not a promise.
Check `kernel/permissions.c`, `kernel/user.c`, `kernel/paging.c` and
`kernel/interrupts.S` when in doubt.

## Kernel and programs

- Programs run in ring 3. Each process has its own address space: the
  lower half of the page tables is private, the upper half (kernel) is
  shared and mapped supervisor-only.
- User pages are mapped with exactly one of read, read+execute or
  read+write (W^X). The loader refuses segments that would be both
  writable and executable. NX is required to boot.
- CR0.WP is on, so the kernel cannot write through read-only mappings.
- SMEP is on when the CPU has it: ring 0 never executes code from user
  pages. UMIP is on when the CPU has it: ring 3 cannot read the GDT and
  IDT addresses with SGDT/SIDT.
- Every pointer a system call receives is checked against the current
  page tables (`paging_user_range_valid`) before use: present, user,
  writable when written. System calls run with interrupts off, so nothing
  can unmap the memory between the check and the use, except where a call
  blocks. Calls that block (keyboard and pipe reads, pipe writes, waits)
  check the memory again after they wake up.
- Before returning to a program with SYSRET, the kernel cleans the saved
  registers (`user_linux_return_check`): RFLAGS keeps only the arithmetic
  flags, DF, AC and ID (never IOPL, so programs cannot reach I/O ports),
  and the return address, stack and FS base must be canonical user
  addresses. Otherwise the process ends with signal 11 instead of the CPU
  faulting in ring 0. `rt_sigreturn` restores registers from user memory,
  which is why this check matters.
- The FPU state restored by `rt_sigreturn` has MXCSR masked to the bits
  the CPU supports; otherwise FXRSTOR would fault in ring 0.
- Program windows: only the process that opened the window can draw in it
  or read its events. Presented pixels are checked and copied into a
  kernel buffer with interrupts off. While a program waits for an event,
  the desktop runs inside that system call with interrupts on; the event
  buffer is checked again before the kernel writes it. Keyboard input goes
  to a program only while its window has focus; the Super shortcuts
  always stay with the desktop. A program that ignores a close request is
  ended (status 137) the next time it waits.
- CPU exceptions in ring 3 end only that process. A fault in ring 0 still
  halts the machine (see "Not done yet").

## Accounts and files

FAT has no owners or permission bits, so `kernel/permissions.c` decides by
location. Names are compared case-insensitively, and an existing entry is
compared by identity (device and inode), so 8.3 aliases such as
`ALEXAN~1` and spellings such as `/disk/opos/accounts.dat` do not get
around the rules. Paths must be canonical (no `.`, `..` or `//`); the
kernel resolves them before asking.

| Location | Built-in programs | Files and editor (standard) | Files and editor (administrator) |
| --- | --- | --- | --- |
| `/disk/OPOS`, `/disk/boot`, `/disk/EFI`, `/disk/OPOS.CFG` | read | read | read and change contents |
| `/disk/OPOS/ACCOUNTS.DAT` | no access | no access | no access |
| `/disk/Users` | read | read | read |
| `/disk/Users/<own name>` | full | full | full |
| `/disk/Users/<other name>` | no access | no access | full |
| `/disk/Users/<any name>/AppData/Permissions.dat` | no access | read (own home) | read |
| everything else (`/disk`, `/disk2`..., the initramfs) | full (initramfs is read-only) | full | full |

`/disk/OPOS.CFG` is a binary settings file. Administrators can change its
contents through Settings; the built-in text editor opens binary files
read-only. For a direct confirmation check in Files, create a text file inside
`/disk/OPOS` while logged in as an administrator.

The protected folders themselves (`/disk/OPOS`, `/disk/Users`, a home
folder, `/disk` itself) cannot be deleted or renamed by anyone, programs
or windows. Homes go away with their account (and are kept on deletion).
`ACCOUNTS.DAT` is only touched by the account code in the kernel.
`Permissions.dat` holds per-program capability grants for the application
sandbox (`docs/app-sandbox.md`). Only the kernel writes it.

When an administrator uses the built-in Files or editor to write, create,
rename, delete or move something in a protected system location, the desktop
shows a warning first. It checks the logged-in administrator's password when
one is configured. A successful confirmation authorizes only that queued
action; it does not unlock the location for later actions or for programs.

Programs run as the account that started them, and child processes
(`fork`, `execve`, spawn) keep that identity. Without an account logged
in, a computer without accounts belongs to whoever is at the keyboard
(administrator); a computer with accounts gives a standard identity with
no home folder (for example the live session started from the ISO).

An executable launched from a disk (`/disk`, `/disk2`, etc.) is confined to
writing under `/disk/Users/<account>/AppData/Programs/<id>`. The kernel
creates this directory when it can; the app obtains its path with
`opos_app_storage()`. If there is no logged-in account or the directory
cannot be created, disk apps have no writable files. The ID is derived
from the executable path, so moving or renaming the executable gives it
new storage. `fork` and spawn inherit the limit, and `execve` cannot
remove it. When an unrestricted process executes a disk app, all open
filesystem handles are closed before it starts. Disk apps cannot call the
administrator update API. Built-in programs from the read-only initramfs
keep the location-based permissions in the table above.

Disk apps can read their own app storage, system files, and the boot
image (`/bin`, `/lib`, fonts, certificates). Shared files on `/disk..`
and `/cdrom..` need the `files.shared` capability, except inside the
app's own program folder (`<volume>/Programs/<folder>`, when the
executable is below such a folder). To read any other file, they call
`opos_pick_file()`. Files shows the requesting program and lets the user
choose a file or cancel. The selected file gets one read grant, checked
against its device and inode so replacing the file at that path does not
extend the grant. A new selection replaces the path grant for future opens;
file handles already opened remain valid. Grants follow `fork` and
`execve` but never allow writing outside app storage.
The picker cannot grant access to another account's home or to the
password file.

Each process also carries a capability set. Built-in programs hold every
capability. A disk app holds `storage`, `files.pick`, `clipboard.write`
and `window`, plus what the account granted that executable (keyed by its
SHA-256) in `Permissions.dat`, minus what was refused. Children inherit
the set and can only narrow it. The kernel checks `files.shared` (shared
file reads), `net` (at `socket()`), `clipboard.read` and
`clipboard.write`. Any disk app may ask once for `net` and
`clipboard.read`, through a desktop prompt the program cannot draw over
or click. A program with a manifest (`OPOS_MANIFEST`) may ask only for
what the manifest lists, including `files.shared`. Without a manifest,
only disk apps that were in `/disk/Programs` at an account's first login
with these rules may ask for `files.shared`. Answers are stored per
executable hash; without an account they last for that process only.
Start > SECURITY CENTER lists the stored answers and changes them, or
forgets a program. Denying there also applies to running copies at once.

Programs run side by side with the desktop. Logging out, or leaving the
desktop for the kernel shell, first ends every running program, so none
keeps running as the account that left. Ending a program also wakes it
from any wait (a pipe, a child, a futex, the keyboard), so it cannot
hide there.

Denied calls return `EACCES` (13). Files shows "ACCESS DENIED".

`sdk/examples/permtest.c` checks the program side of this table.

## Kernel shell

The kernel shell can read and write any disk sector, so it is for
administrators. When the computer has accounts and no administrator is
logged in, the shell asks for an administrator name and password before
it accepts commands. This covers "boot into shell once" (Ctrl+Reboot), a
computer set to start without the desktop, and the Terminal's `shell`
command.

Physical access is still full access: anyone can boot the ISO and read the
disk, because nothing is encrypted.

## Not done yet

- No SMAP: the kernel reads user memory with plain pointers after
  checking it. SMAP would need every such access wrapped in STAC/CLAC.
- A page fault in ring 0 halts the machine. A kernel bug that touches a
  bad user address is a denial of service, not a contained error.
- `getuid` still returns 0 for every program (the Linux tests expect it).
  OPOS programs ask for their account with the native `USER_INFO` call.
- Shared disk files remain readable to all programs, and built-in programs
  retain account-wide read access. A broader app privacy model remains.
- No disk encryption, no verified boot, no signed updates.
- The kernel is written in C and has had no fuzzing or external audit.
