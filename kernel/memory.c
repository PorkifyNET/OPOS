#include <stddef.h>
#include <stdint.h>

#include "memory.h"

#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)
#define MAX_USABLE_RANGES 256

struct usable_range {
    uint64_t start;
    uint64_t end;
    uint64_t next;
};

struct free_page {
    struct free_page *next;
};

static struct usable_range usable_ranges[MAX_USABLE_RANGES];
static size_t usable_range_count;
static size_t current_range;
static struct free_page *free_pages;
static uint64_t direct_map_offset;
static uint64_t total_pages;
static uint64_t free_list_pages;

bool memory_initialize(const struct limine_memmap_response *memory_map,
                       uint64_t hhdm_offset) {
    if (memory_map == NULL || memory_map->entries == NULL || memory_map->entry_count == 0) {
        return false;
    }

    usable_range_count = 0;
    current_range = 0;
    free_pages = NULL;
    direct_map_offset = hhdm_offset;
    total_pages = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; i++) {
        const struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry == NULL) {
            return false;
        }
        if (entry->type != LIMINE_MEMMAP_USABLE || entry->length == 0) {
            continue;
        }
        if (entry->base > UINT64_MAX - entry->length
            || usable_range_count == MAX_USABLE_RANGES) {
            return false;
        }

        uint64_t end = entry->base + entry->length;
        uint64_t start = entry->base;
        if (start == 0) {
            start = PAGE_SIZE;
        }
        if (start > UINT64_MAX - PAGE_MASK) {
            continue;
        }
        start = (start + PAGE_MASK) & ~((uint64_t)PAGE_MASK);
        end &= ~((uint64_t)PAGE_MASK);
        if (start >= end) {
            continue;
        }
        uint64_t range_pages = (end - start) / PAGE_SIZE;
        if (end - PAGE_SIZE > UINTPTR_MAX - hhdm_offset
            || range_pages > UINT64_MAX - total_pages) {
            return false;
        }

        usable_ranges[usable_range_count++] = (struct usable_range) {
            .start = start,
            .end = end,
            .next = start,
        };
        total_pages += range_pages;
    }

    return usable_range_count != 0;
}

uint64_t physical_page_alloc(void) {
    if (free_pages != NULL) {
        struct free_page *page = free_pages;
        free_pages = page->next;
        free_list_pages--;
        return (uint64_t)(uintptr_t)page - direct_map_offset;
    }

    while (current_range < usable_range_count) {
        struct usable_range *range = &usable_ranges[current_range];
        if (range->next < range->end) {
            uint64_t page = range->next;
            range->next += PAGE_SIZE;
            return page;
        }
        current_range++;
    }

    return 0;
}

bool physical_page_free(uint64_t physical_address) {
    if (physical_address == 0 || (physical_address & PAGE_MASK) != 0
        || physical_address > UINTPTR_MAX - direct_map_offset) {
        return false;
    }

    bool was_allocated = false;
    for (size_t i = 0; i < usable_range_count; i++) {
        const struct usable_range *range = &usable_ranges[i];
        if (physical_address >= range->start && physical_address < range->next) {
            was_allocated = true;
            break;
        }
    }
    if (!was_allocated) {
        return false;
    }

    struct free_page *page = (struct free_page *)
        (uintptr_t)(direct_map_offset + physical_address);
    for (struct free_page *free_page = free_pages;
         free_page != NULL;
         free_page = free_page->next) {
        if (free_page == page) {
            return false;
        }
    }

    page->next = free_pages;
    free_pages = page;
    free_list_pages++;
    return true;
}

void *physical_page_to_hhdm(uint64_t physical_address) {
    if (physical_address > UINTPTR_MAX - direct_map_offset) {
        return NULL;
    }
    return (void *)(uintptr_t)(direct_map_offset + physical_address);
}

uint64_t memory_total_pages(void) {
    return total_pages;
}

uint64_t memory_free_pages(void) {
    uint64_t pages = free_list_pages;
    for (size_t i = 0; i < usable_range_count; i++) {
        pages += (usable_ranges[i].end - usable_ranges[i].next) / PAGE_SIZE;
    }
    return pages;
}

uint64_t physical_pages_alloc_contiguous(uint64_t count) {
    if (count == 0 || count > UINT64_MAX / PAGE_SIZE) {
        return 0;
    }
    /*
     * Carve the block from the untouched tail of a usable range. Pages there
     * have never been handed out, so they are contiguous by construction.
     */
    for (size_t i = current_range; i < usable_range_count; i++) {
        struct usable_range *range = &usable_ranges[i];
        if (range->end - range->next >= count * PAGE_SIZE) {
            uint64_t start = range->next;
            range->next += count * PAGE_SIZE;
            return start;
        }
    }
    return 0;
}

/*
 * GCC may call these for large structure copies and clears even in a
 * freestanding kernel. The attribute stops it from turning the loops back
 * into calls to themselves.
 */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *destination, int value, size_t length) {
    uint8_t *bytes = destination;
    for (size_t i = 0; i < length; i++) {
        bytes[i] = (uint8_t)value;
    }
    return destination;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *destination, const void *source, size_t length) {
    uint8_t *to = destination;
    const uint8_t *from = source;
    for (size_t i = 0; i < length; i++) {
        to[i] = from[i];
    }
    return destination;
}

/* For BearSSL (update signature checks). */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memmove(void *destination, const void *source, size_t length) {
    uint8_t *to = destination;
    const uint8_t *from = source;
    if (to < from) {
        for (size_t i = 0; i < length; i++) {
            to[i] = from[i];
        }
    } else {
        for (size_t i = length; i > 0; i--) {
            to[i - 1] = from[i - 1];
        }
    }
    return destination;
}
