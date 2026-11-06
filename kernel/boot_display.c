#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "boot_display.h"
#include "fat16.h"
#include "installer.h"
#include "permissions.h"
#include "vfs.h"

#define BOOT_DISPLAY_MAX_MODES 48
#define BOOT_DISPLAY_MIN_WIDTH 640
#define BOOT_DISPLAY_MIN_HEIGHT 480
#define CONFIG_CAPACITY 16384
/* Room for one added "resolution: WxH" line per OPOS entry. */
#define CONFIG_OUTPUT_CAPACITY (CONFIG_CAPACITY + 2048)
#define CONFIG_PATH_CAPACITY 64

struct mode_size {
    uint32_t width;
    uint32_t height;
};

static struct mode_size modes[BOOT_DISPLAY_MAX_MODES];
static size_t mode_count;
static struct mode_size running;

static bool configured_loaded;
static bool configured_found;
static struct mode_size configured;

static char config_text[CONFIG_CAPACITY];
static char config_output[CONFIG_OUTPUT_CAPACITY];

/* Where OPOS puts limine.conf: the system volume's boot folder, and the
   \EFI\limine folder of an EFI system partition (GPT partition installs). */
static const char *const volume_names[] = {
    "/disk", "/disk2", "/disk3", "/disk4", "/disk5", "/disk6", "/disk7",
    "/disk8",
};
#define VOLUME_NAME_COUNT (sizeof(volume_names) / sizeof(volume_names[0]))

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static bool mode_less(struct mode_size a, struct mode_size b) {
    return a.width != b.width ? a.width < b.width : a.height < b.height;
}

static void insert_mode(uint32_t width, uint32_t height) {
    struct mode_size mode = {width, height};
    size_t at = 0;
    while (at < mode_count && mode_less(modes[at], mode)) {
        at++;
    }
    if (at < mode_count && modes[at].width == width
        && modes[at].height == height) {
        return;
    }
    if (mode_count == BOOT_DISPLAY_MAX_MODES) {
        return;
    }
    for (size_t i = mode_count; i > at; i--) {
        modes[i] = modes[i - 1];
    }
    modes[at] = mode;
    mode_count++;
}

void boot_display_add_mode(uint32_t width, uint32_t height) {
    if (width >= BOOT_DISPLAY_MIN_WIDTH && height >= BOOT_DISPLAY_MIN_HEIGHT
        && width <= 16384 && height <= 16384) {
        insert_mode(width, height);
    }
}

void boot_display_set_running(uint32_t width, uint32_t height) {
    running.width = width;
    running.height = height;
    insert_mode(width, height);
}

size_t boot_display_mode_count(void) {
    return mode_count;
}

bool boot_display_mode_size(size_t index, uint32_t *width, uint32_t *height) {
    if (index >= mode_count) {
        return false;
    }
    *width = modes[index].width;
    *height = modes[index].height;
    return true;
}

void boot_display_running(uint32_t *width, uint32_t *height) {
    *width = running.width;
    *height = running.height;
}

/* ------------------------------------------------------ limine.conf text */

