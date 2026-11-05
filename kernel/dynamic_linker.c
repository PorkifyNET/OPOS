#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096u
#define LIBRARY_MAPPING_SIZE (16u * PAGE_SIZE)
#define MAX_LIBRARIES 4u
#define MAX_NEEDED 4u
#define MAX_LIBRARY_NAME 57u
#define MAX_SYMBOLS 256u
#define MAX_LOAD_SEGMENTS 8u

#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_PHDR 6u
#define PT_TLS 7u
#define PT_GNU_RELRO 0x6474e552u
#define PF_X 1u
#define PF_W 2u
#define PF_R 4u

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_PLTREL 20
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37
#define DT_VERSYM 0x6ffffff0
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEED 0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

#define R_X86_64_64 1u
#define R_X86_64_COPY 5u
#define R_X86_64_GLOB_DAT 6u
#define R_X86_64_JUMP_SLOT 7u
#define R_X86_64_RELATIVE 8u
#define R_X86_64_DTPMOD64 16u
#define R_X86_64_DTPOFF64 17u
#define R_X86_64_TPOFF64 18u
#define R_X86_64_TLSDESC 36u
#define R_X86_64_IRELATIVE 37u

#define STB_LOCAL 0u
#define STB_GLOBAL 1u
#define STB_WEAK 2u

#define STV_DEFAULT 0u
#define STV_PROTECTED 3u

#define STT_OBJECT 1u
#define STT_FUNC 2u
#define STT_TLS 6u
#define STT_GNU_IFUNC 10u

#define RELOCATION_REGULAR 0
#define RELOCATION_IFUNC 1

#define ARCH_SET_FS 0x1002u
#define MAP_PRIVATE 2u
#define MAP_ANONYMOUS 0x20u
#define TLS_DTV_STRIDE 16u
#define TLS_DTV_OFFSET 32u
#define TLS_CONTROL_SIZE \
    (TLS_DTV_OFFSET + (MAX_LIBRARIES + 2u) * TLS_DTV_STRIDE)

#define AT_PHDR 3u
#define AT_PHNUM 5u

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

struct elf64_dynamic {
    int64_t tag;
    uint64_t value;
};

struct elf64_symbol {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t section_index;
    uint64_t value;
    uint64_t size;
};

struct elf64_rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};

struct elf64_verdef {
    uint16_t version;
    uint16_t flags;
    uint16_t index;
    uint16_t count;
    uint32_t hash;
    uint32_t auxiliary;
    uint32_t next;
};

struct elf64_verdaux {
    uint32_t name;
    uint32_t next;
};

struct elf64_verneed {
    uint16_t version;
    uint16_t count;
    uint32_t file;
    uint32_t auxiliary;
    uint32_t next;
};

struct elf64_vernaux {
    uint32_t hash;
    uint16_t flags;
    uint16_t other;
    uint32_t name;
    uint32_t next;
};

struct symbol_version {
    const char *name;
    size_t name_limit;
    uint32_t hash;
    int present;
    int hidden;
};

struct tls_index {
    uint64_t module;
    uint64_t offset;
};

struct tls_resolution {
    int64_t thread_offset;
    uint64_t module_id;
    uint64_t module_offset;
};

struct dynamic_info {
    uint64_t needed[MAX_NEEDED];
    size_t needed_count;
    uint64_t sysv_hash;
    uint64_t gnu_hash;
    uint64_t string_table;
    uint64_t string_size;
    uint64_t symbol_table;
    uint64_t rela;
    uint64_t rela_size;
    uint64_t rela_entry_size;
    uint64_t relr;
    uint64_t relr_size;
    uint64_t relr_entry_size;
    uint64_t jump_rela;
    uint64_t jump_rela_size;
    uint64_t plt_relocation_type;
    uint64_t symbol_entry_size;
    uint64_t init;
    uint64_t fini;
    uint64_t init_array;
    uint64_t init_array_size;
    uint64_t fini_array;
    uint64_t fini_array_size;
    uint64_t version_symbols;
    uint64_t version_definitions;
    uint64_t version_definition_count;
    uint64_t version_needs;
    uint64_t version_need_count;
};

struct object_segment {
    uintptr_t start;
    uintptr_t end;
    uint32_t flags;
};

struct loaded_object {
    uintptr_t base;
    struct object_segment segments[MAX_LOAD_SEGMENTS];
    size_t segment_count;
    uintptr_t executable_start;
    uintptr_t executable_end;
    uintptr_t writable_start;
    uintptr_t writable_end;
    uintptr_t relro_start;
    uintptr_t relro_end;
    uintptr_t tls_image;
    uint64_t tls_file_size;
    uint64_t tls_memory_size;
    uint64_t tls_alignment;
    int64_t tls_offset;
    uint64_t tls_module_id;
    uintptr_t tls_block;
    struct dynamic_info dynamic;
    int addresses_are_absolute;
    char name[MAX_LIBRARY_NAME + 1];
};

extern intptr_t opos_tlsdesc_return(void);
extern intptr_t opos_tlsdesc_undefweak(void);

static struct loaded_object loaded_objects[MAX_LIBRARIES];
static size_t loaded_object_count;
static struct loaded_object loaded_main;
static uint8_t initialization_state[MAX_LIBRARIES];
static uint8_t initialization_order[MAX_LIBRARIES];
static size_t initialization_count;
static int main_initialized;
static int finalized;

_Static_assert(sizeof(struct elf64_header) == 64, "ELF header size");
_Static_assert(sizeof(struct elf64_program_header) == 56, "ELF phdr size");
_Static_assert(sizeof(struct elf64_symbol) == 24, "ELF symbol size");
_Static_assert(sizeof(struct elf64_rela) == 24, "ELF rela size");
_Static_assert(sizeof(struct elf64_verdef) == 20, "ELF verdef size");
_Static_assert(sizeof(struct elf64_verdaux) == 8, "ELF verdaux size");
_Static_assert(sizeof(struct elf64_verneed) == 16, "ELF verneed size");
_Static_assert(sizeof(struct elf64_vernaux) == 16, "ELF vernaux size");

