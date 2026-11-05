#include <stddef.h>
#include <stdint.h>

#include "elf.h"

#define ELF_CLASS_64 2
#define ELF_DATA_LSB 1
#define ELF_VERSION_CURRENT 1
#define ELF_TYPE_EXECUTABLE 2
#define ELF_TYPE_DYNAMIC 3
#define ELF_MACHINE_X86_64 62
#define ELF_PROGRAM_LOAD 1
#define ELF_PROGRAM_INTERPRETER 3
#define ELF_FLAG_EXECUTE 1
#define ELF_FLAG_WRITE 2
#define ELF_FLAG_READ 4
#define ELF_MAX_PROGRAM_HEADERS 16

struct elf64_header {
    uint8_t identity[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t program_header_offset;
    uint64_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_name_index;
};

struct elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};

_Static_assert(sizeof(struct elf64_header) == 64, "ELF64 header layout");
_Static_assert(sizeof(struct elf64_program_header) == 56, "ELF64 program-header layout");

static bool range_valid(size_t total_size, uint64_t offset, uint64_t length) {
    return offset <= total_size && length <= total_size - (size_t)offset;
}

static bool inspect_load_segment(const struct elf64_program_header *header,
                                 size_t image_size, size_t page_size,
                                 uintptr_t load_bias,
                                 uintptr_t minimum_address,
                                 uintptr_t maximum_address,
                                 struct elf_load_segment *segment) {
    if (header->virtual_address > UINTPTR_MAX - load_bias) {
        return false;
    }
    uintptr_t address = (uintptr_t)header->virtual_address + load_bias;
    uintptr_t mapping_base = address & ~(uintptr_t)(page_size - 1);
    size_t page_offset = (size_t)(address - mapping_base);
    if (address < minimum_address || mapping_base < minimum_address
        || header->memory_size < header->file_size || header->memory_size == 0
        || header->memory_size > SIZE_MAX - page_offset
        || header->memory_size > UINTPTR_MAX - address
        || address + header->memory_size > maximum_address
        || (header->offset & (page_size - 1))
            != (header->virtual_address & (page_size - 1))
        || (header->flags != ELF_FLAG_READ
            && header->flags != (ELF_FLAG_READ | ELF_FLAG_EXECUTE)
            && header->flags != (ELF_FLAG_READ | ELF_FLAG_WRITE))
        || (header->alignment > 1
            && ((header->alignment & (header->alignment - 1)) != 0
                || header->virtual_address % header->alignment
                    != header->offset % header->alignment))
        || !range_valid(image_size, header->offset, header->file_size)) {
        return false;
    }
    size_t mapped_size = page_offset + (size_t)header->memory_size;
    size_t pages = (mapped_size - 1) / page_size + 1;
    if (pages == 0 || pages > ELF_MAX_SEGMENT_PAGES) {
        return false;
    }
    *segment = (struct elf_load_segment) {
        .address = address,
        .mapping_base = mapping_base,
        .file_offset = header->offset,
        .file_size = header->file_size,
        .memory_size = header->memory_size,
        .page_offset = page_offset,
        .page_count = pages,
        .flags = header->flags,
    };
    return true;
}

static void load_segment_bytes(const uint8_t *image,
                              const struct elf_load_segment *segment,
                              uint8_t *const destinations[], size_t page_size) {
    for (size_t i = 0; i < (size_t)segment->file_size; i++) {
        size_t mapped_offset = segment->page_offset + i;
        destinations[segment->page_index + mapped_offset / page_size]
                    [mapped_offset % page_size] = image[segment->file_offset + i];
    }
}

static bool load_segment_valid(size_t image_size, size_t page_size,
                               const struct elf_load_segment *segment,
                               size_t page_count, size_t page_index) {
    if (page_size == 0 || page_size > UINT64_MAX / ELF_MAX_SEGMENT_PAGES
        || segment->memory_size == 0
        || segment->memory_size < segment->file_size
        || segment->page_offset >= page_size
        || segment->memory_size > SIZE_MAX - segment->page_offset
        || segment->page_count != page_count || page_count == 0
        || page_count > ELF_MAX_SEGMENT_PAGES
        || segment->page_count
            != (segment->page_offset + segment->memory_size - 1) / page_size + 1
        || segment->page_index != page_index
        || segment->address != segment->mapping_base + segment->page_offset
        || (segment->flags != ELF_FLAG_READ
            && segment->flags != (ELF_FLAG_READ | ELF_FLAG_EXECUTE)
            && segment->flags != (ELF_FLAG_READ | ELF_FLAG_WRITE))
        || !range_valid(image_size, segment->file_offset, segment->file_size)) {
        return false;
    }
    return true;
}

