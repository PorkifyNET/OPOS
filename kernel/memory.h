#ifndef OPOS_MEMORY_H
#define OPOS_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

#include <limine.h>

bool memory_initialize(const struct limine_memmap_response *memory_map,
                       uint64_t hhdm_offset);
uint64_t physical_page_alloc(void);
/* Allocates count physically contiguous pages; returns 0 on failure. */
uint64_t physical_pages_alloc_contiguous(uint64_t count);
bool physical_page_free(uint64_t physical_address);
void *physical_page_to_hhdm(uint64_t physical_address);
uint64_t memory_total_pages(void);
/* Pages not handed out (never used, or freed again). */
uint64_t memory_free_pages(void);

#endif