static long linux_syscall6(long number, long first, long second, long third,
                           long fourth, long fifth, long sixth) {
    register long r10 __asm__("r10") = fourth;
    register long r8 __asm__("r8") = fifth;
    register long r9 __asm__("r9") = sixth;
    long result;
    __asm__ volatile ("syscall"
                      : "=a"(result)
                      : "a"(number), "D"(first), "S"(second), "d"(third),
                        "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return result;
}

static int mapped_range_valid(uint64_t offset, uint64_t size) {
    return offset <= LIBRARY_MAPPING_SIZE
        && size <= LIBRARY_MAPPING_SIZE - offset;
}

static int address_range_valid(uintptr_t start, uintptr_t end,
                               uintptr_t address, size_t size) {
    return address >= start && address <= end
        && size <= end - address;
}

static int object_address(const struct loaded_object *object, uint64_t value,
                          size_t size, uintptr_t *address) {
    if (!object->addresses_are_absolute
        && value > UINTPTR_MAX - object->base) return 0;
    uintptr_t result = object->addresses_are_absolute
        ? (uintptr_t)value : object->base + (uintptr_t)value;
    for (size_t i = 0; i < object->segment_count; i++) {
        if (address_range_valid(object->segments[i].start,
                                object->segments[i].end, result, size)) {
            *address = result;
            return 1;
        }
    }
    return 0;
}

static int object_range_has_flags(const struct loaded_object *object,
                                  uintptr_t address, size_t size,
                                  uint32_t required) {
    for (size_t i = 0; i < object->segment_count; i++) {
        if ((object->segments[i].flags & required) == required
            && address_range_valid(object->segments[i].start,
                                   object->segments[i].end,
                                   address, size)) return 1;
    }
    return 0;
}

static int object_writable_range(const struct loaded_object *object,
                                 uintptr_t address, size_t size) {
    return object_range_has_flags(object, address, size, PF_W);
}

static int object_executable_range(const struct loaded_object *object,
                                   uintptr_t address, size_t size) {
    return object_range_has_flags(object, address, size, PF_X);
}

static size_t bounded_string_length(const char *string, size_t limit) {
    for (size_t i = 0; i < limit; i++) {
        if (string[i] == '\0') return i;
    }
    return limit;
}

static int bounded_strings_equal(const char *left, size_t left_limit,
                                 const char *right, size_t right_limit) {
    size_t left_length = bounded_string_length(left, left_limit);
    size_t right_length = bounded_string_length(right, right_limit);
    if (left_length == left_limit || right_length == right_limit
        || left_length != right_length) return 0;
    for (size_t i = 0; i < left_length; i++) {
        if (left[i] != right[i]) return 0;
    }
    return 1;
}

static int read_dynamic(const struct elf64_dynamic *dynamic,
                        size_t maximum_entries, struct dynamic_info *info) {
    *info = (struct dynamic_info){0};
    for (size_t i = 0; i < maximum_entries; i++) {
        uint64_t value = dynamic[i].value;
        switch (dynamic[i].tag) {
        case DT_NULL: return 1;
        case DT_NEEDED:
            if (info->needed_count == MAX_NEEDED) return 0;
            info->needed[info->needed_count++] = value;
            break;
        case DT_HASH: info->sysv_hash = value; break;
        case DT_GNU_HASH: info->gnu_hash = value; break;
        case DT_STRTAB: info->string_table = value; break;
        case DT_STRSZ: info->string_size = value; break;
        case DT_SYMTAB: info->symbol_table = value; break;
        case DT_SYMENT: info->symbol_entry_size = value; break;
        case DT_RELA: info->rela = value; break;
        case DT_RELASZ: info->rela_size = value; break;
        case DT_RELAENT: info->rela_entry_size = value; break;
        case DT_RELR: info->relr = value; break;
        case DT_RELRSZ: info->relr_size = value; break;
        case DT_RELRENT: info->relr_entry_size = value; break;
        case DT_JMPREL: info->jump_rela = value; break;
        case DT_PLTRELSZ: info->jump_rela_size = value; break;
        case DT_PLTREL: info->plt_relocation_type = value; break;
        case DT_INIT: info->init = value; break;
        case DT_FINI: info->fini = value; break;
        case DT_INIT_ARRAY: info->init_array = value; break;
        case DT_INIT_ARRAYSZ: info->init_array_size = value; break;
        case DT_FINI_ARRAY: info->fini_array = value; break;
        case DT_FINI_ARRAYSZ: info->fini_array_size = value; break;
        case DT_VERSYM: info->version_symbols = value; break;
        case DT_VERDEF: info->version_definitions = value; break;
        case DT_VERDEFNUM: info->version_definition_count = value; break;
        case DT_VERNEED: info->version_needs = value; break;
        case DT_VERNEEDNUM: info->version_need_count = value; break;
        default: break;
        }
    }
    return 0;
}

static uint32_t gnu_symbol_hash(const char *name, size_t limit) {
    uint32_t hash = 5381;
    for (size_t i = 0; i < limit && name[i] != '\0'; i++) {
        hash = hash * 33u + (uint8_t)name[i];
    }
    return hash;
}

static int add_u64(uint64_t left, uint64_t right, uint64_t *result) {
    if (left > UINT64_MAX - right) return 0;
    *result = left + right;
    return 1;
}

static int version_name(const struct loaded_object *object, uint32_t offset,
                        const char **name, size_t *name_limit) {
    if (offset >= object->dynamic.string_size) return 0;
    uintptr_t strings;
    if (!object_address(object, object->dynamic.string_table,
                        object->dynamic.string_size, &strings)) return 0;
    const char *candidate = (const char *)(strings + offset);
    size_t limit = object->dynamic.string_size - offset;
    size_t length = bounded_string_length(candidate, limit);
    if (length == 0 || length == limit) return 0;
    *name = candidate;
    *name_limit = limit;
    return 1;
}

static int find_needed_version(const struct loaded_object *object,
                               uint16_t version_index,
                               struct symbol_version *version) {
    const struct dynamic_info *dynamic = &object->dynamic;
    if (dynamic->version_need_count == 0) return 0;
    if (dynamic->version_need_count > 16 || dynamic->version_needs == 0) return -1;
    uint64_t need_value = dynamic->version_needs;
    for (uint64_t i = 0; i < dynamic->version_need_count; i++) {
        uintptr_t need_address;
        if (!object_address(object, need_value, sizeof(struct elf64_verneed),
                            &need_address)) return -1;
        const struct elf64_verneed *need =
            (const struct elf64_verneed *)need_address;
        if (need->version != 1 || need->count == 0 || need->count > 16
            || need->auxiliary == 0) return -1;
        uint64_t auxiliary_value;
        if (!add_u64(need_value, need->auxiliary, &auxiliary_value)) return -1;
        for (uint16_t j = 0; j < need->count; j++) {
            uintptr_t auxiliary_address;
            if (!object_address(object, auxiliary_value,
                                sizeof(struct elf64_vernaux),
                                &auxiliary_address)) return -1;
            const struct elf64_vernaux *auxiliary =
                (const struct elf64_vernaux *)auxiliary_address;
            if ((auxiliary->other & 0x7fffu) == version_index) {
                if (!version_name(object, auxiliary->name,
                                  &version->name,
                                  &version->name_limit)) return -1;
                version->hash = auxiliary->hash;
                version->present = 1;
                return 1;
            }
            if (j + 1 < need->count
                && (auxiliary->next == 0
                    || !add_u64(auxiliary_value, auxiliary->next,
                                &auxiliary_value))) return -1;
        }
        if (i + 1 < dynamic->version_need_count
            && (need->next == 0
                || !add_u64(need_value, need->next, &need_value))) return -1;
    }
    return 0;
}

static int find_defined_version(const struct loaded_object *object,
                                uint16_t version_index,
                                struct symbol_version *version) {
    const struct dynamic_info *dynamic = &object->dynamic;
    if (dynamic->version_definition_count == 0) return 0;
    if (dynamic->version_definition_count > 16
        || dynamic->version_definitions == 0) return -1;
    uint64_t definition_value = dynamic->version_definitions;
    for (uint64_t i = 0; i < dynamic->version_definition_count; i++) {
        uintptr_t definition_address;
        if (!object_address(object, definition_value,
                            sizeof(struct elf64_verdef),
                            &definition_address)) return -1;
        const struct elf64_verdef *definition =
            (const struct elf64_verdef *)definition_address;
        if (definition->version != 1 || definition->count == 0
            || definition->count > 16 || definition->auxiliary == 0) return -1;
        if ((definition->index & 0x7fffu) == version_index) {
            uint64_t auxiliary_value;
            uintptr_t auxiliary_address;
            if (!add_u64(definition_value, definition->auxiliary,
                         &auxiliary_value)
                || !object_address(object, auxiliary_value,
                                   sizeof(struct elf64_verdaux),
                                   &auxiliary_address)) return -1;
            const struct elf64_verdaux *auxiliary =
                (const struct elf64_verdaux *)auxiliary_address;
            if (!version_name(object, auxiliary->name,
                              &version->name,
                              &version->name_limit)) return -1;
            version->hash = definition->hash;
            version->present = 1;
            return 1;
        }
        if (i + 1 < dynamic->version_definition_count
            && (definition->next == 0
                || !add_u64(definition_value, definition->next,
                            &definition_value))) return -1;
    }
    return 0;
}

static int read_symbol_version(const struct loaded_object *object,
                               uint32_t symbol_index,
                               struct symbol_version *version) {
    *version = (struct symbol_version){0};
    if (object->dynamic.version_symbols == 0) return 1;
    uint64_t entry_offset = (uint64_t)symbol_index * sizeof(uint16_t);
    uint64_t entry_value;
    uintptr_t entry_address;
    if (!add_u64(object->dynamic.version_symbols, entry_offset, &entry_value)
        || !object_address(object, entry_value, sizeof(uint16_t),
                           &entry_address)) return 0;
    uint16_t raw = *(const uint16_t *)entry_address;
    uint16_t index = raw & 0x7fffu;
    version->hidden = (raw & 0x8000u) != 0;
    if (index <= 1) return 1;
    int result = find_needed_version(object, index, version);
    if (result < 0) return 0;
    if (result > 0) return 1;
    result = find_defined_version(object, index, version);
    return result > 0;
}

static int symbol_version_matches(const struct loaded_object *object,
                                  uint32_t symbol_index,
                                  const struct symbol_version *required) {
    struct symbol_version candidate;
    if (!read_symbol_version(object, symbol_index, &candidate)) return 0;
    if (!required->present) return !candidate.present || !candidate.hidden;
    return candidate.present && candidate.hash == required->hash
        && bounded_strings_equal(candidate.name, candidate.name_limit,
                                 required->name, required->name_limit);
}

static uintptr_t symbol_value(const struct loaded_object *object,
                              uint32_t symbol_index,
                              const char *name, size_t name_limit,
                              const struct symbol_version *version,
                              uint8_t *type, uint64_t *size) {
    const struct dynamic_info *dynamic = &object->dynamic;
    uintptr_t symbol_address;
    uintptr_t string_address;
    uint64_t symbol_offset = (uint64_t)symbol_index
        * sizeof(struct elf64_symbol);
    if (symbol_index >= MAX_SYMBOLS
        || dynamic->symbol_entry_size != sizeof(struct elf64_symbol)
        || dynamic->symbol_table > UINT64_MAX - symbol_offset
        || !object_address(object, dynamic->symbol_table + symbol_offset,
            sizeof(struct elf64_symbol), &symbol_address)
        || !object_address(object, dynamic->string_table,
                           dynamic->string_size, &string_address)) return 0;
    const struct elf64_symbol *symbol =
        (const struct elf64_symbol *)symbol_address;
    uint8_t binding = symbol->info >> 4;
    uint8_t visibility = symbol->other & 3u;
    if (symbol->section_index == 0 || symbol->name >= dynamic->string_size
        || (binding != STB_GLOBAL && binding != STB_WEAK)
        || (visibility != STV_DEFAULT && visibility != STV_PROTECTED)) return 0;
    const char *candidate = (const char *)(string_address + symbol->name);
    if (!bounded_strings_equal(candidate, dynamic->string_size - symbol->name,
                               name, name_limit)) return 0;
    if (!symbol_version_matches(object, symbol_index, version)) return 0;
    uintptr_t value;
    if (!object_address(object, symbol->value, 1, &value)) return 0;
    *type = symbol->info & 15u;
    *size = symbol->size;
    return value;
}

static uintptr_t lookup_sysv(const struct loaded_object *object,
                             const char *name, size_t name_limit,
                             const struct symbol_version *version,
                             uint8_t *type, uint64_t *size) {
    const struct dynamic_info *dynamic = &object->dynamic;
    uintptr_t hash_address;
    if (dynamic->sysv_hash == 0
        || !object_address(object, dynamic->sysv_hash,
                           2 * sizeof(uint32_t), &hash_address)) return 0;
    const uint32_t *hash = (const uint32_t *)hash_address;
    uint32_t bucket_count = hash[0];
    uint32_t symbol_count = hash[1];
    uint64_t words = 2u + (uint64_t)bucket_count + symbol_count;
    if (bucket_count == 0 || symbol_count == 0 || symbol_count > MAX_SYMBOLS
        || !object_address(object, dynamic->sysv_hash,
                           words * sizeof(uint32_t), &hash_address)) return 0;
    for (uint32_t i = 1; i < symbol_count; i++) {
        uintptr_t value = symbol_value(object, i, name, name_limit, version,
                                       type, size);
        if (value != 0) return value;
    }
    return 0;
}

static uintptr_t lookup_gnu(const struct loaded_object *object,
                            const char *name, size_t name_limit,
                            const struct symbol_version *version,
                            uint8_t *type, uint64_t *size) {
    const struct dynamic_info *dynamic = &object->dynamic;
    uintptr_t header_address;
    if (dynamic->gnu_hash == 0
        || !object_address(object, dynamic->gnu_hash,
                           4 * sizeof(uint32_t), &header_address)) return 0;
    const uint32_t *header = (const uint32_t *)header_address;
    uint32_t bucket_count = header[0];
    uint32_t symbol_offset = header[1];
    uint32_t bloom_count = header[2];
    uint32_t bloom_shift = header[3];
    uint64_t fixed_size = 4u * sizeof(uint32_t)
        + (uint64_t)bloom_count * sizeof(uint64_t)
        + (uint64_t)bucket_count * sizeof(uint32_t);
    if (bucket_count == 0 || bloom_count == 0 || bloom_count > 32
        || symbol_offset >= MAX_SYMBOLS
        || !object_address(object, dynamic->gnu_hash,
                           fixed_size, &header_address)) return 0;
    const uint64_t *bloom = (const uint64_t *)(header + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_count);
    const uint32_t *chains = buckets + bucket_count;
    uint32_t hash = gnu_symbol_hash(name, name_limit);
    uint64_t bloom_word = bloom[(hash / 64u) % bloom_count];
    uint64_t mask = (UINT64_C(1) << (hash % 64u))
        | (UINT64_C(1) << ((hash >> bloom_shift) % 64u));
    if ((bloom_word & mask) != mask) return 0;
    uint32_t symbol_index = buckets[hash % bucket_count];
    if (symbol_index < symbol_offset || symbol_index >= MAX_SYMBOLS) return 0;
    for (; symbol_index < MAX_SYMBOLS; symbol_index++) {
        uint64_t chain_offset = (uint64_t)(symbol_index - symbol_offset)
            * sizeof(uint32_t);
        uintptr_t chain_address = (uintptr_t)chains + chain_offset;
        uintptr_t checked_address;
        uint64_t chain_value = dynamic->gnu_hash
            + fixed_size + chain_offset;
        if (!object_address(object, chain_value, sizeof(uint32_t),
                            &checked_address)
            || checked_address != chain_address) return 0;
        uint32_t chain_hash = *(const uint32_t *)chain_address;
        if ((chain_hash | 1u) == (hash | 1u)) {
            uintptr_t value = symbol_value(object, symbol_index,
                                           name, name_limit, version, type,
                                           size);
            if (value != 0) return value;
        }
        if ((chain_hash & 1u) != 0) break;
    }
    return 0;
}

static const struct loaded_object *tls_object_by_module(uint64_t module) {
    if (loaded_main.tls_module_id == module) return &loaded_main;
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (loaded_objects[i].tls_module_id == module) {
            return &loaded_objects[i];
        }
    }
    return NULL;
}

