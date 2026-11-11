#ifndef OPOS_DESKTOP_H
#define OPOS_DESKTOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct permission_identity;

void desktop_run(void);

/*
 * A running program's window (OPOS ABI 1, see sdk/include/opos.h). owner
 * is the process ID. Pointers are kernel-checked before these are called.
 */
struct desktop_app_event {
    uint32_t type;
    uint32_t key;
    int32_t x;
    int32_t y;
    uint32_t button;
    uint32_t buttons;
    uint32_t width;
    uint32_t height;
};

int64_t desktop_app_window_create(uint64_t owner, const char *title,
                                  size_t title_length, uint32_t width,
                                  uint32_t height);
int64_t desktop_app_window_present(uint64_t owner, const uint32_t *pixels,
                                   uint32_t width, uint32_t height,
                                   uint32_t stride);
/* Waits up to timeout_ms (negative: forever; 0: just check); the
   desktop and other programs run meanwhile. Returns 1 with an event, 0
   without (also when the program is being ended). Sets
   *terminate when the program ignored a close and must end. */
int64_t desktop_app_window_event(uint64_t owner,
                                 struct desktop_app_event *event,
                                 int64_t timeout_ms, bool *terminate);
/* Asks the person at the desktop to choose one readable file. Returns its
   path length, -125 on cancellation, or another negative errno. */
int64_t desktop_app_pick_file(uint64_t owner, const char *name,
                              const struct permission_identity *identity,
                              char *path, size_t capacity);
/* Asks the person at the desktop whether the program may have one
   PERMISSION_CAP_* capability. Returns 1 for allow, 0 for deny, -38 with
   no desktop or at the login screen, -16 while another prompt is open,
   or -4 when the program is being ended. Call only from a program's task. */
int64_t desktop_app_ask_permission(uint64_t owner, const char *name,
                                   uint32_t capability);
/* Content size as width << 32 | height, or 0 without a window. */
uint64_t desktop_app_window_size(uint64_t owner);
/* The shared clipboard for programs. Set keeps at most 64 KiB. Get copies
   up to capacity bytes and returns the clipboard's full length. */
void desktop_clipboard_set(const char *text, size_t length);
size_t desktop_clipboard_get(char *out, size_t capacity);
/* Every process end: its window goes away. */
void desktop_app_process_exited(uint64_t process_id);

#endif
