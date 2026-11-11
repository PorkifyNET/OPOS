# Application sandbox and permissions (design draft)

Status: proposal. Nothing here is implemented except what the "Current
state" section describes. `docs/security-model.md` stays the description of
the shipped code. This page plans the next step toward "applications are
guests" from `agents/OPOS_Project_Summary.md`.

## Goal

A program that is not part of the system should only touch what the user
has given it. The user sees what a program asks for, can say no, and can
take access back later. The kernel enforces this. A program or a
compatibility layer cannot talk its way around it.

## Current state

- A program launched from a disk is "confined" (`permission_identity.confined`).
  It can write only under its own `AppData/Programs/<id>` directory.
- It can read its own app storage, system files, and everything in the
  shared class (`/disk`, `/disk2`, and so on outside the protected
  folders). It can read one file in the account home after the user picks
  it in Files (`opos_pick_file()`, one grant, checked by device and inode).
- It has full network access and full shared clipboard access. Nothing
  asks the user for either.
- Built-in programs from the initramfs are not confined. They read the
  whole account.
- There is no record of what a program is. The id is a hash of the
  executable path, so a renamed or replaced executable is a different or
  unnoticed program.

The gaps, in order of harm: shared disk files are readable by any disk
program; network and clipboard cannot be refused; a program cannot be
identified across updates; nothing is shown to the user.

## Threat model

In scope: a disk program that is malicious or compromised and runs as the
logged-in user. It may try to read files, send data out, watch the
clipboard, write outside its storage, or start another program to get
wider rights.

Out of scope for this draft: kernel bugs, hardware attacks, someone with
physical access, a malicious administrator, and programs the user
deliberately gave full trust. A user-approved privileged component is a
separate policy (see the project summary, section 7).

## Design

### Capabilities

A capability is a named right that the kernel checks. The first set:

| Capability | Meaning | Default for a new disk program |
| --- | --- | --- |
| `storage` | Read and write its own app storage | granted |
| `files.pick` | Use `opos_pick_file()` and `opos_save_file()` (new) | granted |
| `files.shared` | Read the shared class outside its own directory | denied |
| `net` | Open sockets (one capability, no local/internet split) | denied, asked on first use |
| `clipboard.read` | Read the shared clipboard | denied, asked on first use |
| `clipboard.write` | Replace the clipboard | granted |
| `window` | Open windows and receive their events | granted |

Capabilities are added to `struct permission_identity` as a bit set, next
to `confined`. Child processes inherit the set and can only narrow it, as
`confined` already works. `execve` of another disk program intersects the
two sets.

`files.pick` stays the main way to reach user documents. It is the safe
default because the user chooses the file with Files, which a program
cannot script.

### Manifest

A program declares what it wants in an ELF note, so the SDK can fill it
from a source line. The manifest holds a name, a version and the
capabilities it requests. It is a request, never a grant. The kernel
reads the note at `execve` and ignores it if it is malformed.

As built in step 4 (`kernel/manifest.c`, `OPOS_MANIFEST` in `<opos.h>`):
the note is type 2, owner "OPOS", in the existing `.note.opos` section
and PT_NOTE segment next to crt0's ABI note (type 1). Its descriptor is
56 bytes: format 1, capability bits, a 32-byte name and a 16-byte
version, both NUL-terminated. Two manifests, bad bounds, an empty or
unterminated name, or a control character make the kernel ignore it.

- With a manifest, a program may ask only for what it lists beyond the
  defaults. That includes `files.shared`, which a new program can get no
  other way. The migration record does not add to it.
- Without one, it may ask for `net` and `clipboard.read`, plus what
  migration recorded. Linux programs and older SDK programs work
  unchanged.
- The prompt shows the manifest name. When it differs from the
  executable's file name, the file name follows in parentheses, so a
  program cannot pass for "Files" by its manifest alone.

### Identity of a program

A grant needs a stable key. Options:

1. Executable path. Simple, but a renamed file loses its grants and a
   replaced file keeps them. This is today's behavior for storage.
2. SHA-256 of the executable (`kernel/sha256.c` exists). Exact, but every
   update asks again.
3. Name from the manifest plus the signer's key. Stable across updates,
   needs program signing, which does not exist yet.

Decision: use option 2. Grants are keyed by the SHA-256 of the
executable. A changed executable is a new program and is asked again.
This is stricter than needed for honest updates, but it is safe, and no
signing infrastructure is required. Move to option 3 when the Store and
package signing exist.

### Where grants live

Per account, in a file the kernel alone writes: for example
`/disk/Users/<name>/AppData/Permissions.dat`. It is classed like
`ACCOUNTS.DAT`: programs get no access, and Files shows it read-only. Each
record holds the executable hash, a display name, the granted capabilities
and a timestamp. Two accounts never share grants.

### Enforcement points

All checks run in the kernel, in the path every syscall already uses.

- `files.shared` (implemented): in `permission_program_allows`, the
  `CLASS_SHARED` case lets a confined program read a path on `/disk..` or
  `/cdrom..` only when it holds `files.shared`, the path is in its own
  program folder, or it is the single picked file. The boot image (`/bin`,
  `/lib`, fonts, certificates) stays readable. The program folder is
  `<volume>/Programs/<folder>` for an executable anywhere below such a
  folder. An executable directly in `/disk/Programs`, or elsewhere, has no
  folder, because its directory is shared with other programs. Running a
  disk program (`execve`, spawn) needs no `files.shared`: running confers
  no data to the caller.