static char lower(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool blank(char c) {
    return c == ' ' || c == '\t';
}

/* One line: [start, content_end) without the terminator, next = after it. */
struct line {
    size_t start;
    size_t content_end;
    size_t next;
};

static struct line line_at(const char *text, size_t length, size_t start) {
    struct line line = {start, start, start};
    while (line.content_end < length && text[line.content_end] != '\n') {
        line.content_end++;
    }
    line.next = line.content_end < length ? line.content_end + 1
                                          : line.content_end;
    if (line.content_end > start && text[line.content_end - 1] == '\r') {
        line.content_end--;
    }
    return line;
}

static size_t skip_blanks(const char *text, size_t at, size_t end) {
    while (at < end && blank(text[at])) {
        at++;
    }
    return at;
}

/* Entry headers start with '/' (sub-entries with "//"). */
static bool line_is_header(const char *text, struct line line) {
    size_t at = skip_blanks(text, line.start, line.content_end);
    return at < line.content_end && text[at] == '/';
}

/*
 * True for "key: value" lines with this key (case-insensitive). Sets
 * value_start/value_end to the value without surrounding blanks.
 */
static bool line_option(const char *text, struct line line, const char *key,
                        size_t *value_start, size_t *value_end) {
    size_t at = skip_blanks(text, line.start, line.content_end);
    size_t key_length = text_length(key);
    if (line.content_end - at < key_length) {
        return false;
    }
    for (size_t i = 0; i < key_length; i++) {
        if (lower(text[at + i]) != key[i]) {
            return false;
        }
    }
    at = skip_blanks(text, at + key_length, line.content_end);
    if (at >= line.content_end || text[at] != ':') {
        return false;
    }
    size_t start = skip_blanks(text, at + 1, line.content_end);
    size_t end = line.content_end;
    while (end > start && blank(text[end - 1])) {
        end--;
    }
    *value_start = start;
    *value_end = end;
    return true;
}

static bool value_is(const char *text, size_t start, size_t end,
                     const char *word) {
    size_t length = text_length(word);
    if (end - start != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (lower(text[start + i]) != word[i]) {
            return false;
        }
    }
    return true;
}

/* Whether the entry whose header line starts at `start` boots OPOS
   (protocol: limine), looking up to the next header. */
static bool entry_is_opos(const char *text, size_t length, size_t start) {
    struct line line = line_at(text, length, start);
    for (start = line.next; start < length; start = line.next) {
        line = line_at(text, length, start);
        if (line_is_header(text, line)) {
            return false;
        }
        size_t value_start;
        size_t value_end;
        if (line_option(text, line, "protocol", &value_start, &value_end)
            && value_is(text, value_start, value_end, "limine")) {
            return true;
        }
    }
    return false;
}

/* "1280x800" or "1280x800x32". */
static bool parse_size(const char *text, size_t start, size_t end,
                       struct mode_size *size) {
    uint32_t numbers[2] = {0, 0};
    size_t which = 0;
    size_t digits = 0;
    for (size_t i = start; i < end; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            if (numbers[which] > 100000) {
                return false;
            }
            numbers[which] = numbers[which] * 10 + (uint32_t)(c - '0');
            digits++;
        } else if (lower(c) == 'x' && digits != 0) {
            if (which == 1) {
                break; /* bits per pixel follow */
            }
            which++;
            digits = 0;
        } else {
            return false;
        }
    }
    if (which != 1 || numbers[0] == 0 || numbers[1] == 0) {
        return false;
    }
    size->width = numbers[0];
    size->height = numbers[1];
    return true;
}

/* Size of the first OPOS entry; 0x0 when it sets none. */
static bool config_size(const char *text, size_t length,
                        struct mode_size *size) {
    bool in_opos = false;
    bool found_entry = false;
    size->width = 0;
    size->height = 0;
    for (size_t start = 0; start < length;) {
        struct line line = line_at(text, length, start);
        if (line_is_header(text, line)) {
            if (found_entry) {
                return true;
            }
            in_opos = entry_is_opos(text, length, start);
            found_entry = in_opos;
        } else if (in_opos) {
            size_t value_start;
            size_t value_end;
            if (line_option(text, line, "resolution", &value_start,
                            &value_end)
                && !parse_size(text, value_start, value_end, size)) {
                size->width = 0;
                size->height = 0;
            }
        }
        start = line.next;
    }
    return found_entry;
}

static bool output_append(size_t *used, const char *text, size_t length) {
    if (length > CONFIG_OUTPUT_CAPACITY - *used) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        config_output[*used + i] = text[i];
    }
    *used += length;
    return true;
}

static size_t format_decimal(char *out, uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (size_t i = 0; i < count; i++) {
        out[i] = digits[count - 1 - i];
    }
    return count;
}

/*
 * Copies text into config_output with every OPOS entry's resolution lines
 * removed and, when size is not 0x0, "resolution: WxH" placed right after
 * its protocol line with the same indentation and line ending. Returns the
 * output length, or 0 when nothing fits or no OPOS entry exists.
 */
static size_t rewrite_config(const char *text, size_t length,
                             struct mode_size size) {
    char option[48];
    size_t option_length = 0;
    if (size.width != 0) {
        const char *key = "resolution: ";
        option_length = text_length(key);
        for (size_t i = 0; i < option_length; i++) {
            option[i] = key[i];
        }
        option_length += format_decimal(option + option_length, size.width);
        option[option_length++] = 'x';
        option_length += format_decimal(option + option_length, size.height);
    }
    size_t used = 0;
    bool in_opos = false;
    bool any_opos = false;
    for (size_t start = 0; start < length;) {
        struct line line = line_at(text, length, start);
        size_t value_start;
        size_t value_end;
        if (line_is_header(text, line)) {
            in_opos = entry_is_opos(text, length, start);
            any_opos |= in_opos;
        } else if (in_opos && line_option(text, line, "resolution",
                                          &value_start, &value_end)) {
            start = line.next;
            continue;
        }
        if (!output_append(&used, text + line.start, line.next - line.start)) {
            return 0;
        }
        if (in_opos && option_length != 0
            && line_option(text, line, "protocol", &value_start, &value_end)) {
            const char *ending = "\n";
            size_t ending_length = 1;
            if (line.next == line.content_end) {
                /* Last line without a terminator: end it first. */
                if (!output_append(&used, "\n", 1)) {
                    return 0;
                }
            } else {
                ending = text + line.content_end;
                ending_length = line.next - line.content_end;
            }
            size_t indent_end = skip_blanks(text, line.start, line.content_end);
            if (!output_append(&used, text + line.start,
                               indent_end - line.start)
                || !output_append(&used, option, option_length)
                || !output_append(&used, ending, ending_length)) {
                return 0;
            }
        }
        start = line.next;
    }
    return any_opos ? used : 0;
}

