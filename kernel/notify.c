#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmos.h"
#include "notify.h"

/*
 * The list runs oldest to newest. Programs post from their own threads
 * (step 4) while the desktop reads, so every change and copy happens with
 * interrupts off; entries are small, so that is brief.
 */

static struct notification list[NOTIFY_CAPACITY];
static size_t count;
static uint32_t next_id = 1;
static volatile uint32_t generation = 1;

static uint64_t interrupts_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupts_restore(uint64_t flags) {
    if ((flags & 0x200u) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

/* Copies text, cut to capacity - 1, with control characters as spaces
   and leading spaces skipped. Returns the length. */
static size_t copy_text(char *out, size_t capacity, const char *text) {
    size_t length = 0;
    if (text != NULL) {
        while (*text == ' ') {
            text++;
        }
        for (; text[length] != '\0' && length + 1 < capacity; length++) {
            unsigned char c = (unsigned char)text[length];
            out[length] = c < ' ' || c == 0x7f ? ' ' : (char)c;
        }
    }
    while (length > 0 && out[length - 1] == ' ') {
        length--;
    }
    out[length] = '\0';
    return length;
}

static size_t index_of(uint32_t id) {
    for (size_t i = 0; i < count; i++) {
        if (list[i].id == id) {
            return i;
        }
    }
    return NOTIFY_CAPACITY;
}

static void remove_at(size_t index) {
    for (size_t i = index; i + 1 < count; i++) {
        list[i] = list[i + 1];
    }
    count--;
}

uint32_t notify_post(const char *source, const char *title,
                     const char *body) {
    struct notification entry = {0};
    if (copy_text(entry.title, sizeof(entry.title), title) == 0) {
        return 0;
    }
    if (copy_text(entry.source, sizeof(entry.source), source) == 0) {
        copy_text(entry.source, sizeof(entry.source), "OPOS");
    }
    copy_text(entry.body, sizeof(entry.body), body);
    entry.has_time = rtc_read(&entry.time);
    entry.toast_pending = true;
    uint64_t flags = interrupts_save();
    if (count == NOTIFY_CAPACITY) {
        remove_at(0);
    }
    entry.id = next_id++;
    list[count++] = entry;
    generation++;
    interrupts_restore(flags);
    return entry.id;
}

size_t notify_count(void) {
    return count;
}

bool notify_at(size_t index, struct notification *out) {
    uint64_t flags = interrupts_save();
    bool found = index < count;
    if (found) {
        *out = list[count - 1 - index];
    }
    interrupts_restore(flags);
    return found;
}

bool notify_find(uint32_t id, struct notification *out) {
    uint64_t flags = interrupts_save();
    size_t index = index_of(id);
    bool found = index < count;
    if (found && out != NULL) {
        *out = list[index];
    }
    interrupts_restore(flags);
    return found;
}

bool notify_take_toast(struct notification *out) {
    uint64_t flags = interrupts_save();
    bool found = false;
    for (size_t i = 0; i < count && !found; i++) {
        if (list[i].toast_pending) {
            list[i].toast_pending = false;
            *out = list[i];
            found = true;
        }
    }
    interrupts_restore(flags);
    return found;
}

void notify_mark_read(uint32_t id) {
    uint64_t flags = interrupts_save();
    size_t index = index_of(id);
    if (index < count && !list[index].read) {
        list[index].read = true;
        generation++;
    }
    interrupts_restore(flags);
}

void notify_mark_all_read(void) {
    uint64_t flags = interrupts_save();
    for (size_t i = 0; i < count; i++) {
        if (!list[i].read) {
            list[i].read = true;
            generation++;
        }
    }
    interrupts_restore(flags);
}

bool notify_dismiss(uint32_t id) {
    uint64_t flags = interrupts_save();
    size_t index = index_of(id);
    bool found = index < count;
    if (found) {
        remove_at(index);
        generation++;
    }
    interrupts_restore(flags);
    return found;
}

void notify_clear(void) {
    uint64_t flags = interrupts_save();
    count = 0;
    generation++;
    interrupts_restore(flags);
}

size_t notify_unread(void) {
    size_t unread = 0;
    for (size_t i = 0; i < count; i++) {
        unread += list[i].read ? 0 : 1;
    }
    return unread;
}

uint32_t notify_generation(void) {
    return generation;
}
