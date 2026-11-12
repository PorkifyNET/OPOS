# OPOS Store packages

The packages OPOS publishes in its Store, built from this repository.
`make store` builds each package listed in `STORE_PACKAGES` in
`GNUmakefile`, and each Linux program in `STORE_LINUX` with
`linux/<name>/build.sh` (docs/linux-programs.md), and writes
`build/store/`:

- one `<id>-<version>.opk` per package,
- `index`, listing them with the descriptions in `descriptions.txt`,
- `index.sig`, signed with the store key
  (`STORE_KEY`, default `~/.opos-signing/store-key.pem`).

Linux programs are downloaded from their pinned source releases (each
checked against its SHA-256) and built with a static musl compiler:
`build/musl-local.specs` when present, otherwise `musl-gcc`
(`apt install musl-tools`). Override with `LINUX_CC=...` and
`MUSL_LICENSE=...`.

Upload everything in `build/store/` as the assets of a new release of
`PorkifyNET/opos-packages`. OPOS reads
`releases/latest/download/index` from there. See `docs/packages.md`.

To test without publishing, copy `build/store/` to an OPOS disk and
write its path (for example `/disk/store/`) into
`/disk/OPOS/PACKAGE-SOURCE.TXT`, or put it on a CD or USB stick as
`opos-packages/`.

To publish a new version of a package, raise `version` in its `.pkg`
file and in its `OPOS_MANIFEST`, then run `make store` again. The
index keeps only the versions built this time.
