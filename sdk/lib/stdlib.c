/*
 * OPOS SDK C library: malloc and friends, number parsing, exit.
 *
 * malloc takes arenas from opos_alloc (anonymous mmap) and keeps one free
 * list sorted by address, so neighbouring free blocks merge. Each block
 * has a 16-byte header; returned memory is 16-byte aligned.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>

#define ALIGNMENT 16
#define ARENA_MIN (64 * 1024)

struct block {
    size_t size;        /* usable bytes after the header */
    struct block *next; /* next free block (free list only) */
};

_Static_assert(sizeof(struct block) == ALIGNMENT, "block header size");

static struct block *free_list;

static size_t round_up(size_t value, size_t to) {
    return (value + to - 1) / to * to;
}

static char *block_end(struct block *block) {
    return (char *)(block + 1) + block->size;
}

/* Puts a block back, merging it with free neighbours. */
static void release(struct block *block) {
    struct block *previous = NULL;
    struct block *next = free_list;
    while (next != NULL && next < block) {
        previous = next;
        next = next->next;
    }
    block->next = next;
    if (next != NULL && block_end(block) == (char *)next) {
        block->size += sizeof(struct block) + next->size;
        block->next = next->next;
    }
    if (previous != NULL && block_end(previous) == (char *)block) {
        previous->size += sizeof(struct block) + block->size;
        previous->next = block->next;
    } else if (previous != NULL) {
        previous->next = block;
    } else {
        free_list = block;
    }
}

static bool grow(size_t size) {
    size_t bytes = round_up(size + sizeof(struct block), 4096);
    if (bytes < ARENA_MIN) {
        bytes = ARENA_MIN;
    }
    struct block *arena = opos_alloc(bytes);
    if (arena == NULL) {
        return false;
    }
    arena->size = bytes - sizeof(struct block);
    release(arena);
    return true;
}

void *malloc(size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (size > (size_t)1 << 40) {
        return NULL;
    }
    size = round_up(size, ALIGNMENT);
    for (int attempt = 0; attempt < 2; attempt++) {
        struct block *previous = NULL;
        for (struct block *block = free_list; block != NULL;
             previous = block, block = block->next) {
            if (block->size < size) {
                continue;
            }
            struct block *next = block->next;
            if (block->size >= size + sizeof(struct block) + ALIGNMENT) {
                /* Split: the rest stays free. */
                struct block *rest = (struct block *)
                    ((char *)(block + 1) + size);
                rest->size = block->size - size - sizeof(struct block);
                rest->next = next;
                next = rest;
                block->size = size;
            }
            if (previous != NULL) {
                previous->next = next;
            } else {
                free_list = next;
            }
            block->next = NULL;
            return block + 1;
        }
        if (attempt == 0 && !grow(size)) {
            return NULL;
        }
    }
    return NULL;
}

void free(void *memory) {
    if (memory != NULL) {
        release((struct block *)memory - 1);
    }
}

void *calloc(size_t count, size_t size) {
    if (size != 0 && count > (size_t)-1 / size) {
        return NULL;
    }
    void *memory = malloc(count * size);
    if (memory != NULL) {
        memset(memory, 0, count * size);
    }
    return memory;
}

void *realloc(void *memory, size_t size) {
    if (memory == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(memory);
        return NULL;
    }
    struct block *block = (struct block *)memory - 1;
    if (block->size >= size) {
        return memory;
    }
    void *bigger = malloc(size);
    if (bigger != NULL) {
        memcpy(bigger, memory, block->size);
        free(memory);
    }
    return bigger;
}

int abs(int value) {
    return value < 0 ? -value : value;
}

long labs(long value) {
    return value < 0 ? -value : value;
}

unsigned long strtoul(const char *text, char **end, int base) {
    const char *p = text;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '+') {
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0'
        && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long value = 0;
    const char *digits_start = p;
    for (;; p++) {
        int digit;
        if (isdigit((unsigned char)*p)) {
            digit = *p - '0';
        } else if (isalpha((unsigned char)*p)) {
            digit = tolower((unsigned char)*p) - 'a' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = value * (unsigned long)base + (unsigned long)digit;
    }
    if (end != NULL) {
        *end = (char *)(p == digits_start ? text : p);
    }
    return value;
}

long strtol(const char *text, char **end, int base) {
    const char *p = text;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    bool negative = *p == '-';
    if (negative) {
        p++;
    }
    char *stop;
    unsigned long magnitude = strtoul(p, &stop, base);
    if (end != NULL) {
        *end = stop == p ? (char *)text : stop;
    }
    return negative ? -(long)magnitude : (long)magnitude;
}

int atoi(const char *text) {
    return (int)strtol(text, NULL, 10);
}

long atol(const char *text) {
    return strtol(text, NULL, 10);
}

void exit(int status) {
    opos_exit(status);
}
