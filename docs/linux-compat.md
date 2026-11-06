# Linux compatibility and user-mode internals

Detailed notes on how OPOS runs user programs: the syscall ABIs, the ELF
loader, the dynamic interpreter, processes and threads. They were written
milestone by milestone, and some limits listed here may lag behind the code.
Check the source (`kernel/user.c`, `kernel/elf.c`, `kernel/dynamic_*.c`,
`kernel/scheduler.c`, `kernel/vfs.c`) when in doubt.

These calls prove the ABI; they are not yet a general Linux compatibility
layer.

## Ring 3, shell tests and the OPOS syscall ABI

`run demo` launches the built-in ring-3 program and reports its PID
and exit status. It runs synchronously, one process at a time. `run fault`
tests read-only user code protection; the resulting user page fault terminates
that process with status 142 (128 plus page-fault vector 14) and returns to the
shell. `run nx` attempts to execute code from the non-executable user stack,
testing NX enforcement with the same contained-fault behavior. Other CPU
exceptions originating in user processes are likewise reported and contained
instead of halting the kernel.
`run input` exercises syscall 3: it reads one line from the PS/2 keyboard into
a bounded user buffer and echoes input as it is typed. Backspace edits the
line; Enter completes the read. The syscall waits with interrupts enabled, so
timer and keyboard IRQs continue while the process is blocked; this is
synchronous and does not provide scheduling. `run user-shell` launches a
ring-3 prompt with `help`, `echo`, and `exit` commands; the kernel shell resumes
after it exits.
`ls` lists files in a read-only USTAR initramfs bundled on the boot ISO.
`run elf` is a shortcut for `run /tests/elf-test`; `run path` and `run /path` look up that
archive path and loads the file as an ELF executable in ring 3, with
`argv[0]` and `AT_EXECFN` set to the absolute path. A dynamically linked file
starts in its `PT_INTERP` interpreter with `AT_BASE`, exactly as after
`execve`, so `run /tests/pie-test` and `run /tests/dynamic-test` link and run
directly. The archive is
validated during boot, including USTAR checksums and file bounds. The archive is
also the VFS root, so Linux applications can open `/tests/elf-test` and enumerate
`/` or `/bin`; its contents remain packaged into the boot image. Writable
applications use the mounted FAT16 `/disk` namespace.
The archive packages the primary ELF regression, static and dynamic
exec-replacement images, the bounded `/lib/opos-ld.so` interpreter, and a test
shared object; this is not yet a general application environment. The
ELF program's Linux initial stack includes one argument, an empty
environment, `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`,
root-valued `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID`, `AT_SECURE`, `AT_RANDOM`,
`AT_EXECFN`, `AT_PLATFORM`, `AT_HWCAP`, `AT_HWCAP2`, `AT_CLKTCK`, `AT_FLAGS`,
and `AT_NULL`. `AT_PLATFORM` is `x86_64`, `AT_HWCAP` exposes CPUID leaf-1 EDX,
`AT_HWCAP2` is zero because no corresponding kernel-enabled facility is
advertised, and `AT_CLKTCK` is 100. `AT_RANDOM` addresses 16 aligned stack bytes filled
with `RDRAND` when available or a mixed TSC/timer fallback; that fallback is not
a cryptographic entropy source. The test validates the auxiliary values,
initialized data, and zero-initialized BSS before modifying both data regions.
ELF bytes are copied
into process-owned pages before execution; the module's memory remains
untouched.

