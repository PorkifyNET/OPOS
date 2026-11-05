# Contributing to OPOS

Thanks for your interest in OPOS. The project is young and not yet public, so
these guidelines are short and will grow as more people join.

## Before you start

- Read the [README](README.md) to learn what works today and how to build.
- For anything bigger than a bug fix, open an issue or start a discussion
  first, so the design can be agreed before you write a lot of code.
- Good first areas: desktop polish (window resizing, Files address bar),
  documentation, testing on more VMs and hardware, and build tooling.

## Development setup

Build in Linux or WSL2 as described in the README. Test in a virtual machine:

- **QEMU** is the quickest loop. Use `-accel kvm` when you can; it behaves
  more like real hardware than TCG.
- **VMware Workstation** is the main human test platform. If your change
  touches drivers, display, input or booting, say whether you tested it there.
- Test UEFI boot with OVMF (QEMU) or EFI firmware (VMware), and BIOS boot with
  SeaBIOS or VMware's BIOS mode.

Never run the installer or disk code against a real disk that holds data you
care about. Use throwaway disk images.

## Making changes

- Keep each change focused on one thing. Separate refactors from behaviour
  changes.
- Follow the existing style: C with 4-space indents, `snake_case` names and
  `static` helpers. Bound every loop and buffer that depends on disk or user
  input.
- Comments explain *why* something is done, or a hardware quirk, not what the
  next line does.
- A kernel bug can corrupt anything, so validate every user pointer, length
  and on-disk value before using it.
- New storage or filesystem code must not write to disks or partitions the
  user did not choose. Read-back verification is welcome.
- Update the README or `docs/` when you change user-visible behaviour.

## Testing

- `make` must build without warnings.
- Boot the ISO and check that the desktop still starts and that the feature
  you touched works.
- For storage or filesystem changes, check the disk afterwards from Linux
  (`fsck.fat -n`, `sgdisk -v`, `sfdisk -l`) and confirm that partitions you
  did not touch are byte-identical.
- `make persistence-test` covers FAT writes, rename, delete and the
  Linux-style file syscalls.
- `tools/qemu_cmds.sh` boots the ISO and types shell commands, which is useful
  for scripted checks.

## Commits and pull requests

- Write commit messages in the imperative mood ("Add GPT boot entries", not
  "Added..."), with a short subject line and a body that explains why.
- In the pull request, describe what changed, how you tested it (QEMU, VMware,
  BIOS/UEFI, which disk controller), and anything you could not test.
- Do not commit build output, disk images or ISOs.

## Reporting bugs

Include:

- the VM or hardware (VMware, QEMU, VirtualBox; BIOS or UEFI; disk
  controller), and how much memory it has,
- what you did, what you expected, and what happened,
- serial output if you have it (QEMU `-serial file:opos.log`, or a VMware
  serial port written to a file). Fatal exceptions print the vector, `rip`
  and `cr2` there.

## Security

OPOS does not yet isolate users from each other, so do not rely on it to
protect data. If you find a security problem in code that is meant to
enforce a boundary (process isolation, syscall validation, the password
store), report it privately as described in [SECURITY.md](SECURITY.md), not
in a public issue. For help and questions, see [SUPPORT.md](SUPPORT.md).

## License

OPOS is licensed under the GNU General Public License, version 3 or any later
version (`GPL-3.0-or-later`). By submitting a contribution, you agree to
license it under the same terms. Only submit code you wrote yourself or code
whose license is compatible with GPLv3, and say where any third-party code
came from.
