# OPOS application SDK

Build programs for OPOS without the OPOS source tree. This folder is all
you need: a header, a start-up file and a linker script.

## Build

On Linux or WSL with GCC (or Clang as `CC=clang`):

```sh
make                # builds build/libopos.a and the examples
make APP=myapp      # builds build/myapp from myapp.c in this folder
```

A program is a static x86-64 ELF file of at most 1 MiB, using up to 2 MiB
of memory for its code and data. It links `build/crt0.o` and
`build/libopos.a`:

- `include/opos.h`: the system calls, and helpers such as `opos_print`.
- `include/stdio.h`, `stdlib.h`, `string.h`, `ctype.h`: a small C library.
  It has `printf`, `snprintf` and `puts`, `malloc`, `calloc`, `realloc`
  and `free`, `strtol` and `atoi`, the usual string and memory functions,
  and character classes. There is no floating-point formatting and there
  are no `FILE` streams.
- `include/opos_gfx.h`: drawing into a pixel canvas (rectangles, lines,
  text in the system font) and showing it in the program's window.

```c
#include <stdio.h>

int main(int argc, char **argv, char **envp) {
    printf("Hello, OPOS! I got %d arguments.\n", argc - 1);
    return 0;
}
```

`main` runs on a 128 KiB stack with a guard page below it (set up by
`src/crt0.S`), so ordinary local arrays are fine.

## Run

Copy the file to an OPOS disk, for example with mtools on a disk image
whose FAT partition starts at 1 MiB:

```sh
mcopy -i disk.img@@1M build/hello ::hello
```

In OPOS, type `run /disk/hello` in the Terminal, or double-click the
file in Files. Output appears in the Terminal window, which opens when a
program first writes something.

Arguments follow the path: `run /disk/hello one "two words"` gives
`argv[1]` = `one` and `argv[2]` = `two words`. A path with spaces goes in
double quotes too. Up to 15 arguments fit. Files starts programs without
arguments.

Programs run side by side, each on its own task, and the desktop stays
responsive while they compute. A program reading standard input gets the
lines typed into the Terminal. In the Terminal, `ps` lists running programs,
and `kill PID` or Ctrl+C (the newest program started there) ends one, with
status 137, even while it waits. The Task Manager (Start menu, or
Ctrl+Shift+Esc) shows each program's CPU share, memory and threads, and
ends the selected one. Logging out ends every program. `exit_group` ends
all of a program's threads.

## Windows

A program can open one window. It draws 0x00RRGGBB pixels in its own
memory, presents them, and waits for events:

```c
uint32_t *pixels = opos_alloc(640 * 480 * 4);
opos_window_create("MY APP", 640, 480);
struct opos_event event;
for (;;) {
    opos_window_event(&event, -1);           /* wait */
    if (event.type == OPOS_EVENT_CLOSE) {
        return 0;
    }
    /* ... draw into pixels ... */
    opos_window_present(pixels, 640, 480, 640);
}
```

- `opos_window_create(title, width, height)` asks for a content area of
  that size (at most 1024 x 768). In tiled layout the window gets its tile
  instead. The first event is `OPOS_EVENT_RESIZE` with the real size, and
  another follows whenever the user resizes the window.
- `opos_window_present(pixels, width, height, stride)` copies the pixels
  to the window's top left. `stride` is the row length in pixels. Parts
  outside the window are cut off; the rest of the window is dark.
- `opos_window_event(&event, timeout_ms)`: `-1` waits for ever, `0` only
  checks, otherwise waits at most that long. Returns 1 with an event and 0
  without one. Events:

  | Type | Fields |
  | --- | --- |
  | `OPOS_EVENT_KEY` | `key`: printable ASCII, `'\n'`, `'\b'`, `'\t'`, or `OPOS_KEY_*` (arrows, Home, End, Page Up/Down, Delete, Escape); `buttons`: `OPOS_MOD_CTRL`, `OPOS_MOD_SHIFT` held. Shift already makes letters upper case |
  | `OPOS_EVENT_MOUSE_DOWN`, `_UP` | `x`, `y` in the content area; `button` (`OPOS_BUTTON_LEFT` or `_RIGHT`); `buttons` held |
  | `OPOS_EVENT_MOUSE_MOVE` | `x`, `y`, `buttons`. Sent while the pointer is over the focused window, or while a button pressed in it is held (then `x`, `y` can be outside) |
  | `OPOS_EVENT_RESIZE` | `width`, `height` of the content area |
  | `OPOS_EVENT_CLOSE` | The user closed the window or logged out. Save and exit: waiting again ends the program |
  | `OPOS_EVENT_WHEEL` | `x`, `y`, `buttons`; `OPOS_WHEEL_NOTCHES(event)` is the signed number of wheel notches, positive = scroll up. Sent for the mouse wheel and two-finger touchpad scrolling while the pointer is over the window, even when the window is not focused |