The kernel can enter ring 3 to run tiny user programs. Built-in programs use
the OPOS `int 0x80` syscall ABI, where `RAX` selects `1` to write to standard output
from the buffer in `RSI` with length in `RDX`, `2` exits with status in `RDI`,
`3` reads from standard input into the writable buffer in `RSI`, up to the
length in `RDX` (maximum 128 bytes), `4` spawns a child, and `5` waits for the
child handle in `RDI`. Spawn selector `RDI=0` starts the bounded built-in test;
selector `RDI=1` resolves the initramfs path at `RSI` with length `RDX`, accepts
paths with or without a leading slash, and launches that ELF image. Wait optionally
writes the child's PID through the writable pointer in `RSI` and returns its
exit status. Read, write, and wait buffers are
validated against user mappings, and invalid requests return negative error
numbers. The ELF test also uses the Linux x86-64 `syscall` instruction with
Linux syscall numbers `read` (0), `write` (1), `open` (2), `close` (3),
`stat` (4), `fstat` (5), `lstat` (6), `lseek` (8), `writev` (20), `pipe` (22), `getpid` (39),
`dup` (32), `dup2` (33), `dup3` (292),
`fork` (57), `execve` (59), `wait4` (61),
`truncate` (76), `ftruncate` (77),
`uname` (63), `getcwd` (79), `chdir` (80), `rename` (82), `mkdir` (83), `rmdir` (84),
`unlink` (87), `getuid` (102),
`getgid` (104), `geteuid` (107), `getegid` (108),
`arch_prctl` (158), `getdents64` (217), `clock_gettime` (228),
`clock_getres` (229), `gettimeofday` (96), `nanosleep` (35), `openat` (257),
`mkdirat` (258), `newfstatat` (262), `unlinkat` (263), `renameat` (264),
`pipe2` (293), `renameat2` (316, flags zero only), `exit` (60), and
`exit_group` (231); it verifies system
identity, root UID/GID queries, vectored stdout, monotonic clock reads and
resolution, a PIT-backed relative sleep, uptime-based `gettimeofday`, FS-base thread-local storage
setup/query, rejects invalid user pointers, obtains its process ID, and exits
using that ABI. UID/GID queries return 0 because OPOS currently has a single
root identity and no account or permission model. `uname` returns
the Linux ABI identity with OPOS-specific release and host fields. The clock
calls support `CLOCK_MONOTONIC` only and derive time from the 100 Hz PIT;
`gettimeofday` likewise reports uptime rather than wall-clock epoch time.
`nanosleep` rounds positive durations up to PIT ticks, puts the calling task on
a scheduler wake deadline, and lets other tasks run. It does not support signal
interruption or a remaining-time result. Realtime clock IDs and actual wall-clock
time are not available. `arch_prctl`
currently supports `ARCH_SET_FS` and `ARCH_GET_FS` only, with nonzero FS bases
required to refer to a mapped user address.

## Scheduler, processes and descriptors

The PIT also drives an eight-slot round-robin task scheduler. It saves the
general-purpose interrupt context and switches address spaces, 16 KiB execution
stacks, and separate 16 KiB kernel-entry stacks on timer ticks. `schedtest` runs
two kernel workers for several ticks and verifies that both complete. Ring-3
tasks are preemptible while executing user code. Sleeping tasks deschedule and
wake on PIT deadlines. Keyboard reads atomically block on a scheduler event;
IRQ1 queues a character and wakes matching tasks without a lost-wakeup window.
Each user process now has a process control block that owns its PID, lifecycle
and fault state, address space, bounded image segments and pages, stack page, heap,
anonymous mappings, current directory, and eleven descriptor slots numbered
0 through 10. Opens allocate slots 3 through 10. Processes start in `/`; spawned children inherit the parent's
current directory. Path resolution canonicalizes repeated separators, `.`, and
`..` against the global mount namespace without traversing above `/`.
Each open creates an open-file description with its own offset and access mode.
`dup`, `dup2`, and `dup3` descriptors share that description and offset; the
underlying VFS object remains alive until its final descriptor closes. `dup3`
accepts `O_CLOEXEC`, while `dup2`/`dup3` can replace slots 0 through 10.
Descriptions share one of 32 refcounted open-file records and
support up to 4 KiB per filesystem I/O
call, and are closed automatically on process exit or fault. Standard
input/output/error default to keyboard and console devices at descriptors 0, 1,
and 2, but can now be closed or redirected to an explicit open-file description.
`pipe` and `pipe2` create read/write endpoints backed by one of eight 4 KiB ring
buffers. Reads block while an empty pipe still has writers, writes up to 4 KiB
block until the whole write fits, final-writer close produces EOF, and writing
without readers returns `EPIPE` without signal delivery. `pipe2` accepts
`O_CLOEXEC`; nonblocking mode is not implemented. Pipe endpoints survive
`fork` through shared open-file-description references, support FIFO `fstat`,
and reject `lseek` with `ESPIPE`. The regression redirects a forked child's
stdout through `dup2`, sends a two-vector `writev`, wakes a blocked parent read,
checks EOF and exit status, fills a pipe to force and wake a blocked writer,
then verifies `EPIPE` and resource reuse.

## Sockets, poll and select