void *opos_tls_get_addr(const struct tls_index *index) {
    uintptr_t dtv;
    __asm__ volatile ("movq %%fs:8, %0" : "=r"(dtv));
    if (index == NULL || dtv == 0 || index->module == 0
        || index->module > MAX_LIBRARIES + 1u) return NULL;
    const struct loaded_object *object = tls_object_by_module(index->module);
    if (object == NULL || index->offset >= object->tls_memory_size) return NULL;
    uintptr_t block = *(const uintptr_t *)(dtv
        + index->module * TLS_DTV_STRIDE);
    return block == 0 ? NULL : (void *)(block + index->offset);
}

static uintptr_t resolve_global(const struct loaded_object objects[],
                                size_t object_count,
                                const char *name, size_t name_limit,
                                const struct symbol_version *version,
                                uint8_t *type, uint64_t *size) {
    static const char tls_get_addr_name[] = "__tls_get_addr";
    if (!version->present
        && bounded_strings_equal(name, name_limit, tls_get_addr_name,
                                 sizeof(tls_get_addr_name))) {
        *type = STT_FUNC;
        *size = 0;
        return (uintptr_t)&opos_tls_get_addr;
    }
    uintptr_t value = lookup_sysv(&loaded_main, name, name_limit, version,
                                  type, size);
    if (value == 0) {
        value = lookup_gnu(&loaded_main, name, name_limit, version, type,
                           size);
    }
    if (value != 0) return value;
    for (size_t i = 0; i < object_count; i++) {
        value = lookup_sysv(&objects[i], name, name_limit, version, type,
                            size);
        if (value == 0) {
            value = lookup_gnu(&objects[i], name, name_limit, version, type,
                               size);
        }
        if (value != 0) return value;
    }
    return 0;
}

static uintptr_t resolve_library_global(
        const struct loaded_object objects[], size_t object_count,
        const char *name, size_t name_limit,
        const struct symbol_version *version, uint8_t *type, uint64_t *size,
        const struct loaded_object **definition) {
    for (size_t i = 0; i < object_count; i++) {
        uintptr_t value = lookup_sysv(&objects[i], name, name_limit, version,
                                      type, size);
        if (value == 0) {
            value = lookup_gnu(&objects[i], name, name_limit, version,
                               type, size);
        }
        if (value != 0) {
            *definition = &objects[i];
            return value;
        }
    }
    return 0;
}

static uint32_t gnu_object_symbol_count(const struct loaded_object *object) {
    const struct dynamic_info *dynamic = &object->dynamic;
    uintptr_t header_address;
    if (!object_address(object, dynamic->gnu_hash,
                        4 * sizeof(uint32_t), &header_address)) return 0;
    const uint32_t *header = (const uint32_t *)header_address;
    uint32_t bucket_count = header[0];
    uint32_t symbol_offset = header[1];
    uint32_t bloom_count = header[2];
    uint64_t fixed_size = 4u * sizeof(uint32_t)
        + (uint64_t)bloom_count * sizeof(uint64_t)
        + (uint64_t)bucket_count * sizeof(uint32_t);
    if (bucket_count == 0 || bloom_count == 0 || bloom_count > 32
        || symbol_offset >= MAX_SYMBOLS
        || !object_address(object, dynamic->gnu_hash,
                           fixed_size, &header_address)) return 0;
    const uint64_t *bloom = (const uint64_t *)(header + 4);
    const uint32_t *buckets = (const uint32_t *)(bloom + bloom_count);
    uint32_t symbol_count = symbol_offset;
    for (uint32_t i = 0; i < bucket_count; i++) {
        uint32_t index = buckets[i];
        if (index == 0) continue;
        if (index < symbol_offset || index >= MAX_SYMBOLS) return 0;
        for (;;) {
            uint64_t chain_offset = (uint64_t)(index - symbol_offset)
                * sizeof(uint32_t);
            if (dynamic->gnu_hash > UINT64_MAX - fixed_size
                || dynamic->gnu_hash + fixed_size
                    > UINT64_MAX - chain_offset) return 0;
            uintptr_t chain_address;
            if (!object_address(object,
                    dynamic->gnu_hash + fixed_size + chain_offset,
                    sizeof(uint32_t), &chain_address)) return 0;
            uint32_t chain = *(const uint32_t *)chain_address;
            if (index + 1 > symbol_count) symbol_count = index + 1;
            if ((chain & 1u) != 0) break;
            index++;
            if (index >= MAX_SYMBOLS) return 0;
        }
    }
    return symbol_count;
}

