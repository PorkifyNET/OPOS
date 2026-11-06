/*
 * OPOS application SDK, ABI version 1.
 *
 * Programs are static x86-64 ELF executables. They talk to the kernel
 * with the SYSCALL instruction. Two number ranges exist:
 *
 *   - Linux x86-64 numbers (read = 0, write = 1, ...). OPOS implements a
 *     subset; see docs/linux-compat.md. The wrappers below use only calls
 *     OPOS supports.
 *   - OPOS native numbers from 0x4f500000 ("OP"). Linux never uses them.
 *
 * Stability promise: within ABI version 1, numbers, argument meanings and
 * structure layouts are only ever added to. A structure starts with its
 * size, filled in by the program, so a newer kernel can grow it without
 * writing past an older program's copy.
 *
 * Errors: calls return a negative error number (OPOS_EACCES, ...).
 *
 * Permissions: programs run as the account that started them. They may
 * read system files, but never change them; they reach only their own
 * home folder under /disk/Users; everything else on the disks is shared.
 */
#ifndef OPOS_H
#define OPOS_H

#include <stddef.h>
#include <stdint.h>

#define OPOS_ABI_VERSION 1

/* ------------------------------------------------------------ errors */

#define OPOS_ENOENT 2
#define OPOS_EIO 5
#define OPOS_EBADF 9
#define OPOS_ENOMEM 12
#define OPOS_EACCES 13
#define OPOS_EFAULT 14
#define OPOS_EEXIST 17
#define OPOS_ENOTDIR 20
#define OPOS_EISDIR 21
#define OPOS_EINVAL 22
#define OPOS_EROFS 30
#define OPOS_ENAMETOOLONG 36
#define OPOS_ENOSYS 38
#define OPOS_ENOTEMPTY 39