static bool inspect_user_image(const uint8_t *image, size_t image_size,
                               size_t page_size, uint16_t expected_type,
                               uintptr_t load_bias, bool allow_interpreter,
                               uintptr_t minimum_address,
                               uintptr_t maximum_address,
                               struct elf_load_plan *plan) {
    if (image == NULL || plan == NULL || page_size == 0
        || page_size > UINT64_MAX / ELF_MAX_SEGMENT_PAGES
        || (page_size & (page_size - 1)) != 0
        || load_bias % page_size != 0
        || minimum_address >= maximum_address
        || image_size < sizeof(struct elf64_header)) {
        return false;
    }
    *plan = (struct elf_load_plan){0};

    const struct elf64_header *header = (const struct elf64_header *)image;
    if (header->identity[0] != 0x7f || header->identity[1] != 'E'
        || header->identity[2] != 'L' || header->identity[3] != 'F'
        || header->identity[4] != ELF_CLASS_64 || header->identity[5] != ELF_DATA_LSB
        || header->identity[6] != ELF_VERSION_CURRENT
        || header->type != expected_type || header->machine != ELF_MACHINE_X86_64
        || header->version != ELF_VERSION_CURRENT
        || header->header_size != sizeof(*header)
        || header->program_header_size != sizeof(struct elf64_program_header)
        || header->program_header_count == 0
        || header->program_header_count > ELF_MAX_PROGRAM_HEADERS
        || !range_valid(image_size, header->program_header_offset,
                        (uint64_t)header->program_header_count
                            * header->program_header_size)) {
        return false;
    }

    const struct elf64_program_header *program_headers =
        (const struct elf64_program_header *)(image + header->program_header_offset);
    size_t executable_count = 0;
    size_t writable_count = 0;
    size_t phdr_segment = ELF_MAX_LOAD_SEGMENTS;
    for (size_t i = 0; i < header->program_header_count; i++) {
        const struct elf64_program_header *segment = &program_headers[i];
        if (segment->type == ELF_PROGRAM_INTERPRETER) {
            if (!allow_interpreter || plan->has_interpreter
                || segment->file_size < 2
                || segment->file_size > ELF_INTERPRETER_PATH_MAX
                || !range_valid(image_size, segment->offset,
                                segment->file_size)
                || image[segment->offset + segment->file_size - 1] != '\0') {
                return false;
            }
            for (size_t j = 0; j + 1 < segment->file_size; j++) {
                if (image[segment->offset + j] == '\0') {
                    return false;
                }
                plan->interpreter_path[j] =
                    (char)image[segment->offset + j];
            }
            plan->interpreter_path_length =
                (size_t)segment->file_size - 1;
            plan->interpreter_path[plan->interpreter_path_length] = '\0';
            plan->has_interpreter = true;
            continue;
        }
        if (segment->type != ELF_PROGRAM_LOAD) {
            continue;
        }
        if (plan->segment_count == ELF_MAX_LOAD_SEGMENTS) {
            return false;
        }
        struct elf_load_segment *load =
            &plan->segments[plan->segment_count];
        if (!inspect_load_segment(segment, image_size, page_size, load_bias,
                                  minimum_address, maximum_address, load)
            || load->page_count > ELF_MAX_SEGMENT_PAGES
                - plan->total_page_count) return false;
        uintptr_t mapping_end = load->mapping_base
            + load->page_count * page_size;
        for (size_t j = 0; j < plan->segment_count; j++) {
            const struct elf_load_segment *other = &plan->segments[j];
            uintptr_t other_end = other->mapping_base
                + other->page_count * page_size;
            if (load->mapping_base < other_end
                && other->mapping_base < mapping_end) return false;
        }
        load->page_index = plan->total_page_count;
        plan->total_page_count += load->page_count;
        if (load->flags == (ELF_FLAG_READ | ELF_FLAG_EXECUTE)) {
            plan->executable_segment = plan->segment_count;
            executable_count++;
        } else if (load->flags == (ELF_FLAG_READ | ELF_FLAG_WRITE)) {
            plan->writable_segment = plan->segment_count;
            writable_count++;
        }
        uint64_t phdr_size = (uint64_t)header->program_header_count
            * header->program_header_size;
        if (header->program_header_offset >= segment->offset
            && header->program_header_offset - segment->offset
                <= segment->file_size
            && phdr_size <= segment->file_size
                - (header->program_header_offset - segment->offset)) {
            if (phdr_segment != ELF_MAX_LOAD_SEGMENTS) return false;
            phdr_segment = plan->segment_count;
        }
        plan->segment_count++;
    }

    if (plan->segment_count < 2 || executable_count != 1
        || writable_count != 1 || phdr_segment == ELF_MAX_LOAD_SEGMENTS
        || header->entry > UINTPTR_MAX - load_bias
        || (plan->has_interpreter
            && plan->interpreter_path[0] != '/')) {
        return false;
    }
    plan->entry_point = (uintptr_t)header->entry + load_bias;
    const struct elf_load_segment *executable =
        &plan->segments[plan->executable_segment];
    const struct elf_load_segment *writable =
        &plan->segments[plan->writable_segment];
    if (plan->entry_point < executable->address
        || plan->entry_point - executable->address >= executable->file_size) {
        return false;
    }
    uintptr_t writable_end = writable->mapping_base
        + writable->page_count * page_size;
    for (size_t i = 0; i < plan->segment_count; i++) {
        if (i != plan->writable_segment
            && plan->segments[i].mapping_base >= writable_end) return false;
    }
    const struct elf_load_segment *phdr_load = &plan->segments[phdr_segment];
    uint64_t phdr_delta = header->program_header_offset
        - phdr_load->file_offset;
    if (phdr_delta > UINTPTR_MAX - phdr_load->address) return false;
    plan->program_header_address = phdr_load->address + (uintptr_t)phdr_delta;
    plan->program_header_size = header->program_header_size;
    plan->program_header_count = header->program_header_count;
    return true;
}