`socket`, `connect`, `bind`, `listen`, `accept`/`accept4`, `sendto`/`send`,
`recvfrom`/`recv`, `sendmsg`, `recvmsg`, `shutdown`, `getsockname`,
`getpeername`, `setsockopt` (accepted, ignored) and `getsockopt`
(`SO_ERROR`, `SO_TYPE`) work for `AF_INET` `SOCK_STREAM` (TCP) and
`SOCK_DGRAM` (UDP) sockets on top of `kernel/net.c`. `read`, `write`,
`writev`, `close`, `fstat` (`S_IFSOCK`) and `fork` sharing work on socket
descriptors. `SOCK_NONBLOCK`, `MSG_DONTWAIT` and `fcntl(F_SETFL, O_NONBLOCK)`
give `EAGAIN`/`EINPROGRESS`; `MSG_PEEK`, IPv6, `AF_UNIX` and `socketpair`
are not supported. Blocking calls poll the network card and wait one timer
tick at a time, so a killed process leaves them with `EINTR`.

Limits are system-wide: 8 TCP connections, 4 listening ports with up to 8
queued connections each, and 8 UDP sockets with 8 queued datagrams each.
There is no loopback interface, with one exception: UDP to `127.0.0.1:53` is
forwarded to the DHCP-provided DNS server and the answers come back from
`127.0.0.1`. That is where musl's resolver asks when `/etc/resolv.conf` is
missing, so `getaddrinfo` works unchanged.

`poll`, `ppoll`, `select` and `pselect6` report readiness for sockets and
pipes (regular files are always ready; console input is never reported
ready). `fcntl` handles `F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`, `F_SETFD`,
`F_GETFL` and `F_SETFL`. `open` accepts and ignores `O_LARGEFILE` and
`O_NOCTTY`, which musl adds on its own.

`/tests/musl-net-test [host [port]]` resolves a name with `getaddrinfo`
and fetches `/` over HTTP; `/tests/musl-net-test serve [port]` answers one
HTTP request (test it in QEMU with `hostfwd=tcp::8080-:8080` and `curl`).

The main thread's stack is 32 pages (128 KiB) below `0x80000000`; the top
page holds argv, envp and the auxiliary vector.

## execve and the dynamic interpreter