static uint32_t object_symbol_count(const struct loaded_object *object) {
    const struct dynamic_info *dynamic = &object->dynamic;
    if (dynamic->sysv_hash != 0) {
        uintptr_t hash_address;
        if (!object_address(object, dynamic->sysv_hash,
                            2 * sizeof(uint32_t), &hash_address)) return 0;
        const uint32_t *hash = (const uint32_t *)hash_address;
        return hash[1] <= MAX_SYMBOLS ? hash[1] : 0;
    }
    return dynamic->gnu_hash != 0 ? gnu_object_symbol_count(object) : 0;
}

static int lookup_tls_symbol(const struct loaded_object *object,
                             const char *name, size_t name_limit,
                             const struct symbol_version *version,
                             struct tls_resolution *resolution) {
    if (object->tls_memory_size == 0) return 0;
    uint32_t symbol_count = object_symbol_count(object);
    uintptr_t string_address;
    if (symbol_count == 0
        || !object_address(object, object->dynamic.string_table,
                           object->dynamic.string_size,
                           &string_address)) return 0;
    const char *strings = (const char *)string_address;
    for (uint32_t i = 1; i < symbol_count; i++) {
        uint64_t symbol_offset = (uint64_t)i * sizeof(struct elf64_symbol);
        if (object->dynamic.symbol_table > UINT64_MAX - symbol_offset) return 0;
        uintptr_t symbol_address;
        if (!object_address(object,
                object->dynamic.symbol_table + symbol_offset,
                sizeof(struct elf64_symbol), &symbol_address)) return 0;
        const struct elf64_symbol *symbol =
            (const struct elf64_symbol *)symbol_address;
        uint8_t binding = symbol->info >> 4;
        uint8_t type = symbol->info & 15u;
        uint8_t visibility = symbol->other & 3u;
        if (symbol->section_index == 0 || type != STT_TLS
            || symbol->name >= object->dynamic.string_size
            || symbol->value >= object->tls_memory_size
            || (binding != STB_GLOBAL && binding != STB_WEAK)
            || (visibility != STV_DEFAULT && visibility != STV_PROTECTED)) {
            continue;
        }
        const char *candidate = strings + symbol->name;
        if (!bounded_strings_equal(candidate,
                object->dynamic.string_size - symbol->name,
                name, name_limit)) continue;
        if (!symbol_version_matches(object, i, version)) continue;
        resolution->thread_offset = object->tls_offset
            + (int64_t)symbol->value;
        resolution->module_id = object->tls_module_id;
        resolution->module_offset = symbol->value;
        return 1;
    }
    return 0;
}

static int resolve_tls_global(const struct loaded_object objects[],
                              size_t object_count,
                              const char *name, size_t name_limit,
                              const struct symbol_version *version,
                              struct tls_resolution *resolution) {
    if (lookup_tls_symbol(&loaded_main, name, name_limit,
                          version, resolution)) return 1;
    for (size_t i = 0; i < object_count; i++) {
        if (lookup_tls_symbol(&objects[i], name, name_limit,
                              version, resolution)) return 1;
    }
    return 0;
}

static int executable_address_valid(const struct loaded_object *object,
                                    uintptr_t address) {
    return object_executable_range(object, address, 1);
}

static int executable_address_in_scope(
        uintptr_t address, const struct loaded_object libraries[],
        size_t library_count) {
    if (executable_address_valid(&loaded_main, address)) return 1;
    for (size_t i = 0; i < library_count; i++) {
        if (executable_address_valid(&libraries[i], address)) return 1;
    }
    return 0;
}

static int resolve_ifunc(uintptr_t resolver,
                         const struct loaded_object libraries[],
                         size_t library_count, uintptr_t *value) {
    if (!executable_address_in_scope(resolver, libraries, library_count)) {
        return 0;
    }
    uintptr_t result = ((uintptr_t (*)(void))resolver)();
    if (!executable_address_in_scope(result, libraries, library_count)) {
        return 0;
    }
    *value = result;
    return 1;
}

static int apply_relocations(const struct loaded_object *object,
                             const struct loaded_object libraries[],
                             size_t library_count,
                             uint64_t relocation_address,
                             uint64_t relocation_size,
                             int phase) {
    const struct dynamic_info *dynamic = &object->dynamic;
    if (relocation_size == 0) return 1;
    if ((dynamic->rela_entry_size != 0
         && dynamic->rela_entry_size != sizeof(struct elf64_rela))
        || relocation_size % sizeof(struct elf64_rela) != 0) return 0;
    uintptr_t table;
    if (!object_address(object, relocation_address,
                        relocation_size, &table)) return 0;
    size_t count = relocation_size / sizeof(struct elf64_rela);
    const struct elf64_rela *relocations = (const struct elf64_rela *)table;
    uintptr_t string_table;
    uint32_t symbol_count = object_symbol_count(object);
    if (symbol_count == 0
        || dynamic->symbol_entry_size != sizeof(struct elf64_symbol)
        || !object_address(object, dynamic->string_table,
                           dynamic->string_size, &string_table)) return 0;
    const char *strings = (const char *)string_table;
    for (size_t i = 0; i < count; i++) {
        uint32_t type = (uint32_t)relocations[i].info;
        uint32_t symbol_index = (uint32_t)(relocations[i].info >> 32);
        uintptr_t target;
        size_t target_size = type == R_X86_64_COPY ? 1
            : (type == R_X86_64_TLSDESC ? 2 * sizeof(uint64_t)
                                        : sizeof(uint64_t));
        if (!object_address(object, relocations[i].offset,
                            target_size, &target)
            || !object_writable_range(object, target, target_size)) return 0;
        if (type == R_X86_64_RELATIVE) {
            if (phase == RELOCATION_REGULAR) {
                *(uint64_t *)target = object->base + relocations[i].addend;
            }
            continue;
        }
        if (type == R_X86_64_IRELATIVE) {
            if (phase == RELOCATION_REGULAR) continue;
            uintptr_t resolver;
            if (!object_address(object, relocations[i].addend, 1, &resolver)
                || !executable_address_valid(object, resolver)) return 0;
            uintptr_t value;
            if (!resolve_ifunc(resolver, libraries, library_count, &value)) {
                return 0;
            }
            *(uint64_t *)target = value;
            continue;
        }
        if (type != R_X86_64_64 && type != R_X86_64_COPY
            && type != R_X86_64_GLOB_DAT
            && type != R_X86_64_JUMP_SLOT
            && type != R_X86_64_DTPMOD64
            && type != R_X86_64_DTPOFF64
            && type != R_X86_64_TPOFF64
            && type != R_X86_64_TLSDESC) return 0;
        uintptr_t current_symbol_address;
        uint64_t symbol_offset = (uint64_t)symbol_index
            * sizeof(struct elf64_symbol);
        if (symbol_index >= symbol_count
            || dynamic->symbol_table > UINT64_MAX - symbol_offset
            || !object_address(object, dynamic->symbol_table + symbol_offset,
                sizeof(struct elf64_symbol), &current_symbol_address)) return 0;
        const struct elf64_symbol *symbol =
            (const struct elf64_symbol *)current_symbol_address;
        if (symbol->name >= dynamic->string_size) return 0;
        uint8_t binding = symbol->info >> 4;
        uint8_t visibility = symbol->other & 3u;
        if (binding != STB_LOCAL && binding != STB_GLOBAL
            && binding != STB_WEAK) return 0;
        struct symbol_version version;
        if (!read_symbol_version(object, symbol_index, &version)) return 0;
        if (type == R_X86_64_COPY) {
            if (phase == RELOCATION_IFUNC) continue;
            if (object != &loaded_main || relocations[i].addend != 0
                || (symbol->info & 15u) != STT_OBJECT
                || symbol->section_index == 0
                || symbol->size > SIZE_MAX) return 0;
            size_t copy_size = (size_t)symbol->size;
            uintptr_t symbol_target;
            if (copy_size == 0) continue;
            if (!object_address(object, symbol->value, copy_size,
                                &symbol_target)
                || symbol_target != target
                || !object_writable_range(object, target, copy_size)) return 0;
            uint8_t source_type = 0;
            uint64_t source_size = 0;
            const struct loaded_object *definition = NULL;
            uintptr_t source = resolve_library_global(
                libraries, library_count, strings + symbol->name,
                dynamic->string_size - symbol->name, &version,
                &source_type, &source_size, &definition);
            if (source == 0 || definition == NULL
                || source_type != STT_OBJECT || source_size < symbol->size
                || !object_range_has_flags(definition, source, copy_size,
                                           PF_R)) return 0;
            for (size_t byte = 0; byte < copy_size; byte++) {
                ((uint8_t *)target)[byte] = ((const uint8_t *)source)[byte];
            }
            continue;
        }
        if (type == R_X86_64_DTPMOD64 || type == R_X86_64_DTPOFF64
            || type == R_X86_64_TPOFF64 || type == R_X86_64_TLSDESC) {
            if (phase == RELOCATION_IFUNC) continue;
            struct tls_resolution resolution = {0};
            int found = 0;
            if (symbol_index == 0 && object->tls_module_id != 0) {
                resolution.thread_offset = object->tls_offset;
                resolution.module_id = object->tls_module_id;
                found = 1;
            } else if ((symbol->info & 15u) != STT_TLS) {
                return 0;
            } else if (symbol->section_index != 0
                && (binding == STB_LOCAL || visibility != STV_DEFAULT)) {
                if (object->tls_memory_size != 0
                    && symbol->value < object->tls_memory_size) {
                    resolution.thread_offset = object->tls_offset
                        + (int64_t)symbol->value;
                    resolution.module_id = object->tls_module_id;
                    resolution.module_offset = symbol->value;
                    found = 1;
                }
            } else {
                found = resolve_tls_global(
                    libraries, library_count,
                    strings + symbol->name,
                    dynamic->string_size - symbol->name,
                    &version, &resolution);
            }
            if (!found && binding != STB_WEAK) return 0;
            if (type == R_X86_64_TLSDESC) {
                uintptr_t *descriptor = (uintptr_t *)target;
                descriptor[0] = found ? (uintptr_t)&opos_tlsdesc_return
                                      : (uintptr_t)&opos_tlsdesc_undefweak;
                descriptor[1] = found
                    ? (uintptr_t)(resolution.thread_offset
                                  + relocations[i].addend)
                    : (uintptr_t)relocations[i].addend;
            } else if (type == R_X86_64_DTPMOD64) {
                *(uint64_t *)target = resolution.module_id
                    + relocations[i].addend;
            } else if (type == R_X86_64_DTPOFF64) {
                *(uint64_t *)target = resolution.module_offset
                    + relocations[i].addend;
            } else {
                *(int64_t *)target = resolution.thread_offset
                    + relocations[i].addend;
            }
            continue;
        }
        uintptr_t value = 0;
        uint8_t resolved_type = symbol->info & 15u;
        uint64_t resolved_size = symbol->size;
        if (symbol->section_index != 0
            && (binding == STB_LOCAL || visibility != STV_DEFAULT)) {
            if (!object_address(object, symbol->value, 1, &value)) return 0;
        } else {
            value = resolve_global(
                libraries, library_count,
                strings + symbol->name,
                dynamic->string_size - symbol->name, &version,
                &resolved_type, &resolved_size);
        }
        if (value == 0) {
            if (binding != STB_WEAK) return 0;
            if ((resolved_type == STT_GNU_IFUNC)
                    != (phase == RELOCATION_IFUNC)) continue;
            *(uint64_t *)target = relocations[i].addend;
            continue;
        }
        if (resolved_type == STT_GNU_IFUNC) {
            if (phase == RELOCATION_REGULAR) continue;
            if (!resolve_ifunc(value, libraries, library_count, &value)) {
                return 0;
            }
        } else if (phase == RELOCATION_IFUNC) {
            continue;
        }
        *(uint64_t *)target = value + relocations[i].addend;
    }
    return 1;
}

