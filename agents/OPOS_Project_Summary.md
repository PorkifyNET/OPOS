# OPOS — Project Summary and Direction

*Discussion summary — 5 November 2026*

## 1. What OPOS is

**OPOS** is an independent operating system written in **C**, with its **own original kernel** and other original system components. It is not a Linux distribution. The project currently exists as local, unpublished source code.

The long-term ambition is an operating system for everyone, from office users to gamers and developers. OPOS aims to support **native OPOS software**, **Linux applications**, **Windows applications**, and potentially **macOS applications** later on. Broad compatibility is an ambition, **not a currently demonstrated capability**.

### Core philosophy

> **Simple when you want it. Powerful when you need it.**

The intended combination is **Linux-like user control, transparency, and customizability** with **Windows-like convenience**: ordinary tasks should be straightforward and “just work,” without denying advanced users control of their own machines.

Practical principles discussed:

- Sensible defaults; no unnecessary setup or command-line work for routine tasks.
- Graphical controls for everyday tasks, with scripting/CLI interfaces where practical.
- No mandatory cloud account, advertising, hidden telemetry, or forced ecosystem lock-in as project goals.
- An understandable, consistent desktop and application platform.
- Privacy and user ownership of the machine as design principles.
- The ability to inspect, configure, modify, and rebuild the system.

These are **design goals**, not guarantees of features already implemented.

## 2. Current implementation (reported)

OPOS already includes:

- Its own C kernel.
- A functioning graphical desktop environment.
- Multiple applications running concurrently.
- Movable floating windows (but **not yet resizable**).
- A switch between floating windows and **Hyprland-like automatic tiling**.
- A simple file manager (currently lacking a navigation/address bar).
- A Terminal that can run programs.
- Built-in native **Notepad** and **Settings** programs.
- Settings for date/time, display resolution, automatic VM display fitting, and whether to boot to the shell or desktop.
- A filesystem, a basic OS installer, user accounts, and Multiboot support.
- Some Linux syscall compatibility work and simple Linux test programs.
- **GPT partition support nearing completion** (GPT here means *GUID Partition Table*).

Known limitations and uncertainties:

- No user-permission system yet; accounts are **not equivalent to enforced security separation**.
- Native graphical applications such as Notepad are **compiled directly into OPOS**, rather than being independently installed executables.
- It is not yet established that outside developers can independently compile an OPOS-native executable and run it without rebuilding the OS.
- The details of user/kernel privilege separation, virtual address spaces, and independent ELF loading still need verification.
- Windows and macOS application compatibility, broad Linux compatibility, production-grade gaming, and hardware support remain longer-term objectives.

### Current installation contents

The installation is intentionally barebones: **boot files, EFI-related files, a few libraries, simple Linux tests, `OPOS.cfg` settings, and `accounts.dat` account data**. The bundled applications are integrated into OPOS itself.

## 3. Why someone would choose OPOS

A new OS needs more than “Linux with a different kernel.” The proposed distinctive experience is:

1. **Freedom without friction:** full control for experts while keeping ordinary work easy.
2. **A cohesive system:** one consistent set of conventions, services, settings, and developer interfaces.
3. **A flexible desktop:** both traditional floating windows and tiling built into the same experience.
4. **Transparent privacy and security:** users should understand what software can do and retain the final say.
5. **Eventually, seamless compatibility:** OPOS could detect the application type and use an appropriate execution/compatibility environment without making the user manage it manually.

Linux already provides many comparable features, so OPOS must earn adoption through **execution and user experience**, not merely promises. Developers and OS enthusiasts are the natural first audience; ordinary office users and gamers come later, once applications, drivers, security, stability, and compatibility are good enough.

## 4. Open-source licensing and attribution

The preferred starting point discussed was:

- **GPLv3** for the original kernel and much of the core system, to require distributed modified GPL-covered versions to provide corresponding source code under the license.
- Consider **LGPLv3, Apache 2.0, or another appropriate license** for libraries and APIs where third-party proprietary application interoperability matters; check actual linking and license compatibility.
- **CC BY-SA 4.0** could be considered for documentation.
- Protect the **OPOS name and branding separately through a trademark policy** (after checking name availability).
- Keep contributor credit, copyright notices, provenance, and a transparent development history.

