/*
 * OPOS SDK C library: formatted output to the Terminal.
 *
 * Formats: %d %i %u %x %X %o %c %s %p %%, with the flags - 0 + and space,
 * a width and precision (numbers or *), and the sizes hh h l ll z j t.
 * No floating point. There are no FILE streams yet: use opos_open,
 * opos_read and opos_write from opos.h for files.
 */
#ifndef OPOS_STDIO_H
#define OPOS_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

int printf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
int vprintf(const char *format, va_list arguments);
/* Writes to standard error (shown in red in the Terminal). */
int eprintf(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
int snprintf(char *buffer, size_t size, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buffer, size_t size, const char *format,
              va_list arguments);
int sprintf(char *buffer, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
int puts(const char *text);
int putchar(int character);

#endif