- `opos_window_size(&width, &height)` reads the current content size.
- `opos_clipboard_set(text, length)` replaces the desktop's shared
  clipboard (the one Ctrl+C/V use in every window; at most 64 KiB are
  kept). `opos_clipboard_get(buffer, capacity)` copies up to capacity bytes
  and returns the clipboard's full length.
- The desktop keeps the Super shortcuts; every other key goes to the
  focused program window.
- Windows only open from the desktop. From the kernel shell,
  `opos_window_create` returns `-19`.

Examples built on these calls:

- `examples/sketch.c` is a small drawing program: `run /disk/sketch
  [width height]`.
- `examples/notepad.c` is the OPOS text editor. It is built with the OS
  and shipped as `/bin/notepad`. The Start menu's Text Editor and Files
  (for text files) open it. It shows text drawing, keyboard shortcuts
  with modifiers, mouse selection, and loading and saving files.

Networking: `opos_net.h` has IPv4 TCP and UDP sockets (the Linux socket
calls: `opos_socket`, `opos_connect`, `opos_bind`, `opos_listen`,
`opos_accept`, `opos_send`, `opos_recv`, `opos_sendto`, `opos_recvfrom`,
`opos_poll`, closed with `opos_close`) and `opos_net_resolve` for DNS.
`examples/fetch.c` prints a web page over HTTP or HTTPS; it ships as
`/bin/fetch`: `run /bin/fetch https://example.com`, or
`run /bin/fetch -o /disk/page.html https://example.com` to save the body.

HTTPS: `opos_tls.h` (`opos_tls_connect`, `opos_tls_read`, `opos_tls_write`,
`opos_tls_close`, `opos_tls_error_text`) runs TLS 1.0-1.2 with BearSSL
(`bearssl/`, MIT licence, built as `build/libbearssl.a` and linked only
into programs that use it; about 120 KiB). Certificates are checked
against `/etc/ssl/certs/ca-certificates.crt`, the host name and the clock.
The full BearSSL API is available through `<bearssl.h>` too.

HTTP: `opos_http.h` has `opos_http_get(url, max_body, &response)` and
`opos_http_post(url, content_type, body, length, max_body, &response)`.
They speak HTTP/1.1 over plain TCP or TLS, follow redirects (a POST
redirected with 301, 302 or 303 continues as a GET), and decode chunked and
gzip bodies. `opos_url_resolve` resolves relative links.

Images: `opos_image.h` decodes PNG (every colour type and bit depth,
interlaced too), JPEG (baseline and progressive) and BMP. It is the same
decoder the desktop uses (`lib/image.c` is generated from
`kernel/image.c`; edit that one). It needs no C library: probe the file
for its size and the scratch memory it needs, then decode into that
memory. Pixels come out as 0xAARRGGBB:

```c
struct image_info info;
if (image_probe(data, size, &info) == IMAGE_OK) {
    void *scratch = opos_alloc(info.scratch_bytes);
    struct image picture;
    if (image_decode(data, size, scratch, info.scratch_bytes,
                     &picture) == IMAGE_OK) {
        /* picture.width, picture.height, picture.pixels */
    }
}
```

`image_resample` scales (part of) a picture to another size.

Memory: `opos_alloc(size)` returns zeroed memory (`mmap`);
`opos_free(address, size)` returns it. `malloc` takes its memory from
there too. A program has 64 MiB of such memory in total, including its
128 KiB stack.

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
  | 0x4f500010 | `opos_window_create(title, w, h)` | 0, or -16 (already open), -19 (no desktop) |
  | 0x4f500011 | `opos_window_present(pixels, w, h, stride)` | 0 |
  | 0x4f500012 | `opos_window_event(&event, timeout_ms)` | 1 (event) or 0 |
  | 0x4f500013 | `opos_window_size(&w, &h)` | content area size |
  | 0x4f500014 | `opos_clipboard_set(text, length)` | bytes kept |
  | 0x4f500015 | `opos_clipboard_get(buffer, capacity)` | full clipboard length |

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
home folder in `/disk/Users`. Programs launched from disk can write only
inside their private directory; `opos_app_storage(path, capacity)` returns
its path. To read a file elsewhere in their account home, call
`opos_pick_file(path, capacity)` and open the returned path. The user chooses
the file in Files; Esc or closing Files cancels the request. Built-in
programs from the ISO retain the broader shared-disk
access described in the security model.

`sdk/build/pickfile` is a small example. Copy it to `/disk/Programs`, launch
it from Start > Programs, and select a document in Files. OPOS creates the
folder when Start > Programs opens. A disk app with the same name as a
built-in `/bin` app does not replace the built-in menu entry.
See `docs/security-model.md`.

## Limits

- Each program gets one window. Up to four program windows can be open at
  once, and up to eight programs can be started from the desktop at a time.
- There are no widgets: programs draw their own pixels and text.
- The C library is small. It has no floating-point formatting, no
  `FILE` streams and no `qsort`, `time` or `locale`.
- Program files over 1 MiB do not load. Memory from `opos_alloc`,
  `malloc` and the stack shares 64 MiB per program.
- Programs share the desktop text clipboard through `opos_clipboard_set` and
  `opos_clipboard_get`.
