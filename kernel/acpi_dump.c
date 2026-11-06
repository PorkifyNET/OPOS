/* Touchpad discovery report for the shell "touchpad" command.

   OPOS has no AML interpreter, so this reads the DSDT/SSDT bytes
   directly: it finds the Device blocks (and Scopes, for the path) by
   their package lengths, picks the HID-over-I2C devices (PNP0C50 /
   ACPI0C50) and the I2C host controllers, and decodes the resource
   descriptors inside them (I2cSerialBus, GpioInt, Interrupt, memory).
   Conditional resources in a _CRS method all show up, because nothing is
   evaluated. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi_dump.h"
#include "console.h"
#include "pci.h"
#include "power.h"

#define MAX_TABLES 32
#define MAX_BLOCKS 1024
#define NAME_LENGTH 40
#define HEADER_LENGTH 36

struct block {
    size_t start;   /* first byte of the opcode */
    size_t body;    /* first byte after the name */
    size_t end;     /* one past the last byte */
    char name[NAME_LENGTH];
    bool device;
};

static struct block blocks[MAX_BLOCKS];
static size_t block_count;

static void write_decimal(uint64_t value) {
    char digits[21];
    size_t length = 0;
    do {
        digits[length++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (length > 0) {
        console_write_bytes(&digits[--length], 1);
    }
}

static void write_hex_digits(uint64_t value, int digits) {
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        console_write_bytes(&hex[(value >> (i * 4)) & 0xf], 1);
    }
}

static uint16_t u16_at(const uint8_t *data) {
    return (uint16_t)(data[0] | data[1] << 8);
}

static uint32_t u32_at(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8
        | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static bool name_char(uint8_t c, bool first) {
    return (c >= 'A' && c <= 'Z') || c == '_' || (!first && c >= '0' && c <= '9');
}

/* PkgLength: returns its value and stores how many bytes it took. */
static bool package_length(const uint8_t *aml, size_t at, size_t end,
                           size_t *value, size_t *bytes) {
    if (at >= end) {
        return false;
    }
    uint8_t lead = aml[at];
    size_t follow = lead >> 6;
    if (at + 1 + follow > end) {
        return false;
    }
    if (follow == 0) {
        *value = lead & 0x3f;
    } else {
        size_t length = lead & 0x0f;
        for (size_t i = 0; i < follow; i++) {
            length |= (size_t)aml[at + 1 + i] << (4 + 8 * i);
        }
        *value = length;
    }
    *bytes = 1 + follow;
    return true;
}

/* NameString to dotted text ("\_SB.PCI0.I2C1"). Returns its length in
   bytes, or 0 when it is not a valid name. */
static size_t name_string(const uint8_t *aml, size_t at, size_t end,
                          char *out, size_t out_size) {
    size_t used = 0;
    size_t start = at;
    while (at < end && (aml[at] == '\\' || aml[at] == '^')) {
        if (used + 1 < out_size) {
            out[used++] = (char)aml[at];
        }
        at++;
    }
    size_t segments = 1;
    if (at < end && aml[at] == 0x2e) {
        segments = 2;
        at++;
    } else if (at + 1 < end && aml[at] == 0x2f) {
        segments = aml[at + 1];
        at += 2;
    } else if (at < end && aml[at] == 0x00) {
        out[used] = '\0';
        return at + 1 - start;
    }
    if (segments == 0 || segments > 8) {
        return 0;
    }
    for (size_t s = 0; s < segments; s++) {
        if (at + 4 > end || !name_char(aml[at], true)) {
            return 0;
        }
        if (s > 0 && used + 1 < out_size) {
            out[used++] = '.';
        }
        for (size_t i = 0; i < 4; i++) {
            if (!name_char(aml[at + i], false)) {
                return 0;
            }
            if (used + 1 < out_size) {
                out[used++] = (char)aml[at + i];
            }
        }
        at += 4;
    }
    out[used] = '\0';
    return at - start;
}

/* Finds DeviceOp (5B 82) and ScopeOp (10) blocks with valid names. */
static void find_blocks(const uint8_t *aml, size_t length) {
    block_count = 0;
    for (size_t i = HEADER_LENGTH; i + 6 < length && block_count < MAX_BLOCKS; i++) {
        bool device = aml[i] == 0x5b && aml[i + 1] == 0x82;
        bool scope = aml[i] == 0x10;
        if (!device && !scope) {
            continue;
        }
        size_t at = i + (device ? 2 : 1);
        size_t value;
        size_t bytes;
        if (!package_length(aml, at, length, &value, &bytes)
            || value < bytes + 1 || at + value > length) {
            continue;
        }
        struct block *b = &blocks[block_count];
        size_t name_bytes = name_string(aml, at + bytes, at + value,
                                        b->name, sizeof(b->name));
        if (name_bytes == 0 || b->name[0] == '\0') {
            continue;
        }
        b->start = i;
        b->body = at + bytes + name_bytes;
        b->end = at + value;
        b->device = device;
        block_count++;
    }
}

/* Innermost block that contains offset, or -1. */
static int container_of(size_t offset, int exclude) {
    int best = -1;
    for (size_t i = 0; i < block_count; i++) {
        if ((int)i == exclude || blocks[i].start >= offset
            || blocks[i].end <= offset) {
            continue;
        }
        if (best < 0 || blocks[i].end - blocks[i].start
                            < blocks[best].end - blocks[best].start) {
            best = (int)i;
        }
    }
    return best;
}

static void write_path(int index) {
    int chain[16];
    int depth = 0;
    for (int at = index; at >= 0 && depth < 16;) {
        chain[depth++] = at;
        if (blocks[at].name[0] == '\\') {
            break;
        }
        at = container_of(blocks[at].start, at);
    }
    for (int i = depth - 1; i >= 0; i--) {
        const char *name = blocks[chain[i]].name;
        if (i != depth - 1 && name[0] != '\\') {
            console_write_string(".");
        }
        console_write_string(name);
    }
}

static bool eisa_at(const uint8_t *aml, size_t i, size_t end) {
    return i + 5 <= end && aml[i] == 0x0c;
}

/* Compressed EISA ID (as AML stores it) to text, e.g. "PNP0C50". */
static void eisa_text(const uint8_t *bytes, char out[8]) {
    static const char hex[] = "0123456789ABCDEF";
    uint16_t vendor = (uint16_t)(bytes[0] << 8 | bytes[1]);
    out[0] = (char)('@' + ((vendor >> 10) & 0x1f));
    out[1] = (char)('@' + ((vendor >> 5) & 0x1f));
    out[2] = (char)('@' + (vendor & 0x1f));
    out[3] = hex[bytes[2] >> 4];
    out[4] = hex[bytes[2] & 0xf];
    out[5] = hex[bytes[3] >> 4];
    out[6] = hex[bytes[3] & 0xf];
    out[7] = '\0';
}

static bool string_is(const uint8_t *aml, size_t i, size_t end, const char *text) {
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

static bool is_i2c_hid_at(const uint8_t *aml, size_t i, size_t end) {
    return (eisa_at(aml, i, end) && aml[i + 1] == 0x41 && aml[i + 2] == 0xd0
            && aml[i + 3] == 0x0c && aml[i + 4] == 0x50)
        || string_is(aml, i, end, "PNP0C50")
        || string_is(aml, i, end, "ACPI0C50");
}

static bool name_at(const uint8_t *aml, size_t i, size_t end, const char *name) {
    return i + 4 <= end && aml[i] == (uint8_t)name[0]
        && aml[i + 1] == (uint8_t)name[1] && aml[i + 2] == (uint8_t)name[2]
        && aml[i + 3] == (uint8_t)name[3];
}

/* Prints the first string or EISA ID within 40 bytes after a name such
   as _HID (works for Name() and for a Method that returns a constant). */
static bool write_id_after(const uint8_t *aml, size_t i, size_t end,
                           char *copy, size_t copy_size) {
    for (size_t at = i + 4; at < i + 44 && at < end; at++) {
        if (aml[at] == 0x0d) {
            size_t length = 0;
            while (at + 1 + length < end && aml[at + 1 + length] >= 0x20
                   && aml[at + 1 + length] < 0x7f && length < 24) {
                length++;
            }
            if (length >= 3 && at + 1 + length < end && aml[at + 1 + length] == 0) {
                console_write_bytes((const char *)aml + at + 1, length);
                if (copy != NULL) {
                    size_t n = length < copy_size - 1 ? length : copy_size - 1;
                    for (size_t k = 0; k < n; k++) {
                        copy[k] = (char)aml[at + 1 + k];
                    }
                    copy[n] = '\0';
                }
                return true;
            }
        } else if (eisa_at(aml, at, end) && (aml[at + 1] & 0x80) == 0
                   && aml[at + 1] != 0) {
            char text[8];
            eisa_text(aml + at + 1, text);
            console_write_string(text);
            if (copy != NULL) {
                for (size_t k = 0; k < 8 && k < copy_size; k++) {
                    copy[k] = text[k];
                }
            }
            return true;
        }
    }
    return false;
}

static void write_ids(const uint8_t *aml, const struct block *b) {
    static const char *const ids[] = {"_HID", "_CID", "_UID", "_ADR", "_STA"};
    for (size_t n = 0; n < sizeof(ids) / sizeof(ids[0]); n++) {
        for (size_t i = b->body; i + 4 < b->end; i++) {
            if (!name_at(aml, i, b->end, ids[n])) {
                continue;
            }
            /* Skip names that belong to a child device. */
            int owner = container_of(i, -1);
            if (owner < 0 || &blocks[owner] != b) {
                continue;
            }
            console_write_string("  ");
            console_write_string(ids[n]);
            console_write_string(": ");
            /* MethodOp (14), PkgLength (1-4 bytes), then the name. */
            bool method = false;
            for (size_t back = 2; back <= 5 && back <= i; back++) {
                if (aml[i - back] == 0x14) {
                    method = true;
                }
            }
            if (n <= 1 && write_id_after(aml, i, b->end, NULL, 0)) {
                /* printed */
            } else if (i + 5 < b->end && aml[i - 1] == 0x08) {
                uint8_t op = aml[i + 4];
                if (op == 0x00 || op == 0x01) {
                    write_decimal(op);
                } else if (op == 0x0a) {
                    console_write_string("0x");
                    write_hex_digits(aml[i + 5], 2);
                } else if (op == 0x0b && i + 7 <= b->end) {
                    console_write_string("0x");
                    write_hex_digits(u16_at(aml + i + 5), 4);
                } else if (op == 0x0c && i + 9 <= b->end) {
                    console_write_string("0x");
                    write_hex_digits(u32_at(aml + i + 5), 8);
                } else {
                    console_write_string("(value)");
                }
            } else {
                console_write_string(method ? "(method)" : "(?)");
            }
            console_write_string("\n");
            break;
        }
    }
}

/* Resource source: the controller the resource belongs to. */
static void write_source(const uint8_t *aml, size_t at, size_t limit) {
    size_t length = 0;
    while (at + length < limit && aml[at + length] >= 0x20
           && aml[at + length] < 0x7f && length < 48) {
        length++;
    }
    if (length > 0) {
        console_write_string(" on ");
        console_write_bytes((const char *)aml + at, length);
    }
}

/* Decodes resource descriptors inside [start, end) and prints them. */
static void write_resources(const uint8_t *aml, int owner, size_t start, size_t end,
                            bool memory) {
    for (size_t i = start; i + 3 < end; i++) {
        uint8_t tag = aml[i];
        if (tag != 0x8e && tag != 0x8c && tag != 0x89 && tag != 0x86) {
            continue;
        }
        size_t length = u16_at(aml + i + 1);
        if (i + 3 + length > end) {
            continue;
        }
        /* owner >= 0: skip resources of child devices. */
        if (owner >= 0) {
            int device = container_of(i, -1);
            while (device >= 0 && !blocks[device].device) {
                device = container_of(blocks[device].start, device);
            }
            if (device != owner) {
                continue;
            }
        }
        const uint8_t *d = aml + i;
        if (tag == 0x8e && length >= 15 && d[5] == 1
            && u16_at(d + 10) >= 6) {
            uint32_t speed = u32_at(d + 12);
            if (speed < 10000 || speed > 10000000) {
                continue;
            }
            console_write_string("  I2C: address 0x");
            write_hex_digits(u16_at(d + 16), 2);
            console_write_string((u16_at(d + 7) & 1) ? " (10-bit)" : "");
            console_write_string(", ");
            write_decimal(speed / 1000);
            console_write_string(" kHz");
            write_source(aml, i + 18, i + 3 + length);
            console_write_string("\n");
            i += 2 + length;
        } else if (tag == 0x8c && length >= 20 && d[4] <= 1) {
            size_t pins = u16_at(d + 14);
            size_t name = u16_at(d + 17);
            if (pins == 0 || pins + 2 > 3 + length || name > 3 + length) {
                continue;
            }
            uint16_t flags = u16_at(d + 7);
            console_write_string(d[4] == 0 ? "  GpioInt: pin " : "  GpioIo: pin ");
            write_decimal(u16_at(d + pins));
            if (d[4] == 0) {
                console_write_string((flags & 1) ? ", edge" : ", level");
                uint16_t polarity = (flags >> 1) & 3;
                console_write_string(polarity == 0 ? " high"
                                     : polarity == 1 ? " low" : " both");
            }
            if (name != 0) {
                write_source(aml, i + name, i + 3 + length);
            }
            console_write_string("\n");
            i += 2 + length;
        } else if (tag == 0x89 && length >= 6 && d[4] >= 1
                   && 2 + (size_t)d[4] * 4 <= length) {
            console_write_string("  Interrupt: ");
            write_decimal(u32_at(d + 5));
            console_write_string((d[3] & 2) ? ", edge" : ", level");
            console_write_string((d[3] & 4) ? " low" : " high");
            console_write_string("\n");
            i += 2 + length;
        } else if (memory && tag == 0x86 && length == 9) {
            console_write_string("  Memory: 0x");
            write_hex_digits(u32_at(d + 4), 8);
            console_write_string(" size 0x");
            write_hex_digits(u32_at(d + 8), 8);
            console_write_string("\n");
            i += 2 + length;
        }
    }
}

/* The HID descriptor register comes from _DSM function 1; firmware
   usually returns it as a constant ("Return (0x20)"). Lists the small
   constants returned inside the device's _DSM. */
static void write_dsm_returns(const uint8_t *aml, const struct block *b) {
    for (size_t i = b->body; i + 4 < b->end; i++) {
        if (!name_at(aml, i, b->end, "_DSM")) {
            continue;
        }
        size_t value;
        size_t bytes;
        size_t method = i;
        while (method > b->body && aml[method] != 0x14 && i - method < 6) {
            method--;
        }
        if (aml[method] != 0x14
            || !package_length(aml, method + 1, b->end, &value, &bytes)) {
            continue;
        }
        size_t end = method + 1 + value;
        if (end > b->end) {
            end = b->end;
        }
        console_write_string("  _DSM returns:");
        size_t shown = 0;
        for (size_t at = i + 4; at + 1 < end && shown < 8; at++) {
            if (aml[at] != 0xa4) {
                continue;
            }
            uint8_t op = aml[at + 1];
            if (op == 0x0a && at + 2 < end) {
                console_write_string(" 0x");
                write_hex_digits(aml[at + 2], 2);
                shown++;
            } else if (op == 0x0b && at + 3 < end) {
                console_write_string(" 0x");
                write_hex_digits(u16_at(aml + at + 2), 4);
                shown++;
            } else if (op == 0x01) {
                console_write_string(" 1");
                shown++;
            }
        }
        if (shown == 0) {
            console_write_string(" (none constant)");
        }
        console_write_string("\n");
        return;
    }
}

static bool hid_is_i2c_controller(const char *hid) {
    static const char *const ids[] = {
        "80860F41", "808622C1", "INT33C2", "INT33C3", "INT3432", "INT3433",
        "INT3442", "INT3443", "INT3444", "INT3445", "INT3446", "INT3447",
        "AMDI0010", "AMDI0510", "AMD0010", "APMC0D0F", "HISI02A1",
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const char *a = hid;
        const char *b = ids[i];
        while (*b != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static void write_table_name(const uint8_t *aml) {
    console_write_bytes((const char *)aml, 4);
    console_write_string(" ");
    console_write_bytes((const char *)aml + 16, 8);
}

static void dump_table(const uint8_t *aml, size_t length) {
    find_blocks(aml, length);
    /* HID-over-I2C devices. */
    int shown[MAX_BLOCKS / 8];
    size_t shown_count = 0;
    for (size_t i = HEADER_LENGTH; i < length; i++) {
        if (!is_i2c_hid_at(aml, i, length)) {
            continue;
        }
        int device = container_of(i, -1);
        while (device >= 0 && !blocks[device].device) {
            device = container_of(blocks[device].start, device);
        }
        if (device < 0) {
            continue;
        }
        bool seen = false;
        for (size_t k = 0; k < shown_count; k++) {
            seen = seen || shown[k] == device;
        }
        if (seen || shown_count >= sizeof(shown) / sizeof(shown[0])) {
            continue;
        }
        shown[shown_count++] = device;
        console_write_string("I2C HID device ");
        write_path(device);
        console_write_string(" (");
        write_table_name(aml);
        console_write_string(")\n");
        write_ids(aml, &blocks[device]);
        write_resources(aml, -1, blocks[device].body, blocks[device].end, false);
        write_dsm_returns(aml, &blocks[device]);
    }
    /* I2C host controllers declared by _HID (ACPI-mode LPSS etc.). */
    for (size_t d = 0; d < block_count; d++) {
        const struct block *b = &blocks[d];
        if (!b->device) {
            continue;
        }
        for (size_t i = b->body; i + 4 < b->end; i++) {
            if (!name_at(aml, i, b->end, "_HID") || container_of(i, -1) != (int)d) {
                continue;
            }
            char hid[16] = {0};
            size_t at = i + 4;
            if (at < b->end && aml[at] == 0x0d) {
                size_t n = 0;
                while (n + 1 < sizeof(hid) && at + 1 + n < b->end
                       && aml[at + 1 + n] >= 0x20 && aml[at + 1 + n] < 0x7f) {
                    hid[n] = (char)aml[at + 1 + n];
                    n++;
                }
            }
            if (hid_is_i2c_controller(hid)) {
                console_write_string("I2C controller ");
                write_path((int)d);
                console_write_string(": ");
                console_write_string(hid);
                console_write_string("\n");
                write_resources(aml, (int)d, b->body, b->end, true);
            }
            break;
        }
    }
}

/* ------------------------------------------------- lookup for drivers */

static void append(char *out, size_t size, size_t *used, const char *text) {
    for (; *text != '\0' && *used + 1 < size; text++) {
        out[(*used)++] = *text;
    }
    out[*used] = '\0';
}

static void build_path(int index, char *out, size_t size) {
    int chain[16];
    int depth = 0;
    for (int at = index; at >= 0 && depth < 16;) {
        chain[depth++] = at;
        if (blocks[at].name[0] == '\\') {
            break;
        }
        at = container_of(blocks[at].start, at);
    }
    size_t used = 0;
    out[0] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        if (i != depth - 1) {
            append(out, size, &used, ".");
        }
        append(out, size, &used, blocks[chain[i]].name);
    }
}

/* Compares two ACPI paths, ignoring '\' and the '_' padding of names
   ("\_SB_.PCI0" equals "\_SB.PCI0"). */
static bool same_path(const char *a, const char *b) {
    char x[64];
    char y[64];
    const char *sources[2] = {a, b};
    char *targets[2] = {x, y};
    for (int n = 0; n < 2; n++) {
        const char *s = sources[n];
        size_t used = 0;
        while (*s != '\0' && used + 1 < sizeof(x)) {
            if (*s == '\\' || *s == '^') {
                s++;
                continue;
            }
            /* One segment: copy up to 4 chars, drop trailing '_'. */
            char segment[4];
            size_t length = 0;
            while (*s != '\0' && *s != '.' && length < 4) {
                segment[length++] = *s++;
            }
            while (length > 0 && segment[length - 1] == '_') {
                length--;
            }
            for (size_t i = 0; i < length && used + 1 < sizeof(x); i++) {
                targets[n][used++] = segment[i];
            }
            if (*s == '.') {
                s++;
                if (used + 1 < sizeof(x)) {
                    targets[n][used++] = '.';
                }
            }
        }
        targets[n][used] = '\0';
    }
    size_t i = 0;
    while (x[i] != '\0' && x[i] == y[i]) {
        i++;
    }
    return x[i] == y[i];
}

static bool integer_at(const uint8_t *aml, size_t at, size_t end,
                       uint32_t *value) {
    if (at >= end) {
        return false;
    }
    uint8_t op = aml[at];
    if (op == 0x00 || op == 0x01) {
        *value = op;
    } else if (op == 0xff) {
        *value = 0xffffffffu;
    } else if (op == 0x0a && at + 2 <= end) {
        *value = aml[at + 1];
    } else if (op == 0x0b && at + 3 <= end) {
        *value = u16_at(aml + at + 1);
    } else if (op == 0x0c && at + 5 <= end) {
        *value = u32_at(aml + at + 1);
    } else {
        return false;
    }
    return true;
}

static void fill_resources(const uint8_t *aml, const struct block *b,
                           struct acpi_i2c_hid *out) {
    for (size_t i = b->body; i + 3 < b->end; i++) {
        uint8_t tag = aml[i];
        size_t length = u16_at(aml + i + 1);
        if ((tag != 0x8e && tag != 0x89) || i + 3 + length > b->end) {
            continue;
        }
        const uint8_t *d = aml + i;
        if (tag == 0x8e && length >= 15 && d[5] == 1 && u16_at(d + 10) >= 6
            && !out->address_valid) {
            uint32_t speed = u32_at(d + 12);
            if (speed < 10000 || speed > 10000000) {
                continue;
            }
            out->address = u16_at(d + 16);
            out->speed = speed;
            out->address_valid = true;
            size_t used = 0;
            for (size_t at = i + 18; at < i + 3 + length && aml[at] >= 0x20
                 && aml[at] < 0x7f && used + 1 < sizeof(out->controller); at++) {
                out->controller[used++] = (char)aml[at];
            }
            out->controller[used] = '\0';
            i += 2 + length;
        } else if (tag == 0x89 && length >= 6 && d[4] >= 1 && !out->gsi_valid) {
            out->gsi = u32_at(d + 5);
            out->gsi_valid = true;
            out->level = (d[3] & 2) == 0;
            out->active_low = (d[3] & 4) != 0;
            i += 2 + length;
        }
    }
}

static uint16_t dsm_register(const uint8_t *aml, const struct block *b) {
    for (size_t i = b->body; i + 4 < b->end; i++) {
        if (!name_at(aml, i, b->end, "_DSM")) {
            continue;
        }
        for (size_t at = i + 4; at + 1 < b->end && at < i + 4096; at++) {
            uint32_t value;
            if (aml[at] == 0xa4 && integer_at(aml, at + 1, b->end, &value)
                && value != 0 && value <= 0xffff) {
                return (uint16_t)value;
            }
        }
    }
    return 0;
}

bool acpi_find_i2c_hid(struct acpi_i2c_hid *out) {
    static struct power_aml_table tables[MAX_TABLES];
    *out = (struct acpi_i2c_hid){0};
    size_t count = power_aml_tables(tables, MAX_TABLES);
    bool found = false;
    for (size_t t = 0; t < count && !found; t++) {
        const uint8_t *aml = tables[t].data;
        size_t length = tables[t].length;
        find_blocks(aml, length);
        for (size_t i = HEADER_LENGTH; i < length && !found; i++) {
            if (!is_i2c_hid_at(aml, i, length)) {
                continue;
            }
            int device = container_of(i, -1);
            while (device >= 0 && !blocks[device].device) {
                device = container_of(blocks[device].start, device);
            }
            if (device < 0) {
                continue;
            }
            const struct block *b = &blocks[device];
            fill_resources(aml, b, out);
            if (!out->address_valid) {
                *out = (struct acpi_i2c_hid){0};
                continue;
            }
            build_path(device, out->path, sizeof(out->path));
            for (size_t at = b->body; at + 4 < b->end; at++) {
                if (name_at(aml, at, b->end, "_HID")
                    && container_of(at, -1) == device) {
                    size_t used = 0;
                    if (aml[at + 4] == 0x0d) {
                        for (size_t k = at + 5; k < b->end && aml[k] >= 0x20
                             && aml[k] < 0x7f && used + 1 < sizeof(out->hid); k++) {
                            out->hid[used++] = (char)aml[k];
                        }
                    } else if (eisa_at(aml, at + 4, b->end)) {
                        eisa_text(aml + at + 5, out->hid);
                        used = 7;
                    }
                    out->hid[used] = '\0';
                    break;
                }
            }
            out->descriptor_register = dsm_register(aml, b);
            found = true;
        }
    }
    if (!found || out->controller[0] == '\0') {
        return found;
    }
    /* The controller's _ADR (PCI device << 16 | function). */
    for (size_t t = 0; t < count && !out->controller_adr_valid; t++) {
        const uint8_t *aml = tables[t].data;
        find_blocks(aml, tables[t].length);
        for (size_t d = 0; d < block_count && !out->controller_adr_valid; d++) {
            if (!blocks[d].device) {
                continue;
            }
            char path[64];
            build_path((int)d, path, sizeof(path));
            if (!same_path(path, out->controller)) {
                continue;
            }
            for (size_t at = blocks[d].body; at + 5 < blocks[d].end; at++) {
                uint32_t value;
                if (aml[at] == 0x08 && name_at(aml, at + 1, blocks[d].end, "_ADR")
                    && container_of(at, -1) == (int)d
                    && integer_at(aml, at + 5, blocks[d].end, &value)) {
                    out->controller_adr = value;
                    out->controller_adr_valid = true;
                    break;
                }
            }
        }
    }
    return true;
}

void acpi_dump_touchpad(void) {
    static struct power_aml_table tables[MAX_TABLES];
    size_t count = power_aml_tables(tables, MAX_TABLES);
    if (count == 0) {
        console_write_warning("No ACPI tables found.\n");
    }
    for (size_t t = 0; t < count; t++) {
        dump_table(tables[t].data, tables[t].length);
    }
}

void acpi_dump_pci(void) {
    static struct pci_function_info functions[96];
    size_t total = pci_list(functions, 96);
    size_t stored = total < 96 ? total : 96;
    for (size_t i = 0; i < stored; i++) {
        const struct pci_function_info *f = &functions[i];
        write_hex_digits(f->device.bus, 2);
        console_write_string(":");
        write_hex_digits(f->device.device, 2);
        console_write_string(".");
        write_hex_digits(f->device.function, 1);
        console_write_string(" ");
        write_hex_digits(f->device.vendor_id, 4);
        console_write_string(":");
        write_hex_digits(f->device.device_id, 4);
        console_write_string(" class ");
        write_hex_digits(f->class_code, 6);
        console_write_string((i % 2 == 1 || i + 1 == stored) ? "\n" : "   ");
    }
    if (total > stored) {
        console_write_string("(more functions not shown)\n");
    }
}