static inline size_t opos_strlen(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

/* --------------------------------------------------------- raw calls */

static inline long opos_syscall0(long number) {
    long result;
    __asm__ volatile ("syscall" : "=a"(result) : "a"(number)
                      : "rcx", "r11", "memory");
    return result;
}

static inline long opos_syscall1(long number, long a) {
    long result;
    __asm__ volatile ("syscall" : "=a"(result) : "a"(number), "D"(a)
                      : "rcx", "r11", "memory");
    return result;
}

static inline long opos_syscall2(long number, long a, long b) {
    long result;
    __asm__ volatile ("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b)
                      : "rcx", "r11", "memory");
    return result;
}

static inline long opos_syscall3(long number, long a, long b, long c) {
    long result;
    __asm__ volatile ("syscall" : "=a"(result)
                      : "a"(number), "D"(a), "S"(b), "d"(c)
                      : "rcx", "r11", "memory");
    return result;
}

static inline long opos_syscall4(long number, long a, long b, long c,
                                 long d) {
    long result;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("syscall" : "=a"(result)
                      : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "rcx", "r11", "memory");
    return result;
}

static inline long opos_syscall6(long number, long a, long b, long c,
                                 long d, long e, long f) {
    long result;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile ("syscall" : "=a"(result)
                      : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10),
                        "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return result;
}

/* ------------------------------------------------------- native calls */

#define OPOS_SYS_ABI_VERSION 0x4f500000L
#define OPOS_SYS_USER_INFO 0x4f500001L

/* The kernel's ABI version; at least OPOS_ABI_VERSION on a kernel that
   can run this program. */
static inline long opos_abi_version(void) {
    return opos_syscall0(OPOS_SYS_ABI_VERSION);
}

#define OPOS_USER_ADMIN 1u   /* the account is an administrator */
#define OPOS_USER_ACCOUNT 2u /* name and home are set */

struct opos_user_info {
    uint32_t size; /* set to sizeof(struct opos_user_info) before the call */
    uint32_t flags;
    char name[32];  /* account name, or "" */
    char home[128]; /* e.g. "/disk/Users/Alice", or "" */
};

/* Who this program runs as. */
static inline long opos_user_info(struct opos_user_info *info) {
    info->size = sizeof(*info);
    return opos_syscall1(OPOS_SYS_USER_INFO, (long)info);
}

/* ------------------------------------------------------------ windows */

/*
 * One window per program, drawn by the desktop. Draw 0x00RRGGBB pixels in
 * your own memory and present them; the desktop shows them at the top left
 * of the window's content area (up to the screen's size is kept, so a
 * program should draw as large as its latest OPOS_EVENT_RESIZE).
 * Then wait for events. While a program waits, the desktop keeps working.
 *
 * The window closes when the program ends. When the user closes it, the
 * program gets OPOS_EVENT_CLOSE and should exit; waiting again ends it.
 * Programs started from the kernel shell cannot open windows (-19).
 */
#define OPOS_SYS_WINDOW_CREATE 0x4f500010L
#define OPOS_SYS_WINDOW_PRESENT 0x4f500011L
#define OPOS_SYS_WINDOW_EVENT 0x4f500012L
#define OPOS_SYS_WINDOW_SIZE 0x4f500013L

#define OPOS_EVENT_NONE 0
#define OPOS_EVENT_KEY 1        /* key, buttons = OPOS_MOD_* held */
#define OPOS_EVENT_MOUSE_DOWN 2 /* x, y, button, buttons */
#define OPOS_EVENT_MOUSE_UP 3   /* x, y, button, buttons */
#define OPOS_EVENT_MOUSE_MOVE 4 /* x, y, buttons */
#define OPOS_EVENT_RESIZE 5     /* width, height of the content area */
#define OPOS_EVENT_CLOSE 6      /* the user closed the window */
/* Mouse wheel or two-finger scroll over the window: x, y, buttons, and
   key = notches as a signed number, positive = scroll up. */
#define OPOS_EVENT_WHEEL 7
#define OPOS_WHEEL_NOTCHES(event) ((int32_t)(event).key)

/* Modifiers held with a key. Letters typed with Shift already arrive in
   upper case; Ctrl+S arrives as 's' with OPOS_MOD_CTRL. */
#define OPOS_MOD_CTRL 1u
#define OPOS_MOD_SHIFT 2u

#define OPOS_BUTTON_LEFT 1u
#define OPOS_BUTTON_RIGHT 2u

/* Keys: printable ASCII, '\n' (Enter), '\b' (Backspace), '\t', and: */
#define OPOS_KEY_UP 0x11
#define OPOS_KEY_DOWN 0x12
#define OPOS_KEY_LEFT 0x13
#define OPOS_KEY_RIGHT 0x14
#define OPOS_KEY_DELETE 0x17
#define OPOS_KEY_HOME 0x18
#define OPOS_KEY_END 0x19
#define OPOS_KEY_PAGE_UP 0x1a
#define OPOS_KEY_ESCAPE 0x1b
#define OPOS_KEY_PAGE_DOWN 0x1c

struct opos_event {
    uint32_t type;    /* OPOS_EVENT_* */
    uint32_t key;     /* KEY: the character or OPOS_KEY_* */
    int32_t x;        /* MOUSE_*: position in the content area; may be */
    int32_t y;        /*   outside it while a button is held */
    uint32_t button;  /* MOUSE_DOWN/UP: the button that changed */
    uint32_t buttons; /* MOUSE_*: buttons held now; KEY: OPOS_MOD_* */
    uint32_t width;   /* RESIZE */
    uint32_t height;
};

/* Opens the window with a content area of about width x height. The
   first event is OPOS_EVENT_RESIZE with the size it really got. */
static inline long opos_window_create(const char *title, uint32_t width,
                                      uint32_t height) {
    return opos_syscall4(OPOS_SYS_WINDOW_CREATE, (long)title,
                         (long)opos_strlen(title), width, height);
}

/* Shows width x height pixels; stride is the row length in pixels. */
static inline long opos_window_present(const uint32_t *pixels,
                                       uint32_t width, uint32_t height,
                                       uint32_t stride) {
    return opos_syscall4(OPOS_SYS_WINDOW_PRESENT, (long)pixels, width, height,
                         stride);
}

/* Waits for an event: timeout_ms < 0 waits for ever, 0 only checks.
   Returns 1 with an event, 0 on timeout, or a negative error. */
static inline long opos_window_event(struct opos_event *event,
                                     long timeout_ms) {
    return opos_syscall2(OPOS_SYS_WINDOW_EVENT, (long)event, timeout_ms);
}

/* The content area's current size. */
static inline long opos_window_size(uint32_t *width, uint32_t *height) {
    long size = opos_syscall0(OPOS_SYS_WINDOW_SIZE);
    if (size > 0) {
        *width = (uint32_t)((unsigned long)size >> 32);
        *height = (uint32_t)size;
        return 0;
    }
    return size;
}

/* ------------------------------------------------------------- memory */

/* Zero-filled, readable and writable memory; up to 64 MiB in total per
   program. Returns NULL on failure. */
static inline void *opos_alloc(size_t size) {
    long address = opos_syscall6(9, 0, (long)size, 3 /* read, write */,
                                 0x22 /* private, anonymous */, -1, 0);
    return address < 0 && address > -4096 ? (void *)0 : (void *)address;
}

static inline long opos_free(void *address, size_t size) {
    return opos_syscall2(11, (long)address, (long)size);
}

/* ------------------------------------------------------- Linux calls */

#define OPOS_O_RDONLY 0
#define OPOS_O_WRONLY 1
#define OPOS_O_RDWR 2
#define OPOS_O_CREAT 64
#define OPOS_O_EXCL 128
#define OPOS_O_TRUNC 512
#define OPOS_O_APPEND 1024
#define OPOS_O_DIRECTORY 65536

#define OPOS_STDIN 0
#define OPOS_STDOUT 1
#define OPOS_STDERR 2

/* At most 4096 bytes per call for files, 256 for the console. */
static inline long opos_read(int fd, void *buffer, size_t length) {
    return opos_syscall3(0, fd, (long)buffer, (long)length);
}

static inline long opos_write(int fd, const void *buffer, size_t length) {
    return opos_syscall3(1, fd, (long)buffer, (long)length);
}

static inline long opos_open(const char *path, int flags) {
    return opos_syscall3(2, (long)path, flags, 0);
}

static inline long opos_close(int fd) {
    return opos_syscall1(3, fd);
}

static inline long opos_mkdir(const char *path) {
    return opos_syscall2(83, (long)path, 0755);
}

static inline long opos_rmdir(const char *path) {
    return opos_syscall1(84, (long)path);
}

static inline long opos_unlink(const char *path) {
    return opos_syscall1(87, (long)path);
}

static inline long opos_rename(const char *from, const char *to) {
    return opos_syscall2(82, (long)from, (long)to);
}

static inline long opos_chdir(const char *path) {
    return opos_syscall1(80, (long)path);
}

/* Returns the length including the terminating zero. */
static inline long opos_getcwd(char *buffer, size_t length) {
    return opos_syscall2(79, (long)buffer, (long)length);
}

static inline long opos_getpid(void) {
    return opos_syscall0(39);
}

struct opos_timespec {
    long seconds;
    long nanoseconds;
};

static inline long opos_sleep_ms(long milliseconds) {
    struct opos_timespec time = {milliseconds / 1000,
                                 (milliseconds % 1000) * 1000000};
    return opos_syscall2(35, (long)&time, 0);
}

/* Seconds since boot, from the 100 Hz timer. */
static inline long opos_uptime(struct opos_timespec *time) {
    return opos_syscall2(228, 1 /* CLOCK_MONOTONIC */, (long)time);
}

__attribute__((noreturn)) static inline void opos_exit(int status) {
    for (;;) {
        opos_syscall1(231, status);
    }
}

/* ----------------------------------------------------------- helpers */

/* Writes a zero-terminated string to standard output. */
static inline long opos_print(const char *text) {
    size_t length = opos_strlen(text);
    size_t done = 0;
    while (done < length) {
        size_t chunk = length - done > 256 ? 256 : length - done;
        long written = opos_write(OPOS_STDOUT, text + done, chunk);
        if (written <= 0) {
            return written;
        }
        done += (size_t)written;
    }
    return (long)done;
}

/* Writes a signed decimal number to standard output. */
static inline void opos_print_number(long value) {
    char digits[24];
    size_t length = 0;
    unsigned long magnitude = value < 0 ? 0UL - (unsigned long)value
                                        : (unsigned long)value;
    do {
        digits[sizeof(digits) - 1 - length++] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    if (value < 0) {
        digits[sizeof(digits) - 1 - length++] = '-';
    }
    opos_write(OPOS_STDOUT, digits + sizeof(digits) - length, length);
}

#endif