static int apply_relr_target(const struct loaded_object *object,
                             uintptr_t target) {
    if (!object_writable_range(object, target, sizeof(uint64_t))) return 0;
    *(uint64_t *)target += object->base;
    return 1;
}

static int apply_relr(const struct loaded_object *object) {
    const struct dynamic_info *dynamic = &object->dynamic;
    if (dynamic->relr_size == 0) return dynamic->relr == 0
        && (dynamic->relr_entry_size == 0
            || dynamic->relr_entry_size == sizeof(uint64_t));
    if (dynamic->relr == 0
        || dynamic->relr_entry_size != sizeof(uint64_t)
        || dynamic->relr_size % sizeof(uint64_t) != 0) return 0;
    uintptr_t table_address;
    if (!object_address(object, dynamic->relr, dynamic->relr_size,
                        &table_address)) return 0;
    const uint64_t *entries = (const uint64_t *)table_address;
    size_t entry_count = dynamic->relr_size / sizeof(uint64_t);
    uintptr_t cursor = 0;
    for (size_t i = 0; i < entry_count; i++) {
        uint64_t entry = entries[i];
        if ((entry & 1u) == 0) {
            uintptr_t target;
            if ((entry & (sizeof(uint64_t) - 1)) != 0
                || !object_address(object, entry, sizeof(uint64_t), &target)
                || !apply_relr_target(object, target)
                || target > UINTPTR_MAX - sizeof(uint64_t)) return 0;
            cursor = target + sizeof(uint64_t);
            continue;
        }
        if (cursor == 0 || cursor > UINTPTR_MAX - 62u * sizeof(uint64_t)) {
            return 0;
        }
        uint64_t bitmap = entry >> 1;
        for (size_t bit = 0; bit < 63; bit++) {
            if ((bitmap & (UINT64_C(1) << bit)) != 0
                && !apply_relr_target(object,
                    cursor + bit * sizeof(uint64_t))) return 0;
        }
        if (cursor > UINTPTR_MAX - 63u * sizeof(uint64_t)) return 0;
        cursor += 63u * sizeof(uint64_t);
    }
    return 1;
}

static int copy_library_name(const char *source, size_t limit,
                             char destination[MAX_LIBRARY_NAME + 1]) {
    size_t length = bounded_string_length(source, limit);
    if (length == 0 || length == limit || length > MAX_LIBRARY_NAME) return 0;
    for (size_t i = 0; i < length; i++) {
        if (source[i] == '/') return 0;
        destination[i] = source[i];
    }
    destination[length] = '\0';
    return 1;
}

static int object_name_equal(const struct loaded_object *object,
                             const char *name, size_t name_limit) {
    return bounded_strings_equal(object->name, sizeof(object->name),
                                 name, name_limit);
}