`execve` atomically replaces the current process image from either initramfs or
FAT16 while preserving its PID, cwd, and descriptors not marked `O_CLOEXEC`.
It closes `O_CLOEXEC` descriptors, resets FS base, discards the old heap and
anonymous mappings, and constructs a new Linux initial stack containing the
provided argument and environment vectors plus the supported auxiliary values.
`AT_EXECFN` preserves the `execve` pathname; direct shell, desktop, and disk
launches pass their selected filename through the same launch interface.
An `ET_EXEC` image may name `/lib/opos-ld.so` through one validated `PT_INTERP`.
Main `ET_DYN` PIE images are loaded at the fixed bias `0x04000000`; dynamically
linked PIEs provide `PT_PHDR` so the interpreter can derive that bias from
`AT_PHDR`. ASLR and randomized PIE placement are not implemented.
The kernel loads that `ET_DYN` interpreter at `0x10000000`, adds `AT_BASE`, and
starts at the interpreter entry while `AT_ENTRY` continues to identify the main
program. The interpreter regression validates the argument/environment layout
and auxiliary vector, initialized data and BSS, then recursively walks and
deduplicates up to four direct or transitive `DT_NEEDED` entries. It opens each
below `/lib`, maps a private anonymous 64 KiB image, and copies two to eight
page-congruent `PT_LOAD` segments from their declared file offsets. Each library
must have exactly one RX segment, exactly one RW segment, and only R segments
otherwise; overlapping pages and W+X mappings are rejected. Used pages receive
their final R/RX/RW protections and unused image pages are unmapped. It resolves
symbols through System V or
GNU hash tables and eagerly applies `RELA` records of
types `R_X86_64_RELATIVE`, `R_X86_64_64`, `R_X86_64_GLOB_DAT`, and
`R_X86_64_JUMP_SLOT`. Main-executable `R_X86_64_COPY` records copy bounded
object data from the defining dependency while preserving executable-first
symbol preemption afterward. Ordinary relocations for the full object graph complete
before a second eager resolver pass handles named GNU `STT_GNU_IFUNC` symbols
and local `R_X86_64_IRELATIVE` entries. Resolver addresses and their returned
targets must lie in executable mappings. It also validates `DT_RELR`, `DT_RELRSZ`, and
`DT_RELRENT`, then decodes both direct and 63-slot bitmap entries while limiting
all writes to the object's writable segment. Global lookup searches the main
executable first and then libraries in dependency-load order, supports
default/protected global and weak
definitions, binds non-default definitions within their defining object, and
resolves missing weak references to zero. GNU `DT_VERSYM`, `DT_VERNEED`, and
`DT_VERDEF` metadata select exact name/hash versions; unversioned references use
only unversioned or default definitions, while explicit legacy references may
select hidden non-default definitions. Version definition/need chains and their
auxiliary lists are each bounded to 16 records and validated inside mapped
segments. It collects each `PT_TLS` image into
a bounded one-page static TLS block, preserves initialized bytes, zero-fills
TLS BSS, installs the thread pointer through `ARCH_SET_FS`, and applies
`R_X86_64_TPOFF64` initial-exec relocations across the global symbol scope.
Each TLS-bearing object receives a stable module ID. A minimal x86-64 TCB points
to a fixed DTV containing every module base; `R_X86_64_DTPMOD64` and
`R_X86_64_DTPOFF64` support general/local-dynamic access, and the interpreter
provides `__tls_get_addr` to loaded objects. After all ordinary and IFUNC
relocations, each bounded `PT_GNU_RELRO` range is validated inside its RW
segment and every complete covered page is made read-only before constructors
run.
Eager `R_X86_64_TLSDESC` relocation installs bounded 16-byte descriptors for
static TLS. The x86-64 return trampolines preserve every register except `RAX`
and flags, and support both resolved symbols and unresolved weak TLS addresses.
After relocation it runs `DT_INIT` and `DT_INIT_ARRAY` hooks dependency-first,
followed by the main executable. The interpreter passes
an idempotent finalizer in `RDX`; it runs the main executable's
`DT_FINI_ARRAY`/`DT_FINI` hooks first and then library hooks in reverse
initialization order. The kernel performs no relocations itself. More than four
libraries, alternate search paths, cyclic initialization graphs, symbol
version namespaces beyond GNU name/hash matching, DTV growth, dynamic TLS
allocation, TLS destructors, threads, lazy binding, libc `atexit`, unloading, and a
production dynamic linker remain unimplemented.

## ELF segment model and regression programs

