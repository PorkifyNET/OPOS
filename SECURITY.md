# Security policy

## Current state

OPOS is early development software. Do not use it to protect sensitive data.
In particular:

- User accounts enforce a location-based access policy on OPOS system files
  and home folders. FAT has no ownership metadata; this is not a general Unix
  permission model. See [docs/security-model.md](docs/security-model.md).
- The kernel shell requires an administrator login when accounts exist, but
  physical access still permits booting other media and reading unencrypted
  disks.
- Programs run in isolated address spaces and inherit their account's file
  access. There is no per-application sandbox or permission prompt system.
- OPOS has not had an external security audit. Do not treat these boundaries
  as production-grade protection.

The missing sandbox, disk encryption and external audit are known gaps; they
do not need separate reports. See the README and security-model document.

## Supported versions

Security fixes go to the latest `main` branch only.

| Version | Supported |
| --- | --- |
| `main` | Yes |
| Anything older | No |

## What to report

Please report problems in code that is meant to enforce a boundary or protect
data, for example:

- a user program that can read or write kernel memory, another process's
  memory, or run code in ring 0;
- syscalls that accept invalid user pointers, lengths or flags in a way that
  corrupts the kernel;
- disk, partition or filesystem parsing that a crafted disk image can use to
  crash or take over the kernel;
- the installer or disk code writing to a disk or partition the user did not
  choose;
- weaknesses in how account passwords are stored or checked
  (`kernel/accounts.c`, `kernel/sha256.c`);
- boot chain problems, such as a boot menu entry that starts something other
  than what it names.

## How to report

**Do not report exploitable vulnerabilities in public issues.**

Use GitHub's private vulnerability reporting: open the repository's
**Security** tab and choose **Report a vulnerability**. If that is not
available, contact the maintainer privately and ask for a secure way to send
the details.

Low-risk hardening suggestions that give no one an advantage when made public
(for example, "this buffer should have an extra bounds check") can use the
**Vulnerability report** issue form instead.

Please include:

- the affected file or feature and the OPOS commit you tested,
- the VM or hardware, firmware (BIOS or UEFI) and disk controller,
- steps to reproduce, and a proof of concept if you have one,
- the impact you expect,
- serial output if the kernel crashed.

## What happens next

This is a small, volunteer project, so these are goals rather than promises:

- The maintainer acknowledges a report within 7 days.
- The maintainer confirms or rejects the problem and shares a fix plan within
  30 days.
- Fixes are released, and the report is published, once a fix is available.
  Reporters are credited unless they ask not to be.

Please give the project a reasonable time to fix a problem before disclosing it
publicly.
