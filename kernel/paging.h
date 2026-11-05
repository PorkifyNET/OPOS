#ifndef OPOS_PAGING_H
#define OPOS_PAGING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGING_WRITABLE (1ULL << 1)
#define PAGING_USER (1ULL << 2)
#define PAGING_WRITE_THROUGH (1ULL << 3)
#define PAGING_CACHE_DISABLE (1ULL << 4)
/* PAT bit of a 4 KiB leaf entry; with PWT/PCD it selects PAT entry 4-7. */
#define PAGING_PAT_4K (1ULL << 7)
#define PAGING_NO_EXECUTE (1ULL << 63)

bool paging_nx_supported(void);
bool paging_initialize(void);
uint64_t paging_kernel_address_space(void);
bool paging_address_space_create(uint64_t *physical_address);
bool paging_address_space_activate(uint64_t physical_address);
bool paging_address_space_destroy(uint64_t physical_address);
bool paging_map_page(uintptr_t virtual_address, uint64_t physical_address,
                     uint64_t flags);
bool paging_protect_range(uintptr_t virtual_address, size_t page_count,
                          uint64_t flags);
bool paging_unmap_page(uintptr_t virtual_address, uint64_t *physical_address);
bool paging_translate(uintptr_t virtual_address, uint64_t *physical_address);
bool paging_user_range_valid(uintptr_t virtual_address, size_t length, bool writable,
                             bool executable);

#endif
