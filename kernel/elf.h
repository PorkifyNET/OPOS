#ifndef OPOS_ELF_H
#define OPOS_ELF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ELF_MAX_SEGMENT_PAGES 8192
#define ELF_MAX_LOAD_SEGMENTS 8
#define ELF_INTERPRETER_PATH_MAX 256

#define ELF_SEGMENT_EXECUTE 1
#define ELF_SEGMENT_WRITE 2
#define ELF_SEGMENT_READ 4

struct elf_load_segment {
    uintptr_t address;
    uintptr_t mapping_base;
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t memory_size;
    size_t page_offset;
    size_t page_index;
    size_t page_count;
    uint32_t flags;
};

struct elf_load_plan {
    uintptr_t entry_point;
    uintptr_t program_header_address;
    uint16_t program_header_size;
    uint16_t program_header_count;
    size_t segment_count;
    size_t total_page_count;
    size_t executable_segment;
    size_t writable_segment;
    bool has_interpreter;
    size_t interpreter_path_length;
    char interpreter_path[ELF_INTERPRETER_PATH_MAX];
    struct elf_load_segment segments[ELF_MAX_LOAD_SEGMENTS];
};

bool elf_inspect_user_image(const uint8_t *image, size_t image_size,
                            size_t page_size, uintptr_t pie_load_bias,
                            uintptr_t minimum_address,
                            uintptr_t maximum_address, struct elf_load_plan *plan);
bool elf_inspect_user_interpreter(
    const uint8_t *image, size_t image_size, size_t page_size,
    uintptr_t load_bias, uintptr_t minimum_address,
    uintptr_t maximum_address, struct elf_load_plan *plan);
bool elf_load_user_image(const uint8_t *image, size_t image_size,
                         const struct elf_load_plan *plan,
                         uint8_t *const destinations[],
                         size_t destination_page_count, size_t page_size);

#endif
