# Installed-disk system root migration (design draft)

This is a plan, not current OPOS behavior. The goal is to run an installed
system's `/bin`, `/lib`, `/etc` and `/share` from its own disk while keeping a
small initramfs for boot and recovery. The live ISO must continue to run from
its initramfs. The migration must preserve existing installations and user
files, and a failed update must leave a bootable previous system.

## Implementation status

The first VFS seam is present but is not selected by normal boot. After the
boot FAT volume is mounted, `vfs_select_disk_root()` can map `/` onto a
protected `/disk/OPOS/roots/<generation>` directory. It rejects missing roots,
other volumes, reserved mount-point names and a switch after file objects
have opened. The root alias is read-only; `/disk` still exposes the physical
volume. Permission checks classify both spellings as system files and block
changes to the selected tree, including through its physical path. A QEMU
smoke boot with a temporary selector call confirmed matching device/inode for
`/bin/notepad` and its backing file, and the focused permission test passes.
Normal builds still use the initramfs root. Installer population, signed
generation selection, boot activation, program-discovery changes and updates
remain future stages.

## Current contracts to preserve

| Area | Current reader or writer | Migration consequence |
| --- | --- | --- |
| Boot | `kernel/main.c` requires Limine's `initramfs.tar` module before storage starts. The ISO alone also has `install.tar`. | Keep a required recovery initramfs; choose a disk root only after the boot volume is mounted. Never infer installed mode merely from a mounted FAT disk. |
| Namespace | `kernel/vfs.c` serves `/` from the read-only initramfs, the selected boot volume at `/disk`, and other FAT/ISO volumes at `/disk2` and `/cdrom`. | Change the VFS root provider explicitly. Preserve `/disk` as a compatibility path during migration. |
| Installation | `kernel/installer.c` copies `kernel.elf`, `initramfs.tar` and Limine files to `/disk/boot`; it does not unpack the initramfs into a writable system tree. | New installs need a complete disk system tree plus a recovery image. Existing installs must not be silently repartitioned or reformatted. |
| Programs | `kernel/desktop.c` discovers Terminal commands and Notepad through `initramfs_find_file`; Start > Programs reads `/bin` and `/disk/Programs`. `kernel/user.c` also has an initramfs-only child-spawn path. | Move discovery to VFS lookups and keep the existing launch and account rules. Audit every direct initramfs lookup before switching roots. |
| Data | Accounts, settings, backgrounds, app storage and installed apps use `/disk/OPOS`, `/disk/Users` and `/disk/Programs`. | Keep those paths and their contents during the first transition; move user data only as a separate later migration. |
| Security | `kernel/permissions.c` classifies protected files by `/disk` path; disk-launched executables are confined by path. `/` is currently read-only. | `/bin` and its `/disk` alias must resolve to the same protected object and policy. Executable trust must be based on the selected protected system tree, not on a spelling of its path. |
| Updates | `kernel/update.c` accepts signed manifest paths only below `boot/`, stages in `/disk/OPOS/UPDATE`, backs up kernel/initramfs to `boot/old`, and resumes interrupted copies at boot. | Teach the updater a new manifest format and root-generation transaction before publishing a release that uses it. Old updaters must reject the new format. |

## Proposed layout and boot choice

- Keep `/disk` as the FAT system volume and keep `/disk/Users`,
  `/disk/Programs`, `/disk/OPOS` and `/disk/boot` in place.
- Store each installed system tree under a protected, versioned directory such
  as `/disk/OPOS/roots/<generation>/`. The VFS maps ordinary `/` paths into
  the selected tree, including `/bin`, `/lib`, `/etc` and `/share`, while
  overlaying explicit disk and CD mount points. This is a disk-backed root
  even though its backing directory is below the FAT volume root. Versioned
  trees let a new version be written without changing the running one.
- Keep a boot/recovery initramfs containing only what is needed to mount the
  boot disk, finish or undo an interrupted transition, and offer a usable
  recovery shell. Keep the current full initramfs until those paths work.
- A boot entry must identify the exact system generation it belongs to.
  Resolve how Limine passes that identity (boot path or command line) before
  coding the selector. Do not use an unverified, mutable "latest" pointer to
  choose a root for a previous-version boot entry.
- On the live ISO, always use the initramfs root even if an installed disk is
  visible. On an installed boot, mount the Limine-selected partition, validate
  the selected generation and required files, then select the disk root before
  accounts or desktop programs start. If validation fails, report the reason
  and enter recovery; do not continue with a partly populated disk root.
- Reserve `/disk`, `/disk2` and `/cdrom` mount names and reject conflicting
  names in a system tree. Both `/bin/x` and its backing `/disk/OPOS/roots/...`
  path must have identical identity and access decisions. Files must not gain
  a write path through either spelling.

