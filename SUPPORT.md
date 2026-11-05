# Getting help with OPOS

OPOS is early development software maintained by volunteers. There is no paid
or guaranteed support, but questions and reports are welcome.

## Before asking

- Read the [README](README.md): it lists what works, what does not exist yet,
  and how to build and run OPOS.
- Technical details on running Linux programs are in
  [docs/linux-compat.md](docs/linux-compat.md).
- Search existing issues; your problem may already be known.

## Where to go

| You want to... | Use |
| --- | --- |
| Ask how something works or how to do something | GitHub Discussions (if enabled) or an issue labelled `question` |
| Report a bug | A GitHub issue (see below) |
| Suggest a feature | A GitHub issue; for large changes, start a discussion first |
| Report a security vulnerability | **Not** a public issue: see [SECURITY.md](SECURITY.md) |
| Contribute code or documentation | [CONTRIBUTING.md](CONTRIBUTING.md) |

## Writing a good bug report

Include:

- the OPOS commit or ISO you used,
- the VM or hardware: VMware, QEMU or VirtualBox; BIOS or UEFI; disk
  controller (IDE, SATA, NVMe, SCSI); amount of memory,
- what you did, what you expected, and what happened,
- serial output if you have it. In QEMU use `-serial file:opos.log`; in VMware
  add a serial port that writes to a file. Fatal exceptions print the vector,
  `rip` and `cr2` there,
- a screenshot if the problem is visual.

## What is out of scope

- Data loss on real disks. OPOS's installer and disk tools are experimental;
  only use them on virtual disks or disks with nothing you want to keep.
- Running specific Linux or Windows applications. Broad application
  compatibility is a long-term goal, not a current feature.
- Real hardware other than what the README lists. Reports are still welcome,
  but fixes may take a long time.
