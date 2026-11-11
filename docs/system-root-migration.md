# Installed-disk system root migration

Installed OPOS runs `/bin`, `/lib`, `/etc` and `/share` from its own disk
after verifying a generation against the booted full initramfs. The live ISO
still runs from its initramfs. Shrinking the initramfs and proving the full
hardware and power-loss matrix remain future work.

## Implementation status

The VFS seam is selected on normal installed boots. After the
boot FAT volume is mounted, `vfs_select_disk_root()` can map `/` onto a
protected `/disk/OPOS/roots/<generation>` directory. It rejects missing roots,
other volumes, reserved mount-point names and a switch after file objects
have opened. The root alias is read-only; `/disk` still exposes the physical
volume. Permission checks classify both spellings as system files and block
changes to the selected tree, including through its physical path. A QEMU
smoke boot with a temporary selector call confirmed matching device/inode for
`/bin/notepad` and its backing file, and the focused permission test passes.
Live builds still use the initramfs root. Program discovery and launching
use VFS paths under either root. The selected tree is locked by FAT
device/inode identity, including through its short-name alias and descendants
that do not exist yet; `make permissions-test` covers this guard.

New installs copy the complete initramfs tree into
`/disk/OPOS/roots/generation-<version-with-underscores>`, compare every file
and reject unexpected entries, then write a versioned `.ready` marker. A boot
entry may pin `cmdline: opos.root=generation-0_1_3` (for version 0.1.3).
Installed boots automatically stage a missing generation from their paired
full initramfs, verify the tree, then map it as `/`. A staging failure leaves
the full initramfs active for retry. A ready but damaged generation enters
the recovery shell. An old pin left in a menu after an update is ignored only
when a previous kernel exists; the current kernel chooses its own generation.
The previous menu entry boots its saved kernel/initramfs pair. This is
payload integrity relative to the booted initramfs, not verified boot or a
signed on-disk inventory.

`sh tools/test_disk_root_qemu.sh` installs onto a disposable blank FAT16 disk,
tests that a damaged file or an unexpected program rejects the generation,
then checks namespace, permissions and `/bin/fetch` under the disk root. It
reboots from that disk without a root option, removes the ready marker in a
test hook before selection, and confirms that staging repairs it and selects
the complete generation. The live ISO's menu has no root option.

## Current contracts to preserve

| Area | Current reader or writer | Migration consequence |
| --- | --- | --- |
| Boot | `kernel/main.c` requires Limine's `initramfs.tar` module before storage starts. The ISO alone also has `install.tar`. | Keep a required recovery initramfs; choose a disk root only after the boot volume is mounted. Never infer installed mode merely from a mounted FAT disk. |
| Namespace | `kernel/vfs.c` serves `/` from the read-only initramfs, the selected boot volume at `/disk`, and other FAT/ISO volumes at `/disk2` and `/cdrom`. | Change the VFS root provider explicitly. Preserve `/disk` as a compatibility path during migration. |
| Installation | `kernel/installer.c` copies `kernel.elf`, the full `initramfs.tar` and Limine files to `/disk/boot`, and stages a verified system tree under `/disk/OPOS/roots`. | Keep the full initramfs for recovery and preserve existing installations. |
| Programs | Terminal commands, Notepad, Start > Programs and child spawn use VFS paths under either root. | Keep the existing launch and account rules; audit remaining direct initramfs lookups before switching roots. |
| Data | Accounts, settings, backgrounds, app storage and installed apps use `/disk/OPOS`, `/disk/Users` and `/disk/Programs`. | Keep those paths and their contents during the first transition; move user data only as a separate later migration. |
| Security | `kernel/permissions.c` classifies protected files by `/disk` path; disk-launched executables are confined by path. `/` is currently read-only. | `/bin` and its `/disk` alias must resolve to the same protected object and policy. Executable trust must be based on the selected protected system tree, not on a spelling of its path. |
| Updates | `kernel/update.c` accepts signed manifest paths only below `boot/`, stages in `/disk/OPOS/UPDATE`, backs up kernel/initramfs to `boot/old`, and resumes interrupted copies at boot. | The new kernel stages its own generation from its paired full initramfs on first boot. A later recovery-only initramfs needs a new signed manifest format. |

## Layout and remaining design work

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
- Each kernel derives its generation from its compiled version and paired
  full initramfs. An optional `opos.root=` boot argument pins the current
  kernel's generation. There is no mutable "latest" pointer.
- On the live ISO, always use the initramfs root even if an installed disk is
  visible. On an installed boot, mount the Limine-selected partition, validate
  the selected generation and required files, then select the disk root before
  accounts or desktop programs start. If validation fails, report the reason
  and enter recovery; do not continue with a partly populated disk root.
- Reserve `/disk`, `/disk2` and `/cdrom` mount names and reject conflicting
  names in a system tree. Both `/bin/x` and its backing `/disk/OPOS/roots/...`
  path must have identical identity and access decisions. Files must not gain
  a write path through either spelling.

The current marker is checked together with a byte-for-byte comparison to the
booted full initramfs. A signed on-disk inventory, smaller initramfs and a
boot-menu recovery journal remain future design work. This does not claim
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
root. The disk-root QEMU test covers a full new install, FAT short aliases,
permission decisions, launch from the selected root, rejection of a damaged
tree, and an automatic installed boot. `sh tools/test_update_root_qemu.sh`
installs 0.1.3, updates both kernel and initramfs to a 0.1.4 test payload,
boots the new generation and the saved previous entry, and uses SIGKILL at
five durable checkpoints: prepared state, backup, menu, copying state, and
after the initramfs copy. Each interrupted image resumes to a complete new
generation while its previous entry still boots the old generation. The test
uses a forced unsigned development manifest and QEMU BIOS/MBR with threaded
TCG. An OVMF/GPT install test now installs onto a dedicated OPOS partition
beside an EFI System Partition and a separate 32 MiB data partition. It checks
the data partition byte-for-byte after installation, then boots and verifies
the installed system root. A separate OVMF smoke boot reaches live-ISO network
initialization. Cuts in the middle of individual disk writes, marker and menu
writes, signed release updates, VMware, VirtualBox and physical hardware
remain unproved. The UEFI/GPT test also cuts QEMU at the
prepared, backup, menu, copying, and first-file durable checkpoints. For each
cut it verifies recovery reaches a complete new generation and the previous
entry boots the old generation. These checks do not cover interruption in the
middle of an individual disk, marker, or menu write. A smaller recovery
initramfs waits for the remaining gates.

`sh tools/test_update_root_uefi_qemu.sh` installs 0.1.3 on GPT with OVMF,
updates kernel and initramfs to 0.1.4, verifies the new root, then sets the
Limine default to the saved previous entry and verifies the old kernel/root
boots. The previous-entry check runs through the GPT system-volume menu under
fresh OVMF variables. Its five power-cut cases use separate disk images and
OVMF variables, kill QEMU after each durable marker, and verify both recovery
and previous-version boot.