Before implementation, settle the on-disk marker format, the boot-entry field
that pins a generation, where each generation's signed file inventory is kept,
and how a partly written FAT boot-menu file is recovered. These choices must
be fixed together: a marker alone cannot prove that its tree is complete or
that the selected kernel and root belong together. This plan does not claim
verified boot; the current boot chain does not provide it.

## Rollout sequence

1. **Expand the boot and update code while retaining the old root.** Add a
   versioned root-layout marker and generation format, VFS support for an
   optional disk root, and a manifest format that can stage system-tree files.
   Keep the old boot-only manifest format working. The signed release that
   adds these readers still boots its full initramfs and changes no layout.
2. **Prove namespace and permission equivalence.** Make `/bin` and its disk
   backing path share the same protected policy, including aliases and FAT
   8.3 names. Define core-program provenance for `/bin` versus confined
   `/disk/Programs` apps. Replace hard-coded initramfs program discovery,
   child-spawn, read-only checks and UI paths with VFS-aware behavior. Keep
   the legacy initramfs path selectable throughout this step.
3. **Populate a generation.** On new installs, copy a complete system tree
   and verify it before marking it ready. On existing installs, stage it into
   unused space without moving accounts, settings, installed apps or boot
   files. Refuse activation when space is insufficient or any required file
   fails validation. A retry can resume or discard the incomplete generation.
4. **Activate only after durable verification.** Write and flush all new
   files, then write the ready marker and boot entry last. The boot entry must
   pin its kernel, recovery initramfs and root generation. Preserve the old
   kernel/full-initramfs entry, and preserve the old root generation once
   disk-root versions exist. A power cut before activation boots the old
   entry; a power cut after activation can still choose the previous entry.
5. **Contract after a compatibility window.** Once both new installs and
   upgrades pass the matrix below, remove desktop payload from the new
   recovery initramfs. Keep legacy image and generation cleanup explicit and
   administrator-driven; never delete an account or the only known-good
   bootable generation as a side effect of an update.

The updater must record which generation is staged, ready and active. Its
resume path must be idempotent: rechecking hashes and repeating a copy must
not select a partly written tree. If the boot menu cannot be updated, report
the failure and keep the old entry active. `--no-prev-version` must not be
allowed to remove the only recovery path during this migration.

## Compatibility and rollback rules

- An old kernel ignores the new root marker and continues using its paired
  full initramfs. A new kernel can boot an old installation in legacy mode.
- A release that requires a disk root must be rejected by old updaters, whose
  manifest parser currently accepts only `boot/` paths. Publish the updater
  capability first, then the first root-populating release.
- Previous-version boot must select the matching old root generation as well
  as its kernel and initramfs. The current `boot/old` pair is sufficient only
  while both versions use the initramfs root.
- Rollback changes system selection, not user data. `/disk/Users`, account
  records, settings and app storage remain in place. Any future incompatible
  data-format change needs its own backward-compatible migration.
- Never overwrite the old boot entry or old generation until a new generation
  has booted successfully and the administrator explicitly chooses cleanup.

## Proof gates before enabling disk root by default

| Gate | Required evidence |
| --- | --- |
| Legacy | Current installed image and live ISO still boot; `/disk` data and accounts survive the new kernel and an old-kernel rollback. |
| Namespace | `/`, `/bin`, libraries, fonts, certificates, `/disk`, extra disks and CD mounts resolve correctly; `/disk` aliases cannot bypass protected-file rules. |
| Programs | Terminal `fetch`/`update`, Notepad, Browser, child spawn, SDK apps in `/disk/Programs`, file picker and app storage work under both roots. |
| Permissions | Standard users and disk apps cannot write either spelling of a system file, read `ACCOUNTS.DAT`, or gain built-in privileges by copying/renaming an executable. Administrator UI confirmation still applies once per protected change. |
| Power loss | Cut power during each stage, marker write, boot-menu write and update resume. Every resulting state boots the old or new complete generation, or enters recovery with a clear error. |
| Hardware | BIOS/MBR and UEFI/GPT installs in QEMU, VMware and VirtualBox; boot disk chosen correctly with multiple disks, removable media and a missing or damaged system volume. |

Program discovery in Terminal, Notepad and the kernel shell, child-spawn
executable loading, and root directory listings now use VFS paths under both
the legacy initramfs root and the optional disk root. The editor and Files
read-only checks already use VFS paths; their UI text now describes either
root. The next slice is to prove namespace and permission equivalence,
including disk aliases and FAT name variants. Installer changes, activation
and payload removal follow only after those gates pass.
