# Recovery environment

When OPOS cannot start, or starts but is unusable, a user who never touches
the Terminal must still be able to get the system working again, or at least
get their files out. Today the only fallback is the text recovery shell
(`kernel/main.c`), which starts when the disk system root fails verification,
and the "OPOS (previous version)" boot entry that an update adds.

This plan adds **OPOS Recovery**: a full-screen, mouse-and-keyboard screen
drawn by the kernel, like the installer. It does not depend on `/bin`, the
disk system root, the login screen, startup programs or the Store, so it
works when those are what broke.

## How users get there

1. **Boot menu entry "OPOS Recovery".** Limine entry with
   `cmdline: opos.recovery`, pointing at the current kernel. New installs
   get it from the installer. Existing installs get it on their next boot,
   the same way `boot_display_add_previous_entry()` adds the previous
   version entry (idempotent, every `limine.conf` copy).
2. **Automatically after failed starts.** The kernel counts start attempts
   in `/disk/OPOS/BOOT-ATTEMPTS` (written and flushed right after the disk
   mounts). The count resets once the desktop has run for 60 seconds after
   a login, or after a clean restart or shut-down. When the count reaches 3,
   the next start opens Recovery and says why ("OPOS did not start
   correctly the last 3 times"). Limine's 3 s menu is easy to miss, so this
   is the path most users will take.
3. **Instead of the recovery shell.** When the disk system root fails
   verification, or the `opos.root=` pin does not match, Recovery opens with
   that reason. The text shell stays available from Recovery.
4. **From the desktop.** Settings > UPDATES (or a new RECOVERY tile) gets
   "RESTART INTO RECOVERY", using a one-shot flag like the existing
   boot-shell-once setting.
5. **From the OPOS ISO or USB stick** (later step). The live session's
   first page gets "REPAIR AN INSTALLED OPOS", for when the installed
   kernel itself no longer starts.

## Who may use it

Recovery can read every account's files and change system files, so it asks
an administrator to sign in with their password first (`accounts.c`). If
some administrator has no password, it opens directly: that person could
sign in to the desktop without one anyway. "Restart" and "Shut down" never
need a sign-in. The administrator stays logged in while Recovery is up, so
COMMAND LINE opens without the shell's own login; START OPOS NORMALLY logs
out first, so the normal login screen still decides who uses the desktop.

This is a lock against casual use, not against someone with the computer
in hand: without disk encryption they can read the disk from another
system.

## The Recovery screen

A title, the reason Recovery opened (if any), and large buttons:

| Button | What it does |
| --- | --- |
| START OPOS NORMALLY | Continues the normal start. The attempt counter stays, so a start that fails again comes straight back to Recovery; one that lasts clears it. With damaged system files on the disk, OPOS runs from the copy in its `initramfs.tar`. |
| SAFE START | Starts the desktop with the basics only: no startup programs, no saved display mode (firmware/Limine resolution), no sound, no autostarted Store or update checks. For "the desktop starts but something on it breaks". |
| UNDO THE LAST UPDATE | Shown when `/disk/boot/old/kernel.elf` exists. Swaps the current and previous kernel/initramfs pairs, so the next start runs the previous version, then restarts. Doing it twice gets back to the newer one. |
| REPAIR SYSTEM FILES | Re-checks the disk system root against the booted `initramfs.tar`, removes a damaged generation and stages it again (`installer_stage_system_root()`). With OPOS installation media present, it can also replace a damaged `kernel.elf`/`initramfs.tar` from that media (same version or newer only). |
| RESCUE MY FILES | Copies chosen accounts' folders (`/disk/Users/<name>`) to a USB stick or another disk, into `OPOS-RESCUE-<date>/`, with progress and a summary of files that could not be read. |
| RESET SETTINGS | Resets display mode, appearance and desktop settings (`OPOS.CFG`, appearance files) to defaults. Accounts, files and programs stay. |
| RESET A PASSWORD | Administrator only: sets a new password for an account, or removes it. |
| VIEW START-UP LOG | Shows the kernel log of this start, scrollable, with "SAVE TO USB" so it can be attached to a bug report. |
| COMMAND LINE | The existing text shell, for advanced users. |
| RESTART / SHUT DOWN | As on the desktop. |

Every action that changes the disk says what it will do and asks for
confirmation first. Every action reports what happened in plain words.

## Steps

Each step is its own PR, tested in QEMU (installed-disk images, KVM), with a
VMware check by a human afterwards.

1. **Recovery screen and ways in.** `opos.recovery` boot option (parse the
   command line as space-separated words, so `opos.root=` and
   `opos.recovery` can be combined), the boot menu entry for new and
   existing installs, the attempt counter, replacing the automatic recovery
   shell, administrator sign-in, START OPOS NORMALLY, COMMAND LINE, RESTART
   and SHUT DOWN. *Done:* `kernel/recovery.c` (counter and reason),
   `boot_display_add_recovery_entry()` (added on every installed start
   when missing), and the screen in `kernel/desktop.c` (shown as a login
   screen, so no programs start behind it). The count resets 60 s after a
   login, and on a restart or shut-down from the desktop, Terminal or
   shell.
2. **Undo the last update and repair system files.** *Done:*
   `recovery_undo_update()` copies the older pair to `boot/kernel.new` and
   `boot/initramfs.new` and this pair to `boot/undo/`, then switches by
   renames (initramfs first, kernel last) recorded in
   `/disk/OPOS/UNDO-STATE`, so the boot files are never half-written. A
   start cut short is finished by `recovery_undo_resume()`, which tells by
   the running kernel's hash whether it still is the newer one (finish and
   restart) or already the older one (finish and carry on).
   `installer_repair_system_root()` checks the current generation against
   the booted `initramfs.tar`, rewrites the files that differ, and removes
   and stages the whole tree when that is not enough (extra files).
   Replacing a damaged `kernel.elf`/`initramfs.tar` from installation media
   moves to step 5.
3. **Rescue my files** to a USB stick or another disk. *Done:*
   `recovery_rescue_files()` walks each chosen `/disk/Users/<name>`
   without recursion (24 levels) and copies it to
   `OPOS-RESCUE-<date>[-2..-9]/<name>` on a mounted extra FAT volume.
   Unreadable files are skipped and counted; a failed write (drive full
   or removed) stops the copy and removes the half-written file. The
   Recovery page lists accounts (all ticked) and drives (USB sticks show
   up when plugged in).
4. **Safe start, reset settings, reset a password**, and "RESTART INTO
   RECOVERY" in Settings.
5. **Start-up log viewer** and **"REPAIR AN INSTALLED OPOS"** on the live
   ISO/USB, including replacing a damaged kernel or initramfs from that
   media.

Later ideas, not planned yet: a FAT disk check (lost clusters, cross-linked
files), restore points of settings and installed programs ("System
Restore"), and a smaller recovery-only initramfs (see
`docs/system-root-migration.md`).

## Testing

- Installed disk image from WSL `~/opos-installed-clean.img`, booted with
  KVM; the Limine entry is chosen by sending keys during the menu.
- Failed-start counter: kill QEMU during start three times (or write
  `BOOT-ATTEMPTS` with `mount -o loop`), then check that Recovery opens.
- Damaged root: flip one byte of a file in the generation, as in
  `tools/test_disk_root_qemu.sh`.
- Undo update: use the two-version setup of `tools/test_update_root_qemu.sh`.
- Rescue files: attach a second FAT image as a USB stick (`-device
  usb-storage`) and compare the copied tree.
