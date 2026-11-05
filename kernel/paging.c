#include <stddef.h>
#include <stdint.h>

#include "memory.h"
#include "paging.h"

#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)
#define PAGE_TABLE_ADDRESS_MASK 0x000ffffffffff000ULL
#define PAGE_PRESENT (1ULL << 0)
#define PAGE_HUGE (1ULL << 7)
#define PAGE_ACCESSED (1ULL << 5)
#define PAGE_DIRTY (1ULL << 6)
#define PAGE_2M_ADDRESS_MASK (PAGE_TABLE_ADDRESS_MASK & ~((1ULL << 21) - 1))
#define PAGE_1G_ADDRESS_MASK (PAGE_TABLE_ADDRESS_MASK & ~((1ULL << 30) - 1))
#define PAGE_FLAGS_MASK \
    (PAGE_PRESENT | PAGING_WRITABLE | PAGING_USER | PAGING_WRITE_THROUGH \
     | PAGING_CACHE_DISABLE | PAGING_PAT_4K | PAGING_NO_EXECUTE)
#define CR4_LA57 (1ULL << 12)
#define CR0_WRITE_PROTECT (1ULL << 16)
#define IA32_EFER_MSR 0xc0000080u
#define IA32_EFER_NXE (1u << 11)
#define CPUID_EXTENDED_MAX 0x80000000u
#define CPUID_EXTENDED_FEATURES 0x80000001u
#define CPUID_NX (1u << 20)

#define CR4_UMIP (1ULL << 11)
#define CR4_SMEP (1ULL << 20)
#define CPUID7_EBX_SMEP (1u << 7)
#define CPUID7_ECX_UMIP (1u << 2)

static bool smep_enabled;
static bool umip_enabled;

bool paging_smep_enabled(void) {
    return smep_enabled;
}

bool paging_umip_enabled(void) {
    return umip_enabled;
}

static volatile uint64_t *root_table;
static uint64_t kernel_root_physical;
static uint64_t active_root_physical;
static size_t paging_levels;

bool paging_nx_supported(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(CPUID_EXTENDED_MAX));
    if (eax < CPUID_EXTENDED_FEATURES) {
        return false;
    }

    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(CPUID_EXTENDED_FEATURES));
    return (edx & CPUID_NX) != 0;
}

static bool canonical_address(uintptr_t address) {
    unsigned canonical_bit = paging_levels == 5 ? 57 : 48;
    uint64_t upper = (uint64_t)address >> canonical_bit;
    uint64_t upper_mask = (1ULL << (64 - canonical_bit)) - 1;
    bool sign_bit = (((uint64_t)address >> (canonical_bit - 1)) & 1) != 0;
    return sign_bit ? upper == upper_mask : upper == 0;
}

static volatile uint64_t *table_from_entry(uint64_t entry) {
    return physical_page_to_hhdm(entry & PAGE_TABLE_ADDRESS_MASK);
}

static volatile uint64_t *allocate_table(uint64_t *physical_address) {
    *physical_address = physical_page_alloc();
    if (*physical_address == 0) {
        return NULL;
    }

    volatile uint64_t *table = physical_page_to_hhdm(*physical_address);
    if (table == NULL) {
        physical_page_free(*physical_address);
        *physical_address = 0;
        return NULL;
    }
    for (size_t i = 0; i < PAGE_SIZE / sizeof(*table); i++) {
        table[i] = 0;
    }
    return table;
}

