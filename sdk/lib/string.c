/* OPOS SDK C library: memory and string functions. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* GCC may call memcpy and memset for structure copies, and would turn the
   loops below back into calls to themselves without this. */
#define NO_LOOP_CALLS __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_LOOP_CALLS void *memcpy(void *destination, const void *source,
                           size_t length) {
    unsigned char *to = destination;
    const unsigned char *from = source;
    for (size_t i = 0; i < length; i++) {
        to[i] = from[i];
    }
    return destination;
}

NO_LOOP_CALLS void *memmove(void *destination, const void *source,
                            size_t length) {
    unsigned char *to = destination;
    const unsigned char *from = source;
    if (to == from || length == 0) {
        return destination;
    }
    if ((uintptr_t)to < (uintptr_t)from) {
        for (size_t i = 0; i < length; i++) {
            to[i] = from[i];
        }
    } else {
        for (size_t i = length; i != 0; i--) {
            to[i - 1] = from[i - 1];
        }
    }
    return destination;
}

NO_LOOP_CALLS void *memset(void *destination, int value, size_t length) {
    unsigned char *to = destination;
    for (size_t i = 0; i < length; i++) {
        to[i] = (unsigned char)value;
    }
    return destination;
}

int memcmp(const void *a, const void *b, size_t length) {
    const unsigned char *left = a;
    const unsigned char *right = b;
    for (size_t i = 0; i < length; i++) {
        if (left[i] != right[i]) {
            return left[i] < right[i] ? -1 : 1;
        }
    }
    return 0;
}

void *memchr(const void *memory, int value, size_t length) {
    const unsigned char *bytes = memory;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] == (unsigned char)value) {
            return (void *)(bytes + i);
        }
    }
    return NULL;
}

size_t strlen(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

size_t strnlen(const char *text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length] != '\0') {
        length++;
    }
    return length;
}

int strcmp(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

char *strcpy(char *destination, const char *source) {
    size_t i = 0;
    do {
        destination[i] = source[i];
    } while (source[i++] != '\0');
    return destination;
}

char *strncpy(char *destination, const char *source, size_t length) {
    size_t i = 0;
    for (; i < length && source[i] != '\0'; i++) {
        destination[i] = source[i];
    }
    for (; i < length; i++) {
        destination[i] = '\0';
    }
    return destination;
}

char *strcat(char *destination, const char *source) {
    strcpy(destination + strlen(destination), source);
    return destination;
}

char *strncat(char *destination, const char *source, size_t length) {
    size_t end = strlen(destination);
    size_t i = 0;
    for (; i < length && source[i] != '\0'; i++) {
        destination[end + i] = source[i];
    }
    destination[end + i] = '\0';
    return destination;
}

char *strchr(const char *text, int character) {
    for (;; text++) {
        if (*text == (char)character) {
            return (char *)text;
        }
        if (*text == '\0') {
            return NULL;
        }
    }
}

char *strrchr(const char *text, int character) {
    const char *found = NULL;
    for (;; text++) {
        if (*text == (char)character) {
            found = text;
        }
        if (*text == '\0') {
            return (char *)found;
        }
    }
}

char *strstr(const char *text, const char *part) {
    size_t length = strlen(part);
    if (length == 0) {
        return (char *)text;
    }
    for (; *text != '\0'; text++) {
        if (*text == *part && strncmp(text, part, length) == 0) {
            return (char *)text;
        }
    }
    return NULL;
}

char *strdup(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}
