#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "package.h"

/*
 * See package.h. Every rule here has a twin in sdk/tools/mkopk.py; change
 * both together. Text is checked as bytes, nothing is trusted to be
 * terminated, and every index is bounded by the text length.
 */

#define CAP_FILES_SHARED (1u << 2)
#define CAP_NET (1u << 3)
#define CAP_CLIPBOARD_READ (1u << 4)

/* ------------------------------------------------------------ helpers */

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static bool span_is(const char *data, size_t length, const char *word) {
    size_t i = 0;
    for (; i < length; i++) {
        if (word[i] == '\0' || data[i] != word[i]) {
            return false;
        }
    }
    return word[i] == '\0';
}

static bool digit(char c) {
    return c >= '0' && c <= '9';
}

static bool alnum_lower(char c) {
    return digit(c) || (c >= 'a' && c <= 'z');
}

static void set_reason(char *reason, const char *first, const char *data,
                       size_t data_length, const char *last) {
    if (reason == NULL) {
        return;
    }
    size_t used = 0;
    for (size_t i = 0; first[i] != '\0' && used < PACKAGE_REASON_MAX; i++) {
        reason[used++] = first[i];
    }
    if (data_length > 40) {
        data_length = 40;
    }
    for (size_t i = 0; i < data_length && used < PACKAGE_REASON_MAX; i++) {
        reason[used++] = data[i];
    }
    for (size_t i = 0; last[i] != '\0' && used < PACKAGE_REASON_MAX; i++) {
        reason[used++] = last[i];
    }
    reason[used] = '\0';
}

static enum package_result fail(char *reason, enum package_result result,
                                const char *first, const char *data,
                                size_t data_length, const char *last) {
    set_reason(reason, first, data, data_length, last);
    return result;
}

static bool copy_field(char *out, size_t capacity, const char *data,
                       size_t length) {
    if (length >= capacity) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        out[i] = data[i];
    }
    out[length] = '\0';
    return true;
}

/* ----------------------------------------------------------- versions */

bool package_version_valid(const char *text, size_t length) {
    if (length == 0 || length > PACKAGE_VERSION_MAX) {
        return false;
    }
    size_t parts = 1;
    size_t digits = 0;
    for (size_t i = 0; i < length; i++) {
        if (digit(text[i])) {
            if (++digits > 5) {
                return false;
            }
        } else if (text[i] == '.' && digits != 0 && parts < 4) {
            parts++;
            digits = 0;
        } else {
            return false;
        }
    }
    return digits != 0;
}

static void version_parts(const char *text, uint32_t parts[4]) {
    for (size_t i = 0; i < 4; i++) {
        parts[i] = 0;
    }
    size_t part = 0;
    for (size_t i = 0; text[i] != '\0' && part < 4; i++) {
        if (text[i] == '.') {
            part++;
        } else {
            parts[part] = parts[part] * 10 + (uint32_t)(text[i] - '0');
        }
    }
}

