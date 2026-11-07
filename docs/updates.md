# OPOS updates

Installed copies of OPOS update themselves from this repository's GitHub
Releases. Settings > UPDATES checks when it is first opened and shows the
release notes, an UPDATE button, progress, and the updater's log under
SHOW DETAILS. In the Terminal or kernel shell, `update` checks and
`update install` installs. Only an administrator can install updates.
Terminal installs show a summary and require typing `YES` before payload files
are downloaded. Development-only flags are available:

- `--ignoreversion` permits same-version installs and downgrades.
- `--force` bypasses signature and file SHA-256 checks. It does not bypass
  administrator, manifest/path, download-completeness or disk-write checks.
- `--no-prev-version` skips backing up the current kernel and initramfs and
  does not create a new previous-version boot entry.

These flags compose. The summary explains every active flag and its risks.
Using any of them implies `install`, so `update --ignoreversion` is shorthand
for `update install --ignoreversion`.

## Publishing a release

1. Raise the number in `VERSION` (for example `0.1.0` to `0.2.0`). Normal
   installs only accept a version higher than their own. Up to four numbers.
2. `make release` (in WSL). It builds the ISO and writes `build/release/`:
   - `opos-update.manifest`: the version and each system file's SHA-256,
     size, release file name and install path;
   - `opos-update.sig`: the manifest's signature;
   - `kernel.elf`, `initramfs.tar`: the files themselves;
   - `opos-release-notes.txt`: a copy of `release-notes.txt` (edit it for
     each release, at most 4 KB; shown before installing, not signed);
   - `opos-VERSION.iso`: the installer, for new installs. It also carries
     the signed manifest, signature and notes in `/boot`, so it works as
     update media (see below).
3. Create a GitHub Release (tag e.g. `v0.2.0`), upload every file from
   `build/release/`, and leave it marked as the latest release. OPOS reads
   `releases/latest/download/opos-update.manifest`.

`make release` signs with `~/.opos-signing/update-key.pem` (override with
`UPDATE_KEY=...`) and refuses a key that does not match
`kernel/update_key.h`.

## The signing key

`tools/update-keygen.sh` made the key (ECDSA P-256) and wrote its public
half into `kernel/update_key.h`. The private key never goes into the
repository. Anyone who has it can publish updates that every OPOS install
accepts, so keep it private and keep a backup (an offline copy). If it is
lost, a new key needs a new `update_key.h`, and existing installs only
accept updates signed with the new key after they are reinstalled from an
ISO built with it.

## What an install does

`/bin/update` (sdk/examples/update.c) downloads; the kernel
(`kernel/update.c`) checks and installs:

1. The kernel checks the manifest's signature against `update_key.h` and
   that its version is newer, then compares every listed file with the
   installed one. Only files that differ are downloaded.
2. Each changed file goes into `/disk/OPOS/UPDATE` and its SHA-256 is
   checked as it arrives. A damaged download is retried (3 tries).
3. The running `boot/kernel.elf` and `boot/initramfs.tar` are copied to
   `boot/old/`, and `limine.conf` gets an "OPOS (previous version)" entry
   that boots them. Then the new files are copied into place, the kernel
   last, and each is checked again.
4. The new version runs after a restart.

`STATE.TXT` in the staging folder records the step. If the power fails
during step 3, OPOS finishes the update at the next start (before the
desktop opens); until then "OPOS (previous version)" still boots.

Manifest paths must be under `boot/` (not `boot/limine/` or `boot/old/`):
updates do not change the bootloader or the boot menu. A manifest line this
kernel does not know makes the update "need a newer updater", so a later
format can add features (compression, components, bootloader updates) by
first shipping a kernel that understands them.

## Where updates come from

The updater uses the first of:

1. `--source URL` or `--source /FOLDER/` on the command line;
2. update media: a CD/DVD or extra disk (USB stick) whose top folder,
   `opos-update/` or `boot/` holds `opos-update.manifest` (with
   `opos-update.sig` and the files next to it). The release ISO is such
   media: attach it to a VM's CD drive, or copy `build/release/` to a USB
   stick's `opos-update/` folder. No network is needed. OPOS's own disk
   (`/disk`) is not searched;
3. the first line of `/disk/OPOS/UPDATE-SOURCE.TXT` (a URL or folder);
4. GitHub.

Settings > UPDATES uses the same order, so attaching media is enough there.

### From the VM host

`tools/serve-release.ps1` serves `build/release/` over HTTP from Windows
(no Python or admin rights needed):

    powershell -ExecutionPolicy Bypass -File tools\serve-release.ps1

It prints the host's addresses. In OPOS run
`update install --source http://HOST-IP:8000/` with the address on the VM's
network (VMware NAT: the "VMnet8" adapter). Allow PowerShell through the
firewall on private networks if Windows asks.

## Testing locally

Serve a release folder over HTTP from WSL and point the updater at it.
QEMU's user network reaches the host at 10.0.2.2:

    python3 -m http.server 8000 --directory build/release
    update install --source http://10.0.2.2:8000/

During development, add `--ignoreversion` to reinstall rebuilt files without
changing `VERSION`. Add `--force` only when intentionally testing unsigned or
hash-mismatched files. Both modes remain administrator-only. The first build
using these flags must be installed normally so the running kernel supports
the new updater options.

Settings > UPDATES takes no arguments: put the URL on the first line of
`/disk/OPOS/UPDATE-SOURCE.TXT` instead (delete the file to go back to
GitHub). The signature is checked whatever the source unless the explicitly
unsafe `--force` flag is used, so a normal test release must be signed with the
real key too.