#define PREVIOUS_HEADER "/OPOS (previous version)"
#define KERNEL_TAIL "/boot/kernel.elf"
#define OLD_KERNEL_TAIL "/boot/old/kernel.elf"

/* Whether [start, end) contains `word` (case-insensitive); sets *at. */
static bool find_folded(const char *text, size_t start, size_t end,
                        const char *word, size_t *at) {
    size_t length = text_length(word);
    for (size_t i = start; i + length <= end; i++) {
        size_t j = 0;
        while (j < length && lower(text[i + j]) == lower(word[j])) {
            j++;
        }
        if (j == length) {
            *at = i;
            return true;
        }
    }
    return false;
}

/*
 * Copies text into config_output with a copy of the first OPOS entry that
 * boots .../boot/kernel.elf inserted after it, renamed PREVIOUS_HEADER
 * and booting .../boot/old/kernel.elf. Returns the output length, or 0
 * when there is no such entry or the copy is already there.
 */
static size_t add_previous_entry(const char *text, size_t length) {
    for (size_t start = 0; start < length;) {
        struct line line = line_at(text, length, start);
        size_t at;
        if (line_is_header(text, line)
            && find_folded(text, line.start, line.content_end,
                           PREVIOUS_HEADER, &at)) {
            return 0;
        }
        start = line.next;
    }
    /* The entry: [entry_start, entry_end). */
    size_t entry_start = length;
    size_t entry_end = length;
    for (size_t start = 0; start < length;) {
        struct line line = line_at(text, length, start);
        if (line_is_header(text, line)) {
            if (entry_start != length) {
                entry_end = start;
                break;
            }
            if (entry_is_opos(text, length, start)) {
                /* Only an entry whose path is the system kernel. */
                for (size_t s = line.next; s < length;) {
                    struct line option = line_at(text, length, s);
                    size_t value_start;
                    size_t value_end;
                    size_t found;
                    if (line_is_header(text, option)) {
                        break;
                    }
                    if ((line_option(text, option, "path", &value_start,
                                     &value_end)
                         || line_option(text, option, "kernel_path",
                                        &value_start, &value_end))
                        && find_folded(text, value_start, value_end,
                                       KERNEL_TAIL, &found)
                        && found + text_length(KERNEL_TAIL) == value_end) {
                        entry_start = start;
                        break;
                    }
                    s = option.next;
                }
            }
        }
        start = line.next;
    }
    if (entry_start == length) {
        return 0;
    }
    size_t used = 0;
    if (!output_append(&used, text, entry_end)) {
        return 0;
    }
    struct line header = line_at(text, length, entry_start);
    const char *ending = "\n";
    size_t ending_length = 1;
    if (header.next != header.content_end) {
        ending = text + header.content_end;
        ending_length = header.next - header.content_end;
    }
    if (used != 0 && config_output[used - 1] != '\n'
        && !output_append(&used, ending, ending_length)) {
        return 0;
    }
    size_t indent_end = skip_blanks(text, header.start, header.content_end);
    if (!output_append(&used, text + header.start, indent_end - header.start)
        || !output_append(&used, PREVIOUS_HEADER, text_length(PREVIOUS_HEADER))
        || !output_append(&used, ending, ending_length)) {
        return 0;
    }
    for (size_t s = header.next; s < entry_end;) {
        struct line option = line_at(text, length, s);
        size_t value_start;
        size_t value_end;
        size_t found;
        bool is_path = (line_option(text, option, "path", &value_start,
                                    &value_end)
                        || line_option(text, option, "kernel_path",
                                       &value_start, &value_end))
            && find_folded(text, value_start, value_end, KERNEL_TAIL, &found)
            && found + text_length(KERNEL_TAIL) == value_end;
        if (is_path) {
            if (!output_append(&used, text + option.start,
                               found - option.start)
                || !output_append(&used, OLD_KERNEL_TAIL,
                                  text_length(OLD_KERNEL_TAIL))
                || !output_append(&used, text + value_end,
                                  option.next - value_end)) {
                return 0;
            }
        } else if (!output_append(&used, text + option.start,
                                  option.next - option.start)) {
            return 0;
        }
        s = option.next;
    }
    if (used != 0 && config_output[used - 1] != '\n'
        && !output_append(&used, ending, ending_length)) {
        return 0;
    }
    if (!output_append(&used, text + entry_end, length - entry_end)) {
        return 0;
    }
    return used;
}

