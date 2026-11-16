#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat16.h"
#include "installer.h"
#include "recovery.h"
#include "vfs.h"

/*
 * A start counts as unfinished from the moment the boot volume is mounted
 * until recovery_start_finished(). The count is decimal text, so a person
 * can read or clear it from another system.
 */
#define ATTEMPTS_PATH "/disk/OPOS/BOOT-ATTEMPTS"
#define ATTEMPTS_PATH_LENGTH (sizeof(ATTEMPTS_PATH) - 1)

static enum recovery_reason current_reason = RECOVERY_NONE;
static uint32_t failed_starts;
static bool counted;

static bool installed(void) {
    return !installer_available() && fat16_system_mounted();
}

static void write_count(uint32_t count) {
    char text[12];
    size_t length = 0;
    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + count % 10);
        count /= 10;
    } while (count != 0 && digit_count < sizeof(digits));
    while (digit_count != 0) {
        text[length++] = digits[--digit_count];
    }
    text[length++] = '\n';
    (void)vfs_write_file(ATTEMPTS_PATH, ATTEMPTS_PATH_LENGTH,
                         (const uint8_t *)text, length);
}

uint32_t recovery_count_start(void) {
    if (counted || !installed()) {
        return failed_starts;
    }
    counted = true;
    char text[12];
    size_t length = 0;
    uint32_t count = 0;
    if (vfs_read_file(ATTEMPTS_PATH, ATTEMPTS_PATH_LENGTH, (uint8_t *)text,
                      sizeof(text), &length)) {
        for (size_t i = 0; i < length && text[i] >= '0' && text[i] <= '9'
                 && count < 100000; i++) {
            count = count * 10 + (uint32_t)(text[i] - '0');
        }
    }
    failed_starts = count;
    write_count(count + 1);
    return failed_starts;
}

void recovery_start_finished(void) {
    if (!counted) {
        return;
    }
    counted = false;
    write_count(0);
}

void recovery_set_reason(enum recovery_reason reason) {
    current_reason = reason;
}

enum recovery_reason recovery_reason(void) {
    return current_reason;
}

uint32_t recovery_failed_starts(void) {
    return failed_starts;
}
