#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "memory.h"
#include "paging.h"
#include "power.h"

#define ACPI_MAX_TABLE_SIZE (1024 * 1024)
#define ACPI_PM1_SLEEP_TYPE_MASK (7u << 10)
#define ACPI_PM1_SLEEP_ENABLE (1u << 13)

struct acpi_rsdp_v1 {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct acpi_rsdp_v2 {
    struct acpi_rsdp_v1 first;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_table_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* FADT fields used for reboot (ACPI 2.0+). */
#define ACPI_FADT_FLAGS_OFFSET 112
#define ACPI_FADT_RESET_REG_OFFSET 116
#define ACPI_FADT_RESET_VALUE_OFFSET 128
#define ACPI_FADT_RESET_REG_SUPPORTED (1u << 10)
#define ACPI_SPACE_MEMORY 0
#define ACPI_SPACE_IO 1
#define ACPI_SPACE_PCI_CONFIG 2
/* Free kernel window for a memory-mapped reset register. */
#define ACPI_RESET_MMIO_BASE 0xffffc00000100000ULL

static uint16_t pm1a_control_port;
static uint16_t pm1b_control_port;
static uint8_t sleep_type_a;
static uint8_t sleep_type_b;
static bool shutdown_available;

/* Kept for power_scan_input_devices. */
static const struct acpi_table_header *acpi_root;
static size_t acpi_entry_size;
static const struct acpi_table_header *acpi_fadt;

static bool reset_register_available;
static uint8_t reset_space;
static uint64_t reset_address;
static uint8_t reset_value;

static bool checksum_valid(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static uint32_t read_u32(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8
        | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint64_t read_u64(const uint8_t *data) {
    return (uint64_t)read_u32(data) | (uint64_t)read_u32(data + 4) << 32;
}

static const struct acpi_table_header *physical_table(uint64_t address) {
    const struct acpi_table_header *table = physical_page_to_hhdm(address);
    if (table == NULL || table->length < sizeof(*table)
        || table->length > ACPI_MAX_TABLE_SIZE
        || address > UINTPTR_MAX - table->length
        || !checksum_valid(table, table->length)) {
        return NULL;
    }
    return table;
}

static bool aml_integer(const uint8_t *aml, size_t end, size_t *offset,
                        uint64_t *value) {
    if (*offset >= end) {
        return false;
    }
    uint8_t opcode = aml[(*offset)++];
    if (opcode == 0x00) {
        *value = 0;
    } else if (opcode == 0x01) {
        *value = 1;
    } else if (opcode == 0xff) {
        *value = UINT64_MAX;
    } else {
        size_t length;
        if (opcode == 0x0a) {
            length = 1;
        } else if (opcode == 0x0b) {
            length = 2;
        } else if (opcode == 0x0c) {
            length = 4;
        } else if (opcode == 0x0e) {
            length = 8;
        } else {
            return false;
        }
        if (length > end - *offset) {
            return false;
        }
        *value = 0;
        for (size_t i = 0; i < length; i++) {
            *value |= (uint64_t)aml[(*offset)++] << (i * 8);
        }
    }
    return true;
}

static bool aml_package_length(const uint8_t *aml, size_t end, size_t *offset,
                               size_t *package_end) {
    if (*offset >= end) {
        return false;
    }
    uint8_t first = aml[(*offset)++];
    size_t follow_count = first >> 6;
    uint64_t length;
    if (follow_count == 0) {
        length = first & 0x3f;
    } else {
        length = first & 0x0f;
        for (size_t i = 0; i < follow_count; i++) {
            if (*offset >= end) {
                return false;
            }
            length |= (uint64_t)aml[(*offset)++] << (4 + 8 * i);
        }
    }
    size_t length_start = *offset - follow_count - 1;
    if (length > end - length_start) {
        return false;
    }
    *package_end = length_start + (size_t)length;
    return *package_end >= *offset;
}

static bool find_s5(const struct acpi_table_header *dsdt,
                    uint8_t *type_a, uint8_t *type_b) {
    const uint8_t *aml = (const uint8_t *)dsdt + sizeof(*dsdt);
    size_t aml_length = dsdt->length - sizeof(*dsdt);
    for (size_t i = 0; i + 6 <= aml_length; i++) {
        size_t name_offset = i + 1;
        if (aml[i] != 0x08) {
            continue;
        }
        if (aml[name_offset] == 0x5c) {
            name_offset++;
        }
        if (name_offset + 5 > aml_length
            || aml[name_offset] != '_' || aml[name_offset + 1] != 'S'
            || aml[name_offset + 2] != '5' || aml[name_offset + 3] != '_'
            || aml[name_offset + 4] != 0x12) {
            continue;
        }
        size_t offset = name_offset + 5;
        size_t package_end;
        if (!aml_package_length(aml, aml_length, &offset, &package_end)
            || offset >= package_end) {
            continue;
        }
        uint8_t element_count = aml[offset++];
        uint64_t first;
        uint64_t second;
        if (element_count < 2
            || !aml_integer(aml, package_end, &offset, &first)
            || !aml_integer(aml, package_end, &offset, &second)
            || first > 7 || second > 7) {
            continue;
        }
        *type_a = (uint8_t)first;
        *type_b = (uint8_t)second;
        return true;
    }
    return false;
}

static const struct acpi_table_header *find_fadt(
    const struct acpi_table_header *root, size_t entry_size) {
    size_t entries_length = root->length - sizeof(*root);
    if (entries_length % entry_size != 0) {
        return NULL;
    }
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);
    for (size_t offset = 0; offset < entries_length; offset += entry_size) {
        uint64_t address = entry_size == 8
            ? read_u64(entries + offset) : read_u32(entries + offset);
        const struct acpi_table_header *table = physical_table(address);
        if (table != NULL && table->signature[0] == 'F'
            && table->signature[1] == 'A' && table->signature[2] == 'C'
            && table->signature[3] == 'P') {
            return table;
        }
    }
    return NULL;
}

bool power_initialize(const void *rsdp_address) {
    shutdown_available = false;
    if (rsdp_address == NULL) {
        return false;
    }
    const struct acpi_rsdp_v1 *rsdp = rsdp_address;
    if (rsdp->signature[0] != 'R' || rsdp->signature[1] != 'S'
        || rsdp->signature[2] != 'D' || rsdp->signature[3] != ' '
        || rsdp->signature[4] != 'P' || rsdp->signature[5] != 'T'
        || rsdp->signature[6] != 'R' || rsdp->signature[7] != ' '
        || !checksum_valid(rsdp, sizeof(*rsdp))) {
        return false;
    }

    const struct acpi_table_header *root;
    size_t entry_size;
    if (rsdp->revision >= 2) {
        const struct acpi_rsdp_v2 *extended = rsdp_address;
        if (extended->length < sizeof(*extended)
            || extended->length > 4096
            || !checksum_valid(extended, extended->length)) {
            return false;
        }
        root = physical_table(extended->xsdt_address);
        entry_size = sizeof(uint64_t);
        if (root == NULL || root->signature[0] != 'X'
            || root->signature[1] != 'S' || root->signature[2] != 'D'
            || root->signature[3] != 'T') {
            return false;
        }
    } else {
        root = physical_table(rsdp->rsdt_address);
        entry_size = sizeof(uint32_t);
        if (root == NULL || root->signature[0] != 'R'
            || root->signature[1] != 'S' || root->signature[2] != 'D'
            || root->signature[3] != 'T') {
            return false;
        }
    }

    const struct acpi_table_header *fadt = find_fadt(root, entry_size);
    acpi_root = root;
    acpi_entry_size = entry_size;
    acpi_fadt = fadt;
    if (fadt == NULL || fadt->length < 90) {
        return false;
    }
    const uint8_t *fields = (const uint8_t *)fadt;
    reset_register_available = false;
    if (fadt->revision >= 2 && fadt->length > ACPI_FADT_RESET_VALUE_OFFSET
        && (read_u32(fields + ACPI_FADT_FLAGS_OFFSET)
            & ACPI_FADT_RESET_REG_SUPPORTED) != 0) {
        const uint8_t *reg = fields + ACPI_FADT_RESET_REG_OFFSET;
        reset_space = reg[0];
        reset_address = read_u64(reg + 4);
        reset_value = fields[ACPI_FADT_RESET_VALUE_OFFSET];
        reset_register_available = reset_address != 0
            && (reset_space == ACPI_SPACE_MEMORY
                || (reset_space == ACPI_SPACE_IO
                    && reset_address <= UINT16_MAX)
                || reset_space == ACPI_SPACE_PCI_CONFIG);
    }
    uint64_t dsdt_address = read_u32(fields + 40);
    uint32_t control_a = read_u32(fields + 64);
    uint32_t control_b = read_u32(fields + 68);
    uint8_t control_length = fields[89];
    if (fadt->length >= 148) {
        uint64_t extended_dsdt = read_u64(fields + 140);
        if (extended_dsdt != 0) {
            dsdt_address = extended_dsdt;
        }
    }
    if (control_a == 0 || control_a > UINT16_MAX
        || control_b > UINT16_MAX || control_length < 2 || dsdt_address == 0) {
        return false;
    }

    const struct acpi_table_header *dsdt = physical_table(dsdt_address);
    if (dsdt == NULL || dsdt->signature[0] != 'D'
        || dsdt->signature[1] != 'S' || dsdt->signature[2] != 'D'
        || dsdt->signature[3] != 'T') {
        return false;
    }
    if (!find_s5(dsdt, &sleep_type_a, &sleep_type_b)) {
        return false;
    }

    pm1a_control_port = control_a;
    pm1b_control_port = control_b;
    shutdown_available = true;
    return true;
}

bool power_shutdown(void) {
    if (!shutdown_available) {
        return false;
    }
    __asm__ volatile ("cli" : : : "memory");
    uint16_t value_a = (io_in16(pm1a_control_port)
        & (uint16_t)~(ACPI_PM1_SLEEP_TYPE_MASK | ACPI_PM1_SLEEP_ENABLE))
        | (uint16_t)(sleep_type_a << 10) | ACPI_PM1_SLEEP_ENABLE;
    io_out16(pm1a_control_port, value_a);
    if (pm1b_control_port != 0) {
        uint16_t value_b = (io_in16(pm1b_control_port)
            & (uint16_t)~(ACPI_PM1_SLEEP_TYPE_MASK | ACPI_PM1_SLEEP_ENABLE))
            | (uint16_t)(sleep_type_b << 10) | ACPI_PM1_SLEEP_ENABLE;
        io_out16(pm1b_control_port, value_b);
    }
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* Roughly count microseconds; port 0x80 reads take about 1 us on PCs. */
static void reboot_delay(uint32_t microseconds) {
    for (uint32_t i = 0; i < microseconds; i++) {
        io_in8(0x80);
    }
}

/* Writes the FADT RESET_VALUE to RESET_REG (memory, I/O or PCI bus 0). */
static void acpi_reset(void) {
    if (!reset_register_available) {
        return;
    }
    if (reset_space == ACPI_SPACE_IO) {
        io_out8((uint16_t)reset_address, reset_value);
    } else if (reset_space == ACPI_SPACE_MEMORY) {
        uint64_t page = reset_address & ~(uint64_t)0xfff;
        if (paging_map_page(ACPI_RESET_MMIO_BASE, page,
                            PAGING_WRITABLE | PAGING_CACHE_DISABLE
                            | PAGING_NO_EXECUTE)) {
            volatile uint8_t *reg = (volatile uint8_t *)(uintptr_t)
                (ACPI_RESET_MMIO_BASE + (reset_address & 0xfff));
            *reg = reset_value;
        }
    } else {
        /* Address: device in bits 32-47, function 16-31, offset 0-15. */
        uint32_t device = (uint32_t)(reset_address >> 32) & 0x1f;
        uint32_t function = (uint32_t)(reset_address >> 16) & 0x07;
        uint32_t offset = (uint32_t)reset_address & 0xff;
        io_out32(0xcf8, 0x80000000u | device << 11 | function << 8
                            | (offset & 0xfc));
        io_out8((uint16_t)(0xcfc + (offset & 3)), reset_value);
    }
    reboot_delay(100000);
}

/* Intel-style reset control register: system reset, then full reset. */
static void reset_control_register(void) {
    uint8_t value = io_in8(0xcf9) & (uint8_t)~0x0e;
    io_out8(0xcf9, value | 0x02);
    reboot_delay(50);
    io_out8(0xcf9, value | 0x0e);
    reboot_delay(100000);
}

/*
 * Tries, in order: the ACPI reset register, the 8042 keyboard controller,
 * the 0xCF9 reset control register, and a triple fault. Many UEFI laptops
 * have no working 8042 reset and need one of the others.
 */
void power_reboot(void) {
    __asm__ volatile ("cli" : : : "memory");
    acpi_reset();
    uint32_t wait_count = 0;
    while ((io_in8(0x64) & 0x02) != 0 && wait_count < 1000000) {
        wait_count++;
        __asm__ volatile ("pause");
    }
    if (wait_count < 1000000) {
        io_out8(0x64, 0xfe);
    }
    reboot_delay(100000);
    reset_control_register();
    struct {
        uint16_t limit;
        uintptr_t base;
    } __attribute__((packed)) invalid_idt = {0, 0};
    __asm__ volatile ("lidt %0; int3" : : "m"(invalid_idt) : "memory");
    __builtin_unreachable();
}

/* ------------------------------------------- input device discovery */

/* EISA IDs as AML stores them after a DWordPrefix (0x0C): the compressed
   vendor "PNP" is 0x41 0xD0, then the product number, high byte first. */
static bool eisa_id_at(const uint8_t *aml, size_t i, size_t end,
                       uint8_t product_high, int product_low) {
    return i + 5 <= end && aml[i] == 0x0c && aml[i + 1] == 0x41
        && aml[i + 2] == 0xd0 && aml[i + 3] == product_high
        && (product_low < 0 || aml[i + 4] == (uint8_t)product_low);
}

static bool string_at(const uint8_t *aml, size_t i, size_t end,
                      const char *text) {
    /* StringPrefix (0x0D), the text, then NUL. */
    if (i >= end || aml[i] != 0x0d) {
        return false;
    }
    size_t at = i + 1;
    for (; *text != '\0'; text++, at++) {
        if (at >= end || aml[at] != (uint8_t)*text) {
            return false;
        }
    }
    return at < end && aml[at] == 0;
}

static bool starts_with(const char *text, const char *prefix) {
    for (; *prefix != '\0'; prefix++, text++) {
        if (*text != *prefix) {
            return false;
        }
    }
    return true;
}

/* _HID strings of the common touchpad makers (PS/2 and I2C). */
static bool touchpad_vendor(const char *hid) {
    static const char *const prefixes[] = {
        "SYN", "ELAN", "ETD", "ALP", "DLL", "CYAP", "FTE", "MSFT0001",
        "ATML", "GXTP", "PIXA", "HTIX", "SPPT", "BLTP", "LEN0", "ACPI0C50",
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (starts_with(hid, prefixes[i])) {
            return true;
        }
    }
    return false;
}

static void note_hid(struct power_input_report *report, const char *hid) {
    for (size_t i = 0; i < report->touchpad_hid_count; i++) {
        size_t j = 0;
        while (j < sizeof(report->touchpad_hids[i]) && hid[j] != '\0'
               && report->touchpad_hids[i][j] == hid[j]) {
            j++;
        }
        if (hid[j] == '\0' && report->touchpad_hids[i][j] == '\0') {
            return;
        }
    }
    if (report->touchpad_hid_count < POWER_INPUT_MAX_HIDS) {
        char *slot = report->touchpad_hids[report->touchpad_hid_count++];
        size_t j = 0;
        for (; j + 1 < sizeof(report->touchpad_hids[0]) && hid[j] != '\0'; j++) {
            slot[j] = hid[j];
        }
        slot[j] = '\0';
    }
}

static void scan_table(const struct acpi_table_header *table,
                       struct power_input_report *report) {
    const uint8_t *aml = (const uint8_t *)table;
    size_t end = table->length;
    for (size_t i = sizeof(*table); i < end; i++) {
        if (eisa_id_at(aml, i, end, 0x0f, -1)
            || string_at(aml, i, end, "PNP0F13")) {
            report->ps2_mouse++;            /* PNP0Fxx: PS/2 mouse */
        } else if (eisa_id_at(aml, i, end, 0x03, -1)) {
            report->ps2_keyboard++;         /* PNP03xx: PS/2 keyboard */
        } else if (eisa_id_at(aml, i, end, 0x0c, 0x50)
                   || string_at(aml, i, end, "PNP0C50")
                   || string_at(aml, i, end, "ACPI0C50")) {
            report->i2c_hid++;              /* HID over I2C */
        }
        /* Name (_HID, "XXXX....") */
        if (i + 6 < end && aml[i] == 0x08 && aml[i + 1] == '_'
            && aml[i + 2] == 'H' && aml[i + 3] == 'I' && aml[i + 4] == 'D'
            && aml[i + 5] == 0x0d) {
            char hid[POWER_INPUT_HID_LENGTH];
            size_t length = 0;
            size_t at = i + 6;
            while (at < end && aml[at] >= 0x20 && aml[at] < 0x7f
                   && length + 1 < sizeof(hid)) {
                hid[length++] = (char)aml[at++];
            }
            hid[length] = '\0';
            if (at < end && aml[at] == 0 && length >= 7 && touchpad_vendor(hid)) {
                note_hid(report, hid);
            }
        }
    }
}

static bool signature_is(const struct acpi_table_header *table,
                         const char *signature) {
    return table->signature[0] == signature[0]
        && table->signature[1] == signature[1]
        && table->signature[2] == signature[2]
        && table->signature[3] == signature[3];
}

const uint8_t *power_find_table(const char *signature, size_t *length) {
    if (acpi_root == NULL) {
        return NULL;
    }
    size_t entries_length = acpi_root->length - sizeof(*acpi_root);
    const uint8_t *entries = (const uint8_t *)acpi_root + sizeof(*acpi_root);
    for (size_t offset = 0; offset + acpi_entry_size <= entries_length;
         offset += acpi_entry_size) {
        uint64_t address = acpi_entry_size == 8
            ? read_u64(entries + offset) : read_u32(entries + offset);
        const struct acpi_table_header *table = physical_table(address);
        if (table != NULL && signature_is(table, signature)) {
            *length = table->length;
            return (const uint8_t *)table;
        }
    }
    return NULL;
}

size_t power_aml_tables(struct power_aml_table *tables, size_t max) {
    size_t count = 0;
    if (acpi_root == NULL) {
        return 0;
    }
    if (acpi_fadt != NULL && acpi_fadt->length >= 44 && count < max) {
        const uint8_t *fields = (const uint8_t *)acpi_fadt;
        uint64_t dsdt_address = read_u32(fields + 40);
        if (acpi_fadt->length >= 148 && read_u64(fields + 140) != 0) {
            dsdt_address = read_u64(fields + 140);
        }
        const struct acpi_table_header *dsdt =
            dsdt_address != 0 ? physical_table(dsdt_address) : NULL;
        if (dsdt != NULL && signature_is(dsdt, "DSDT")) {
            tables[count++] = (struct power_aml_table){
                (const uint8_t *)dsdt, dsdt->length,
            };
        }
    }
    size_t entries_length = acpi_root->length - sizeof(*acpi_root);
    const uint8_t *entries = (const uint8_t *)acpi_root + sizeof(*acpi_root);
    for (size_t offset = 0; offset + acpi_entry_size <= entries_length
         && count < max; offset += acpi_entry_size) {
        uint64_t address = acpi_entry_size == 8
            ? read_u64(entries + offset) : read_u32(entries + offset);
        const struct acpi_table_header *table = physical_table(address);
        if (table != NULL && signature_is(table, "SSDT")) {
            tables[count++] = (struct power_aml_table){
                (const uint8_t *)table, table->length,
            };
        }
    }
    return count;
}

void power_scan_input_devices(struct power_input_report *report) {
    *report = (struct power_input_report){0};
    if (acpi_root == NULL) {
        return;
    }
    if (acpi_fadt != NULL) {
        const uint8_t *fields = (const uint8_t *)acpi_fadt;
        /* IAPC_BOOT_ARCH (ACPI 2.0+), bit 1: the board has an 8042. */
        if (acpi_fadt->revision >= 2 && acpi_fadt->length >= 111) {
            report->boot_flags_valid = true;
            report->has_8042 = (fields[109] & 0x02) != 0;
        }
        uint64_t dsdt_address = acpi_fadt->length >= 44 ? read_u32(fields + 40) : 0;
        if (acpi_fadt->length >= 148 && read_u64(fields + 140) != 0) {
            dsdt_address = read_u64(fields + 140);
        }
        const struct acpi_table_header *dsdt =
            dsdt_address != 0 ? physical_table(dsdt_address) : NULL;
        if (dsdt != NULL && signature_is(dsdt, "DSDT")) {
            scan_table(dsdt, report);
            report->tables_scanned++;
        }
    }
    size_t entries_length = acpi_root->length - sizeof(*acpi_root);
    const uint8_t *entries = (const uint8_t *)acpi_root + sizeof(*acpi_root);
    for (size_t offset = 0; offset + acpi_entry_size <= entries_length;
         offset += acpi_entry_size) {
        uint64_t address = acpi_entry_size == 8
            ? read_u64(entries + offset) : read_u32(entries + offset);
        const struct acpi_table_header *table = physical_table(address);
        if (table != NULL && signature_is(table, "SSDT")) {
            scan_table(table, report);
            report->tables_scanned++;
        }
    }
}