/* --------------------------------------------------------------- files */

static size_t config_path(size_t index, char *path) {
    size_t volume = index / 2;
    const char *name = volume_names[volume];
    const char *tail = index % 2 == 0 ? "/boot/limine/limine.conf"
                                      : "/EFI/limine/limine.conf";
    size_t length = 0;
    for (size_t i = 0; name[i] != '\0'; i++) {
        path[length++] = name[i];
    }
    for (size_t i = 0; tail[i] != '\0'; i++) {
        path[length++] = tail[i];
    }
    path[length] = '\0';
    return length;
}

/*
 * Reads candidate `index` into config_text. Only the system volume's boot
 * folder counts as "boot" (other volumes may hold other OPOS installs).
 */
static bool read_config(size_t index, size_t *length) {
    if (index % 2 == 0 && index != 0) {
        return false;
    }
    char path[CONFIG_PATH_CAPACITY];
    size_t path_length = config_path(index, path);
    size_t size;
    if (!vfs_file_size(path, path_length, &size) || size == 0
        || size > CONFIG_CAPACITY) {
        return false;
    }
    if (!vfs_read_file(path, path_length, (uint8_t *)config_text,
                       CONFIG_CAPACITY, length)
        || *length != size) {
        return false;
    }
    return true;
}

static bool installed(void) {
    return !installer_available() && fat16_system_mounted();
}

void boot_display_reload(void) {
    configured_loaded = true;
    configured_found = false;
    if (!installed()) {
        return;
    }
    for (size_t index = 0; index < VOLUME_NAME_COUNT * 2; index++) {
        size_t length;
        if (read_config(index, &length)
            && config_size(config_text, length, &configured)) {
            configured_found = true;
            return;
        }
    }
}

bool boot_display_configured(uint32_t *width, uint32_t *height) {
    if (!configured_loaded) {
        boot_display_reload();
    }
    *width = configured.width;
    *height = configured.height;
    return configured_found;
}

enum boot_display_result boot_display_configure(uint32_t width,
                                                uint32_t height) {
    if (!installed()) {
        return BOOT_DISPLAY_NOT_INSTALLED;
    }
    if (!permission_session_identity().admin) {
        return BOOT_DISPLAY_NOT_ADMIN;
    }
    struct mode_size size = {width, height};
    bool wrote = false;
    for (size_t index = 0; index < VOLUME_NAME_COUNT * 2; index++) {
        size_t length;
        if (!read_config(index, &length)) {
            continue;
        }
        size_t output_length = rewrite_config(config_text, length, size);
        if (output_length == 0) {
            continue;
        }
        char path[CONFIG_PATH_CAPACITY];
        size_t path_length = config_path(index, path);
        if (!vfs_write_file(path, path_length, (const uint8_t *)config_output,
                            output_length)) {
            boot_display_reload();
            return BOOT_DISPLAY_WRITE_FAILED;
        }
        wrote = true;
    }
    boot_display_reload();
    return wrote ? BOOT_DISPLAY_OK : BOOT_DISPLAY_NOT_INSTALLED;
}

bool boot_display_add_previous_entry(void) {
    if (!installed()) {
        return false;
    }
    bool any = false;
    bool failed = false;
    for (size_t index = 0; index < VOLUME_NAME_COUNT * 2; index++) {
        size_t length;
        if (!read_config(index, &length)) {
            continue;
        }
        size_t at;
        bool present = false;
        for (size_t start = 0; start < length;) {
            struct line line = line_at(config_text, length, start);
            if (line_is_header(config_text, line)
                && find_folded(config_text, line.start, line.content_end,
                               PREVIOUS_HEADER, &at)) {
                present = true;
                break;
            }
            start = line.next;
        }
        if (present) {
            any = true;
            continue;
        }
        size_t output_length = add_previous_entry(config_text, length);
        if (output_length == 0) {
            continue;
        }
        char path[CONFIG_PATH_CAPACITY];
        size_t path_length = config_path(index, path);
        if (vfs_write_file(path, path_length, (const uint8_t *)config_output,
                           output_length)) {
            any = true;
        } else {
            failed = true;
        }
    }
    boot_display_reload();
    return any && !failed;
}