bool paging_initialize(void) {
    uint64_t cr3;
    uint64_t cr4;
    uint64_t cr0;
    uint32_t efer_low;
    uint32_t efer_high;
    uint32_t efer_msr = IA32_EFER_MSR;

    if (!paging_nx_supported()) {
        return false;
    }

    __asm__ volatile ("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(efer_msr));
    efer_low |= IA32_EFER_NXE;
    __asm__ volatile ("wrmsr" : : "a"(efer_low), "d"(efer_high), "c"(efer_msr));
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= CR0_WRITE_PROTECT;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));

    /*
     * SMEP: ring 0 never runs code from user pages, so a kernel bug cannot
     * jump into code a program wrote. UMIP: ring 3 cannot read the GDT/IDT
     * addresses (SGDT/SIDT/SLDT/SMSW/STR), which would leak kernel layout.
     */
    uint32_t leaf_max;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid" : "=a"(leaf_max), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(0));
    if (leaf_max >= 7) {
        uint32_t eax;
        __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                          : "a"(7), "c"(0));
        uint64_t enable = 0;
        if ((ebx & CPUID7_EBX_SMEP) != 0) {
            enable |= CR4_SMEP;
        }
        if ((ecx & CPUID7_ECX_UMIP) != 0) {
            enable |= CR4_UMIP;
        }
        if (enable != 0) {
            cr4 |= enable;
            __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
        }
        smep_enabled = (enable & CR4_SMEP) != 0;
        umip_enabled = (enable & CR4_UMIP) != 0;
    }

    paging_levels = (cr4 & CR4_LA57) != 0 ? 5 : 4;
    kernel_root_physical = cr3 & PAGE_TABLE_ADDRESS_MASK;
    active_root_physical = kernel_root_physical;
    root_table = physical_page_to_hhdm(kernel_root_physical);
    return root_table != NULL;
}

uint64_t paging_kernel_address_space(void) {
    return kernel_root_physical;
}

bool paging_address_space_create(uint64_t *physical_address) {
    if (physical_address == NULL || root_table == NULL
        || active_root_physical != kernel_root_physical) {
        return false;
    }

    uint64_t new_root_physical;
    volatile uint64_t *new_root = allocate_table(&new_root_physical);
    if (new_root == NULL) {
        return false;
    }

    volatile uint64_t *kernel_root =
        physical_page_to_hhdm(kernel_root_physical);
    if (kernel_root == NULL) {
        physical_page_free(new_root_physical);
        return false;
    }

    for (size_t i = 256; i < PAGE_SIZE / sizeof(*new_root); i++) {
        new_root[i] = kernel_root[i];
    }
    *physical_address = new_root_physical;
    return true;
}

bool paging_address_space_activate(uint64_t physical_address) {
    if ((physical_address & PAGE_MASK) != 0
        || (physical_address & ~PAGE_TABLE_ADDRESS_MASK) != 0) {
        return false;
    }

    volatile uint64_t *new_root = physical_page_to_hhdm(physical_address);
    if (new_root == NULL) {
        return false;
    }

    root_table = new_root;
    active_root_physical = physical_address;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(physical_address) : "memory");
    return true;
}

static bool release_user_page_table(uint64_t physical_address, size_t level) {
    volatile uint64_t *table = physical_page_to_hhdm(physical_address);
    if (table == NULL || level == 0) {
        return false;
    }
    if (level == 1) {
        return true;
    }

    bool released = true;
    for (size_t i = 0; i < PAGE_SIZE / sizeof(*table); i++) {
        uint64_t entry = table[i];
        if ((entry & PAGE_PRESENT) == 0) {
            continue;
        }
        if ((entry & PAGE_HUGE) != 0) {
            table[i] = 0;
            continue;
        }

        uint64_t child_physical = entry & PAGE_TABLE_ADDRESS_MASK;
        if (!release_user_page_table(child_physical, level - 1)
            || !physical_page_free(child_physical)) {
            released = false;
            continue;
        }
        table[i] = 0;
    }
    return released;
}

bool paging_address_space_destroy(uint64_t physical_address) {
    if ((physical_address & PAGE_MASK) != 0
        || (physical_address & ~PAGE_TABLE_ADDRESS_MASK) != 0
        || physical_address == kernel_root_physical
        || physical_address == active_root_physical) {
        return false;
    }

    volatile uint64_t *root = physical_page_to_hhdm(physical_address);
    if (root == NULL) {
        return false;
    }

    bool released = true;
    for (size_t i = 0; i < 256; i++) {
        uint64_t entry = root[i];
        if ((entry & PAGE_PRESENT) == 0) {
            continue;
        }

        uint64_t child_physical = entry & PAGE_TABLE_ADDRESS_MASK;
        if (!release_user_page_table(child_physical, paging_levels - 1)
            || !physical_page_free(child_physical)) {
            released = false;
            continue;
        }
        root[i] = 0;
    }
    return released && physical_page_free(physical_address);
}

