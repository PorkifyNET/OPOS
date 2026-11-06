# OPOS

**Simple when you want it. Powerful when you need it.**

OPOS is an independent operating system for x86-64 PCs, written from scratch
in C with its own kernel. The goal is a daily-use desktop that combines the
control and transparency of Linux with the convenience of Windows, and that can
eventually run native OPOS, Linux and Windows applications.

OPOS is in early, active development. It boots into a graphical desktop and can
install itself to a disk, but it is not ready for everyday use. Broad
application compatibility, security isolation between users, and networking
are goals, not current features.

## What works today

**Desktop**
- Window manager with floating windows and Hyprland-style dwindle tiling,
  switchable per user (Super+T). Windows resize by their edges (floating),
  by dragging the gap between them (tiled), or with Super+Shift+arrows.
- Taskbar with Start menu, window buttons and clock.
- Built-in apps: Terminal, Files, Text Editor, System Info, Settings, Accounts,
  and a Welcome window shown on each user's first login.
- Files: browse folders, open and run files, rename, delete, and create files
  and folders (right-click menu); sidebar with home folders and every mounted
  volume; back/forward/up buttons and an address bar (Ctrl+L).
- Text Editor: selection, clipboard, common Ctrl shortcuts, CRLF-preserving
  save.
- User accounts with optional passwords (salted, iterated SHA-256), a login
  screen, and per-user home folders and settings. Programs run as their
  account: they cannot change system files, read password hashes, or open
  other users' homes. See [docs/security-model.md](docs/security-model.md).

**Installer**
- Runs from the live ISO, or close it to try OPOS without installing.
- Choose the disk; erase it or install into a partition. Create, delete and
  reuse partitions on MBR and GPT disks.
- Detects other systems (Windows, GRUB/Linux, UEFI loaders, other disks) and
  adds them to the Limine boot menu.
- Boots on BIOS (MBR disks) and UEFI.

**Hardware and storage**
- Disks: IDE (ATA PIO), AHCI SATA, NVMe, VMware PVSCSI, LSI Fusion-MPT (LSI
  Logic parallel and SAS), BusLogic (untested).
- Partition tables: MBR and GPT. Filesystems: FAT16 and FAT32 with long file
  names; every FAT volume is mounted (`/disk`, `/disk2`...).
- Input: PS/2 keyboard (Num Lock, keypad, LEDs) and mouse, VMware absolute
  mouse.
- Display: Limine framebuffer, VMware SVGA II with resolution switching and VM
  window autofit.
- RTC clock, ACPI shutdown, reboot.

**Kernel and programs**
- Preemptive scheduler, isolated per-process address spaces, NX and
  write-protected user code, SMEP and UMIP when the CPU has them.
- Native application SDK in [sdk/](sdk/README.md): build an OPOS program
  with any x86-64 GCC, copy it to the disk and run it with arguments. Programs
  can open a window, draw pixels and get keyboard and mouse events.
- ELF loader with a small dynamic interpreter, and a subset of Linux x86-64
  syscalls (files, pipes, fork/execve/wait, mmap, threads with futexes) that
  runs static musl programs. See [docs/linux-compat.md](docs/linux-compat.md).

**Not yet:** networking, USB, sound, per-application sandboxes, a widget
toolkit for apps, general Linux or Windows application support.

## Building

OPOS builds on Linux, or in WSL2 on Windows (Ubuntu).

```sh
sudo apt update
sudo apt install build-essential curl musl-tools xorriso qemu-system-x86
make
```

`make` produces `build/opos.iso`. The build downloads a pinned Limine release
(currently 12.9.6, SHA-256 verified) and the Limine protocol header into
`build/`.

The musl test programs need `x86_64-linux-musl-gcc`. If your musl package only
installs `musl-gcc`, use `make MUSL_CC=musl-gcc`.

Other targets:

| Target | What it does |
| --- | --- |
| `make run` | Boots the ISO in QEMU with serial output in the terminal (no display window). |
| `make persistence-test` | Builds a 16 MiB FAT16 disk, boots OPOS twice and checks the files it wrote. |
| `make clean` | Removes `build/`. |

## Running

Boot `build/opos.iso` in a virtual machine. VMware Workstation is the main
test platform; QEMU and VirtualBox should also work. Give the VM at least
512 MiB of RAM and a disk if you want to install.

QEMU with a display, KVM and a SATA disk:

```sh
qemu-img create -f raw disk.img 1G
qemu-system-x86_64 -accel kvm -cpu host -m 1G -cdrom build/opos.iso \
    -drive file=disk.img,format=raw,if=none,id=d0 \
    -device ahci,id=ahci -device ide-hd,drive=d0,bus=ahci.0 -boot d
```

Add `-vga vmware` to test the SVGA driver, or the OVMF firmware
(`-drive if=pflash,...`) to test UEFI boot.

> [!WARNING]
> The installer erases or repartitions disks. Only install OPOS on virtual disks or on hardware with nothing you want to keep.

### Desktop keys

| Keys | Action |
| --- | --- |
| Super | Start menu |
| Super+Enter | Terminal |
| Super+E | Files |
| Super+T | Toggle tiling and floating |
| Super+Q | Close window |
| Super+Tab, Super+Arrows | Move focus |
| Ctrl while choosing Reboot | Boot into the kernel shell once |

The terminal command `shell` leaves the desktop for the kernel shell; type
`help` there for its commands (disk tools, installer, test programs). On a
computer with accounts the kernel shell first asks for an administrator.

## Project layout

| Path | Contents |
| --- | --- |
| `kernel/main.c` | Boot sequence, framebuffer console, exception handling |
| `kernel/desktop.c` | Window manager and all built-in apps |
| `kernel/installer.c`, `partition.c` | Installer engine, MBR/GPT tables, boot menu |
| `kernel/fat16.c`, `vfs.c`, `block.c` | FAT16/FAT32, mount namespace, disk table |
| `kernel/ata.c`, `ahci.c`, `nvme.c`, `scsi.c`, `pvscsi.c`, `mpt.c`, `buslogic.c` | Storage drivers |
| `kernel/user.c`, `elf.c`, `scheduler.c`, `dynamic_*.c` | Processes, syscalls, ELF loading |
| `kernel/svga.c`, `vmware.c` | VMware display and backdoor (mouse, autofit) |
| `kernel/accounts.c`, `permissions.c`, `cmos.c` | Accounts, file access rules, settings |
| `sdk/` | Application SDK: header, start-up code, linker script, examples |
| `tools/` | Host tools and QEMU test scripts |
| `docs/` | Technical notes |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). For help, see [SUPPORT.md](SUPPORT.md);
to report a security problem, see [SECURITY.md](SECURITY.md).

## License

OPOS is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version (`GPL-3.0-or-later`). See [LICENSE](LICENSE).

OPOS is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.

The build downloads the [Limine](https://github.com/Limine-Bootloader/Limine)
bootloader, which is under the BSD 2-Clause license. ISO images built from this
repository contain Limine binaries, so anyone who distributes those images must
include Limine's license text with them.