- `net` (implemented): checked in `socket()` (`user_linux_socket`). Name
  lookups are made in user space over UDP sockets, so every network path
  starts there. A denied call returns `EACCES`. A socket a parent already
  holds is inherited like any other descriptor; passing one to a child is
  the parent's choice.
- `clipboard.read` and `clipboard.write` (implemented): checked in
  `OPOS_SYSCALL_CLIPBOARD_GET` and `_SET`. `clipboard.write` is a default
  and is only missing if the person refused it.
- `net` and `clipboard.read` are asked for on first use by every disk
  program (`PERMISSION_CAPS_ASK_ON_USE`), within what its parent held.
  With no account, the answer lasts for that process only.
- Linux-compatibility programs use the same identity. The compatibility
  layer translates calls into the same kernel checks. It is not trusted to
  enforce them itself.

### Asking the user

The first time a program uses a denied capability that it could request,
the syscall blocks and the desktop shows a prompt: program name, the
capability in plain words, "Allow once", "Always allow", "Deny". The
program receives `EACCES` on deny. The prompt belongs to the desktop, a
program cannot draw over or click it, and while it is open the requesting
program gets no input. If no desktop runs (kernel shell, ISO with no
account), the answer is deny.

As built in step 2 (`desktop_app_ask_permission`): the prompt has "Deny"
and "Allow" only, and both are stored. "Allow once" is left for later.
A process asks at most once per capability. Escape and Enter deny;
allowing takes a click, and clicks in the first half second are ignored
so a click meant for something else cannot answer it. No desktop, a
prompt already open, or the program ending all deny without storing
anything.

Programs that only run in the Terminal use the same prompt. The Terminal
window shows that a request is pending.

### Security Center

A new built-in window lists every program with grants, what each has, and
the last time each was used. The user can revoke a capability. A revoke
takes effect at the next check, because the kernel reads the grant from
the identity of the running process, which the desktop updates.
It also shows a short log of blocked actions, kept locally and never sent
anywhere.

As built in step 5 (Start > SECURITY CENTER, `security_*` in
`kernel/desktop.c`):

- One row per record in the logged-in account's `Permissions.dat`. Each
  row shows the program's name, the first 4 hex digits of its hash, and
  Network, Clipboard (read) and Shared files as ALLOWED, DENIED or
  NOT ASKED. It also shows the date of the last change. The window
  reloads every two seconds, so answers given in a prompt appear.
- Clicking a setting, or pressing 1, 2 or 3, changes it. DENIED becomes
  ALLOWED, and anything else becomes DENIED. A grant counts from the
  program's next start, even for a capability its manifest does not
  list, because the person chose it here. A denial also reaches running
  copies at once through `user_revoke_capabilities`. Sockets already
  open stay open.
- FORGET PROGRAM (Delete) removes the record, so the program asks again.
  Running copies lose everything beyond the defaults.
- Not done: "last used" and the log of blocked actions. Without an
  account the window says to log in. With a damaged store it says that
  programs are refused everything extra.

### Built-in programs

Programs from the initramfs keep their account-wide read today. Later
they should go through the same model, with defaults that match their job
(Files has `files.shared` and `files.all`, for example). This is last in
the order below, because the desktop tools are integrated and the risk is
lower than a third-party program.

## Migration of existing disk programs

Disk programs installed before step 2 read shared files freely. They keep
that ability through a prompt, not silently and not by removal.

- At an account's first login with step 2, the kernel records every ELF
  executable below `/disk/Programs` (three levels deep) by hash, marked
  as allowed to ask for `files.shared`. A marker in the grants store says
  this ran, so it runs once per account.
- The first time such a program tries to read a shared file, the desktop
  asks once: program name, "read every file on your disks that is not
  private", "Deny", "Allow". It asks on first use, not at launch, so a
  program that never reads shared files never asks.
- Allow stores `files.shared` for that executable hash. Deny stores the
  refusal. Neither asks again until the user changes it in the Security
  Center.
- A program installed after that login has no record, gets no prompt and
  is refused shared files. It can still use the picker and its program
  folder.
- Programs outside `/disk/Programs` were never listed in Start, so they
  are not recorded. They are refused like new programs.
- Changing an executable changes its hash, so an updated older program
  loses its record and is treated as new.

## Order of work

1. Done. Add the capability bit set and the grants store
   (`kernel/permission_grants.c`). Tests are in
   `tools/test_app_permissions.c` and `tools/test_permission_grants.c`.
   No behavior change yet, except that programs cannot reach
   `Permissions.dat`.
2. Done. Enforce `files.shared`, with the migration prompt above.
   `sdk/examples/sharedread.c` reports which paths a disk program may
   read.
3. Done. Enforce `net` and `clipboard.read` with the desktop prompt.
4. Done. Manifest note and SDK support.
5. Done. Security Center window.
6. Built-in programs.

Each step must keep ABI version 1 working. New calls and fields are
added. Existing ones do not change meaning, except that a call a program
was never meant to have can now fail with `EACCES`.

## Open questions

- Resolved: a denied `net` call fails at `socket()`, the one entry point.
  A program that creates a socket and never connects asks anyway, which
  is rare.
- How are grants handled for programs run by the Terminal from a script?
- Should the picker also hand out directory grants, not only one file?
  A document editor wants a folder.
- How does a user grant `files.shared` knowing it is broad? The prompt may
  need to say "every file on this disk that is not private".

## Not covered

Memory and CPU limits, per-program network filters, camera and microphone
(no drivers exist), and sandboxing built-in desktop tools from each other.