static bool walk_page_tables(uintptr_t virtual_address, bool create,
                             uint64_t flags, volatile uint64_t **leaf_entry) {
    if (root_table == NULL || !canonical_address(virtual_address)) {
        return false;
    }

    unsigned indices[5] = {
        (unsigned)((virtual_address >> 48) & 0x1ff),
        (unsigned)((virtual_address >> 39) & 0x1ff),
        (unsigned)((virtual_address >> 30) & 0x1ff),
        (unsigned)((virtual_address >> 21) & 0x1ff),
        (unsigned)((virtual_address >> 12) & 0x1ff),
    };
    size_t first_level = paging_levels == 5 ? 0 : 1;
    volatile uint64_t *table = root_table;
    volatile uint64_t *created_entries[4];
    uint64_t created_pages[4];
    size_t created_count = 0;

    for (size_t level = first_level; level < 4; level++) {
        volatile uint64_t *entry = &table[indices[level]];
        if ((*entry & PAGE_PRESENT) != 0) {
            if (level >= 2 && (*entry & PAGE_HUGE) != 0) {
                return false;
            }
            if ((flags & PAGING_WRITABLE) != 0) {
                *entry |= PAGING_WRITABLE;
            }
            if ((flags & PAGING_USER) != 0) {
                *entry |= PAGING_USER;
            }
            table = table_from_entry(*entry);
            if (table == NULL) {
                return false;
            }
            continue;
        }

        if (!create) {
            return false;
        }

        uint64_t new_table_physical;
        volatile uint64_t *new_table = allocate_table(&new_table_physical);
        if (new_table == NULL) {
            while (created_count != 0) {
                created_count--;
                *created_entries[created_count] = 0;
                physical_page_free(created_pages[created_count]);
            }
            return false;
        }

        *entry = new_table_physical | PAGE_PRESENT | PAGING_WRITABLE
               | (flags & PAGING_USER);
        created_entries[created_count] = entry;
        created_pages[created_count] = new_table_physical;
        created_count++;
        table = new_table;
    }

    *leaf_entry = &table[indices[4]];
    return true;
}

bool paging_map_page(uintptr_t virtual_address, uint64_t physical_address,
                     uint64_t flags) {
    if ((virtual_address & PAGE_MASK) != 0 || (physical_address & PAGE_MASK) != 0
        || (physical_address & ~PAGE_TABLE_ADDRESS_MASK) != 0
        || (flags & ~PAGE_FLAGS_MASK) != 0
        || !canonical_address(virtual_address)) {
        return false;
    }

    volatile uint64_t *entry;
    if (!walk_page_tables(virtual_address, true, flags, &entry)
        || ((*entry & PAGE_PRESENT) != 0
            && (*entry & PAGE_TABLE_ADDRESS_MASK) != physical_address)) {
        return false;
    }

    uint64_t preserved = *entry & (PAGE_ACCESSED | PAGE_DIRTY);
    *entry = physical_address | PAGE_PRESENT | flags | preserved;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return true;
}

bool paging_protect_range(uintptr_t virtual_address, size_t page_count,
                          uint64_t flags) {
    uint64_t changeable_flags = PAGING_WRITABLE | PAGING_NO_EXECUTE;
    if (page_count == 0 || (virtual_address & PAGE_MASK) != 0
        || (flags & ~(changeable_flags | PAGING_USER)) != 0
        || (flags & PAGING_USER) == 0
        || page_count - 1 > (UINTPTR_MAX - virtual_address) / PAGE_SIZE) {
        return false;
    }

    for (size_t i = 0; i < page_count; i++) {
        uintptr_t address = virtual_address + i * PAGE_SIZE;
        volatile uint64_t *entry;
        if (!walk_page_tables(address, false, 0, &entry)
            || (*entry & (PAGE_PRESENT | PAGING_USER))
                != (PAGE_PRESENT | PAGING_USER)) {
            return false;
        }
    }

    for (size_t i = 0; i < page_count; i++) {
        uintptr_t address = virtual_address + i * PAGE_SIZE;
        volatile uint64_t *entry;
        walk_page_tables(address, false, 0, &entry);
        *entry = (*entry & ~changeable_flags) | (flags & changeable_flags);
        __asm__ volatile ("invlpg (%0)" : : "r"(address) : "memory");
    }
    return true;
}

bool paging_unmap_page(uintptr_t virtual_address, uint64_t *physical_address) {
    if ((virtual_address & PAGE_MASK) != 0 || !canonical_address(virtual_address)) {
        return false;
    }

    volatile uint64_t *entry;
    if (!walk_page_tables(virtual_address, false, 0, &entry)
        || (*entry & PAGE_PRESENT) == 0) {
        return false;
    }

    if (physical_address != NULL) {
        *physical_address = *entry & PAGE_TABLE_ADDRESS_MASK;
    }
    *entry = 0;
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return true;
}