int package_compare_versions(const char *a, const char *b) {
    uint32_t first[4];
    uint32_t second[4];
    version_parts(a, first);
    version_parts(b, second);
    for (size_t i = 0; i < 4; i++) {
        if (first[i] != second[i]) {
            return first[i] < second[i] ? -1 : 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- id */

bool package_id_valid(const char *text, size_t length) {
    if (length == 0 || length > PACKAGE_ID_MAX) {
        return false;
    }
    size_t labels = 0;
    size_t start = 0;
    for (size_t i = 0; i <= length; i++) {
        if (i < length && text[i] != '.') {
            if (!alnum_lower(text[i]) && text[i] != '-') {
                return false;
            }
            continue;
        }
        /* A label: alphanumeric at both ends, '-' only inside. */
        if (i == start || !alnum_lower(text[start])
            || !alnum_lower(text[i - 1])) {
            return false;
        }
        labels++;
        start = i + 1;
    }
    return labels >= 2;
}

/* -------------------------------------------------------------- paths */

static bool path_character_bad(char c) {
    return c < 0x20 || c >= 0x7F || c == '"' || c == '*' || c == ':'
        || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|'
        || c == ' ';
}

static enum package_result check_path(const char *path, size_t length,
                                      char *reason) {
    if (length > PACKAGE_PATH_MAX) {
        return fail(reason, PACKAGE_BAD, "", path, length,
                    "...: path longer than 150 characters");
    }
    for (size_t i = 0; i < length; i++) {
        if (path_character_bad(path[i])) {
            return fail(reason, PACKAGE_BAD, "", path, length,
                        ": path has a character OPOS cannot store");
        }
    }
    if (length < 7 || !span_is(path, 6, "files/")) {
        return fail(reason, PACKAGE_BAD, "", path, length,
                    ": path must start with files/");
    }
    size_t start = 6;
    for (size_t i = 6; i <= length; i++) {
        if (i < length && path[i] != '/') {
            continue;
        }
        size_t part = i - start;
        if (part == 0 || (part == 1 && path[start] == '.')
            || (part == 2 && path[start] == '.' && path[start + 1] == '.')) {
            return fail(reason, PACKAGE_BAD, "", path, length,
                        ": empty, '.' or '..' path part");
        }
        if (path[i - 1] == '.') {
            return fail(reason, PACKAGE_BAD, "", path, length,
                        ": path part ends with '.'");
        }
        start = i + 1;
    }
    return PACKAGE_OK;
}

static bool same_folded(const char *a, size_t a_length, const char *b,
                        size_t b_length) {
    if (a_length != b_length) {
        return false;
    }
    for (size_t i = 0; i < a_length; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

/* True when folder (ignoring case) is a parent folder of path. */
static bool parent_of(const char *folder, size_t folder_length,
                      const char *path, size_t path_length) {
    return folder_length < path_length && path[folder_length] == '/'
        && same_folded(folder, folder_length, path, folder_length);
}

/* --------------------------------------------------------------- lines */

static int hex_value(char c) {
    if (digit(c)) {
        return c - '0';
    }
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* "SHA256 SIZE": 64 lowercase hex digits, a decimal without leading
   zeros that fits a FAT file. */
static bool parse_hash_size(const char *hash, size_t hash_length,
                            const char *size, size_t size_length,
                            struct package_file *file) {
    if (hash_length != 64 || size_length == 0 || size_length > 10
        || (size_length > 1 && size[0] == '0')) {
        return false;
    }
    for (size_t i = 0; i < 32; i++) {
        int high = hex_value(hash[i * 2]);
        int low = hex_value(hash[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        file->hash[i] = (uint8_t)(high << 4 | low);
    }
    uint64_t value = 0;
    for (size_t i = 0; i < size_length; i++) {
        if (!digit(size[i])) {
            return false;
        }
        value = value * 10 + (uint64_t)(size[i] - '0');
    }
    if (value > 0xFFFFFFFFull) {
        return false;
    }
    file->size = value;
    return true;
}

static enum package_result parse_capabilities(const char *text, size_t length,
                                              uint32_t *bits, char *reason) {
    *bits = 0;
    size_t i = 0;
    while (i < length) {
        while (i < length && (text[i] == ' ' || text[i] == ',')) {
            i++;
        }
        size_t start = i;
        while (i < length && text[i] != ' ' && text[i] != ',') {
            i++;
        }
        if (i == start) {
            break;
        }
        const char *name = text + start;
        size_t name_length = i - start;
        if (span_is(name, name_length, "files.shared")) {
            *bits |= CAP_FILES_SHARED;
        } else if (span_is(name, name_length, "net")) {
            *bits |= CAP_NET;
        } else if (span_is(name, name_length, "clipboard.read")) {
            *bits |= CAP_CLIPBOARD_READ;
        } else {
            return fail(reason, PACKAGE_BAD,
                        "capabilities: unknown capability '", name,
                        name_length, "'");
        }
    }
    return PACKAGE_OK;
}

enum single_key {
    KEY_FORMAT,
    KEY_ID,
    KEY_NAME,
    KEY_VERSION,
    KEY_PUBLISHER,
    KEY_PLATFORM,
    KEY_REQUIRES,
    KEY_CAPABILITIES,
    KEY_ORIGIN,
    SINGLE_KEYS,
};

static const char *const single_names[SINGLE_KEYS] = {
    "format", "id", "name", "version", "publisher", "platform",
    "requires-opos", "capabilities", "origin",
};

enum package_result package_parse(const char *text, size_t length,
                                  bool record, struct package *package,
                                  char *reason) {
    if (reason != NULL) {
        reason[0] = '\0';
    }
    if (text == NULL || package == NULL) {
        return PACKAGE_INVALID;
    }
    if (length > PACKAGE_TEXT_MAX + (record ? 32 : 0)) {
        return fail(reason, PACKAGE_BAD, "PACKAGE: larger than 65536 bytes",
                    "", 0, "");
    }
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (c != '\n' && (c < 0x20 || c >= 0x7F)) {
            return fail(reason, PACKAGE_BAD,
                        "PACKAGE: only printable ASCII is allowed", "", 0,
                        "");
        }
    }
    uint8_t *bytes = (uint8_t *)package;
    for (size_t i = 0; i < sizeof(*package); i++) {
        bytes[i] = 0;
    }
    struct package_span values[SINGLE_KEYS] = {{0, 0}};
    bool present[SINGLE_KEYS] = {false};
    size_t line_start = 0;
    while (line_start < length) {
        size_t line_end = line_start;
        while (line_end < length && text[line_end] != '\n') {
            line_end++;
        }
        const char *line = text + line_start;
        size_t line_length = line_end - line_start;
        size_t next = line_end + 1;
        bool blank = true;
        for (size_t i = 0; i < line_length; i++) {
            blank &= line[i] == ' ';
        }
        if (blank) {
            line_start = next;
            continue;
        }
        if (line[0] == ' ' || line[line_length - 1] == ' ') {
            return fail(reason, PACKAGE_BAD, "PACKAGE: '", line, line_length,
                        "': leading or trailing space");
        }
        size_t colon = 0;
        while (colon + 1 < line_length
               && !(line[colon] == ':' && line[colon + 1] == ' ')) {
            colon++;
        }
        if (colon + 1 >= line_length || colon + 2 >= line_length) {
            return fail(reason, PACKAGE_BAD, "PACKAGE: '", line, line_length,
                        "': expected 'key: value'");
        }
        const char *key = line;
        size_t key_length = colon;
        const char *value = line + colon + 2;
        size_t value_length = line_length - colon - 2;
        uint32_t value_offset = (uint32_t)(value - text);

        bool known = false;
        for (size_t k = 0; k < SINGLE_KEYS; k++) {
            if (!span_is(key, key_length, single_names[k])
                || (k == KEY_ORIGIN && !record)) {
                continue;
            }
            if (present[k]) {
                return fail(reason, PACKAGE_BAD, "PACKAGE: '", key,
                            key_length, "' given twice");
            }
            present[k] = true;
            values[k] = (struct package_span){value_offset,
                                              (uint32_t)value_length};
            known = true;
        }
        if (span_is(key, key_length, "file")) {
            known = true;
            if (package->file_count == PACKAGE_FILES_MAX) {
                return fail(reason, PACKAGE_BAD,
                            "PACKAGE: more than 512 files", "", 0, "");
            }
            struct package_file *file = &package->files[package->file_count];
            /* path SHA256 SIZE [exec] */
            const char *fields[5];
            size_t field_lengths[5];
            size_t count = 0;
            size_t start = 0;
            for (size_t i = 0; i <= value_length; i++) {
                if (i < value_length && value[i] != ' ') {
                    continue;
                }
                if (count == 5) {
                    count = 6;
                    break;
                }
                fields[count] = value + start;
                field_lengths[count++] = i - start;
                start = i + 1;
            }
            if (count >= 2 && span_is(fields[count - 1],
                                      field_lengths[count - 1], "exec")) {
                file->exec = true;
                count--;
            }
            if (count != 3
                || !parse_hash_size(fields[1], field_lengths[1], fields[2],
                                    field_lengths[2], file)) {
                return fail(reason, PACKAGE_BAD, "PACKAGE: '", line,
                            line_length,
                            "': expected 'file: files/NAME SHA256 SIZE'");
            }
            enum package_result result = check_path(fields[0],
                                                    field_lengths[0], reason);
            if (result != PACKAGE_OK) {
                return result;
            }
            for (size_t j = 0; j < package->file_count; j++) {
                const struct package_file *other = &package->files[j];
                if (same_folded(fields[0], field_lengths[0],
                                text + other->path.offset,
                                other->path.length)) {
                    return fail(reason, PACKAGE_BAD, "", fields[0],
                                field_lengths[0],
                                ": listed twice (names ignore case)");
                }
            }
            file->path = (struct package_span){
                (uint32_t)(fields[0] - text), (uint32_t)field_lengths[0]};
            package->total_size += file->size;
            package->file_count++;
        } else if (span_is(key, key_length, "start")) {
            known = true;
            if (package->start_count == PACKAGE_STARTS_MAX) {
                return fail(reason, PACKAGE_BAD,
                            "PACKAGE: more than 16 start lines", "", 0, "");
            }
            size_t space = 0;
            while (space < value_length && value[space] != ' ') {
                space++;
            }
            size_t label_length = space < value_length
                ? value_length - space - 1 : 0;
            if (label_length == 0 || label_length > PACKAGE_NAME_MAX) {
                return fail(reason, PACKAGE_BAD, "PACKAGE: '", line,
                            line_length,
                            "': start needs a label of 1 to 31 characters");
            }
            struct package_start *entry =
                &package->starts[package->start_count++];
            /* The path is resolved once every file is known; keep it in
               file for now as a span offset (fixed below). */
            entry->label = (struct package_span){
                (uint32_t)(value_offset + space + 1), (uint32_t)label_length};
            entry->file = (uint32_t)space; /* path length, for now */
        }
        if (!known) {
            return fail(reason, PACKAGE_UNSUPPORTED, "PACKAGE: unknown key '",
                        key, key_length, "' (needs a newer OPOS)");
        }
        line_start = next;
    }

    static const enum single_key required[] = {
        KEY_FORMAT, KEY_ID, KEY_NAME, KEY_VERSION, KEY_PUBLISHER,
        KEY_PLATFORM,
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!present[required[i]]) {
            return fail(reason, PACKAGE_BAD, "PACKAGE: '",
                        single_names[required[i]],
                        text_length(single_names[required[i]]),
                        "' is missing");
        }
    }
#define VALUE(k) (text + values[k].offset), (size_t)values[k].length
    if (!span_is(VALUE(KEY_FORMAT), "1")) {
        return fail(reason, PACKAGE_UNSUPPORTED, "format: ", VALUE(KEY_FORMAT),
                    " is not supported (needs a newer OPOS)");
    }
    if (!package_id_valid(VALUE(KEY_ID))) {
        return fail(reason, PACKAGE_BAD, "id: '", VALUE(KEY_ID),
                    "' must look like org.example.name");
    }
    (void)copy_field(package->id, sizeof(package->id), VALUE(KEY_ID));
    if (!copy_field(package->name, sizeof(package->name), VALUE(KEY_NAME))) {
        return fail(reason, PACKAGE_BAD, "name: longer than 31 characters",
                    "", 0, "");
    }
    if (!copy_field(package->publisher, sizeof(package->publisher),
                    VALUE(KEY_PUBLISHER))) {
        return fail(reason, PACKAGE_BAD,
                    "publisher: longer than 63 characters", "", 0, "");
    }
    if (!package_version_valid(VALUE(KEY_VERSION))) {
        return fail(reason, PACKAGE_BAD, "version: '", VALUE(KEY_VERSION),
                    "' is not 1 to 4 numbers separated by dots");
    }
    (void)copy_field(package->version, sizeof(package->version),
                     VALUE(KEY_VERSION));
    if (present[KEY_REQUIRES]) {
        if (!package_version_valid(VALUE(KEY_REQUIRES))) {
            return fail(reason, PACKAGE_BAD, "requires-opos: '",
                        VALUE(KEY_REQUIRES),
                        "' is not 1 to 4 numbers separated by dots");
        }
        (void)copy_field(package->requires_opos,
                         sizeof(package->requires_opos), VALUE(KEY_REQUIRES));
    }
    if (span_is(VALUE(KEY_PLATFORM), "opos")) {
        package->platform = PACKAGE_PLATFORM_OPOS;
    } else if (span_is(VALUE(KEY_PLATFORM), "linux")) {
        package->platform = PACKAGE_PLATFORM_LINUX;
    } else if (span_is(VALUE(KEY_PLATFORM), "windows")) {
        package->platform = PACKAGE_PLATFORM_WINDOWS;
    } else {
        return fail(reason, PACKAGE_BAD, "platform: '", VALUE(KEY_PLATFORM),
                    "' is not one of opos, linux, windows");
    }
    if (package->platform == PACKAGE_PLATFORM_OPOS) {
        if (present[KEY_CAPABILITIES]) {
            return fail(reason, PACKAGE_BAD,
                        "capabilities: an opos package declares them with "
                        "OPOS_MANIFEST", "", 0, "");
        }
    } else if (present[KEY_CAPABILITIES]) {
        enum package_result result = parse_capabilities(
            VALUE(KEY_CAPABILITIES), &package->capabilities, reason);
        if (result != PACKAGE_OK) {
            return result;
        }
    }
    if (record) {
        if (!present[KEY_ORIGIN]) {
            return fail(reason, PACKAGE_BAD, "record: no origin", "", 0, "");
        }
        if (span_is(VALUE(KEY_ORIGIN), "store")) {
            package->store = true;
        } else if (!span_is(VALUE(KEY_ORIGIN), "sideloaded")) {
            return fail(reason, PACKAGE_BAD, "record: unknown origin", "", 0,
                        "");
        }
    }
#undef VALUE
    if (package->file_count == 0) {
        return fail(reason, PACKAGE_BAD, "PACKAGE: no files", "", 0, "");
    }
    /* A name cannot be both a file and a folder. */
    for (size_t i = 0; i < package->file_count; i++) {
        const struct package_file *a = &package->files[i];
        for (size_t j = 0; j < package->file_count; j++) {
            const struct package_file *b = &package->files[j];
            if (parent_of(text + a->path.offset, a->path.length,
                          text + b->path.offset, b->path.length)) {
                return fail(reason, PACKAGE_BAD, "", text + a->path.offset,
                            a->path.length, ": both a file and a folder");
            }
        }
    }
    /* Start entries name a listed exec file. */
    for (size_t i = 0; i < package->start_count; i++) {
        struct package_start *entry = &package->starts[i];
        size_t path_length = entry->file;
        const char *path = text + entry->label.offset - path_length - 1;
        long found = package_find_file(package, text, path, path_length);
        if (found < 0 || !package->files[found].exec) {
            return fail(reason, PACKAGE_BAD, "start: ", path, path_length,
                        " is not a listed exec file");
        }
        entry->file = (uint32_t)found;
    }
    return PACKAGE_OK;
}

long package_find_file(const struct package *package, const char *text,
                       const char *name, size_t length) {
    for (size_t i = 0; i < package->file_count; i++) {
        const struct package_file *file = &package->files[i];
        if (file->path.length != length) {
            continue;
        }
        bool same = true;
        for (size_t j = 0; j < length && same; j++) {
            same = text[file->path.offset + j] == name[j];
        }
        if (same) {
            return (long)i;
        }
    }
    return -1;
}

/* -------------------------------------------------------------- ustar */

/* An octal header field: optional leading spaces, digits, then NUL or
   spaces to the end. Empty means 0 when allow_empty. */
static bool octal_field(const uint8_t *field, size_t width, bool allow_empty,
                        uint64_t *value) {
    size_t i = 0;
    while (i < width && field[i] == ' ') {
        i++;
    }
    uint64_t result = 0;
    size_t digits = 0;
    while (i < width && field[i] >= '0' && field[i] <= '7') {
        result = result * 8 + (uint64_t)(field[i] - '0');
        digits++;
        i++;
    }
    while (i < width && (field[i] == ' ' || field[i] == '\0')) {
        i++;
    }
    if (i != width || (digits == 0 && !allow_empty) || digits > 11) {
        return false;
    }
    *value = result;
    return true;
}

bool package_ustar_header(const uint8_t block[512], char *name,
                          uint64_t *size, bool *end, char *reason) {
    *end = false;
    bool zero = true;
    for (size_t i = 0; i < 512 && zero; i++) {
        zero = block[i] == 0;
    }
    if (zero) {
        *end = true;
        return true;
    }
    static const char magic[] = "ustar";
    for (size_t i = 0; i < 6; i++) {
        if (block[257 + i] != (uint8_t)magic[i]) {
            set_reason(reason, "archive: not a POSIX ustar archive", "", 0,
                       "");
            return false;
        }
    }
    if (block[263] != '0' || block[264] != '0') {
        set_reason(reason, "archive: not a POSIX ustar archive", "", 0, "");
        return false;
    }
    uint64_t stored;
    uint64_t sum = 0;
    for (size_t i = 0; i < 512; i++) {
        sum += i >= 148 && i < 156 ? ' ' : block[i];
    }
    if (!octal_field(block + 148, 8, false, &stored) || stored != sum) {
        set_reason(reason, "archive: header checksum is wrong", "", 0, "");
        return false;
    }
    if (!octal_field(block + 124, 12, true, size)) {
        set_reason(reason, "archive: header field is not octal", "", 0, "");
        return false;
    }
    size_t used = 0;
    size_t prefix_length = 0;
    while (prefix_length < 155 && block[345 + prefix_length] != 0) {
        prefix_length++;
    }
    size_t name_length = 0;
    while (name_length < 100 && block[name_length] != 0) {
        name_length++;
    }
    if (prefix_length + name_length + 1 > PACKAGE_PATH_MAX) {
        set_reason(reason, "archive: member path longer than 150 characters",
                   "", 0, "");
        return false;
    }
    for (size_t i = 0; i < prefix_length; i++) {
        name[used++] = (char)block[345 + i];
    }
    if (prefix_length != 0) {
        name[used++] = '/';
    }
    for (size_t i = 0; i < name_length; i++) {
        name[used++] = (char)block[i];
    }
    name[used] = '\0';
    for (size_t i = 0; i < used; i++) {
        if ((uint8_t)name[i] >= 0x80) {
            set_reason(reason, "archive: member name is not ASCII", "", 0, "");
            return false;
        }
    }
    uint8_t kind = block[156];
    if (kind != '0' && kind != 0) {
        const char *what = kind == '1' ? ": hard link entries are not allowed"
            : kind == '2' ? ": symbolic link entries are not allowed"
            : kind == '5' ? ": folder entries are not allowed"
                          : ": special file entries are not allowed";
        set_reason(reason, "", name, used, what);
        return false;
    }
    return true;
}
