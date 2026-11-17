/*
 * OPOS SDK C library: formatted output to the Terminal.
 *
 * Formats: %d %i %u %x %X %o %c %s %p %%, with the flags - 0 + and space,
 * a width and precision (numbers or *), and the sizes hh h l ll z j t.
 * %f %e %g print doubles in fixed notation only, and are not exactly
 * rounded (good enough for messages, not for number conversion).
 * The only FILE streams are stdout and stderr (write only); use
 * opos_open, opos_read and opos_write from opos.h for files.
 */
#ifndef OPOS_STDIO_H
#define OPOS_STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

typedef struct opos_file FILE;
extern FILE *stdout;
extern FILE *stderr;

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

int fprintf(FILE *stream, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
int vfprintf(FILE *stream, const char *format, va_list arguments);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int fputs(const char *text, FILE *stream);
size_t fwrite(const void *data, size_t size, size_t count, FILE *stream);
int fflush(FILE *stream);

#endif