bool paging_translate(uintptr_t virtual_address, uint64_t *physical_address) {
    if (physical_address == NULL || !canonical_address(virtual_address)
        || root_table == NULL) {
        return false;
    }

    unsigned indices[5] = {
        (unsigned)((virtual_address >> 48) & 0x1ff),
        (unsigned)((virtual_address >> 39) & 0x1ff),
        (unsigned)((virtual_address >> 30) & 0x1ff),
        (unsigned)((virtual_address >> 21) & 0x1ff),
        (unsigned)((virtual_address >> 12) & 0x1ff),
    };
    size_t first_level = paging_levels == 5 ? 0 : 1;
    volatile uint64_t *table = root_table;

    for (size_t level = first_level; level < 4; level++) {
        uint64_t entry = table[indices[level]];
        if ((entry & PAGE_PRESENT) == 0) {
            return false;
        }
        if (level >= 2 && (entry & PAGE_HUGE) != 0) {
            uint64_t page_mask = level == 2 ? PAGE_1G_ADDRESS_MASK : PAGE_2M_ADDRESS_MASK;
            uint64_t offset_mask = level == 2 ? (1ULL << 30) - 1 : (1ULL << 21) - 1;
            *physical_address = (entry & page_mask) | ((uint64_t)virtual_address & offset_mask);
            return true;
        }
        table = table_from_entry(entry);
        if (table == NULL) {
            return false;
        }
    }

    uint64_t entry = table[indices[4]];
    if ((entry & PAGE_PRESENT) == 0) {
        return false;
    }
    *physical_address = (entry & PAGE_TABLE_ADDRESS_MASK)
                      | ((uint64_t)virtual_address & PAGE_MASK);
    return true;
}

static bool user_page_accessible(uintptr_t virtual_address, bool writable,
                                 bool executable) {
    if (root_table == NULL || !canonical_address(virtual_address)) {
        return false;
    }

    unsigned user_bit = paging_levels == 5 ? 56 : 47;
    if (((uint64_t)virtual_address & (1ULL << user_bit)) != 0) {
        return false;
    }

    unsigned indices[5] = {
        (unsigned)((virtual_address >> 48) & 0x1ff),
        (unsigned)((virtual_address >> 39) & 0x1ff),
        (unsigned)((virtual_address >> 30) & 0x1ff),
        (unsigned)((virtual_address >> 21) & 0x1ff),
        (unsigned)((virtual_address >> 12) & 0x1ff),
    };
    size_t first_level = paging_levels == 5 ? 0 : 1;
    volatile uint64_t *table = root_table;

    for (size_t level = first_level; level < 4; level++) {
        uint64_t entry = table[indices[level]];
        if ((entry & (PAGE_PRESENT | PAGING_USER)) != (PAGE_PRESENT | PAGING_USER)
            || (writable && (entry & PAGING_WRITABLE) == 0)
            || (executable && (entry & PAGING_NO_EXECUTE) != 0)) {
            return false;
        }
        if (level >= 2 && (entry & PAGE_HUGE) != 0) {
            return true;
        }
        table = table_from_entry(entry);
        if (table == NULL) {
            return false;
        }
    }

    uint64_t entry = table[indices[4]];
    return (entry & (PAGE_PRESENT | PAGING_USER)) == (PAGE_PRESENT | PAGING_USER)
        && (!writable || (entry & PAGING_WRITABLE) != 0)
        && (!executable || (entry & PAGING_NO_EXECUTE) == 0);
}

bool paging_user_range_valid(uintptr_t virtual_address, size_t length, bool writable,
                             bool executable) {
    if (length == 0) {
        return true;
    }
    if (length - 1 > UINTPTR_MAX - virtual_address) {
        return false;
    }

    uintptr_t last_address = virtual_address + length - 1;
    for (uintptr_t current = virtual_address;;) {
        if (!user_page_accessible(current, writable, executable)) {
            return false;
        }

        uintptr_t remaining_in_page = PAGE_SIZE - (current & PAGE_MASK);
        if (last_address - current < remaining_in_page) {
            return true;
        }
        current += remaining_in_page;
    }
}