static int load_object(const char *name, size_t name_limit,
                       struct loaded_object *object) {
    if (!copy_library_name(name, name_limit, object->name)) return 0;
    char path[64] = {'/', 'l', 'i', 'b', '/'};
    size_t name_length = bounded_string_length(object->name, sizeof(object->name));
    for (size_t i = 0; i <= name_length; i++) path[5 + i] = object->name[i];
    long descriptor = linux_syscall6(2, (long)path, 0, 0, 0, 0, 0);
    if (descriptor < 0) return 0;
    long file_mapping = linux_syscall6(9, 0, LIBRARY_MAPPING_SIZE, 1,
                                       MAP_PRIVATE, descriptor, 0);
    (void)linux_syscall6(3, descriptor, 0, 0, 0, 0, 0);
    if (file_mapping < 0) return 0;
    const uint8_t *file = (const uint8_t *)(uintptr_t)file_mapping;
    const struct elf64_header *header = (const struct elf64_header *)file;
    if (header->identity[0] != 0x7f || header->identity[1] != 'E'
        || header->identity[2] != 'L' || header->identity[3] != 'F'
        || header->type != 3 || header->machine != 62
        || header->header_size != sizeof(*header)
        || header->program_header_size != sizeof(struct elf64_program_header)
        || header->program_header_count == 0 || header->program_header_count > 16
        || !mapped_range_valid(header->program_header_offset,
            (uint64_t)header->program_header_count
                * sizeof(struct elf64_program_header))) return 0;
    const struct elf64_program_header *phdr =
        (const struct elf64_program_header *)(file
                                              + header->program_header_offset);
    long image_mapping = linux_syscall6(
        9, 0, LIBRARY_MAPPING_SIZE, 3,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (image_mapping < 0) return 0;
    object->base = (uintptr_t)image_mapping;
    uint8_t *image = (uint8_t *)object->base;
    uint8_t mapped_pages[LIBRARY_MAPPING_SIZE / PAGE_SIZE] = {0};
    size_t executable_count = 0;
    size_t writable_count = 0;
    for (size_t i = 0; i < header->program_header_count; i++) {
        if (phdr[i].type != PT_LOAD) continue;
        if (object->segment_count == MAX_LOAD_SEGMENTS
            || phdr[i].memory_size == 0
            || phdr[i].memory_size < phdr[i].file_size
            || (phdr[i].flags != PF_R
                && phdr[i].flags != (PF_R | PF_X)
                && phdr[i].flags != (PF_R | PF_W))
            || (phdr[i].offset & (PAGE_SIZE - 1))
                != (phdr[i].virtual_address & (PAGE_SIZE - 1))
            || (phdr[i].alignment > 1
                && ((phdr[i].alignment & (phdr[i].alignment - 1)) != 0
                    || phdr[i].virtual_address % phdr[i].alignment
                        != phdr[i].offset % phdr[i].alignment))
            || !mapped_range_valid(phdr[i].offset, phdr[i].file_size)
            || !mapped_range_valid(phdr[i].virtual_address,
                                   phdr[i].memory_size)) return 0;
        uintptr_t start = object->base + phdr[i].virtual_address;
        uintptr_t end = start + phdr[i].memory_size;
        uintptr_t page_start = start & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t page_end = (end + PAGE_SIZE - 1)
            & ~(uintptr_t)(PAGE_SIZE - 1);
        if (end < start || page_end < end) return 0;
        size_t first_page = (page_start - object->base) / PAGE_SIZE;
        size_t page_count = (page_end - page_start) / PAGE_SIZE;
        for (size_t page = first_page; page < first_page + page_count; page++) {
            mapped_pages[page] = 1;
        }
        for (size_t j = 0; j < object->segment_count; j++) {
            uintptr_t other_page_start = object->segments[j].start
                & ~(uintptr_t)(PAGE_SIZE - 1);
            uintptr_t other_page_end = (object->segments[j].end
                                        + PAGE_SIZE - 1)
                & ~(uintptr_t)(PAGE_SIZE - 1);
            if (page_start < other_page_end
                && other_page_start < page_end) return 0;
        }
        struct object_segment *segment =
            &object->segments[object->segment_count++];
        segment->start = start;
        segment->end = end;
        segment->flags = phdr[i].flags;
        if (phdr[i].flags == (PF_R | PF_X)) {
            executable_count++;
            object->executable_start = start;
            object->executable_end = end;
        } else if (phdr[i].flags == (PF_R | PF_W)) {
            writable_count++;
            object->writable_start = start;
            object->writable_end = end;
        }
        for (size_t byte = 0; byte < (size_t)phdr[i].file_size; byte++) {
            image[phdr[i].virtual_address + byte] =
                file[phdr[i].offset + byte];
        }
    }
    if (object->segment_count < 2 || executable_count != 1
        || writable_count != 1) return 0;
    const struct elf64_dynamic *dynamic = NULL;
    size_t dynamic_count = 0;
    for (size_t i = 0; i < header->program_header_count; i++) {
        uintptr_t address;
        if (phdr[i].type == PT_DYNAMIC) {
            if (dynamic != NULL
                || phdr[i].memory_size < sizeof(struct elf64_dynamic)
                || phdr[i].memory_size / sizeof(struct elf64_dynamic) > 64
                || !object_address(object, phdr[i].virtual_address,
                                   (size_t)phdr[i].memory_size,
                                   &address)) return 0;
            dynamic = (const struct elf64_dynamic *)address;
            dynamic_count = (size_t)phdr[i].memory_size
                / sizeof(struct elf64_dynamic);
        } else if (phdr[i].type == PT_TLS) {
            uint64_t alignment = phdr[i].alignment == 0 ? 1 : phdr[i].alignment;
            if (object->tls_memory_size != 0
                || phdr[i].memory_size == 0
                || phdr[i].file_size > phdr[i].memory_size
                || alignment > PAGE_SIZE
                || (alignment & (alignment - 1)) != 0
                || !object_address(object, phdr[i].virtual_address,
                                   (size_t)phdr[i].memory_size,
                                   &address)
                || !object_writable_range(object, address,
                                          (size_t)phdr[i].memory_size)) {
                return 0;
            }
            object->tls_image = address;
            object->tls_file_size = phdr[i].file_size;
            object->tls_memory_size = phdr[i].memory_size;
            object->tls_alignment = alignment;
        } else if (phdr[i].type == PT_GNU_RELRO) {
            if (object->relro_end != 0 || phdr[i].memory_size == 0
                || !object_address(object, phdr[i].virtual_address,
                                   (size_t)phdr[i].memory_size,
                                   &address)
                || !object_writable_range(object, address,
                                          (size_t)phdr[i].memory_size)) {
                return 0;
            }
            object->relro_start = address;
            object->relro_end = address + phdr[i].memory_size;
        }
    }
    if (dynamic == NULL
        || !read_dynamic(dynamic, dynamic_count, &object->dynamic)
        || object->dynamic.string_table == 0
        || object->dynamic.symbol_table == 0
        || (object->dynamic.sysv_hash == 0
            && object->dynamic.gnu_hash == 0)) return 0;
    (void)linux_syscall6(11, file_mapping, LIBRARY_MAPPING_SIZE, 0, 0, 0, 0);
    for (size_t i = 0; i < object->segment_count; i++) {
        uintptr_t page_start = object->segments[i].start
            & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t page_end = (object->segments[i].end + PAGE_SIZE - 1)
            & ~(uintptr_t)(PAGE_SIZE - 1);
        long protection = 1;
        if ((object->segments[i].flags & PF_W) != 0) protection |= 2;
        if ((object->segments[i].flags & PF_X) != 0) protection |= 4;
        if (linux_syscall6(10, (long)page_start,
                           (long)(page_end - page_start), protection,
                           0, 0, 0) != 0) return 0;
    }
    for (size_t first = 0; first < LIBRARY_MAPPING_SIZE / PAGE_SIZE;) {
        if (mapped_pages[first] != 0) {
            first++;
            continue;
        }
        size_t end = first + 1;
        while (end < LIBRARY_MAPPING_SIZE / PAGE_SIZE
               && mapped_pages[end] == 0) end++;
        if (linux_syscall6(11, image_mapping + (long)(first * PAGE_SIZE),
                           (long)((end - first) * PAGE_SIZE),
                           0, 0, 0, 0) != 0) return 0;
        first = end;
    }
    return 1;
}

static int ensure_object(const char *name, size_t name_limit,
                         struct loaded_object objects[], size_t *object_count) {
    for (size_t i = 0; i < *object_count; i++) {
        if (object_name_equal(&objects[i], name, name_limit)) return 1;
    }
    if (*object_count == MAX_LIBRARIES
        || !load_object(name, name_limit, &objects[*object_count])) return 0;
    (*object_count)++;
    return 1;
}

static int place_tls_module(struct loaded_object *object, uint8_t *mapping,
                            size_t thread_pointer_offset, size_t *cursor) {
    if (object->tls_memory_size == 0) return 1;
    size_t alignment = (size_t)object->tls_alignment;
    size_t memory_size = (size_t)object->tls_memory_size;
    if (memory_size > *cursor) return 0;
    size_t start = (*cursor - memory_size) & ~(alignment - 1);
    if (start > *cursor || thread_pointer_offset - start > INT64_MAX) return 0;
    const uint8_t *source = (const uint8_t *)object->tls_image;
    for (size_t i = 0; i < (size_t)object->tls_file_size; i++) {
        mapping[start + i] = source[i];
    }
    object->tls_offset = -(int64_t)(thread_pointer_offset - start);
    object->tls_block = (uintptr_t)mapping + start;
    *cursor = start;
    return 1;
}

static int install_static_tls(void) {
    size_t maximum_alignment = loaded_main.tls_alignment == 0
        ? 1 : (size_t)loaded_main.tls_alignment;
    int has_tls = loaded_main.tls_memory_size != 0;
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (loaded_objects[i].tls_memory_size != 0) has_tls = 1;
        if (loaded_objects[i].tls_alignment > maximum_alignment) {
            maximum_alignment = (size_t)loaded_objects[i].tls_alignment;
        }
    }
    if (!has_tls) return 1;
    uint64_t next_module_id = 1;
    if (loaded_main.tls_memory_size != 0) {
        loaded_main.tls_module_id = next_module_id++;
    }
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (loaded_objects[i].tls_memory_size != 0) {
            loaded_objects[i].tls_module_id = next_module_id++;
        }
    }
    size_t thread_pointer_offset = (PAGE_SIZE - TLS_CONTROL_SIZE)
        & ~(maximum_alignment - 1);
    long mapped = linux_syscall6(9, 0, PAGE_SIZE, 3,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped < 0) return 0;
    uint8_t *mapping = (uint8_t *)(uintptr_t)mapped;
    size_t cursor = thread_pointer_offset;
    if (!place_tls_module(&loaded_main, mapping, thread_pointer_offset,
                          &cursor)) return 0;
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (!place_tls_module(&loaded_objects[i], mapping,
                              thread_pointer_offset, &cursor)) return 0;
    }
    uintptr_t thread_pointer = (uintptr_t)mapping + thread_pointer_offset;
    uintptr_t dtv = thread_pointer + TLS_DTV_OFFSET;
    *(uintptr_t *)(thread_pointer + 0) = thread_pointer;
    *(uintptr_t *)(thread_pointer + 8) = dtv;
    *(uintptr_t *)(thread_pointer + 16) = thread_pointer;
    *(uint64_t *)dtv = next_module_id - 1;
    if (loaded_main.tls_module_id != 0) {
        *(uintptr_t *)(dtv
            + loaded_main.tls_module_id * TLS_DTV_STRIDE) =
                loaded_main.tls_block;
    }
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (loaded_objects[i].tls_module_id != 0) {
            *(uintptr_t *)(dtv
                + loaded_objects[i].tls_module_id * TLS_DTV_STRIDE) =
                    loaded_objects[i].tls_block;
        }
    }
    return linux_syscall6(158, ARCH_SET_FS, (long)thread_pointer,
                          0, 0, 0, 0) == 0;
}