The kernel applies the same segment model to `ET_EXEC` main images and `ET_DYN`
interpreters: two to eight non-overlapping `PT_LOAD` entries, exactly one RX,
exactly one RW, remaining entries R, and at most 16 mapped pages total. File and
virtual offsets may differ and need only be page-congruent. Process ownership,
fork cloning, exec replacement, fault cleanup, and normal exit preserve every
segment's R/RX/RW permissions.
The current bounded implementation accepts at most 16 arguments, 16 environment
entries, 1024 total string bytes, a 1 MiB executable file (2 MiB of loaded segments), and a 64 KiB interpreter. The regression
executes `/tests/exec-test` and verifies replacement, arguments, descriptor
inheritance, close-on-exec, FS reset, private file mappings, dynamic-interpreter
handoff, eager `JUMP_SLOT` relocation, a call into `/lib/libopos-test.so`, and
a transitive call from that System V-hashed consumer into the GNU-hashed
`/lib/libopos-base.so`. Constructor state must advance base to consumer to main;
finalizer state must advance main to consumer to base. The consumer also resolves
an exported main-executable symbol, observes main-executable preemption over a
weak dependency definition, and receives zero for an unresolved weak symbol
before checking initialized and zero-filled TLS in the executable and both
libraries. The consumer additionally resolves a TLS symbol from its transitive
dependency before successful exit. Compiler-generated general-dynamic access to
that dependency and local-dynamic access to the consumer then mutate and reread
the same TLS slots through `__tls_get_addr`.
The version regression resolves both legacy `OPOS_1.0` and default `OPOS_2.0`
definitions from the GNU-hashed base library and proves that an unversioned
main-executable export cannot preempt either versioned reference.
The RELR regression relocates four adjacent function pointers using one direct
entry and one bitmap entry; the base library verifies every relocated pointer
before successful exit.
The IFUNC regression resolves an exported indirect function through the
consumer's `JUMP_SLOT` and a local indirect function through the base library's
`IRELATIVE` entry; both select and execute the expected implementation.
The copy-relocation regression emits a real 16-byte `R_X86_64_COPY` record in
the non-PIE main executable and verifies both words imported from the consumer
library before successful exit.
The GNU2 TLS regression is compiled with `-mtls-dialect=gnu2`; it exercises
named, local-module-base, and unresolved-weak TLS descriptors while retaining
the separate compiler-generated general/local-dynamic regression.
The consumer-library layout regression uses separate R, RX, R, and RW load
segments, an RW file offset different from its virtual address, a read-only
data probe, and a page-aligned GNU RELRO boundary while keeping mutable data on
the following writable page.
The main executable and `/lib/opos-ld.so` regressions also use separate R, RX,
R, and RW load segments. Main read-only data is accessed before dynamic calls,
the interpreter reads its isolated `.rodata`, both RW segments begin at page
offset `0x20`, and repeated process runs prove segment-page ownership is
reusable.
The exec-replacement chain now runs `/tests/exec-test`, dynamically linked
`ET_DYN` `/tests/pie-test`, non-PIE `/tests/dynamic-test`, and a real statically
musl-linked `/tests/musl-test`, which replaces itself with the real
`/tests/musl-pthread-test`, in sequence. The pthread program creates and joins a
pair of threads, forces mutex contention while each sleeps, verifies their
shared-memory result and return values, then exits normally.
The PIE resolves shared-library calls
at its nonzero bias before replacing itself. The
static image and dynamic interpreter both validate identity/security entries,
the bounded 16-byte `AT_RANDOM` target, and `AT_EXECFN` contents.
Read-only directory descriptors can open `/`, `/bin`, `/disk`, and nested
directories; `getdents64` returns Linux x86-64 records for `.`, `..`, mount
points, initramfs entries, and visible FAT16 entries, and supports EOF
and rewind through `lseek`. Stat-family calls report regular, directory, or
character-device modes, distinct initramfs/FAT16 device IDs, 512-byte file
blocks, zero UID/GID, and zero timestamps.

## Process lifecycle tests, memory mappings, fork and threads

