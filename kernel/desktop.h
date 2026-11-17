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
/* Asks the person to choose a folder and file name to save to, starting
   from suggested (may be ""). Returns the path length, -125 on
   cancellation, or another negative errno. The caller grants the write. */
int64_t desktop_app_save_file(uint64_t owner, const char *name,
                              const struct permission_identity *identity,
                              const char *suggested, char *path,
                              size_t capacity);
/* Asks the person at the desktop whether the program may have one
   PERMISSION_CAP_* capability. Returns 1 for allow, 0 for deny, -38 with
   no desktop or at the login screen, -16 while another prompt is open,
   or -4 when the program is being ended. Call only from a program's task. */
int64_t desktop_app_ask_permission(uint64_t owner, const char *name,
                                   uint32_t capability);
/* Content size as width << 32 | height, or 0 without a window. */
uint64_t desktop_app_window_size(uint64_t owner);
/* Window options: DESKTOP_APP_KEY_STATE adds an event for every key press
   and release (APP_EVENT_KEY_STATE, 8), besides the typed characters.
   Returns 0, -9 without a window, or -22 for unknown options. */
#define DESKTOP_APP_KEY_STATE 1u
/* DESKTOP_APP_QUIET: while the window is in front, notifications skip
   their toast and sound (a game, a presentation). */
#define DESKTOP_APP_QUIET 2u
/* DESKTOP_APP_FULLSCREEN: the window covers the whole screen without a
   frame, and the taskbar stays hidden while it is in front. A smaller
   picture is scaled up to fit, keeping its shape. */
#define DESKTOP_APP_FULLSCREEN 4u
/* DESKTOP_APP_ASK_CLOSE: the close button and Super+Q send CLOSE but keep
   the window, so the program can ask about unsaved work and exit by
   itself. A program that has not taken an earlier CLOSE for a while (it
   hangs) is closed as usual on the next request. */
#define DESKTOP_APP_ASK_CLOSE 8u
int64_t desktop_app_window_options(uint64_t owner, uint32_t options);
/* Asks for a new content size (a game changing its resolution). The
   floating frame follows on the desktop's next pass, limited to the work
   area; the program then gets a RESIZE event with the size it really has.
   Returns 0, -9 without a window, or -22 for a zero or too large size. */
int64_t desktop_app_window_resize(uint64_t owner, uint32_t width,
                                  uint32_t height);
/* Changes the window's title (printable ASCII; other bytes are dropped,
   at most 31 kept). Returns 0, or -9 without a window. */
int64_t desktop_app_window_title(uint64_t owner, const char *title,
                                 size_t title_length);
/* The screen size as width << 32 | height (what a full-screen window
   gets), or 0 without a screen. */
uint64_t desktop_screen_size(void);
/* The shared clipboard for programs. Set keeps at most 64 KiB. Get copies
   up to capacity bytes and returns the clipboard's full length. */
void desktop_clipboard_set(const char *text, size_t length);
size_t desktop_clipboard_get(char *out, size_t capacity);
/* Goes up by one every time the clipboard is set. */
uint32_t desktop_clipboard_generation(void);
/* Every process end: its window goes away. */
void desktop_app_process_exited(uint64_t process_id);

#endif