static int apply_object_relocations(struct loaded_object *object,
                                    const struct loaded_object libraries[],
                                    size_t library_count, int phase) {
    if (phase == RELOCATION_REGULAR && !apply_relr(object)) return 0;
    if (object->dynamic.rela_size != 0
        && !apply_relocations(object, libraries, library_count,
                              object->dynamic.rela,
                              object->dynamic.rela_size, phase)) return 0;
    return object->dynamic.jump_rela_size == 0
        || (object->dynamic.plt_relocation_type == DT_RELA
            && apply_relocations(object, libraries, library_count,
                                 object->dynamic.jump_rela,
                                 object->dynamic.jump_rela_size, phase));
}

static int seal_relro(const struct loaded_object *object) {
    if (object->relro_start == 0 && object->relro_end == 0) return 1;
    if (object->relro_start == 0
        || object->relro_end <= object->relro_start
        || !object_writable_range(object, object->relro_start,
                                  object->relro_end - object->relro_start)) {
        return 0;
    }
    uintptr_t page_start = object->relro_start
        & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t page_end = object->relro_end
        & ~(uintptr_t)(PAGE_SIZE - 1);
    if (page_end <= page_start) return 1;
    return linux_syscall6(10, (long)page_start,
                          (long)(page_end - page_start), 1,
                          0, 0, 0) == 0;
}

static int call_lifecycle_function(uintptr_t function,
                                   uintptr_t executable_start,
                                   uintptr_t executable_end) {
    if (!address_range_valid(executable_start, executable_end,
                             function, 1)) return 0;
    ((void (*)(void))function)();
    return 1;
}

static int call_lifecycle_array(uintptr_t base,
                                uintptr_t executable_start,
                                uintptr_t executable_end,
                                uintptr_t writable_start,
                                uintptr_t writable_end,
                                uint64_t array_address, uint64_t array_size,
                                int addresses_are_absolute, int reverse) {
    if (array_size == 0) return 1;
    if (array_address == 0 || array_size % sizeof(uintptr_t) != 0
        || array_size > 64u * sizeof(uintptr_t)
        || (!addresses_are_absolute
            && !mapped_range_valid(array_address, array_size))) return 0;
    uintptr_t address = addresses_are_absolute
        ? (uintptr_t)array_address : base + array_address;
    if (!address_range_valid(writable_start, writable_end,
                             address, (size_t)array_size)) return 0;
    const uintptr_t *functions = (const uintptr_t *)address;
    size_t count = (size_t)array_size / sizeof(uintptr_t);
    for (size_t i = 0; i < count; i++) {
        size_t index = reverse ? count - i - 1 : i;
        if (!call_lifecycle_function(functions[index], executable_start,
                                     executable_end)) return 0;
    }
    return 1;
}

static int call_initializers(uintptr_t base,
                             uintptr_t executable_start,
                             uintptr_t executable_end,
                             uintptr_t writable_start,
                             uintptr_t writable_end,
                             const struct dynamic_info *dynamic,
                             int addresses_are_absolute) {
    if (dynamic->init != 0) {
        if (!addresses_are_absolute
            && !mapped_range_valid(dynamic->init, 1)) return 0;
        uintptr_t function = addresses_are_absolute
            ? (uintptr_t)dynamic->init : base + dynamic->init;
        if (!call_lifecycle_function(function, executable_start,
                                     executable_end)) return 0;
    }
    return call_lifecycle_array(base, executable_start, executable_end,
                                writable_start, writable_end,
                                dynamic->init_array,
                                dynamic->init_array_size,
                                addresses_are_absolute, 0);
}

static int call_finalizers(uintptr_t base,
                           uintptr_t executable_start,
                           uintptr_t executable_end,
                           uintptr_t writable_start,
                           uintptr_t writable_end,
                           const struct dynamic_info *dynamic,
                           int addresses_are_absolute) {
    if (!call_lifecycle_array(base, executable_start, executable_end,
                              writable_start, writable_end,
                              dynamic->fini_array,
                              dynamic->fini_array_size,
                              addresses_are_absolute, 1)) return 0;
    if (dynamic->fini != 0) {
        if (!addresses_are_absolute
            && !mapped_range_valid(dynamic->fini, 1)) return 0;
        uintptr_t function = addresses_are_absolute
            ? (uintptr_t)dynamic->fini : base + dynamic->fini;
        if (!call_lifecycle_function(function, executable_start,
                                     executable_end)) return 0;
    }
    return 1;
}

static size_t find_loaded_object(const char *name, size_t name_limit) {
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (object_name_equal(&loaded_objects[i], name, name_limit)) return i;
    }
    return MAX_LIBRARIES;
}

static int initialize_object(size_t index) {
    if (index >= loaded_object_count) return 0;
    if (initialization_state[index] == 2) return 1;
    if (initialization_state[index] != 0) return 0;
    initialization_state[index] = 1;
    struct loaded_object *object = &loaded_objects[index];
    if (!mapped_range_valid(object->dynamic.string_table,
                            object->dynamic.string_size)) return 0;
    const char *strings = (const char *)(object->base
                                         + object->dynamic.string_table);
    for (size_t i = 0; i < object->dynamic.needed_count; i++) {
        uint64_t offset = object->dynamic.needed[i];
        if (offset >= object->dynamic.string_size) return 0;
        size_t dependency = find_loaded_object(
            strings + offset, object->dynamic.string_size - offset);
        if (dependency == MAX_LIBRARIES
            || !initialize_object(dependency)) return 0;
    }
    if (!call_initializers(object->base, object->executable_start,
                           object->executable_end, object->writable_start,
                           object->writable_end, &object->dynamic, 0)) return 0;
    initialization_state[index] = 2;
    initialization_order[initialization_count++] = (uint8_t)index;
    return 1;
}

void opos_dynamic_finalize(void) {
    if (finalized) return;
    finalized = 1;
    if (main_initialized) {
        (void)call_finalizers(loaded_main.base, loaded_main.executable_start,
                              loaded_main.executable_end,
                              loaded_main.writable_start,
                              loaded_main.writable_end,
                              &loaded_main.dynamic,
                              loaded_main.addresses_are_absolute);
    }
    for (size_t i = initialization_count; i != 0; i--) {
        struct loaded_object *object =
            &loaded_objects[initialization_order[i - 1]];
        (void)call_finalizers(object->base, object->executable_start,
                              object->executable_end, object->writable_start,
                              object->writable_end, &object->dynamic, 0);
    }
}

