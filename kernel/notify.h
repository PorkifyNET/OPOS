#ifndef OPOS_NOTIFY_H
#define OPOS_NOTIFY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmos.h"

/*
 * Notifications (docs/notifications.md, step 2). The kernel keeps the
 * session's last NOTIFY_CAPACITY notifications in memory; the desktop shows
 * new ones as toasts and lists them all in the Notification Center. Log
 * out clears them. Nothing is written to disk.
 */

#define NOTIFY_CAPACITY 64u
/* Room for "Program Name (executable)" (see user.c). */
#define NOTIFY_SOURCE_MAX 72u
#define NOTIFY_TITLE_MAX 64u
#define NOTIFY_BODY_MAX 192u

struct notification {
    uint32_t id;           /* 1 and up, never reused in a boot */
    bool read;             /* seen in a toast click or the Center */
    bool toast_pending;    /* not shown as a toast yet */
    bool has_time;
    struct rtc_time time;  /* when it was posted */
    char source[NOTIFY_SOURCE_MAX]; /* "OPOS" or the program's name */
    char title[NOTIFY_TITLE_MAX];
    char body[NOTIFY_BODY_MAX];
};

/* Adds a notification (text is cut to fit; control characters become
   spaces). The oldest one goes when the list is full. Returns its id, or
   0 for an empty title. */
uint32_t notify_post(const char *source, const char *title, const char *body);

/* How many there are, and a copy of one: index 0 is the newest. */
size_t notify_count(void);
bool notify_at(size_t index, struct notification *out);
/* A copy by id; false once it was dismissed or dropped. */
bool notify_find(uint32_t id, struct notification *out);

/* The oldest notification still waiting for its toast, which is then no
   longer waiting. */
bool notify_take_toast(struct notification *out);

void notify_mark_read(uint32_t id);
void notify_mark_all_read(void);
bool notify_dismiss(uint32_t id);
void notify_clear(void);
size_t notify_unread(void);

/* Changes with every post, read mark or dismissal (for redrawing). */
uint32_t notify_generation(void);

#endif
