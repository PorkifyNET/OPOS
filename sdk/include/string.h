/* OPOS SDK C library: memory and string functions. */
#ifndef OPOS_STRING_H
#define OPOS_STRING_H

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t length);
void *memmove(void *destination, const void *source, size_t length);
void *memset(void *destination, int value, size_t length);
int memcmp(const void *a, const void *b, size_t length);
void *memchr(const void *memory, int value, size_t length);

size_t strlen(const char *text);
size_t strnlen(const char *text, size_t limit);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t length);
char *strcpy(char *destination, const char *source);
char *strncpy(char *destination, const char *source, size_t length);
char *strcat(char *destination, const char *source);
char *strncat(char *destination, const char *source, size_t length);
char *strchr(const char *text, int character);
char *strrchr(const char *text, int character);
char *strstr(const char *text, const char *part);
char *strdup(const char *text);

#endif