Cleanup retains failed ownership records instead of
silently reusing a dirty process slot. `proctest` runs a contained page fault,
then starts the full ELF heap and mmap test in a fresh lifecycle; repeating the
command verifies that all resources can be reclaimed and reused.
`multitest` launches two processes with independent PCBs and address spaces.
Both execute at the same virtual addresses, block concurrently in `nanosleep`,
wake through the scheduler, resume under their own CR3 and kernel-GS state,
issue Linux-style syscalls, and exit independently. The scheduler eagerly
enables x87/SSE, keeps a zeroed 512-byte FXSAVE image per task, saves and
restores it on each task switch, resets it across `execve`, and inherits it
across task creation. Each concurrent process retains its own PID sentinel in
XMM0 across preemption. AVX/XSAVE state is not enabled yet.
`inputwaittest` starts a ring-3 reader and a background kernel task. The reader
blocks without polling, the background task completes, and keyboard IRQ input
wakes the reader. This proves an input wait does not monopolize the CPU.
`run linux-read` exercises a Linux-style blocking `read` into the user stack
and echoes the line with `write`. `run filetest` uses Linux `openat`, `write`,
`lseek`, `read`, `close`, legacy `open`, `stat`, `fstat`, `newfstatat`,
`getcwd`, `chdir`, `mkdirat`, `truncate`, `ftruncate`, `unlink`, `rmdir`,
`unlinkat`, `rename`, `renameat`, `renameat2`, and `getdents64`
to create, truncate, persist, reopen, inspect, remove, reclaim, and discover
`/disk/DOCS/USER.TXT`; it verifies exact metadata and directory-record layouts,
cwd-relative and directory-FD-relative paths, `.`/`..` normalization, error
paths, same-parent and cross-parent rename, replacement cleanup, open-object
survival after rename/replacement/unlink, descriptor-relative lookup through a
moved directory handle, detached-anchor rejection after `rmdir`, deferred file and directory chain
reclamation, directory parent repair, cycle rejection, cross-cluster directory
growth, paginated enumeration, EOF, and directory rewind. Running it twice also proves automatic
descriptor cleanup between processes. The ELF test opens, reads, and stats
`/tests/elf-test`, proves duplicated descriptors share offsets and last-close
lifetime, enumerates the root mount namespace, changes cwd to `/`, and
verifies read-only initramfs mutation errors and per-mount `st_dev` values. It
also uses `brk` (12) to allocate,
verify zeroed pages, write across the heap, shrink back, and check the heap
limit; the heap is capped at 128 pages. Linux `mmap` (9), `mprotect` (10), and
`munmap` (11) support private anonymous mappings and eager private copies of
regular files in a bounded 1024-page (4 MiB) region. File offsets must be page-aligned;
bytes beyond EOF are zero-filled. Read-only, writable/non-executable, and
executable/read-only file mappings are supported without writeback or sharing.
`MAP_SHARED`, `MAP_FIXED`, demand paging, and writable-executable mappings are
not implemented. `PROT_NONE` reserves physically backed but user-inaccessible
pages so musl can allocate guarded thread stacks. `mprotect` is restricted to
those mappings, rejects write-plus-execute, and supports inaccessible, readable,
writable/non-executable, or executable/read-only page permissions. Anonymous
executable mappings at allocation time remain rejected. The ELF test verifies
zeroed pages, private file-copy isolation, RX file mappings, permission
transitions, executable mappings, unmapping, address reuse, invalid ranges,
mapping limits, and W^X rejection.
These calls are only an ABI proof, not a Linux compatibility layer. A TSS and
the syscall entry state select the current task's kernel-entry stack. Multiple
processes can run concurrently under timer preemption. The bounded OPOS
spawn/wait ABI owns child jobs by parent PID, blocks parents on scheduler events,
and releases unwaited orphan jobs after their child exits. `run spawntest` proves
a ring-3 parent can launch one built-in child and `/tests/elf-test`, then wait for
both with distinct PIDs. Linux `fork` eagerly copies the current code, data,
stack, heap, and anonymous mappings into an isolated address space; cwd, FS base,
and descriptor table are inherited, while open-file descriptions and offsets
remain shared. The child resumes after `fork` with return value zero and the
parent receives its PID. `wait4` blocks for an exact positive PID or any child
with PID `-1`, returns the reaped PID, and writes the normal Linux exit-status
encoding. Options and resource usage are currently unsupported and must be zero
and null. Thread-style Linux `clone` shares VM, files, filesystem state, and
signal-handler state while giving each thread its own stack, TLS base, TID,
signal mask, FPU state, scheduler context, and saved syscall return stack.
`set_tid_address`, `gettid`, robust-list registration, `rt_sigprocmask`, and
untimed `FUTEX_WAIT`/`FUTEX_WAKE` (including private variants) support musl's
pthread create/join lifecycle. Thread exit clears the configured child TID and
wakes one joiner. Robust-futex owner-death recovery, timed/PI futex operations,
actual signal delivery, general clone flag combinations, `clone3`, and `vfork`
are not implemented. Copy-on-write and process groups are also absent. The
per-task syscall return stack prevents a blocking thread from having its user
RSP overwritten by another thread's syscall. The regression proves writable
data, stack, and mmap isolation,
shared descriptor offsets, FS-base preservation across scheduling, status 42
encoding, and one-time child reaping. Limine loads one required initramfs
module from the boot ISO. The ELF loader accepts `ET_EXEC` images with exactly
two page-aligned load
segments below the mmap region: read-only executable code followed by writable,
non-executable data (including zero-filled BSS), each limited to 16 pages.
Such an image may additionally contain one absolute-path `PT_INTERP`; its
interpreter must be an `ET_DYN` image with the same two-segment W^X layout and
no nested interpreter. Interpreter pages occupy the mmap arena at the fixed
`0x10000000` bias.
Segment addresses and the entry point come from the ELF headers; the test image
is deliberately linked away from OPOS's built-in program address. Its test
crosses a page boundary in both segments. It does not yet load general ELF
applications, resolve unbounded or production library graphs, support the full
Linux relocation ABI, or randomize load addresses. The
shell currently runs in the kernel, and user code pages are
read-only while data and stack pages are non-executable. NX support is required
and write-protect is enabled for supervisor writes. The timer setup maps the local
APIC MMIO page and routes legacy PIC interrupts through ExtINT. This first
timer path does not support x2APIC.
Exception reports include their vector, error code, and instruction address.
Limine is used as the bootloader for now; a custom bootloader can be a later
project.

