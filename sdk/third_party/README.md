# Third-party decoders

Single-file libraries used by OPOS programs (the Media Player). They are
copied here **unchanged**, as published upstream. Any change belongs
upstream; OPOS only sets their documented configuration macros, such as
`DR_WAV_NO_STDIO` and `PLM_NO_STDIO`, before including them.

| File | Library | License | Upstream commit | SHA-256 |
| --- | --- | --- | --- | --- |
| `dr_wav.h` | dr_wav 0.14.6 (David Reid) | public domain or MIT-0 | [mackron/dr_libs `dfe8377`](https://github.com/mackron/dr_libs/tree/dfe8377631000664666519fdb83da193fd8037f4) | `03e70c1a2d9787cd7ed3e966c075bea7bac6373f759db9cd7ca9ccdfc4ec4493` |
| `dr_mp3.h` | dr_mp3 0.7.4 (David Reid; based on minimp3) | public domain or MIT-0 | same | `997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f` |
| `dr_flac.h` | dr_flac 0.13.4 (David Reid) | public domain or MIT-0 | same | `111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c` |
| `stb_vorbis.c` | stb_vorbis 1.22 (Sean Barrett) | MIT or public domain | [nothings/stb `fe3a2a7`](https://github.com/nothings/stb/tree/fe3a2a73bfd5f38f8117d223896de72cc2aea30c) | `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636` |
| `pl_mpeg.h` | PL_MPEG (Dominic Szablewski) | MIT | [phoboslab/pl_mpeg `c871f2b`](https://github.com/phoboslab/pl_mpeg/tree/c871f2be022ece7ef4f64230b4fb8e1fb9eb6023) | `3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9` |

`NOTICES.txt` holds their license notices; it ships in every package
that uses them.

## QuickJS (the browser's JavaScript engine)

`quickjs/` holds the engine files of QuickJS 2026-06-04 by Fabrice
Bellard and Charlie Gordon (MIT, see `quickjs/LICENSE`), unchanged from
the release tarball `https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz`
(SHA-256 `b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a`).
Only the files the engine needs are kept: `quickjs.c`, `libregexp.c`,
`libunicode.c`, `cutils.c`, `dtoa.c`, their headers, `LICENSE` and
`VERSION`. The SDK Makefile builds them into `build/libquickjs.a` with
only SDK headers; `quickjs-opos/pthread.h` (written for OPOS) stands in
for the pthread calls, since OPOS programs have one thread. The licence
ships in the system as `/share/licenses/quickjs.txt`.

To update it, unpack the new release, copy the same files over, rebuild
the browser and update the version and SHA-256 above and the
`CONFIG_VERSION` in the SDK Makefile.

To update one, download the new upstream file, check that it builds and
plays, and update its row here (commit and `sha256sum`).
