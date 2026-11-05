# OPOS application SDK

Build programs for OPOS without the OPOS source tree. This folder is all
you need: a header, a start-up file and a linker script.

## Build

On Linux or WSL with GCC (or Clang as `CC=clang`):

```sh
make                # builds build/hello and build/permtest
make APP=myapp      # builds build/myapp from myapp.c in this folder
```

A program is a static x86-64 ELF file of at most 64 KiB, with no C library.
`include/opos.h` has the system calls and a few helpers (`opos_print`,
`opos_print_number`, `opos_strlen`).

```c
#include <opos.h>

int main(int argc, char **argv, char **envp) {
    opos_print("Hello, OPOS!\n");
    return 0;
}
```

## Run

Copy the file to an OPOS disk, for example with mtools on a disk image
whose FAT partition starts at 1 MiB:

```sh
mcopy -i disk.img@@1M build/hello ::hello
```

In OPOS, type `run /disk/hello` in the Terminal, or double-click the
file in Files. Output appears in the Terminal window.

## The ABI

- Static ELF64 executables, loaded at 0x400000 (`opos.ld`). Each segment
  gets one permission set: the kernel refuses writable code.
- Entry: `_start` in `src/crt0.S`, with the Linux x86-64 initial stack
  (argc, argv, envp, auxiliary vector). It calls
  `main(argc, argv, envp)` and exits with its result.
- System calls use the `syscall` instruction. Numbers below 0x4f500000
  are Linux x86-64 numbers (OPOS implements a subset, see
  `docs/linux-compat.md`). Numbers from 0x4f500000 are OPOS's own:

  | Number | Call | Result |
  | --- | --- | --- |
  | 0x4f500000 | `opos_abi_version()` | kernel ABI version (1) |
  | 0x4f500001 | `opos_user_info(&info)` | account name, home folder, admin flag |

- Errors are negative error numbers (`-OPOS_EACCES`, ...).
- Every file carries an ELF note named `OPOS` (type 1) with the ABI
  version it was built for. Later loaders can use it to tell OPOS
  programs from Linux ones.

ABI version 1 is only ever extended: numbers, arguments and structure
layouts stay the same. Structures begin with a size field that the program
fills in, so the kernel can grow them without breaking older programs.

## What a program may do

Programs run as the account that started them. They can read system files
but not change them, cannot read the password file, reach only their own
home folder in `/disk/Users`, and can use everything else on the disks.
See `docs/security-model.md`.

## Limits

- No C library yet: no `printf`, `malloc` (use `brk`/`mmap` through
  `opos_syscall*`), or floating-point formatting.
- No graphics or windows yet; programs use the Terminal for text.
- Programs started from the desktop get only `argv[0]`.
- Images over 64 KiB do not load.