long opos_dynamic_link(uint64_t *initial_stack) {
    loaded_object_count = 0;
    initialization_count = 0;
    main_initialized = 0;
    finalized = 0;
    loaded_main = (struct loaded_object){0};
    for (size_t i = 0; i < MAX_LIBRARIES; i++) {
        loaded_objects[i] = (struct loaded_object){0};
        initialization_state[i] = 0;
        initialization_order[i] = 0;
    }
    uint64_t argument_count = initial_stack[0];
    uint64_t *cursor = initial_stack + 1 + argument_count + 1;
    while (*cursor != 0) cursor++;
    cursor++;
    uintptr_t main_phdr_address = 0;
    uint64_t main_phdr_count = 0;
    while (cursor[0] != 0) {
        if (cursor[0] == AT_PHDR) main_phdr_address = cursor[1];
        if (cursor[0] == AT_PHNUM) main_phdr_count = cursor[1];
        cursor += 2;
    }
    if (main_phdr_address == 0 || main_phdr_count == 0
        || main_phdr_count > 16) return 1;
    const struct elf64_program_header *main_phdr =
        (const struct elf64_program_header *)main_phdr_address;
    int main_phdr_segment_found = 0;
    for (size_t i = 0; i < main_phdr_count; i++) {
        if (main_phdr[i].type != PT_PHDR) continue;
        if (main_phdr_segment_found
            || main_phdr[i].virtual_address > main_phdr_address
            || main_phdr[i].memory_size
                < main_phdr_count * sizeof(struct elf64_program_header)) {
            return 1;
        }
        loaded_main.base = main_phdr_address
            - (uintptr_t)main_phdr[i].virtual_address;
        main_phdr_segment_found = 1;
    }
    loaded_main.addresses_are_absolute = loaded_main.base == 0;
    size_t main_executable_count = 0;
    size_t main_writable_count = 0;
    for (size_t i = 0; i < main_phdr_count; i++) {
        if (main_phdr[i].type != PT_LOAD) continue;
        if (loaded_main.segment_count == MAX_LOAD_SEGMENTS
            || main_phdr[i].memory_size == 0
            || (main_phdr[i].flags != PF_R
                && main_phdr[i].flags != (PF_R | PF_X)
                && main_phdr[i].flags != (PF_R | PF_W))
            || main_phdr[i].virtual_address > UINTPTR_MAX
                - main_phdr[i].memory_size) return 2;
        if (main_phdr[i].virtual_address > UINTPTR_MAX - loaded_main.base) {
            return 2;
        }
        uintptr_t start = loaded_main.base
            + (uintptr_t)main_phdr[i].virtual_address;
        uintptr_t end = start + (uintptr_t)main_phdr[i].memory_size;
        uintptr_t page_start = start & ~(uintptr_t)(PAGE_SIZE - 1);
        uintptr_t page_end = (end + PAGE_SIZE - 1)
            & ~(uintptr_t)(PAGE_SIZE - 1);
        if (page_end < end) return 2;
        for (size_t j = 0; j < loaded_main.segment_count; j++) {
            uintptr_t other_page_start = loaded_main.segments[j].start
                & ~(uintptr_t)(PAGE_SIZE - 1);
            uintptr_t other_page_end = (loaded_main.segments[j].end
                                        + PAGE_SIZE - 1)
                & ~(uintptr_t)(PAGE_SIZE - 1);
            if (page_start < other_page_end
                && other_page_start < page_end) return 2;
        }
        struct object_segment *segment =
            &loaded_main.segments[loaded_main.segment_count++];
        segment->start = start;
        segment->end = end;
        segment->flags = main_phdr[i].flags;
        if (main_phdr[i].flags == (PF_R | PF_X)) {
            main_executable_count++;
            loaded_main.executable_start = start;
            loaded_main.executable_end = end;
        } else if (main_phdr[i].flags == (PF_R | PF_W)) {
            main_writable_count++;
            loaded_main.writable_start = start;
            loaded_main.writable_end = end;
        }
    }
    if (loaded_main.segment_count < 2 || main_executable_count != 1
        || main_writable_count != 1) return 2;
    const struct elf64_dynamic *main_dynamic = NULL;
    size_t main_dynamic_count = 0;
    for (size_t i = 0; i < main_phdr_count; i++) {
        uintptr_t address;
        if (main_phdr[i].type == PT_DYNAMIC) {
            if (main_dynamic != NULL
                || main_phdr[i].memory_size < sizeof(struct elf64_dynamic)
                || main_phdr[i].memory_size / sizeof(struct elf64_dynamic) > 64
                || !object_address(&loaded_main,
                                   main_phdr[i].virtual_address,
                                   (size_t)main_phdr[i].memory_size,
                                   &address)) return 2;
            main_dynamic = (const struct elf64_dynamic *)address;
            main_dynamic_count = (size_t)main_phdr[i].memory_size
                / sizeof(struct elf64_dynamic);
        } else if (main_phdr[i].type == PT_TLS) {
            uint64_t alignment = main_phdr[i].alignment == 0
                ? 1 : main_phdr[i].alignment;
            if (loaded_main.tls_memory_size != 0
                || main_phdr[i].memory_size == 0
                || main_phdr[i].file_size > main_phdr[i].memory_size
                || alignment > PAGE_SIZE
                || (alignment & (alignment - 1)) != 0
                || !object_address(&loaded_main,
                                   main_phdr[i].virtual_address,
                                   (size_t)main_phdr[i].memory_size,
                                   &address)
                || !object_writable_range(&loaded_main, address,
                                          (size_t)main_phdr[i].memory_size)) {
                return 2;
            }
            loaded_main.tls_image = address;
            loaded_main.tls_file_size = main_phdr[i].file_size;
            loaded_main.tls_memory_size = main_phdr[i].memory_size;
            loaded_main.tls_alignment = alignment;
        } else if (main_phdr[i].type == PT_GNU_RELRO) {
            if (loaded_main.relro_end != 0 || main_phdr[i].memory_size == 0
                || !object_address(&loaded_main,
                                   main_phdr[i].virtual_address,
                                   (size_t)main_phdr[i].memory_size,
                                   &address)
                || !object_writable_range(&loaded_main, address,
                                          (size_t)main_phdr[i].memory_size)) {
                return 2;
            }
            loaded_main.relro_start = address;
            loaded_main.relro_end = address + main_phdr[i].memory_size;
        }
    }
    if (main_dynamic == NULL) return 2;
    struct dynamic_info main_info;
    if (!read_dynamic(main_dynamic, main_dynamic_count, &main_info)
        || main_info.needed_count == 0 || main_info.string_table == 0
        || main_info.symbol_table == 0
        || (main_info.sysv_hash == 0 && main_info.gnu_hash == 0)) return 3;
    loaded_main.dynamic = main_info;

    uintptr_t main_string_address;
    if (!object_address(&loaded_main, main_info.string_table,
                        main_info.string_size, &main_string_address)) return 3;
    const char *main_strings = (const char *)main_string_address;
    for (size_t i = 0; i < main_info.needed_count; i++) {
        if (main_info.needed[i] >= main_info.string_size
            || !ensure_object(main_strings + main_info.needed[i],
                              main_info.string_size - main_info.needed[i],
                              loaded_objects, &loaded_object_count)) return 4;
    }
    for (size_t object_index = 0; object_index < loaded_object_count;
         object_index++) {
        struct loaded_object *object = &loaded_objects[object_index];
        if (!mapped_range_valid(object->dynamic.string_table,
                                object->dynamic.string_size)) return 5;
        const char *strings = (const char *)(object->base
                                             + object->dynamic.string_table);
        for (size_t i = 0; i < object->dynamic.needed_count; i++) {
            uint64_t offset = object->dynamic.needed[i];
            if (offset >= object->dynamic.string_size
                || !ensure_object(strings + offset,
                                  object->dynamic.string_size - offset,
                                  loaded_objects, &loaded_object_count)) return 5;
        }
    }
    if (!install_static_tls()) return 6;
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (!apply_object_relocations(&loaded_objects[i], loaded_objects,
                                      loaded_object_count,
                                      RELOCATION_REGULAR)) return 7;
    }
    if (!apply_relr(&loaded_main)) return 8;
    if (main_info.rela_size != 0
        && !apply_relocations(&loaded_main, loaded_objects,
                              loaded_object_count, main_info.rela,
                              main_info.rela_size,
                              RELOCATION_REGULAR)) return 9;
    if (main_info.jump_rela_size != 0
        && (main_info.plt_relocation_type != DT_RELA
            || !apply_relocations(&loaded_main, loaded_objects,
                                  loaded_object_count,
                                  main_info.jump_rela,
                                  main_info.jump_rela_size,
                                  RELOCATION_REGULAR))) return 10;
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (!apply_object_relocations(&loaded_objects[i], loaded_objects,
                                      loaded_object_count,
                                      RELOCATION_IFUNC)) return 7;
    }
    if (main_info.rela_size != 0
        && !apply_relocations(&loaded_main, loaded_objects,
                              loaded_object_count, main_info.rela,
                              main_info.rela_size,
                              RELOCATION_IFUNC)) return 9;
    if (main_info.jump_rela_size != 0
        && (main_info.plt_relocation_type != DT_RELA
            || !apply_relocations(&loaded_main, loaded_objects,
                                  loaded_object_count,
                                  main_info.jump_rela,
                                  main_info.jump_rela_size,
                                  RELOCATION_IFUNC))) return 10;
    for (size_t i = 0; i < loaded_object_count; i++) {
        if (!seal_relro(&loaded_objects[i])) return 11;
    }
    if (!seal_relro(&loaded_main)) return 11;
    for (size_t i = 0; i < main_info.needed_count; i++) {
        size_t dependency = find_loaded_object(
            main_strings + main_info.needed[i],
            main_info.string_size - main_info.needed[i]);
        if (dependency == MAX_LIBRARIES
            || !initialize_object(dependency)) return 12;
    }
    if (!call_initializers(loaded_main.base, loaded_main.executable_start,
                           loaded_main.executable_end,
                           loaded_main.writable_start,
                           loaded_main.writable_end,
                           &loaded_main.dynamic,
                           loaded_main.addresses_are_absolute)) return 13;
    main_initialized = 1;
    return 0;
}
