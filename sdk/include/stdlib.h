/* OPOS SDK C library: memory allocation, numbers, exit. */
#ifndef OPOS_STDLIB_H
#define OPOS_STDLIB_H

#include <stddef.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/* Blocks come from opos_alloc in arenas of at least 64 KiB; a program
   has 4 MiB of such memory in total. */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *memory, size_t size);
void free(void *memory);

int abs(int value);
long labs(long value);
int atoi(const char *text);
long atol(const char *text);
long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);

/* Sorts count elements of size bytes (heap sort: not stable). */
void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *));

__attribute__((noreturn)) void exit(int status);

#endif