bool elf_inspect_user_image(const uint8_t *image, size_t image_size,
                            size_t page_size, uintptr_t pie_load_bias,
                            uintptr_t minimum_address,
                            uintptr_t maximum_address,
                            struct elf_load_plan *plan) {
    if (image == NULL || image_size < sizeof(struct elf64_header)) return false;
    const struct elf64_header *header = (const struct elf64_header *)image;
    uint16_t expected_type;
    uintptr_t load_bias;
    if (header->type == ELF_TYPE_EXECUTABLE) {
        expected_type = ELF_TYPE_EXECUTABLE;
        load_bias = 0;
    } else if (header->type == ELF_TYPE_DYNAMIC && pie_load_bias != 0) {
        expected_type = ELF_TYPE_DYNAMIC;
        load_bias = pie_load_bias;
    } else {
        return false;
    }
    return inspect_user_image(image, image_size, page_size,
                              expected_type, load_bias, true,
                              minimum_address, maximum_address, plan);
}

bool elf_inspect_user_interpreter(
    const uint8_t *image, size_t image_size, size_t page_size,
    uintptr_t load_bias, uintptr_t minimum_address,
    uintptr_t maximum_address, struct elf_load_plan *plan) {
    return inspect_user_image(image, image_size, page_size,
                              ELF_TYPE_DYNAMIC, load_bias, false,
                              minimum_address, maximum_address, plan);
}

bool elf_load_user_image(const uint8_t *image, size_t image_size,
                         const struct elf_load_plan *plan,
                         uint8_t *const destinations[],
                         size_t destination_page_count, size_t page_size) {
    if (image == NULL || plan == NULL || destinations == NULL
        || plan->segment_count < 2
        || plan->segment_count > ELF_MAX_LOAD_SEGMENTS
        || destination_page_count != plan->total_page_count
        || destination_page_count > ELF_MAX_SEGMENT_PAGES) {
        return false;
    }
    size_t page_index = 0;
    for (size_t i = 0; i < plan->segment_count; i++) {
        const struct elf_load_segment *segment = &plan->segments[i];
        if (!load_segment_valid(image_size, page_size, segment,
                                segment->page_count, page_index)) return false;
        page_index += segment->page_count;
    }
    if (page_index != destination_page_count) return false;
    for (size_t i = 0; i < destination_page_count; i++) {
        if (destinations[i] == NULL) return false;
    }
    for (size_t i = 0; i < plan->segment_count; i++) {
        load_segment_bytes(image, &plan->segments[i], destinations, page_size);
    }
    return true;
}
