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

static inline size_t opos_strlen(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

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