The motivation is **not** to prohibit companies from making money with OPOS. It is to prevent them from concealing its origins, falsely claiming to have invented it, or taking community code proprietary in violation of its license.

**Important legal limit:** GPLv3 and recognized open-source licenses **do permit commercial redistribution**, including paid copies. GPLv3 constrains how covered code can be redistributed, not whether it can be sold. A distributor can sell a compliant derivative, and recipients retain the relevant source-code and redistribution rights. Appropriate copyright notices, attribution terms permitted by GPLv3, and trademark rules can address different kinds of misrepresentation. They cannot guarantee that no company will ever charge money. Have final license and trademark wording reviewed before release.

Audit any borrowed Linux-related code for its **actual provenance and license obligations** before publishing.

## 5. Making OPOS extensible

Although OPOS has native applications, they are currently built into the system. A major future milestone is allowing independent third-party programs to be developed, loaded, and executed.

Potential work:

1. Confirm or implement a standalone executable loader (ELF may be useful given existing Linux compatibility work).
2. Define a stable **OPOS userspace ABI** and system-call interface.
3. Provide a basic C runtime, toolchain, libraries, and documentation.
4. Support independently built command-line applications first (for example, a `Hello, OPOS!` test).
5. Provide documented graphics/windowing APIs for third-party graphical applications.
6. Eventually move some built-in utilities (perhaps Notepad first) into independently loadable applications.

**Security boundaries must be established before arbitrary untrusted programs are encouraged.** Executable loading without isolation is not a secure application platform.

## 6. Security and anti-malware philosophy

> **Your computer belongs to you. Applications are guests.**

The strategic idea is to **prevent or contain unwanted behavior at the operating-system level**, rather than relying exclusively on antivirus signatures after software has already been given sweeping privileges.

### Proposed architecture

- User/kernel privilege separation and robust per-process address-space isolation.
- Enforced user and filesystem permissions; meaningful account boundaries.
- Permission-aware system calls and kernel-mediated access to files, processes, devices, and other resources.
- **Sandboxing by default** for applications, with narrowly scoped privileges.
- OS-controlled file pickers that grant access only to selected files, rather than entire folders or drives by default.
- Explicit, understandable permissions for sensitive facilities such as camera, microphone, and background access.
- A local **Security Center** showing permissions, blocked actions, and relevant activity, without requiring upload of private activity logs.
- Recovery from ransomware through protected snapshots/version history and independently protected backups; behavioral detection can be an additional heuristic layer.
- Kernel hardening (for example, NX/DEP, ASLR where appropriate, strict syscall validation) and cryptographically verified updates/boot when feasible.
- User-controlled boot verification and the ability to run modified kernels, rather than a single vendor-exclusive trust policy.

These are **proposals**, not implemented OPOS security features. A security dashboard cannot itself guarantee detection of malware. Since OPOS is written in C, memory-corruption risks deserve particular attention.

### Compatibility and security

A Windows or Linux compatibility layer should translate application requests into **OPOS-enforced** access decisions. Its security cannot rely on a voluntarily cooperating compatibility layer alone: applications must not be able to bypass restrictions via unrestricted kernel, process, or disk access.

## 7. Kernel-level anti-cheat and user sovereignty

Three positions were established:

1. **A game is not automatically a good reason to compromise system security.**
2. **OPOS is open-source and user-modifiable.** That complicates anti-cheat vendors' attempts to attest to a trustworthy environment, though open source does **not** make kernel-level anti-cheat technically impossible.
3. **The user has the final decision** about whether to permit privileged third-party components. OPOS should never silently install or grant kernel privileges merely because an application demands them.

Suggested policy:

- No third-party kernel access by default.
- Privileged components require deliberate, informed authorization via a trusted OS-controlled process.
- Users can inspect and disable installed privileged components and rebuild/replace their own kernel.
- Prefer narrowly scoped services over granting arbitrary kernel execution.
- Clearly explain that refusing a game's anti-cheat may mean **the game cannot run**. OPOS need not weaken its policies or promise to bypass server-side restrictions.
- Even an explicitly authorized kernel module can undermine OS safeguards; consent does **not** make arbitrary kernel code safe.

**Guiding statement:** OPOS should not require users to surrender control of their own system simply to launch an application.

## 8. Building a community

Because OPOS already boots into a functioning graphical environment, it has something concrete to demonstrate. It need not wait for complete Windows compatibility or a consumer-ready release to invite developer interest.

### Before publishing source code

- Put the local source under **Git**, including a reliable independent backup.
- Optionally host it in a **private remote repository** while preparing the release.
- Review committed files for secrets, personal information, third-party code, and licensing conflicts.
- Make sure a newcomer can build and boot it using reproducible, clearly documented steps.
- Prepare a VM-focused distribution or QEMU instructions; caution against using untested disk-installation code on valuable real disks.
- Provide a clear feature list distinguishing what works today from what is planned.

Suggested project files: `README.md`, `BUILDING.md`, `CONTRIBUTING.md`, `ARCHITECTURE.md`, `ROADMAP.md`, `LICENSE`, `CODE_OF_CONDUCT.md`, and a security/known-limitations notice.

### A possible first public release

**OPOS 0.1.0-alpha — Developer Preview** (illustrative title, not an announced release).

A short showcase could demonstrate booting into the GUI; opening Terminal, File Manager, Notepad, and Settings together; switching between floating and tiling modes; changing resolution with VM autofit; using the shell/filesystem; and running the installer in a disposable VM.

### First contributors

Aim initially for **a small number of active contributors**, not a large passive community. Share real engineering milestones and technical challenges through the OSDev community, GitHub, and occasional short videos or screenshots. Make it easy for newcomers to try the OS and submit useful patches.

Potential contribution areas include window resizing, file-manager navigation, documentation, testing, build tooling, permissions, executable loading, and desktop polish. Some kernel/security tasks are necessarily advanced.

For substantial architecture choices, consider an **OPOS Enhancement Proposal (OEP)** process, with states such as Draft → Discussion → Accepted/Rejected → Implemented.

## 9. Suggested technical priorities

This is a **proposed sequence**, subject to what OPOS already implements internally:

1. **Back up, version-control, and document** the working system.
2. **Verify existing security boundaries:** kernel vs. user mode, virtual address spaces, privilege handling, and filesystem access checks.
3. **Implement/enforce process isolation and permissions** before supporting untrusted third-party applications.
4. **Finish and test disk/GPT work safely**, using disposable VM disks.
5. **Define a stable native application loading and ABI path**; demonstrate a separately compiled command-line program.
6. **Provide a minimal native application SDK** and eventually standalone graphical applications.
7. **Introduce application sandboxes and understandable permission controls.**
8. Improve desktop usability: resizable windows, file-manager navigation, and a polished first-run experience.
9. Expand compatibility gradually: small Linux applications first, then progressively more complex Linux/Windows software; gaming and macOS remain longer-term research areas.
10. Develop security dashboard, update integrity, recovery, and hardware support as the underlying architecture matures.

## 10. Project identity — proposed README introduction

> **OPOS — Simple when you want it. Powerful when you need it.**
>
> OPOS is an independent, open-source operating system built from scratch in C. Its goal is to combine the flexibility and transparency of Linux with the simplicity and convenience of Windows, giving everyone control over their computer without making everyday tasks unnecessarily complicated.
>
> OPOS is currently in active development. It already has its own kernel, graphical desktop, tiling and floating window modes, filesystem, Terminal, basic built-in applications, and an installer. Broad third-party application compatibility, security isolation, and mainstream desktop readiness are ongoing goals—not current guarantees.

---

*This is a working summary of decisions, reported features, and ideas discussed—not an official specification, security audit, legal opinion, or verified build report.*
