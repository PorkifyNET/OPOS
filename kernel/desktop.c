#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accounts.h"
#include "block.h"
#include "cmos.h"
#include "console.h"
#include "desktop.h"
#include "fat16.h"
#include "font.h"
#include "initramfs.h"
#include "installer.h"
#include "keyboard.h"
#include "permissions.h"
#include "memory.h"
#include "mouse.h"
#include "power.h"
#include "scheduler.h"
#include "svga.h"
#include "vmware.h"
#include "timer.h"
#include "user.h"
#include "vfs.h"

/*
 * OPOS desktop: a small window manager with floating and Hyprland-style
 * dwindle tiling layouts, a taskbar with a start menu, and built-in apps.
 * Every redraw renders the whole scene into the console back buffer and
 * presents it in one copy, so windows can overlap without flicker.
 */

#define CHAR_WIDTH ((FONT_WIDTH + 1) * 2)
#define CHAR_HEIGHT (FONT_HEIGHT * 2)
#define TASKBAR_HEIGHT 40
#define TITLE_HEIGHT 28
#define BORDER 2
#define GAP 10
#define LINE_HEIGHT 24
#define MENU_WIDTH 280
#define MENU_ITEM_HEIGHT 32
#define TASK_BUTTON_WIDTH 150
#define START_BUTTON_WIDTH 90
#define LAYOUT_BUTTON_WIDTH 110
#define CLOCK_WIDTH 290
#define MIN_WINDOW_WIDTH 240
#define MIN_WINDOW_HEIGHT 140

struct rect {
    int64_t x;
    int64_t y;
    int64_t width;
    int64_t height;
};

enum window_kind {
    WINDOW_TERMINAL,
    WINDOW_FILES,
    WINDOW_SYSTEM_INFO,
    WINDOW_WELCOME,
    WINDOW_SETTINGS,
    WINDOW_EDITOR,
    WINDOW_INSTALLER,
    WINDOW_ACCOUNTS,
    WINDOW_TASKS,
    /* Program windows: one kind per slot (see "program windows"). */
    WINDOW_APP,
    WINDOW_APP_LAST = WINDOW_APP + 3,
    WINDOW_COUNT
};
#define APP_WINDOW_COUNT ((size_t)(WINDOW_APP_LAST - WINDOW_APP + 1))

static bool window_is_app(enum window_kind kind) {
    return kind >= WINDOW_APP && kind <= WINDOW_APP_LAST;
}

static const char *const window_titles[WINDOW_COUNT] = {
    "TERMINAL", "FILES", "SYSTEM INFO", "WELCOME", "SETTINGS", "TEXT EDITOR",
    "INSTALL OPOS", "ACCOUNTS", "TASK MANAGER", "PROGRAM"
};

static const char *app_window_title(size_t slot);

/* A program's window shows the title the program chose. */
static const char *window_title(enum window_kind kind) {
    return window_is_app(kind) ? app_window_title(kind - WINDOW_APP)
                               : window_titles[kind];
}

struct window {
    bool open;
    struct rect floating;
};

static struct window windows[WINDOW_COUNT];
/* Stacking order, bottom first; the last entry has keyboard focus. */
static enum window_kind z_order[WINDOW_COUNT];
static size_t z_count;
/* Tiling order: windows in the order they were opened. */
static enum window_kind tile_order[WINDOW_COUNT];
static size_t tile_count;
static bool tiled_layout = true;
/* Dwindle split i gives tile_order[i] this share of its area, in permille. */
#define SPLIT_DEFAULT 500
#define SPLIT_MIN 150
#define SPLIT_MAX 850
static uint16_t split_ratio[WINDOW_COUNT];
static void layout_load(void);
static void welcome_open_first_time(void);
static bool start_menu_open;
static size_t menu_selection;
static size_t settings_selection;
static bool exit_to_shell;
static char status_text[64];

enum menu_action {
    MENU_TERMINAL,
    MENU_FILES,
    MENU_TEXT_EDITOR,
    MENU_SYSTEM_INFO,
    MENU_TASK_MANAGER,
    MENU_TOGGLE_LAYOUT,
    MENU_SETTINGS,
    MENU_ACCOUNTS,
    MENU_INSTALL,
    MENU_LOG_OUT,
    MENU_REBOOT,
    MENU_SHUTDOWN,
    MENU_COUNT
};

static const char *const menu_labels[MENU_COUNT] = {
    "TERMINAL", "FILES", "TEXT EDITOR", "SYSTEM INFO", "TASK MANAGER", "TOGGLE TILING", "SETTINGS", "ACCOUNTS", "INSTALL OPOS", "LOG OUT", "REBOOT", "SHUT DOWN"
};

/* ---------------------------------------------------------------- text */

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static void text_copy(char *destination, size_t capacity, const char *source) {
    size_t i = 0;
    while (source[i] != '\0' && i + 1 < capacity) {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
}

static size_t append_text(char *buffer, size_t capacity, size_t offset,
                          const char *text) {
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    buffer[offset] = '\0';
    return offset;
}

static size_t append_decimal(char *buffer, size_t capacity, size_t offset,
                             uint64_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = '0' + value % 10;
        value /= 10;
    } while (value != 0 && count < sizeof(digits));

    while (count != 0 && offset + 1 < capacity) {
        buffer[offset++] = digits[--count];
    }
    buffer[offset] = '\0';
    return offset;
}

static size_t append_two_digits(char *buffer, size_t capacity, size_t offset,
                                uint64_t value) {
    if (value < 10) {
        offset = append_text(buffer, capacity, offset, "0");
    }
    return append_decimal(buffer, capacity, offset, value);
}

static bool text_equals(const char *text, size_t length, const char *expected) {
    size_t i = 0;
    while (i < length && expected[i] != '\0' && text[i] == expected[i]) {
        i++;
    }
    return i == length && expected[i] == '\0';
}

/* Draws text clipped to max_width pixels. */
static void draw_text_clipped(int64_t x, int64_t y, int64_t max_width,
                              const char *text,
                              uint8_t red, uint8_t green, uint8_t blue) {
    if (x < 0 || y < 0 || max_width < CHAR_WIDTH) {
        return;
    }
    char clipped[160];
    size_t limit = (size_t)(max_width / CHAR_WIDTH);
    if (limit >= sizeof(clipped)) {
        limit = sizeof(clipped) - 1;
    }
    size_t i = 0;
    while (i < limit && text[i] != '\0') {
        clipped[i] = text[i];
        i++;
    }
    clipped[i] = '\0';
    console_draw_text((uint64_t)x, (uint64_t)y, clipped, red, green, blue);
}

static void fill(struct rect area, uint8_t red, uint8_t green, uint8_t blue) {
    if (area.width <= 0 || area.height <= 0) {
        return;
    }
    if (area.x < 0) {
        area.width += area.x;
        area.x = 0;
    }
    if (area.y < 0) {
        area.height += area.y;
        area.y = 0;
    }
    if (area.width <= 0 || area.height <= 0) {
        return;
    }
    console_draw_rectangle((uint64_t)area.x, (uint64_t)area.y,
                           (uint64_t)area.width, (uint64_t)area.height,
                           red, green, blue);
}

static bool rect_contains(struct rect area, int32_t x, int32_t y) {
    return x >= area.x && x < area.x + area.width
        && y >= area.y && y < area.y + area.height;
}

static int64_t screen_width(void) {
    return (int64_t)console_screen_width();
}

static int64_t screen_height(void) {
    return (int64_t)console_screen_height();
}

static void set_status(const char *text) {
    text_copy(status_text, sizeof(status_text), text);
}

static bool text_contains(const char *text, const char *needle) {
    for (; *text != '\0'; text++) {
        size_t i = 0;
        while (needle[i] != '\0' && text[i] == needle[i]) {
            i++;
        }
        if (needle[i] == '\0') {
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------- clipboard */

/* One clipboard for every text input in the desktop (Ctrl+C/X/V). */
#define CLIPBOARD_CAPACITY (64 * 1024)

static char clipboard[CLIPBOARD_CAPACITY];
static size_t clipboard_length;

static void clipboard_set(const char *text, size_t length) {
    if (length > CLIPBOARD_CAPACITY) {
        length = CLIPBOARD_CAPACITY;
    }
    for (size_t i = 0; i < length; i++) {
        clipboard[i] = text[i];
    }
    clipboard_length = length;
}

/*
 * Ctrl shortcuts for one-line inputs, which type and delete at the end of
 * the text: Ctrl+C copies the whole text, Ctrl+X cuts it, Ctrl+V pastes the
 * first clipboard line, Ctrl+Backspace deletes the last word. Secret
 * (password) text is never copied. Returns true for any Ctrl combination,
 * so callers do not type the letter.
 */
static bool line_ctrl_key(char *text, size_t *length, size_t limit,
                          bool secret, char character) {
    if (!keyboard_last_ctrl()) {
        return false;
    }
    switch (character) {
    case 'c':
    case 'C':
        if (!secret) {
            clipboard_set(text, *length);
        }
        break;
    case 'x':
    case 'X':
        if (!secret) {
            clipboard_set(text, *length);
            *length = 0;
            text[0] = '\0';
        }
        break;
    case 'v':
    case 'V':
        for (size_t i = 0; i < clipboard_length && *length < limit; i++) {
            char c = clipboard[i];
            if (c == '\n' || c == '\r') {
                break;
            }
            if (c == '\t') {
                c = ' ';
            }
            if (c >= ' ' && c <= '~') {
                text[(*length)++] = c;
            }
        }
        text[*length] = '\0';
        break;
    case '\b':
        while (*length > 0 && text[*length - 1] == ' ') {
            (*length)--;
        }
        while (*length > 0 && text[*length - 1] != ' ') {
            (*length)--;
        }
        text[*length] = '\0';
        break;
    default:
        break;
    }
    return true;
}

/* ------------------------------------------------------------ terminal */

#define TERMINAL_HISTORY 32
#define TERMINAL_COLUMNS 96

static char terminal_line[64];
static size_t terminal_length;
static char terminal_history[TERMINAL_HISTORY][TERMINAL_COLUMNS];
static size_t terminal_history_count;

static void terminal_print(const char *text) {
    if (terminal_history_count == TERMINAL_HISTORY) {
        for (size_t i = 1; i < TERMINAL_HISTORY; i++) {
            text_copy(terminal_history[i - 1], TERMINAL_COLUMNS,
                      terminal_history[i]);
        }
        terminal_history_count--;
    }
    text_copy(terminal_history[terminal_history_count++], TERMINAL_COLUMNS,
              text);
}

static void terminal_reset(void) {
    terminal_length = 0;
    terminal_line[0] = '\0';
    terminal_history_count = 0;
    terminal_print("OPOS TERMINAL - TYPE HELP");
}

/*
 * Programs run next to the desktop (see "program windows" at the end of
 * this file). Their standard output and error land in the Terminal a line
 * at a time; terminal_output collects the current line.
 */
static char terminal_output[TERMINAL_COLUMNS];
static size_t terminal_output_length;
static void draw_scene(void);
static void window_open(enum window_kind kind);
static void window_raise(enum window_kind kind);
static bool desktop_active;
/* Programs waiting to read a line typed into the Terminal. */
static size_t app_input_readers;
static void app_window_closed(size_t slot);
static void app_key(size_t slot, char character);
static void app_mouse_press(size_t slot, int32_t x, int32_t y,
                            uint32_t button);
static void draw_app(size_t slot, struct rect area);
static void tasks_open(void);
static void draw_tasks(struct rect area, bool focused);
static void tasks_click(struct rect area, int32_t x, int32_t y);
static void tasks_key(char character);
static const char *window_title(enum window_kind kind);

static void terminal_output_flush(void) {
    if (terminal_output_length != 0) {
        terminal_output[terminal_output_length] = '\0';
        terminal_print(terminal_output);
        terminal_output_length = 0;
    }
}

static void terminal_output_put(char c) {
    if (c == '\n') {
        terminal_output[terminal_output_length] = '\0';
        terminal_print(terminal_output);
        terminal_output_length = 0;
    } else if (c == '\b') {
        if (terminal_output_length != 0) {
            terminal_output_length--;
        }
    } else if (c == '\t' || (c >= ' ' && c <= '~')) {
        terminal_output[terminal_output_length++] = c == '\t' ? ' ' : c;
        if (terminal_output_length + 1 == TERMINAL_COLUMNS) {
            terminal_output_flush();
        }
    }
}

static void terminal_report_exit(bool started, uint64_t exit_status) {
    terminal_print(!started ? "APPLICATION COULD NOT START"
                   : exit_status == 0 ? "APPLICATION EXITED WITH STATUS 0"
                   : exit_status == 128 + 9 ? "APPLICATION WAS ENDED"
                                            : "APPLICATION EXITED WITH ERROR");
}

enum run_result {
    RUN_STARTED,
    RUN_NOT_FOUND,
    RUN_DENIED,
    RUN_TOO_LARGE,
    RUN_NOT_PROGRAM,
    RUN_FAILED,
    RUN_BUSY,
};

/* Who started a program decides what happens when it ends. */
enum app_origin {
    APP_FROM_TERMINAL,
    APP_FROM_FILES,
    APP_FROM_NOTEPAD,
};

static enum run_result app_launch(enum app_origin origin, const char *typed,
                                  size_t typed_length, const char *arguments,
                                  size_t arguments_length,
                                  const char *document,
                                  size_t document_length);
static enum run_result app_launch_builtin(enum user_program program,
                                          const char *name);
static bool app_kill_terminal_job(void);
static void app_input_push(char character);
static void apps_list(void);
static void apps_stop_all(void);

static const char *run_result_text(enum run_result result) {
    switch (result) {
    case RUN_NOT_FOUND: return "FILE NOT FOUND";
    case RUN_DENIED: return "ACCESS DENIED";
    case RUN_TOO_LARGE: return "PROGRAM TOO LARGE (1 MIB LIMIT)";
    case RUN_NOT_PROGRAM: return "NOT A PROGRAM (NO ELF HEADER)";
    case RUN_FAILED: return "PROGRAM COULD NOT START";
    case RUN_BUSY: return "TOO MANY PROGRAMS RUNNING";
    default: return "";
    }
}

static void terminal_list_files(void) {
    size_t count = initramfs_file_count();
    if (count == 0) {
        terminal_print("NO FILES");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        char path[128];
        size_t file_size;
        if (!initramfs_file_at(i, path, sizeof(path), &file_size)) {
            terminal_print("FILE LIST UNAVAILABLE");
            return;
        }
        char line[TERMINAL_COLUMNS] = "/";
        size_t length = append_text(line, sizeof(line), 1, path);
        length = append_text(line, sizeof(line), length, "  ");
        length = append_decimal(line, sizeof(line), length, file_size);
        append_text(line, sizeof(line), length, " BYTES");
        terminal_print(line);
    }
}

/* Executes the typed line. Returns false when the terminal should close. */
static bool terminal_execute(void) {
    char echo[TERMINAL_COLUMNS] = "OPOS> ";
    append_text(echo, sizeof(echo), 6, terminal_line);
    terminal_print(echo);

    if (text_equals(terminal_line, terminal_length, "help")) {
        terminal_print("HELP LS CLEAR UPTIME ELF DEMO RUN PATH [ARGS]");
        terminal_print("PS KILL PID  SHELL REBOOT SHUTDOWN EXIT");
        terminal_print("CTRL+C ENDS THE LAST PROGRAM STARTED HERE");
    } else if (text_equals(terminal_line, terminal_length, "ls")) {
        terminal_list_files();
    } else if (text_equals(terminal_line, terminal_length, "clear")) {
        terminal_history_count = 0;
    } else if (text_equals(terminal_line, terminal_length, "uptime")) {
        char line[48] = "UPTIME: ";
        size_t length = append_decimal(line, sizeof(line), 8,
                                       timer_ticks() / 100);
        append_text(line, sizeof(line), length, " SECONDS");
        terminal_print(line);
    } else if (text_equals(terminal_line, terminal_length, "elf")) {
        enum run_result result =
            app_launch_builtin(USER_PROGRAM_MODULE_ELF, "ELF");
        if (result != RUN_STARTED) {
            terminal_print(run_result_text(result));
        }
    } else if (text_equals(terminal_line, terminal_length, "demo")) {
        enum run_result result = app_launch_builtin(USER_PROGRAM_DEMO,
                                                    "DEMO");
        if (result != RUN_STARTED) {
            terminal_print(run_result_text(result));
        }
    } else if (terminal_length > 4 && text_equals(terminal_line, 4, "run ")) {
        /* run PATH [ARGUMENTS]. A path with spaces goes in double
           quotes. A name without a leading '/' is looked up from the
           root, so "run bin/elf-test" still works. */
        char path[sizeof(terminal_line) + 1];
        size_t length = 0;
        size_t i = 4;
        while (i < terminal_length && terminal_line[i] == ' ') {
            i++;
        }
        bool quoted = i < terminal_length && terminal_line[i] == '"';
        if (quoted) {
            i++;
        }
        if (i < terminal_length && terminal_line[i] != '/') {
            path[length++] = '/';
        }
        for (; i < terminal_length; i++) {
            char c = terminal_line[i];
            if (quoted ? c == '"' : c == ' ') {
                i++;
                break;
            }
            path[length++] = c;
        }
        /* The program runs next to the Terminal, which stays usable;
           its output and exit status show up here. */
        enum run_result result = app_launch(
            APP_FROM_TERMINAL, path, length, terminal_line + i,
            terminal_length - i, NULL, 0);
        if (result != RUN_STARTED) {
            terminal_print(run_result_text(result));
        }
    } else if (text_equals(terminal_line, terminal_length, "ps")) {
        apps_list();
    } else if (terminal_length > 5 && text_equals(terminal_line, 5, "kill ")) {
        uint64_t process_id = 0;
        size_t i = 5;
        while (i < terminal_length && terminal_line[i] >= '0'
               && terminal_line[i] <= '9' && process_id < 100000000) {
            process_id = process_id * 10 + (uint64_t)(terminal_line[i] - '0');
            i++;
        }
        terminal_print(i == terminal_length && user_process_kill(process_id)
                       ? "ENDING PROGRAM" : "NO SUCH PROGRAM");
    } else if (text_equals(terminal_line, terminal_length, "reboot")) {
        power_reboot();
    } else if (text_equals(terminal_line, terminal_length, "shutdown")) {
        if (!power_shutdown()) {
            terminal_print("ACPI SHUTDOWN UNAVAILABLE");
        }
    } else if (text_equals(terminal_line, terminal_length, "exit")) {
        return false;
    } else if (text_equals(terminal_line, terminal_length, "shell")) {
        /* The kernel shell asks for an administrator unless one is
           logged in (see shell_lock_update in shell.c). */
        exit_to_shell = true;
    } else if (terminal_length != 0) {
        terminal_print("UNKNOWN COMMAND - TYPE HELP");
    }
    return true;
}

/* --------------------------------------------------------------- layout */

static struct rect workspace_area(void) {
    return (struct rect){
        GAP, GAP, screen_width() - 2 * GAP,
        screen_height() - TASKBAR_HEIGHT - 2 * GAP
    };
}

/*
 * Dwindle layout: each window takes half of the remaining area, split along
 * the longer side, and the last window takes whatever is left.
 */
static struct rect tiled_rect(enum window_kind kind) {
    struct rect area = workspace_area();
    for (size_t i = 0; i < tile_count; i++) {
        if (i + 1 == tile_count) {
            return area;
        }
        struct rect first = area;
        int64_t ratio = split_ratio[i];
        if (area.width >= area.height) {
            first.width = (area.width - GAP) * ratio / 1000;
            area.x += first.width + GAP;
            area.width -= first.width + GAP;
        } else {
            first.height = (area.height - GAP) * ratio / 1000;
            area.y += first.height + GAP;
            area.height -= first.height + GAP;
        }
        if (tile_order[i] == kind) {
            return first;
        }
    }
    return area;
}

/* After the screen shrinks, pulls floating windows back inside it. */
static void windows_fit_screen(void) {
    struct rect area = workspace_area();
    for (size_t i = 0; i < WINDOW_COUNT; i++) {
        struct rect *frame = &windows[i].floating;
        if (frame->width == 0) {
            continue;
        }
        if (frame->width > area.width) {
            frame->width = area.width;
        }
        if (frame->height > area.height) {
            frame->height = area.height;
        }
        if (frame->x + frame->width > area.x + area.width) {
            frame->x = area.x + area.width - frame->width;
        }
        if (frame->y + frame->height > area.y + area.height) {
            frame->y = area.y + area.height - frame->height;
        }
        if (frame->x < area.x) {
            frame->x = area.x;
        }
        if (frame->y < area.y) {
            frame->y = area.y;
        }
    }
}

static struct rect window_rect(enum window_kind kind) {
    return tiled_layout ? tiled_rect(kind) : windows[kind].floating;
}

static struct rect title_rect(struct rect frame) {
    return (struct rect){frame.x + BORDER, frame.y + BORDER,
                         frame.width - 2 * BORDER, TITLE_HEIGHT};
}

static struct rect close_rect(struct rect frame) {
    return (struct rect){frame.x + frame.width - BORDER - TITLE_HEIGHT,
                         frame.y + BORDER, TITLE_HEIGHT, TITLE_HEIGHT};
}

static struct rect content_rect(struct rect frame) {
    return (struct rect){frame.x + BORDER, frame.y + BORDER + TITLE_HEIGHT,
                         frame.width - 2 * BORDER,
                         frame.height - 2 * BORDER - TITLE_HEIGHT};
}

static bool window_focused(enum window_kind kind) {
    return z_count != 0 && z_order[z_count - 1] == kind;
}

static void window_raise(enum window_kind kind) {
    size_t index = 0;
    while (index < z_count && z_order[index] != kind) {
        index++;
    }
    if (index == z_count) {
        return;
    }
    for (size_t i = index; i + 1 < z_count; i++) {
        z_order[i] = z_order[i + 1];
    }
    z_order[z_count - 1] = kind;
}

static void remove_from(enum window_kind *list, size_t *count,
                        enum window_kind kind) {
    size_t out = 0;
    for (size_t i = 0; i < *count; i++) {
        if (list[i] != kind) {
            list[out++] = list[i];
        }
    }
    *count = out;
}

static void files_refresh(void);
static bool editor_may_discard(void);
static void editor_reset(void);

static void window_open(enum window_kind kind) {
    if (windows[kind].open) {
        window_raise(kind);
        return;
    }
    windows[kind].open = true;
    z_order[z_count++] = kind;
    /* The window that was last now gets a split of its own. */
    if (tile_count != 0) {
        split_ratio[tile_count - 1] = SPLIT_DEFAULT;
    }
    tile_order[tile_count++] = kind;
    if (kind == WINDOW_TERMINAL) {
        terminal_reset();
    }
    if (kind == WINDOW_FILES) {
        files_refresh();
    }
    if (windows[kind].floating.width == 0) {
        struct rect area = workspace_area();
        int64_t width = area.width * 3 / 5;
        int64_t height = area.height * 3 / 5;
        int64_t offset = 40 * (int64_t)kind;
        windows[kind].floating = (struct rect){
            area.x + 40 + offset, area.y + 30 + offset, width, height
        };
    }
}

static void window_close(enum window_kind kind) {
    if (window_is_app(kind)) {
        app_window_closed(kind - WINDOW_APP);
    }
    if (kind == WINDOW_EDITOR) {
        if (!editor_may_discard()) {
            return;
        }
        editor_reset();
    }
    windows[kind].open = false;
    /* Its split goes with it; the other windows keep their sizes. */
    for (size_t i = 0; i < tile_count; i++) {
        if (tile_order[i] == kind) {
            for (size_t j = i; j + 1 < WINDOW_COUNT; j++) {
                split_ratio[j] = split_ratio[j + 1];
            }
            break;
        }
    }
    remove_from(z_order, &z_count, kind);
    remove_from(tile_order, &tile_count, kind);
}

/* Returns the topmost window under the pointer, or WINDOW_COUNT. */
static enum window_kind window_at(int32_t x, int32_t y) {
    for (size_t i = z_count; i != 0; i--) {
        enum window_kind kind = z_order[i - 1];
        if (rect_contains(window_rect(kind), x, y)) {
            return kind;
        }
    }
    return WINDOW_COUNT;
}

/* -------------------------------------------------------------- content */

static void draw_terminal(struct rect area, bool focused) {
    fill(area, 12, 16, 26);
    int64_t text_x = area.x + 10;
    int64_t text_width = area.width - 20;
    int64_t prompt_y = area.y + area.height - LINE_HEIGHT - 4;
    int64_t visible = (prompt_y - area.y - 8) / LINE_HEIGHT;
    if (visible < 0) {
        visible = 0;
    }
    size_t first = terminal_history_count > (size_t)visible
                 ? terminal_history_count - (size_t)visible : 0;
    int64_t y = area.y + 8;
    bool colors = settings_get(SETTING_TERMINAL_COLORS);
    for (size_t i = first; i < terminal_history_count; i++) {
        const char *line = terminal_history[i];
        bool echo = line[0] == 'O' && line[4] == '>';
        uint8_t red = echo ? 120 : 220;
        uint8_t green = echo ? 210 : 228;
        uint8_t blue = echo ? 170 : 240;
        if (!echo && colors) {
            if (text_contains(line, "ERROR") || text_contains(line, "UNKNOWN")
                || text_contains(line, "NOT ")) {
                red = 255; green = 110; blue = 110;
            } else if (text_contains(line, "UNAVAILABLE")
                       || text_contains(line, "WARNING")) {
                red = 240; green = 205; blue = 100;
            }
        }
        draw_text_clipped(text_x, y, text_width, line, red, green, blue);
        y += LINE_HEIGHT;
    }
    if (app_input_readers != 0) {
        /* A program reads a line: show what it printed and the echo. */
        char pending[TERMINAL_COLUMNS];
        for (size_t i = 0; i < terminal_output_length; i++) {
            pending[i] = terminal_output[i];
        }
        pending[terminal_output_length] = '\0';
        draw_text_clipped(text_x, prompt_y, text_width, pending,
                          240, 245, 255);
        int64_t caret_x = text_x
            + (int64_t)terminal_output_length * CHAR_WIDTH;
        if (focused && caret_x + CHAR_WIDTH < area.x + area.width
            && (timer_ticks() / 50) % 2 == 0) {
            fill((struct rect){caret_x, prompt_y, CHAR_WIDTH - 2,
                               CHAR_HEIGHT}, 120, 210, 170);
        }
        return;
    }
    draw_text_clipped(text_x, prompt_y, text_width, "OPOS>", 120, 210, 170);
    int64_t line_x = text_x + 6 * CHAR_WIDTH;
    draw_text_clipped(line_x, prompt_y, text_width - 6 * CHAR_WIDTH,
                      terminal_line, 240, 245, 255);
    if (focused) {
        int64_t caret_x = line_x + (int64_t)terminal_length * CHAR_WIDTH;
        if (caret_x + CHAR_WIDTH < area.x + area.width
            && (timer_ticks() / 50) % 2 == 0) {
            fill((struct rect){caret_x, prompt_y, CHAR_WIDTH - 2, CHAR_HEIGHT},
                 120, 210, 170);
        }
    }
}

/* ----------------------------------------------------------- text editor */

/*
 * Plain-text editor window. The whole file lives in one buffer (64 KiB,
 * like the program load limit). CRLF files are edited as LF and written back
 * as CRLF. Files that cannot be written (initramfs) or that look binary open
 * read-only; Ctrl+S then asks for a new name on the disk.
 */
#define EDITOR_CAPACITY (64 * 1024)
#define EDITOR_PATH_CAPACITY 256
#define EDITOR_TAB_WIDTH 4
#define EDITOR_HEADER_HEIGHT 32
#define EDITOR_GUTTER_COLUMNS 5

static char editor_text[EDITOR_CAPACITY];
/* Output buffer for saving CRLF files (every LF may grow to CRLF). */
static uint8_t editor_save_buffer[EDITOR_CAPACITY * 2];
static size_t editor_length;
static size_t editor_cursor;
static size_t editor_goal_column;
static size_t editor_top_line;
static size_t editor_left_column;
static size_t editor_page_lines = 1;
static char editor_path[EDITOR_PATH_CAPACITY];
static size_t editor_path_length; /* 0: untitled */
static bool editor_modified;
static bool editor_read_only;
static bool editor_crlf;
static bool editor_close_armed;
static bool editor_prompt_active;
static char editor_prompt[EDITOR_PATH_CAPACITY];
static size_t editor_prompt_length;
/* Selection: from editor_anchor to editor_cursor while editor_selecting. */
static size_t editor_anchor;
static bool editor_selecting;

static void editor_reset(void) {
    editor_length = 0;
    editor_cursor = 0;
    editor_selecting = false;
    editor_goal_column = 0;
    editor_top_line = 0;
    editor_left_column = 0;
    editor_path_length = 0;
    editor_path[0] = '\0';
    editor_modified = false;
    editor_read_only = false;
    editor_crlf = false;
    editor_close_armed = false;
    editor_prompt_active = false;
}

static size_t editor_line_start(size_t position) {
    while (position > 0 && editor_text[position - 1] != '\n') {
        position--;
    }
    return position;
}

static size_t editor_line_end(size_t position) {
    while (position < editor_length && editor_text[position] != '\n') {
        position++;
    }
    return position;
}

static size_t editor_column(size_t position) {
    return position - editor_line_start(position);
}

static size_t editor_line_of(size_t position) {
    size_t line = 0;
    for (size_t i = 0; i < position; i++) {
        if (editor_text[i] == '\n') {
            line++;
        }
    }
    return line;
}

/* Offset where line starts, or the start of the last line. */
static size_t editor_line_offset(size_t line) {
    size_t position = 0;
    while (line > 0) {
        size_t end = editor_line_end(position);
        if (end == editor_length) {
            break;
        }
        position = end + 1;
        line--;
    }
    return position;
}

static bool editor_is_writable_path(const char *path, size_t length) {
    return length != 0 && !vfs_path_read_only(path, length)
        && permission_ui_allows(path, length, PERMISSION_WRITE);
}

/* Shows unsaved-changes warnings; true when the caller may go ahead. */
static bool editor_may_discard(void) {
    if (!editor_modified || editor_close_armed) {
        return true;
    }
    editor_close_armed = true;
    set_status("UNSAVED CHANGES - REPEAT TO DISCARD");
    return false;
}

static void editor_set_path(const char *path, size_t length) {
    if (length >= EDITOR_PATH_CAPACITY) {
        length = EDITOR_PATH_CAPACITY - 1;
    }
    for (size_t i = 0; i < length; i++) {
        editor_path[i] = path[i];
    }
    editor_path[length] = '\0';
    editor_path_length = length;
}

/* Opens path in the editor window; false (with a status) on failure. */
static bool editor_open(const char *path, size_t length) {
    if (windows[WINDOW_EDITOR].open && !editor_may_discard()) {
        window_open(WINDOW_EDITOR);
        return false;
    }
    if (!permission_ui_allows(path, length, PERMISSION_READ)) {
        set_status("ACCESS DENIED");
        return false;
    }
    size_t size = 0;
    if (!vfs_read_file(path, length, (uint8_t *)editor_text,
                       sizeof(editor_text), &size)) {
        set_status(size > sizeof(editor_text)
                   ? "FILE TOO LARGE TO EDIT (64 KIB LIMIT)"
                   : "FILE COULD NOT BE READ");
        return false;
    }
    editor_reset();
    /* Drop the CR of CRLF pairs; remember to write them back. */
    size_t out = 0;
    bool binary = false;
    for (size_t i = 0; i < size; i++) {
        char c = editor_text[i];
        if (c == '\r' && i + 1 < size && editor_text[i + 1] == '\n') {
            editor_crlf = true;
            continue;
        }
        if ((unsigned char)c < ' ' && c != '\n' && c != '\t') {
            binary = true;
        }
        editor_text[out++] = c;
    }
    editor_length = out;
    editor_set_path(path, length);
    editor_read_only = binary || !editor_is_writable_path(path, length);
    window_open(WINDOW_EDITOR);
    if (binary) {
        set_status("BINARY FILE - OPENED READ-ONLY");
    } else if (editor_read_only) {
        set_status("READ-ONLY - CTRL+S SAVES A COPY ON THE DISK");
    }
    return true;
}

static void editor_new(void) {
    if (windows[WINDOW_EDITOR].open && !editor_may_discard()) {
        window_open(WINDOW_EDITOR);
        return;
    }
    editor_reset();
    window_open(WINDOW_EDITOR);
}

static void editor_save_to(const char *typed, size_t typed_length) {
    char path[EDITOR_PATH_CAPACITY];
    size_t length = typed_length >= 2 && typed[0] == '/'
        ? permission_canonicalize(typed, typed_length, path, sizeof(path))
        : 0;
    if (length < 2) {
        set_status("ENTER A FULL PATH, E.G. /DISK/NOTES.TXT");
        return;
    }
    if (!permission_ui_allows(path, length, PERMISSION_WRITE)) {
        set_status("ACCESS DENIED - SAVE IN YOUR HOME FOLDER");
        return;
    }
    if (!editor_is_writable_path(path, length)) {
        set_status("THAT LOCATION IS READ-ONLY - USE /DISK/...");
        return;
    }
    const uint8_t *data = (const uint8_t *)editor_text;
    size_t size = editor_length;
    if (editor_crlf) {
        size = 0;
        for (size_t i = 0; i < editor_length; i++) {
            if (editor_text[i] == '\n') {
                editor_save_buffer[size++] = '\r';
            }
            editor_save_buffer[size++] = (uint8_t)editor_text[i];
        }
        data = editor_save_buffer;
    }
    if (!vfs_write_file(path, length, data, size)) {
        set_status(fat16_system_mounted()
                   ? "COULD NOT SAVE - CHECK THE NAME AND DISK SPACE"
                   : "COULD NOT SAVE - NO DISK MOUNTED");
        return;
    }
    editor_set_path(path, length);
    editor_modified = false;
    editor_read_only = false;
    editor_close_armed = false;
    editor_prompt_active = false;
    set_status("SAVED");
    if (windows[WINDOW_FILES].open) {
        files_refresh();
    }
}

static void editor_start_prompt(void) {
    /* Suggest <folder>/<name>, keeping the current file name if any. */
    const char *name = "NEW.TXT";
    size_t name_length = 7;
    if (editor_path_length != 0) {
        size_t start = editor_path_length;
        while (start > 0 && editor_path[start - 1] != '/') {
            start--;
        }
        name = editor_path + start;
        name_length = editor_path_length - start;
    }
    /* Logged-in users save to their Documents folder by default. */
    char folder[96];
    if (accounts_home_path("Documents", folder, sizeof(folder)) == 0) {
        text_copy(folder, sizeof(folder), "/disk");
    }
    size_t length = append_text(editor_prompt, sizeof(editor_prompt), 0,
                                folder);
    length = append_text(editor_prompt, sizeof(editor_prompt), length, "/");
    for (size_t i = 0; i < name_length && length + 1 < sizeof(editor_prompt);
         i++) {
        editor_prompt[length++] = name[i];
    }
    editor_prompt[length] = '\0';
    editor_prompt_length = length;
    editor_prompt_active = true;
}

static void editor_save(void) {
    if (editor_path_length == 0 || editor_read_only) {
        editor_start_prompt();
        return;
    }
    editor_save_to(editor_path, editor_path_length);
}

static void editor_changed(void) {
    editor_modified = true;
    editor_close_armed = false;
}

static bool editor_insert(const char *text, size_t count) {
    if (editor_read_only) {
        set_status("READ-ONLY - CTRL+S SAVES A COPY ON THE DISK");
        return false;
    }
    if (count > sizeof(editor_text) - editor_length) {
        set_status("FILE IS FULL (64 KIB LIMIT)");
        return false;
    }
    for (size_t i = editor_length; i > editor_cursor; i--) {
        editor_text[i - 1 + count] = editor_text[i - 1];
    }
    for (size_t i = 0; i < count; i++) {
        editor_text[editor_cursor + i] = text[i];
    }
    editor_length += count;
    editor_cursor += count;
    editor_goal_column = editor_column(editor_cursor);
    editor_changed();
    return true;
}

/* Removes the character at position. */
static void editor_delete_at(size_t position) {
    if (editor_read_only) {
        set_status("READ-ONLY - CTRL+S SAVES A COPY ON THE DISK");
        return;
    }
    if (position >= editor_length) {
        return;
    }
    for (size_t i = position; i + 1 < editor_length; i++) {
        editor_text[i] = editor_text[i + 1];
    }
    editor_length--;
    if (editor_cursor > position) {
        editor_cursor--;
    }
    editor_goal_column = editor_column(editor_cursor);
    editor_changed();
}

static void editor_move_vertical(bool down, size_t lines) {
    for (size_t i = 0; i < lines; i++) {
        size_t start = editor_line_start(editor_cursor);
        size_t target;
        if (down) {
            size_t end = editor_line_end(editor_cursor);
            if (end == editor_length) {
                break;
            }
            target = end + 1;
        } else {
            if (start == 0) {
                break;
            }
            target = editor_line_start(start - 1);
        }
        size_t target_end = editor_line_end(target);
        size_t column = editor_goal_column;
        editor_cursor = target + (column < target_end - target
                                  ? column : target_end - target);
    }
}

static void editor_prompt_key(char character) {
    if (line_ctrl_key(editor_prompt, &editor_prompt_length,
                      sizeof(editor_prompt) - 1, false, character)) {
        return;
    }
    if (character == KEY_ESCAPE) {
        editor_prompt_active = false;
    } else if (character == '\n') {
        editor_save_to(editor_prompt, editor_prompt_length);
    } else if (character == '\b') {
        if (editor_prompt_length > 0) {
            editor_prompt[--editor_prompt_length] = '\0';
        }
    } else if (character >= ' ' && character <= '~'
               && editor_prompt_length + 1 < sizeof(editor_prompt)) {
        editor_prompt[editor_prompt_length++] = character;
        editor_prompt[editor_prompt_length] = '\0';
    }
}


static bool editor_selection(size_t *start, size_t *end) {
    if (!editor_selecting || editor_anchor == editor_cursor) {
        return false;
    }
    *start = editor_anchor < editor_cursor ? editor_anchor : editor_cursor;
    *end = editor_anchor < editor_cursor ? editor_cursor : editor_anchor;
    return true;
}

/* Removes start..end; the cursor goes to start. */
static void editor_delete_range(size_t start, size_t end) {
    if (editor_read_only) {
        set_status("READ-ONLY - CTRL+S SAVES A COPY ON THE DISK");
        return;
    }
    if (end > editor_length) {
        end = editor_length;
    }
    if (start >= end) {
        return;
    }
    size_t count = end - start;
    for (size_t i = start; i + count < editor_length; i++) {
        editor_text[i] = editor_text[i + count];
    }
    editor_length -= count;
    editor_cursor = start;
    editor_selecting = false;
    editor_goal_column = editor_column(editor_cursor);
    editor_changed();
}

/* Deletes the selection, if any; true when something was selected. */
static bool editor_delete_selection(void) {
    size_t start;
    size_t end;
    if (!editor_selection(&start, &end)) {
        editor_selecting = false;
        return false;
    }
    if (!editor_read_only) {
        editor_delete_range(start, end);
    } else {
        set_status("READ-ONLY - CTRL+S SAVES A COPY ON THE DISK");
    }
    return true;
}

/* 0: space, tab or line break; 1: letter, digit or '_'; 2: other. */
static int editor_char_class(char c) {
    if (c == ' ' || c == '\t' || c == '\n') {
        return 0;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_') {
        return 1;
    }
    return 2;
}

/* Start of the next word (Ctrl+Right). */
static size_t editor_word_right(size_t position) {
    if (position < editor_length) {
        int kind = editor_char_class(editor_text[position]);
        while (kind != 0 && position < editor_length
               && editor_char_class(editor_text[position]) == kind) {
            position++;
        }
    }
    while (position < editor_length
           && editor_char_class(editor_text[position]) == 0) {
        position++;
    }
    return position;
}

/* Start of this or the previous word (Ctrl+Left). */
static size_t editor_word_left(size_t position) {
    while (position > 0 && editor_char_class(editor_text[position - 1]) == 0) {
        position--;
    }
    if (position > 0) {
        int kind = editor_char_class(editor_text[position - 1]);
        while (position > 0
               && editor_char_class(editor_text[position - 1]) == kind) {
            position--;
        }
    }
    return position;
}

static void editor_copy(void) {
    size_t start;
    size_t end;
    if (editor_selection(&start, &end)) {
        clipboard_set(editor_text + start, end - start);
    }
}

static void editor_paste(void) {
    if (editor_read_only) {
        set_status("READ-ONLY - CTRL+S SAVES A COPY ON THE DISK");
        return;
    }
    size_t count = 0;
    for (size_t i = 0; i < clipboard_length; i++) {
        char c = clipboard[i];
        if (c == '\n' || c == '\t' || (c >= ' ' && c <= '~')) {
            count++;
        }
    }
    if (count > sizeof(editor_text) - editor_length) {
        set_status("FILE IS FULL (64 KIB LIMIT)");
        return;
    }
    (void)editor_delete_selection();
    /* Insert in pieces of printable text (control characters dropped). */
    size_t piece = 0;
    for (size_t i = 0; i <= clipboard_length; i++) {
        char c = i < clipboard_length ? clipboard[i] : '\0';
        bool keep = i < clipboard_length
            && (c == '\n' || c == '\t' || (c >= ' ' && c <= '~'));
        if (!keep) {
            if (i > piece) {
                (void)editor_insert(clipboard + piece, i - piece);
            }
            piece = i + 1;
        }
    }
}

/* Ctrl combinations in the editor. */
static void editor_ctrl_key(char character, bool shift) {
    switch (character) {
    case 's':
    case 'S':
        editor_save();
        return;
    case 'a':
    case 'A':
        editor_anchor = 0;
        editor_cursor = editor_length;
        editor_selecting = true;
        editor_goal_column = editor_column(editor_cursor);
        return;
    case 'c':
    case 'C':
        editor_copy();
        return;
    case 'x':
    case 'X':
        editor_copy();
        (void)editor_delete_selection();
        return;
    case 'v':
    case 'V':
        editor_paste();
        return;
    case '\b':
        if (!editor_delete_selection()) {
            editor_delete_range(editor_word_left(editor_cursor),
                                editor_cursor);
        }
        return;
    case KEY_DELETE:
        if (!editor_delete_selection()) {
            editor_delete_range(editor_cursor,
                                editor_word_right(editor_cursor));
        }
        return;
    default:
        break;
    }
    /* Ctrl+movement; Shift extends the selection. */
    size_t target;
    switch (character) {
    case KEY_LEFT: target = editor_word_left(editor_cursor); break;
    case KEY_RIGHT: target = editor_word_right(editor_cursor); break;
    case KEY_HOME: target = 0; break;
    case KEY_END: target = editor_length; break;
    default: return;
    }
    if (shift && !editor_selecting) {
        editor_anchor = editor_cursor;
        editor_selecting = true;
    } else if (!shift) {
        editor_selecting = false;
    }
    editor_cursor = target;
    editor_goal_column = editor_column(editor_cursor);
}

static void editor_key(char character) {
    if (editor_prompt_active) {
        editor_prompt_key(character);
        return;
    }
    bool shift = keyboard_last_shift();
    if (keyboard_last_ctrl()) {
        editor_ctrl_key(character, shift);
        return;
    }
    bool movement = character == KEY_LEFT || character == KEY_RIGHT
        || character == KEY_UP || character == KEY_DOWN
        || character == KEY_HOME || character == KEY_END
        || character == KEY_PAGE_UP || character == KEY_PAGE_DOWN;
    if (movement) {
        size_t start;
        size_t end;
        if (shift) {
            if (!editor_selecting) {
                editor_anchor = editor_cursor;
                editor_selecting = true;
            }
        } else if (editor_selection(&start, &end)
                   && (character == KEY_LEFT || character == KEY_RIGHT)) {
            /* Left/Right on a selection go to its edge. */
            editor_cursor = character == KEY_LEFT ? start : end;
            editor_selecting = false;
            editor_goal_column = editor_column(editor_cursor);
            return;
        } else {
            editor_selecting = false;
        }
    } else if (character == '\b' || character == KEY_DELETE) {
        if (editor_delete_selection()) {
            return;
        }
    } else if (character == '\t' || character == '\n'
               || (character >= ' ' && character <= '~')) {
        /* Typing replaces the selection. */
        (void)editor_delete_selection();
    }
    switch (character) {
    case KEY_LEFT:
        if (editor_cursor > 0) {
            editor_cursor--;
        }
        editor_goal_column = editor_column(editor_cursor);
        break;
    case KEY_RIGHT:
        if (editor_cursor < editor_length) {
            editor_cursor++;
        }
        editor_goal_column = editor_column(editor_cursor);
        break;
    case KEY_UP:
        editor_move_vertical(false, 1);
        break;
    case KEY_DOWN:
        editor_move_vertical(true, 1);
        break;
    case KEY_PAGE_UP:
        editor_move_vertical(false, editor_page_lines);
        break;
    case KEY_PAGE_DOWN:
        editor_move_vertical(true, editor_page_lines);
        break;
    case KEY_HOME:
        editor_cursor = editor_line_start(editor_cursor);
        editor_goal_column = 0;
        break;
    case KEY_END:
        editor_cursor = editor_line_end(editor_cursor);
        editor_goal_column = editor_column(editor_cursor);
        break;
    case '\b':
        if (editor_cursor > 0) {
            editor_delete_at(editor_cursor - 1);
        }
        break;
    case KEY_DELETE:
        editor_delete_at(editor_cursor);
        break;
    case '\t': {
        char spaces[EDITOR_TAB_WIDTH];
        size_t count = EDITOR_TAB_WIDTH
            - editor_column(editor_cursor) % EDITOR_TAB_WIDTH;
        for (size_t i = 0; i < count; i++) {
            spaces[i] = ' ';
        }
        (void)editor_insert(spaces, count);
        break;
    }
    case '\n': {
        /* New line keeps the indentation of the current one. */
        char text[1 + 64];
        size_t count = 0;
        text[count++] = '\n';
        size_t start = editor_line_start(editor_cursor);
        for (size_t i = start; i < editor_cursor && count < sizeof(text)
             && (editor_text[i] == ' ' || editor_text[i] == '\t'); i++) {
            text[count++] = editor_text[i];
        }
        (void)editor_insert(text, count);
        break;
    }
    default:
        if (character >= ' ' && character <= '~') {
            (void)editor_insert(&character, 1);
        }
        break;
    }
}

static struct rect editor_text_rect(struct rect area) {
    int64_t gutter = (EDITOR_GUTTER_COLUMNS + 1) * CHAR_WIDTH;
    return (struct rect){area.x + 8 + gutter, area.y + EDITOR_HEADER_HEIGHT + 4,
                         area.width - 16 - gutter,
                         area.height - EDITOR_HEADER_HEIGHT - 8 - LINE_HEIGHT};
}

/* Keeps the cursor on screen by moving the scroll position. */
static void editor_scroll_to_cursor(struct rect text) {
    size_t rows = text.height > 0 ? (size_t)(text.height / LINE_HEIGHT) : 1;
    size_t columns = text.width > 0 ? (size_t)(text.width / CHAR_WIDTH) : 1;
    if (rows == 0) {
        rows = 1;
    }
    if (columns < 2) {
        columns = 2;
    }
    editor_page_lines = rows > 1 ? rows - 1 : 1;
    size_t line = editor_line_of(editor_cursor);
    size_t column = editor_column(editor_cursor);
    if (line < editor_top_line) {
        editor_top_line = line;
    } else if (line >= editor_top_line + rows) {
        editor_top_line = line - rows + 1;
    }
    if (column < editor_left_column) {
        editor_left_column = column;
    } else if (column >= editor_left_column + columns - 1) {
        editor_left_column = column - columns + 2;
    }
}

static void draw_editor(struct rect area, bool focused) {
    fill(area, 20, 25, 38);

    /* Header: file name, state, cursor position. */
    struct rect bar = {area.x + 6, area.y + 6, area.width - 12, 26};
    fill(bar, 32, 40, 62);
    char position[40] = "LN ";
    size_t used = append_decimal(position, sizeof(position), 3,
                                 editor_line_of(editor_cursor) + 1);
    used = append_text(position, sizeof(position), used, "  COL ");
    append_decimal(position, sizeof(position), used,
                   editor_column(editor_cursor) + 1);
    int64_t position_width = (int64_t)text_length(position) * CHAR_WIDTH;
    char title[EDITOR_PATH_CAPACITY + 24];
    used = append_text(title, sizeof(title), 0,
                       editor_path_length != 0 ? editor_path : "UNTITLED");
    if (editor_modified) {
        used = append_text(title, sizeof(title), used, " *");
    }
    if (editor_read_only) {
        append_text(title, sizeof(title), used, "  [READ-ONLY]");
    }
    draw_text_clipped(bar.x + 6, bar.y + 4, bar.width - position_width - 24,
                      title, 230, 238, 255);
    draw_text_clipped(bar.x + bar.width - position_width - 6, bar.y + 4,
                      position_width, position, 151, 190, 235);

    struct rect text = editor_text_rect(area);
    editor_scroll_to_cursor(text);
    size_t rows = text.height > 0 ? (size_t)(text.height / LINE_HEIGHT) : 0;
    size_t columns = text.width > 0 ? (size_t)(text.width / CHAR_WIDTH) : 0;
    size_t cursor_line = editor_line_of(editor_cursor);
    size_t position_in_text = editor_line_offset(editor_top_line);
    bool last_line_drawn = false;
    for (size_t row = 0; row < rows && !last_line_drawn; row++) {
        size_t line = editor_top_line + row;
        int64_t y = text.y + (int64_t)row * LINE_HEIGHT;
        if (row != 0) {
            size_t end = editor_line_end(position_in_text);
            if (end == editor_length) {
                break;
            }
            position_in_text = end + 1;
        }
        size_t end = editor_line_end(position_in_text);
        last_line_drawn = end == editor_length;

        char number[16];
        number[0] = '\0';
        append_decimal(number, sizeof(number), 0, line + 1);
        int64_t number_width = (int64_t)text_length(number) * CHAR_WIDTH;
        draw_text_clipped(text.x - CHAR_WIDTH - number_width, y + 3,
                          number_width, number,
                          line == cursor_line ? 151 : 90,
                          line == cursor_line ? 190 : 104,
                          line == cursor_line ? 235 : 135);

        /* Selection highlight (a selected line break shows as one cell). */
        size_t selection_start;
        size_t selection_end;
        if (editor_selection(&selection_start, &selection_end)) {
            size_t first = position_in_text + editor_left_column;
            size_t last = end + (end < editor_length ? 1 : 0);
            size_t from = selection_start > first ? selection_start : first;
            size_t to = selection_end < last ? selection_end : last;
            if (from < to) {
                int64_t x = text.x
                    + (int64_t)(from - first) * CHAR_WIDTH;
                int64_t width = (int64_t)(to - from) * CHAR_WIDTH;
                if (x + width > text.x + text.width) {
                    width = text.x + text.width - x;
                }
                if (width > 0) {
                    fill((struct rect){x, y + 1, width, LINE_HEIGHT - 2},
                         52, 84, 140);
                }
            }
        }

        char visible[160];
        size_t count = 0;
        for (size_t i = position_in_text + editor_left_column;
             i < end && count < columns && count + 1 < sizeof(visible); i++) {
            char c = editor_text[i];
            visible[count++] = c == '\t' ? ' '
                : (c >= ' ' && c <= '~') ? c : '.';
        }
        visible[count] = '\0';
        if (count != 0) {
            draw_text_clipped(text.x, y + 3, text.width, visible,
                              230, 238, 255);
        }
    }

    /* Cursor bar. */
    if (focused && !editor_prompt_active
        && cursor_line >= editor_top_line
        && cursor_line < editor_top_line + rows) {
        size_t column = editor_column(editor_cursor);
        if (column >= editor_left_column) {
            int64_t x = text.x
                + (int64_t)(column - editor_left_column) * CHAR_WIDTH;
            int64_t y = text.y
                + (int64_t)(cursor_line - editor_top_line) * LINE_HEIGHT;
            if (x + 2 <= text.x + text.width) {
                fill((struct rect){x, y + 2, 2, LINE_HEIGHT - 4},
                     120, 210, 170);
            }
        }
    }

    /* Footer: save prompt or key hints. */
    int64_t footer_y = area.y + area.height - LINE_HEIGHT + 2;
    if (editor_prompt_active) {
        struct rect line = {area.x + 6, footer_y - 3, area.width - 12,
                            LINE_HEIGHT};
        fill(line, 46, 62, 96);
        char prompt[EDITOR_PATH_CAPACITY + 16];
        size_t length = append_text(prompt, sizeof(prompt), 0, "SAVE AS: ");
        length = append_text(prompt, sizeof(prompt), length, editor_prompt);
        append_text(prompt, sizeof(prompt), length, "_");
        draw_text_clipped(line.x + 6, footer_y, line.width - 12, prompt,
                          230, 238, 255);
    } else {
        draw_text_clipped(area.x + 12, footer_y, area.width - 24,
                          "CTRL+S: SAVE  CTRL+A/C/X/V  SHIFT+ARROWS: SELECT",
                          120, 140, 175);
    }
}

static void editor_click(struct rect area, int32_t x, int32_t y) {
    if (editor_prompt_active) {
        return;
    }
    struct rect text = editor_text_rect(area);
    if (y < text.y || y >= text.y + text.height) {
        return;
    }
    size_t line = editor_top_line + (size_t)((y - text.y) / LINE_HEIGHT);
    int64_t offset = x - text.x + CHAR_WIDTH / 2;
    size_t column = editor_left_column
        + (offset > 0 ? (size_t)(offset / CHAR_WIDTH) : 0);
    size_t start = editor_line_offset(line);
    editor_selecting = false;
    if (editor_line_of(start) != line) {
        /* Clicked below the last line: go to the end of the text. */
        editor_cursor = editor_length;
    } else {
        size_t end = editor_line_end(start);
        editor_cursor = start + (column < end - start ? column : end - start);
    }
    editor_goal_column = editor_column(editor_cursor);
}

/*
 * Files window: browses the whole VFS (initramfs at /, FAT disk at /disk).
 * The listing of the current folder is cached and only reloaded when the
 * folder changes or the window opens, so redraws do not touch the disk.
 */
#define FILES_ENTRY_CAPACITY 256
#define FILES_NAME_CAPACITY 128
#define FILES_PATH_CAPACITY 256
#define FILES_SIDEBAR_WIDTH 150
#define FILES_LIST_TOP 40
#define FILES_DOUBLE_CLICK_TICKS 50

struct files_entry {
    char name[FILES_NAME_CAPACITY];
    bool directory;
    size_t size;
};

static struct files_entry files_entries[FILES_ENTRY_CAPACITY];
static size_t files_count;
static size_t files_skipped;
static char files_path[FILES_PATH_CAPACITY] = "/";
static size_t files_path_length = 1;
/* Selected row and first visible row; row 0 is ".." outside the root. */
static size_t files_selection;
static size_t files_scroll;
static size_t files_last_click_row = (size_t)-1;
static uint64_t files_last_click_tick;

struct files_place {
    const char *label;
    char path[96];
};

/* Sidebar places: the logged-in account's home folders, then the system
   places. Rebuilt on login, logout and account rename. */
static struct files_place files_places[20];
static size_t files_place_count;

static void files_place_add(const char *label, const char *path) {
    if (files_place_count >= sizeof(files_places) / sizeof(files_places[0])) {
        return;
    }
    struct files_place *place = &files_places[files_place_count++];
    place->label = label;
    text_copy(place->path, sizeof(place->path), path);
}

static void files_places_rebuild(void) {
    static const char *const home_labels[] = {
        "DOCUMENTS", "PICTURES", "MUSIC", "VIDEOS", "DOWNLOADS",
    };
    static const char *const home_folders[] = {
        "Documents", "Pictures", "Music", "Videos", "Downloads",
    };
    files_place_count = 0;
    char path[96];
    if (accounts_home_path(NULL, path, sizeof(path)) != 0) {
        files_place_add("HOME", path);
        for (size_t i = 0; i < sizeof(home_labels) / sizeof(home_labels[0]);
             i++) {
            if (accounts_home_path(home_folders[i], path, sizeof(path)) != 0) {
                files_place_add(home_labels[i], path);
            }
        }
    }
    files_place_add("ROOT", "/");
    files_place_add("PROGRAMS", "/bin");
    files_place_add("LIBRARIES", "/lib");
    files_place_add("DISK", "/disk");
    /* Other FAT volumes (files_place_visible hides unmounted ones). */
    static const char *const extra_labels[] = {
        "DISK 2", "DISK 3", "DISK 4", "DISK 5", "DISK 6", "DISK 7", "DISK 8",
    };
    static const char *const extra_paths[] = {
        "/disk2", "/disk3", "/disk4", "/disk5", "/disk6", "/disk7", "/disk8",
    };
    for (size_t i = 0; i < sizeof(extra_labels) / sizeof(extra_labels[0]);
         i++) {
        files_place_add(extra_labels[i], extra_paths[i]);
    }
}

/* Footer prompt for rename / new / delete (see files_key). */
enum files_prompt_kind {
    FILES_PROMPT_NONE,
    FILES_PROMPT_RENAME,
    FILES_PROMPT_NEW_FOLDER,
    FILES_PROMPT_NEW_FILE,
    FILES_PROMPT_DELETE,
};

static enum files_prompt_kind files_prompt_kind;
static char files_prompt[FILES_NAME_CAPACITY];
static size_t files_prompt_length;
/* Entry index the rename/delete prompt acts on. */
static size_t files_prompt_target;
/* Right-click menu. */
static bool files_menu_open;
static bool files_menu_on_item;
static int64_t files_menu_x;
static int64_t files_menu_y;

static void files_draw_prompt(struct rect list);
static void draw_files_menu(void);

static bool files_at_root(void) {
    return files_path_length == 1;
}

static size_t files_row_count(void) {
    return files_count + (files_at_root() ? 0 : 1);
}

static bool files_place_visible(size_t index) {
    const char *path = files_places[index].path;
    struct vfs_metadata metadata;
    return vfs_stat(path, text_length(path), &metadata)
        && metadata.type == VFS_NODE_DIRECTORY;
}

static bool files_name_less(const struct files_entry *a,
                            const struct files_entry *b) {
    if (a->directory != b->directory) {
        return a->directory;
    }
    for (size_t i = 0;; i++) {
        char x = a->name[i];
        char y = b->name[i];
        if (x >= 'a' && x <= 'z') {
            x = (char)(x - 32);
        }
        if (y >= 'a' && y <= 'z') {
            y = (char)(y - 32);
        }
        if (x != y || x == '\0') {
            return x < y;
        }
    }
}

/* Reads the current folder into files_entries (folders first, by name). */
static bool files_load(void) {
    files_count = 0;
    files_skipped = 0;
    size_t count;
    if (!vfs_directory_count(files_path, files_path_length, &count)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(files_path, files_path_length, i,
                                    &entry)) {
            break;
        }
        if (files_count == FILES_ENTRY_CAPACITY
            || entry.name_length >= FILES_NAME_CAPACITY
            || files_path_length + 1 + entry.name_length
                   >= FILES_PATH_CAPACITY) {
            files_skipped++;
            continue;
        }
        struct files_entry item = {
            .directory = entry.type == VFS_NODE_DIRECTORY,
            .size = entry.size,
        };
        for (size_t j = 0; j <= entry.name_length; j++) {
            item.name[j] = entry.name[j];
        }
        size_t at = files_count++;
        while (at > 0 && files_name_less(&item, &files_entries[at - 1])) {
            files_entries[at] = files_entries[at - 1];
            at--;
        }
        files_entries[at] = item;
    }
    return true;
}

static void files_set_path(const char *path, size_t length) {
    for (size_t i = 0; i < length; i++) {
        files_path[i] = path[i];
    }
    files_path[length] = '\0';
    files_path_length = length;
}

/* Address bar: typed path, edited after a click or Ctrl+L. */
static bool files_address_editing;
static char files_address[FILES_PATH_CAPACITY];
static size_t files_address_length;

/* Back/forward history; the oldest entry drops off when full. */
#define FILES_HISTORY 16
struct files_history {
    char paths[FILES_HISTORY][FILES_PATH_CAPACITY];
    size_t count;
};
static struct files_history files_back;
static struct files_history files_forward;

static void files_history_push(struct files_history *history,
                               const char *path, size_t length) {
    if (history->count == FILES_HISTORY) {
        for (size_t i = 0; i + 1 < FILES_HISTORY; i++) {
            text_copy(history->paths[i], FILES_PATH_CAPACITY,
                      history->paths[i + 1]);
        }
        history->count--;
    }
    char *slot = history->paths[history->count++];
    for (size_t i = 0; i < length; i++) {
        slot[i] = path[i];
    }
    slot[length] = '\0';
}

/*
 * Switches to path (absolute, no trailing '/'); stays put on failure and
 * returns false. Does not touch the back/forward history.
 */
static bool files_navigate(const char *path, size_t length) {
    if (length == 0 || length >= FILES_PATH_CAPACITY || path[0] != '/') {
        return false;
    }
    char previous[FILES_PATH_CAPACITY];
    size_t previous_length = files_path_length;
    for (size_t i = 0; i <= previous_length; i++) {
        previous[i] = files_path[i];
    }
    files_set_path(path, length);
    files_selection = 0;
    files_scroll = 0;
    files_last_click_row = (size_t)-1;
    files_prompt_kind = FILES_PROMPT_NONE;
    files_menu_open = false;
    files_address_editing = false;
    bool allowed = permission_ui_allows(path, length, PERMISSION_READ);
    if (allowed && files_load()) {
        return true;
    }
    set_status(allowed ? "FOLDER COULD NOT BE OPENED" : "ACCESS DENIED");
    files_set_path(previous, previous_length);
    if (!files_load()) {
        files_set_path("/", 1);
        (void)files_load();
    }
    return false;
}

static bool files_path_is(const char *path, size_t length) {
    if (length != files_path_length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        if (path[i] != files_path[i]) {
            return false;
        }
    }
    return true;
}

/* User navigation: like files_navigate, but recorded for BACK. */
static bool files_visit(const char *path, size_t length) {
    if (files_path_is(path, length)) {
        return files_navigate(path, length);
    }
    char previous[FILES_PATH_CAPACITY];
    size_t previous_length = files_path_length;
    for (size_t i = 0; i <= previous_length; i++) {
        previous[i] = files_path[i];
    }
    if (!files_navigate(path, length)) {
        return false;
    }
    files_history_push(&files_back, previous, previous_length);
    files_forward.count = 0;
    return true;
}

/* BACK (forward false) or FORWARD (forward true). */
static void files_history_step(bool forward) {
    struct files_history *from = forward ? &files_forward : &files_back;
    struct files_history *to = forward ? &files_back : &files_forward;
    while (from->count != 0) {
        const char *path = from->paths[--from->count];
        char previous[FILES_PATH_CAPACITY];
        size_t previous_length = files_path_length;
        for (size_t i = 0; i <= previous_length; i++) {
            previous[i] = files_path[i];
        }
        /* A folder that is gone (deleted, disk removed) is skipped. */
        if (files_navigate(path, text_length(path))) {
            status_text[0] = '\0';
            files_history_push(to, previous, previous_length);
            return;
        }
    }
}

/*
 * Turns typed text into an absolute path without a trailing '/'. Relative
 * text starts at the current folder; "." and ".." are resolved.
 */
static size_t files_resolve_typed(const char *text, size_t text_length_in,
                                  char *path, size_t capacity) {
    char joined[FILES_PATH_CAPACITY * 2];
    size_t joined_length = 0;
    if (text_length_in == 0 || text[0] != '/') {
        for (size_t i = 0; i < files_path_length; i++) {
            joined[joined_length++] = files_path[i];
        }
        joined[joined_length++] = '/';
    }
    for (size_t i = 0; i < text_length_in
         && joined_length + 1 < sizeof(joined); i++) {
        joined[joined_length++] = text[i] == '\\' ? '/' : text[i];
    }
    size_t length = 0;
    size_t i = 0;
    while (i < joined_length) {
        while (i < joined_length && joined[i] == '/') {
            i++;
        }
        size_t start = i;
        while (i < joined_length && joined[i] != '/') {
            i++;
        }
        size_t part = i - start;
        if (part == 0 || (part == 1 && joined[start] == '.')) {
            continue;
        }
        if (part == 2 && joined[start] == '.' && joined[start + 1] == '.') {
            while (length > 0 && path[length - 1] != '/') {
                length--;
            }
            if (length > 0) {
                length--;
            }
            continue;
        }
        if (length + 1 + part >= capacity) {
            return 0;
        }
        path[length++] = '/';
        for (size_t j = 0; j < part; j++) {
            path[length++] = joined[start + j];
        }
    }
    if (length == 0) {
        path[length++] = '/';
    }
    path[length] = '\0';
    return length;
}

static void files_address_start(void) {
    files_address_editing = true;
    files_menu_open = false;
    files_prompt_kind = FILES_PROMPT_NONE;
    files_address_length = files_path_length;
    for (size_t i = 0; i <= files_path_length; i++) {
        files_address[i] = files_path[i];
    }
}

static void files_run_program(const char *path, size_t length);

/* Enter in the address bar: folders open in Files, files are opened. */
static void files_address_commit(void) {
    char path[FILES_PATH_CAPACITY];
    size_t length = files_resolve_typed(files_address, files_address_length,
                                        path, sizeof(path));
    struct vfs_metadata metadata;
    if (length == 0 || !vfs_stat(path, length, &metadata)) {
        set_status("PATH NOT FOUND");
        return;
    }
    files_address_editing = false;
    if (metadata.type == VFS_NODE_DIRECTORY) {
        (void)files_visit(path, length);
        return;
    }
    /* Show the file's folder, select it, then open it. */
    size_t parent = length;
    while (parent > 1 && path[parent - 1] != '/') {
        parent--;
    }
    size_t name_start = parent;
    if (parent > 1) {
        parent--;
    }
    if (files_visit(path, parent)) {
        for (size_t i = 0; i < files_count; i++) {
            const char *name = files_entries[i].name;
            size_t name_length = text_length(name);
            if (name_length != length - name_start) {
                continue;
            }
            bool same = true;
            for (size_t j = 0; j < name_length; j++) {
                char a = name[j];
                char b = path[name_start + j];
                a = (a >= 'a' && a <= 'z') ? (char)(a - 32) : a;
                b = (b >= 'a' && b <= 'z') ? (char)(b - 32) : b;
                same = same && a == b;
            }
            if (same) {
                files_selection = i + (files_at_root() ? 0 : 1);
                break;
            }
        }
    }
    files_run_program(path, length);
}

static void files_address_key(char character) {
    if (character == KEY_ESCAPE) {
        files_address_editing = false;
    } else if (character == '\n') {
        files_address_commit();
    } else if (line_ctrl_key(files_address, &files_address_length,
                             sizeof(files_address) - 1, false, character)) {
        return;
    } else if (character == '\b') {
        if (files_address_length > 0) {
            files_address[--files_address_length] = '\0';
        }
    } else if (character >= ' ' && character <= '~'
               && files_address_length + 1 < sizeof(files_address)) {
        files_address[files_address_length++] = character;
        files_address[files_address_length] = '\0';
    }
}

/* Reloads the current folder, e.g. when the window opens. */
static void files_refresh(void) {
    size_t selection = files_selection;
    char path[FILES_PATH_CAPACITY];
    size_t length = files_path_length;
    for (size_t i = 0; i <= length; i++) {
        path[i] = files_path[i];
    }
    (void)files_navigate(path, length);
    files_selection = selection;
}

static void files_go_up(void) {
    if (files_at_root()) {
        return;
    }
    size_t length = files_path_length;
    while (length > 1 && files_path[length - 1] != '/') {
        length--;
    }
    size_t child_start = length;
    if (length > 1) {
        length--; /* drop the '/' too, except for the root */
    }
    /* Remember the folder we came from, to select it afterwards. */
    char child[FILES_NAME_CAPACITY];
    size_t child_length = 0;
    for (size_t i = child_start; i < files_path_length
         && child_length + 1 < sizeof(child); i++) {
        child[child_length++] = files_path[i];
    }
    child[child_length] = '\0';
    char path[FILES_PATH_CAPACITY];
    for (size_t i = 0; i < length; i++) {
        path[i] = files_path[i];
    }
    (void)files_visit(path, length);
    for (size_t i = 0; i < files_count; i++) {
        const char *name = files_entries[i].name;
        if (text_length(name) != child_length) {
            continue;
        }
        bool same = true;
        for (size_t j = 0; j < child_length; j++) {
            same = same && name[j] == child[j];
        }
        if (same) {
            files_selection = i + (files_at_root() ? 0 : 1);
            break;
        }
    }
}

/* Builds the full path of entry index into path; returns its length. */
static size_t files_entry_path(size_t index, char *path, size_t capacity) {
    size_t length = 0;
    for (size_t i = 0; i < files_path_length && length + 1 < capacity; i++) {
        path[length++] = files_path[i];
    }
    if (!files_at_root() && length + 1 < capacity) {
        path[length++] = '/';
    }
    const char *name = files_entries[index].name;
    for (size_t i = 0; name[i] != '\0' && length + 1 < capacity; i++) {
        path[length++] = name[i];
    }
    path[length] = '\0';
    return length;
}

/*
 * Opens Notepad (/bin/notepad, an ordinary SDK program from sdk/) on a
 * file, or empty without one. Returns false when it cannot start: too many
 * programs run, or the initramfs has no Notepad. If it starts but cannot
 * run, the built-in editor opens instead (see apps_poll).
 */
static bool notepad_launch(const char *path, size_t length) {
    static const char notepad[] = "/bin/notepad";
    struct initramfs_file file;
    if (!initramfs_find_file(notepad + 1, &file)) {
        return false;
    }
    char arguments[FILES_PATH_CAPACITY + 3];
    size_t used = 0;
    if (length != 0) {
        if (length + 3 > sizeof(arguments)) {
            return false;
        }
        arguments[used++] = '"';
        for (size_t i = 0; i < length; i++) {
            arguments[used++] = path[i];
        }
        arguments[used++] = '"';
    }
    return app_launch(APP_FROM_NOTEPAD, notepad, sizeof(notepad) - 1,
                      arguments, used, path, length) == RUN_STARTED;
}

/* Text files open in Notepad. Programs cannot change system files, so an
   administrator editing one gets the built-in editor, which can. */
static void files_open_text(const char *path, size_t length) {
    struct permission_identity identity = permission_session_identity();
    bool system_file = permission_ui_allows(path, length, PERMISSION_WRITE)
        && !permission_program_allows(&identity, path, length,
                                      PERMISSION_WRITE);
    if (system_file || !notepad_launch(path, length)) {
        (void)editor_open(path, length);
    }
}

/* Starts a program; a file that is not one opens as text when the
   attempt ends (apps_poll). */
static void files_run_program(const char *path, size_t length) {
    enum run_result result = app_launch(APP_FROM_FILES, path, length, NULL,
                                        0, path, length);
    if (result != RUN_STARTED) {
        set_status(run_result_text(result));
    }
}

/* Opens a row: ".." goes up, folders are entered, programs run. */
static void files_open_row(size_t row) {
    if (!files_at_root()) {
        if (row == 0) {
            files_go_up();
            return;
        }
        row--;
    }
    if (row >= files_count) {
        return;
    }
    char path[FILES_PATH_CAPACITY];
    size_t length = files_entry_path(row, path, sizeof(path));
    if (files_entries[row].directory) {
        (void)files_visit(path, length);
    } else {
        files_run_program(path, length);
    }
}

static struct rect files_list_area(struct rect area) {
    int64_t sidebar = area.width >= 480 ? FILES_SIDEBAR_WIDTH : 0;
    return (struct rect){area.x + sidebar, area.y,
                         area.width - sidebar, area.height};
}

static struct rect files_place_rect(struct rect area, size_t slot) {
    return (struct rect){area.x + 6, area.y + 34 + (int64_t)slot * LINE_HEIGHT,
                         FILES_SIDEBAR_WIDTH - 12, LINE_HEIGHT};
}

/* Toolbar: BACK, FORWARD and UP buttons, then the address bar. */
#define FILES_NAV_BUTTONS 3
#define FILES_NAV_WIDTH 30

static struct rect files_nav_rect(struct rect list, size_t index) {
    return (struct rect){list.x + 6 + (int64_t)index * (FILES_NAV_WIDTH + 4),
                         list.y + 6, FILES_NAV_WIDTH, 26};
}

static struct rect files_address_rect(struct rect list) {
    int64_t x = list.x + 6 + FILES_NAV_BUTTONS * (FILES_NAV_WIDTH + 4);
    return (struct rect){x, list.y + 6, list.x + list.width - 6 - x, 26};
}

static bool files_nav_enabled(size_t index) {
    return index == 0 ? files_back.count != 0
         : index == 1 ? files_forward.count != 0
         : !files_at_root();
}

static struct rect files_line_rect(struct rect list, size_t index) {
    return (struct rect){list.x + 6,
                         list.y + FILES_LIST_TOP + (int64_t)index * LINE_HEIGHT,
                         list.width - 12, LINE_HEIGHT};
}

static size_t files_visible_rows(struct rect list) {
    int64_t rows = (list.height - FILES_LIST_TOP - LINE_HEIGHT) / LINE_HEIGHT;
    return rows > 0 ? (size_t)rows : 0;
}

/* Keeps the selection inside the list and scrolled into view. */
static void files_clamp(struct rect list) {
    size_t count = files_row_count();
    if (count == 0) {
        files_selection = 0;
        files_scroll = 0;
        return;
    }
    if (files_selection >= count) {
        files_selection = count - 1;
    }
    size_t visible = files_visible_rows(list);
    if (visible == 0) {
        files_scroll = files_selection;
        return;
    }
    if (files_selection < files_scroll) {
        files_scroll = files_selection;
    } else if (files_selection >= files_scroll + visible) {
        files_scroll = files_selection - visible + 1;
    }
    if (files_scroll + visible > count) {
        files_scroll = count > visible ? count - visible : 0;
    }
}

/* Longest sidebar place that contains the current folder. */
static size_t files_active_place(void) {
    size_t best = 0;
    size_t best_length = 0;
    for (size_t i = 0; i < files_place_count; i++) {
        const char *place = files_places[i].path;
        size_t length = text_length(place);
        if (length > files_path_length || length <= best_length) {
            continue;
        }
        bool prefix = true;
        for (size_t j = 0; j < length; j++) {
            prefix = prefix && files_path[j] == place[j];
        }
        if (prefix && (length == 1 || length == files_path_length
                       || files_path[length] == '/')) {
            best = i;
            best_length = length;
        }
    }
    return best;
}

static void draw_files(struct rect area, bool focused) {
    fill(area, 24, 30, 46);
    struct rect list = files_list_area(area);
    if (list.x != area.x) {
        struct rect sidebar = {area.x, area.y, FILES_SIDEBAR_WIDTH,
                               area.height};
        fill(sidebar, 18, 23, 36);
        draw_text_clipped(area.x + 12, area.y + 12, FILES_SIDEBAR_WIDTH - 24,
                          "PLACES", 151, 190, 235);
        size_t active = files_active_place();
        size_t slot = 0;
        for (size_t i = 0; i < files_place_count; i++) {
            if (!files_place_visible(i)) {
                continue;
            }
            struct rect line = files_place_rect(area, slot++);
            if (i == active) {
                fill(line, 46, 62, 96);
            }
            draw_text_clipped(line.x + 6, line.y + 3, line.width - 12,
                              files_places[i].label, 230, 238, 255);
        }
    }

    /* Toolbar: navigation buttons, then the address bar. */
    static const char *const nav_labels[FILES_NAV_BUTTONS] = {"<", ">", "^"};
    for (size_t i = 0; i < FILES_NAV_BUTTONS; i++) {
        struct rect button = files_nav_rect(list, i);
        bool enabled = files_nav_enabled(i);
        fill(button, enabled ? 46 : 32, enabled ? 62 : 38, enabled ? 96 : 52);
        draw_text_clipped(button.x + (FILES_NAV_WIDTH - CHAR_WIDTH) / 2 + 1,
                          button.y + 4, CHAR_WIDTH, nav_labels[i],
                          enabled ? 230 : 90, enabled ? 238 : 100,
                          enabled ? 255 : 120);
    }
    struct rect bar = files_address_rect(list);
    if (files_address_editing) {
        fill(bar, 110, 160, 255);
        struct rect inner = {bar.x + 2, bar.y + 2, bar.width - 4,
                             bar.height - 4};
        fill(inner, 32, 40, 62);
        /* Show the end of long text, where the caret is. */
        int64_t columns = (inner.width - 12) / CHAR_WIDTH;
        size_t shown_columns = columns > 1 ? (size_t)columns - 1 : 0;
        size_t start = files_address_length > shown_columns
                     ? files_address_length - shown_columns : 0;
        char shown[FILES_PATH_CAPACITY + 2];
        size_t length = 0;
        for (size_t i = start; i < files_address_length; i++) {
            shown[length++] = files_address[i];
        }
        if (focused && (timer_ticks() / 50) % 2 == 0) {
            shown[length++] = '_';
        }
        shown[length] = '\0';
        draw_text_clipped(inner.x + 4, bar.y + 4, inner.width - 8, shown,
                          240, 245, 255);
    } else {
        char count_text[32];
        size_t count_length = append_decimal(count_text, sizeof(count_text), 0,
                                             files_count);
        append_text(count_text, sizeof(count_text), count_length,
                    files_count == 1 ? " ITEM" : " ITEMS");
        int64_t count_width = (int64_t)text_length(count_text) * CHAR_WIDTH;
        fill(bar, 32, 40, 62);
        /* A long path shows its end; that is the folder you are in. */
        int64_t columns = (bar.width - count_width - 24) / CHAR_WIDTH;
        size_t start = columns > 0 && files_path_length > (size_t)columns
                     ? files_path_length - (size_t)columns : 0;
        draw_text_clipped(bar.x + 6, bar.y + 4, bar.width - count_width - 24,
                          files_path + start, 230, 238, 255);
        draw_text_clipped(bar.x + bar.width - count_width - 6, bar.y + 4,
                          count_width, count_text, 151, 190, 235);
    }

    files_clamp(list);
    size_t rows = files_row_count();
    size_t visible = files_visible_rows(list);
    int64_t detail_width = 10 * CHAR_WIDTH;
    for (size_t row = files_scroll; row < rows
         && row < files_scroll + visible; row++) {
        struct rect line = files_line_rect(list, row - files_scroll);
        char text[TERMINAL_COLUMNS];
        char detail[24];
        detail[0] = '\0';
        bool directory = true;
        if (!files_at_root() && row == 0) {
            append_text(text, sizeof(text), 0, "..");
        } else {
            const struct files_entry *entry =
                &files_entries[row - (files_at_root() ? 0 : 1)];
            size_t length = append_text(text, sizeof(text), 0, entry->name);
            directory = entry->directory;
            if (directory) {
                append_text(text, sizeof(text), length, "/");
                append_text(detail, sizeof(detail), 0, "FOLDER");
            } else {
                size_t size = entry->size;
                const char *unit = " B";
                if (size >= 10 * 1024 * 1024) {
                    size /= 1024 * 1024;
                    unit = " MB";
                } else if (size >= 10 * 1024) {
                    size /= 1024;
                    unit = " KB";
                }
                size_t used = append_decimal(detail, sizeof(detail), 0, size);
                append_text(detail, sizeof(detail), used, unit);
            }
        }
        if (row == files_selection) {
            fill(line, focused ? 70 : 52, focused ? 100 : 66,
                 focused ? 160 : 100);
        } else if (row % 2 == 0) {
            fill(line, 30, 38, 58);
        }
        if (directory) {
            draw_text_clipped(line.x + 6, line.y + 3,
                              line.width - detail_width - 18, text,
                              250, 220, 140);
        } else {
            draw_text_clipped(line.x + 6, line.y + 3,
                              line.width - detail_width - 18, text,
                              230, 238, 255);
        }
        draw_text_clipped(line.x + line.width - detail_width - 6, line.y + 3,
                          detail_width, detail, 151, 190, 235);
    }
    if (rows == 0) {
        draw_text_clipped(list.x + 12, list.y + FILES_LIST_TOP + 4,
                          list.width - 24, "THIS FOLDER IS EMPTY",
                          151, 190, 235);
    }
    if (files_prompt_kind != FILES_PROMPT_NONE) {
        files_draw_prompt(list);
        return;
    }
    const char *hint = files_address_editing
        ? "ENTER: GO  ESC: CANCEL  RELATIVE PATHS AND .. WORK"
        : files_skipped != 0
        ? "SOME ITEMS ARE NOT SHOWN (NAME OR COUNT LIMIT)"
        : "ENTER: OPEN  BACKSPACE: UP  CTRL+L: PATH  RIGHT-CLICK: MORE";
    draw_text_clipped(list.x + 12, list.y + list.height - LINE_HEIGHT + 2,
                      list.width - 24, hint, 120, 140, 175);
}

static void draw_system_info(struct rect area) {
    fill(area, 24, 30, 46);
    char memory[48] = "USABLE MEMORY: ";
    size_t length = append_decimal(memory, sizeof(memory), 15,
                                   memory_total_pages() * 4096 / (1024 * 1024));
    append_text(memory, sizeof(memory), length, " MB");
    char uptime[48] = "UPTIME: ";
    length = append_decimal(uptime, sizeof(uptime), 8, timer_ticks() / 100);
    append_text(uptime, sizeof(uptime), length, " SECONDS");
    char screen[48] = "SCREEN: ";
    length = append_decimal(screen, sizeof(screen), 8, console_screen_width());
    length = append_text(screen, sizeof(screen), length, " X ");
    append_decimal(screen, sizeof(screen), length, console_screen_height());

    const char *lines[] = {
        "OPOS X86-64", memory, uptime, screen, "PIT TIMER: 100 HZ",
        tiled_layout ? "LAYOUT: DWINDLE TILING" : "LAYOUT: FLOATING",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        draw_text_clipped(area.x + 16, area.y + 16 + (int64_t)i * 30,
                          area.width - 32, lines[i],
                          i == 0 ? 151 : 230, i == 0 ? 190 : 238, 255);
    }
}

static void draw_welcome(struct rect area) {
    fill(area, 24, 30, 46);
    const char *lines[] = {
        "WELCOME TO OPOS",
        "",
        "SUPER: START MENU (ARROWS, ENTER, ESC)",
        "TERMINAL COMMAND SHELL: OPOS SHELL",
        "CTRL+REBOOT: BOOT INTO SHELL ONCE",
        "SUPER+ARROWS: FOCUS WINDOW",
        "SUPER+ENTER: TERMINAL  SUPER+E: FILES",
        "SUPER+Q: CLOSE  SUPER+T: TILE/FLOAT",
        "SUPER+TAB: CYCLE FOCUS",
        "CTRL+SHIFT+ESC: TASK MANAGER",
        "DRAG TITLE BARS IN FLOATING MODE",
        "LINUX ELF PROGRAMS RUN FROM FILES",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        draw_text_clipped(area.x + 16, area.y + 16 + (int64_t)i * 26,
                          area.width - 32, lines[i],
                          i == 0 ? 245 : 200, i == 0 ? 248 : 214, 255);
    }
}

enum setting_row_kind {
    ROW_TOGGLE,
    ROW_DATE_FORMAT,
    ROW_YEAR,
    ROW_MONTH,
    ROW_DAY,
    ROW_HOUR,
    ROW_MINUTE,
    ROW_RESOLUTION,
};

struct setting_row {
    const char *label;
    enum setting_row_kind kind;
    uint8_t setting; /* ROW_TOGGLE only */
};

static const struct setting_row setting_rows[] = {
    {"OPEN DESKTOP ON BOOT", ROW_TOGGLE, SETTING_AUTOSTART_DESKTOP},
    {"COLORED TERMINAL MESSAGES", ROW_TOGGLE, SETTING_TERMINAL_COLORS},
    {"12-HOUR CLOCK", ROW_TOGGLE, SETTING_CLOCK_12H},
    {"DATE FORMAT", ROW_DATE_FORMAT, 0},
    {"YEAR", ROW_YEAR, 0},
    {"MONTH", ROW_MONTH, 0},
    {"DAY", ROW_DAY, 0},
    {"HOUR", ROW_HOUR, 0},
    {"MINUTE", ROW_MINUTE, 0},
    {"RESOLUTION", ROW_RESOLUTION, 0},
};
#define SETTING_ROW_COUNT (sizeof(setting_rows) / sizeof(setting_rows[0]))
#define SETTING_VALUE_WIDTH (14 * CHAR_WIDTH + 12)

static const char *const date_format_names[DATE_FORMAT_COUNT] = {
    "YYYY-MM-DD", "DD.MM.YYYY", "MM/DD/YYYY", "DD/MM/YYYY",
};

static struct rect setting_row_rect(struct rect area, size_t index) {
    return (struct rect){area.x + 8, area.y + 44 + (int64_t)index * 36,
                         area.width - 16, 32};
}

static struct rect setting_value_rect(struct rect row) {
    return (struct rect){row.x + 8, row.y + 6, SETTING_VALUE_WIDTH, 20};
}

/* Shown when the RTC holds garbage, so the user can still set it. */
static const struct rtc_time rtc_fallback = {2000, 1, 1, 0, 0, 0};

static size_t append_date(char *buffer, size_t capacity, size_t length,
                          const struct rtc_time *time) {
    switch (settings_date_format()) {
    case DATE_FORMAT_DMY_DOT:
        length = append_two_digits(buffer, capacity, length, time->day);
        length = append_text(buffer, capacity, length, ".");
        length = append_two_digits(buffer, capacity, length, time->month);
        length = append_text(buffer, capacity, length, ".");
        return append_decimal(buffer, capacity, length, time->year);
    case DATE_FORMAT_MDY_SLASH:
        length = append_two_digits(buffer, capacity, length, time->month);
        length = append_text(buffer, capacity, length, "/");
        length = append_two_digits(buffer, capacity, length, time->day);
        length = append_text(buffer, capacity, length, "/");
        return append_decimal(buffer, capacity, length, time->year);
    case DATE_FORMAT_DMY_SLASH:
        length = append_two_digits(buffer, capacity, length, time->day);
        length = append_text(buffer, capacity, length, "/");
        length = append_two_digits(buffer, capacity, length, time->month);
        length = append_text(buffer, capacity, length, "/");
        return append_decimal(buffer, capacity, length, time->year);
    default:
        length = append_decimal(buffer, capacity, length, time->year);
        length = append_text(buffer, capacity, length, "-");
        length = append_two_digits(buffer, capacity, length, time->month);
        length = append_text(buffer, capacity, length, "-");
        return append_two_digits(buffer, capacity, length, time->day);
    }
}

/* Appends an hour as 01-12 or 00-23, following the 12-hour setting. */
static size_t append_hour(char *buffer, size_t capacity, size_t length,
                          uint8_t hour) {
    if (settings_get(SETTING_CLOCK_12H)) {
        hour = (uint8_t)(hour % 12 == 0 ? 12 : hour % 12);
    }
    return append_two_digits(buffer, capacity, length, hour);
}

static size_t append_time(char *buffer, size_t capacity, size_t length,
                          const struct rtc_time *time) {
    length = append_hour(buffer, capacity, length, time->hour);
    length = append_text(buffer, capacity, length, ":");
    length = append_two_digits(buffer, capacity, length, time->minute);
    length = append_text(buffer, capacity, length, ":");
    length = append_two_digits(buffer, capacity, length, time->second);
    if (settings_get(SETTING_CLOCK_12H)) {
        length = append_text(buffer, capacity, length,
                             time->hour >= 12 ? " PM" : " AM");
    }
    return length;
}

static void setting_value_text(const struct setting_row *row,
                               const struct rtc_time *now,
                               char *text, size_t capacity) {
    size_t length = append_text(text, capacity, 0, "< ");
    switch (row->kind) {
    case ROW_DATE_FORMAT:
        length = append_text(text, capacity, length,
                             date_format_names[settings_date_format()]);
        break;
    case ROW_YEAR:
        length = append_decimal(text, capacity, length, now->year);
        break;
    case ROW_MONTH:
        length = append_two_digits(text, capacity, length, now->month);
        break;
    case ROW_DAY:
        length = append_two_digits(text, capacity, length, now->day);
        break;
    case ROW_HOUR:
        length = append_hour(text, capacity, length, now->hour);
        if (settings_get(SETTING_CLOCK_12H)) {
            length = append_text(text, capacity, length,
                                 now->hour >= 12 ? " PM" : " AM");
        }
        break;
    case ROW_MINUTE:
        length = append_two_digits(text, capacity, length, now->minute);
        break;
    case ROW_RESOLUTION: {
        uint32_t width = 0, height = 0;
        size_t mode = svga_current_mode();
        if (vmware_autofit_enabled()) {
            length = append_text(text, capacity, length, "AUTOFIT");
        } else if (mode == 0) {
            length = append_text(text, capacity, length, "DEFAULT");
        } else if (svga_mode_size(mode, &width, &height)) {
            length = append_decimal(text, capacity, length, width);
            length = append_text(text, capacity, length, "x");
            length = append_decimal(text, capacity, length, height);
        }
        break;
    }
    default:
        break;
    }
    append_text(text, capacity, length, " >");
}

static void draw_settings(struct rect area, bool focused) {
    fill(area, 24, 30, 46);
    draw_text_clipped(area.x + 16, area.y + 14, area.width - 32,
                      "ENTER: TOGGLE  LEFT/RIGHT: CHANGE", 151, 190, 235);
    struct rtc_time now;
    if (!rtc_read(&now)) {
        now = rtc_fallback;
    }
    int64_t bottom = area.y + area.height;
    for (size_t i = 0; i < SETTING_ROW_COUNT; i++) {
        struct rect row = setting_row_rect(area, i);
        if (row.y + row.height > bottom) {
            break;
        }
        if (focused && i == settings_selection) {
            fill(row, 43, 56, 84);
        }
        struct rect value = setting_value_rect(row);
        if (setting_rows[i].kind == ROW_TOGGLE) {
            bool on = settings_get(setting_rows[i].setting);
            fill(value, on ? 70 : 60, on ? 160 : 66, on ? 110 : 88);
            draw_text_clipped(value.x + 6, value.y + 1, value.width - 12,
                              on ? "ON" : "OFF", 255, 255, 255);
        } else {
            char text[32];
            setting_value_text(&setting_rows[i], &now, text, sizeof(text));
            fill(value, 52, 66, 100);
            draw_text_clipped(value.x + 6, value.y + 1, value.width - 12,
                              text, 255, 255, 255);
        }
        int64_t label_x = value.x + value.width + 16;
        const char *label = setting_rows[i].label;
        if (setting_rows[i].kind == ROW_RESOLUTION && !svga_available()) {
            label = "RESOLUTION (NEEDS VMWARE SVGA ADAPTER)";
        }
        draw_text_clipped(label_x, row.y + 7, row.x + row.width - label_x - 8,
                          label, 230, 238, 255);
    }
    struct rect footer = setting_row_rect(area, SETTING_ROW_COUNT);
    if (footer.y + CHAR_HEIGHT + 12 <= bottom) {
        bool disk = fat16_system_mounted();
        draw_text_clipped(area.x + 16, footer.y + 12, area.width - 32,
                          disk ? "SAVED TO OPOS.CFG ON THE DISK"
                               : "NO FAT DISK: SAVED IN CMOS ONLY",
                          disk ? 110 : 240, disk ? 140 : 205,
                          disk ? 180 : 120);
    }
}

static bool leap_year(uint16_t year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static uint8_t days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[12] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
    return month == 2 && leap_year(year) ? 29 : days[month - 1];
}

/* Steps value by delta, wrapping inside [low, high]. */
static int wrap_step(int value, int delta, int low, int high) {
    int span = high - low + 1;
    return low + ((value - low + delta) % span + span) % span;
}

/*
 * Steps through DEFAULT, the fixed sizes and (in VMware) AUTOFIT, skipping
 * sizes the adapter cannot show. Applies and saves the choice. Choice
 * index svga_mode_count() stands for AUTOFIT.
 */
static void settings_change_resolution(int delta) {
    if (!svga_available()) {
        set_status("RESOLUTION NEEDS THE VMWARE SVGA ADAPTER");
        return;
    }
    int count = (int)svga_mode_count();
    int choices = count + 1;
    int current = vmware_autofit_enabled() ? count
                : (int)svga_current_mode(); /* custom size reads as -1 */
    int choice = current;
    for (int step = 0; step < choices; step++) {
        choice = wrap_step(choice, delta < 0 ? -1 : 1, 0, choices - 1);
        if (choice == count ? vmware_autofit_available()
                            : svga_mode_supported((size_t)choice)) {
            break;
        }
    }
    if (choice == current) {
        return;
    }
    if (choice == count) {
        vmware_set_autofit(true);
        settings_set_display_mode(VMWARE_DISPLAY_AUTOFIT);
    } else {
        vmware_set_autofit(false);
        if (!svga_set_mode((size_t)choice)) {
            set_status("DISPLAY MODE COULD NOT BE SET");
            return;
        }
        settings_set_display_mode((uint8_t)choice);
    }
    windows_fit_screen();
}

/* Changes a setting row by delta (+1/-1). Toggles ignore the direction. */
static void settings_change(size_t index, int delta) {
    const struct setting_row *row = &setting_rows[index];
    if (row->kind == ROW_TOGGLE) {
        settings_set(row->setting, !settings_get(row->setting));
        return;
    }
    if (row->kind == ROW_DATE_FORMAT) {
        settings_set_date_format((enum date_format)wrap_step(
            settings_date_format(), delta, 0, DATE_FORMAT_COUNT - 1));
        return;
    }
    if (row->kind == ROW_RESOLUTION) {
        settings_change_resolution(delta);
        return;
    }
    struct rtc_time now;
    if (!rtc_read(&now)) {
        now = rtc_fallback;
    }
    if (now.year < 2000 || now.year > 2099) {
        now.year = 2000;
    }
    switch (row->kind) {
    case ROW_YEAR:
        now.year = (uint16_t)wrap_step(now.year, delta, 2000, 2099);
        break;
    case ROW_MONTH:
        now.month = (uint8_t)wrap_step(now.month, delta, 1, 12);
        break;
    case ROW_DAY:
        now.day = (uint8_t)wrap_step(now.day, delta, 1,
                                     days_in_month(now.year, now.month));
        break;
    case ROW_HOUR:
        now.hour = (uint8_t)wrap_step(now.hour, delta, 0, 23);
        break;
    case ROW_MINUTE:
        now.minute = (uint8_t)wrap_step(now.minute, delta, 0, 59);
        now.second = 0;
        break;
    default:
        return;
    }
    uint8_t last_day = days_in_month(now.year, now.month);
    if (now.day > last_day) {
        now.day = last_day;
    }
    (void)rtc_write(&now);
}

static void draw_scene(void);
static void draw_button(struct rect area, const char *label, bool active);

/* ----------------------------------------------------------- text fields */

/* One-line text input; secret fields draw '*' per character. */
struct text_field {
    char text[ACCOUNT_PASSWORD_MAX + 1];
    size_t length;
    size_t limit;
    bool secret;
};

static void field_reset(struct text_field *field, size_t limit, bool secret) {
    for (size_t i = 0; i < sizeof(field->text); i++) {
        field->text[i] = '\0';
    }
    field->length = 0;
    field->limit = limit < ACCOUNT_PASSWORD_MAX ? limit : ACCOUNT_PASSWORD_MAX;
    field->secret = secret;
}

static void field_set(struct text_field *field, const char *text) {
    field->length = 0;
    while (text[field->length] != '\0' && field->length < field->limit) {
        field->text[field->length] = text[field->length];
        field->length++;
    }
    field->text[field->length] = '\0';
}

/* Returns true when the key edited the field. */
static bool field_key(struct text_field *field, char character) {
    if (line_ctrl_key(field->text, &field->length, field->limit,
                      field->secret, character)) {
        return true;
    }
    if (character == '\b') {
        if (field->length != 0) {
            field->text[--field->length] = '\0';
        }
        return true;
    }
    if (character >= ' ' && character <= '~' && field->length < field->limit) {
        field->text[field->length++] = character;
        field->text[field->length] = '\0';
        return true;
    }
    return false;
}

static void draw_field(struct rect area, const struct text_field *field,
                       bool focused) {
    fill(area, focused ? 110 : 58, focused ? 160 : 66, focused ? 255 : 88);
    fill((struct rect){area.x + 2, area.y + 2, area.width - 4,
                       area.height - 4}, 32, 40, 62);
    char shown[ACCOUNT_PASSWORD_MAX + 2];
    size_t length = 0;
    for (; length < field->length; length++) {
        shown[length] = field->secret ? '*' : field->text[length];
    }
    if (focused && (timer_ticks() / 50) % 2 == 0) {
        shown[length++] = '_';
    }
    shown[length] = '\0';
    draw_text_clipped(area.x + 8, area.y + (area.height - CHAR_HEIGHT) / 2,
                      area.width - 16, shown, 240, 245, 255);
}

static void draw_text_button(struct rect area, const char *label,
                             bool enabled) {
    if (enabled) {
        draw_button(area, label, false);
        return;
    }
    fill(area, 43, 48, 60);
    draw_text_clipped(area.x + 10, area.y + (area.height - CHAR_HEIGHT) / 2,
                      area.width - 20, label, 110, 118, 135);
}

/* ---------------------------------------------------------- login screen */

/*
 * Shown instead of the desktop when the disk has several accounts or a
 * password-protected one (see accounts_login_required).
 */
#define LOGIN_PANEL_WIDTH 560
#define LOGIN_ROW_HEIGHT 36

static bool login_active;
static size_t login_selection;
static struct text_field login_password;
static const char *login_message = "";

static void desktop_session_start(void);
static void desktop_session_end(void);

static struct rect login_panel_rect(void) {
    int64_t height = 150 + (int64_t)accounts_count() * LOGIN_ROW_HEIGHT
        + 2 * 48 + 40;
    int64_t width = LOGIN_PANEL_WIDTH < screen_width() - 20
        ? LOGIN_PANEL_WIDTH : screen_width() - 20;
    return (struct rect){(screen_width() - width) / 2,
                         (screen_height() - height) / 2, width, height};
}

static struct rect login_row_rect(size_t index) {
    struct rect panel = login_panel_rect();
    return (struct rect){panel.x + 24,
                         panel.y + 84 + (int64_t)index * LOGIN_ROW_HEIGHT,
                         panel.width - 48, LOGIN_ROW_HEIGHT - 4};
}

static struct rect login_field_rect(void) {
    struct rect row = login_row_rect(accounts_count());
    return (struct rect){row.x, row.y + 36, row.width, 36};
}

static struct rect login_button_rect(size_t index) {
    /* 0: LOG IN, 1: RESTART, 2: SHUT DOWN */
    struct rect panel = login_panel_rect();
    int64_t width = (panel.width - 48 - 2 * 12) / 3;
    return (struct rect){panel.x + 24 + (int64_t)index * (width + 12),
                         panel.y + panel.height - 56, width, 36};
}

static void login_show(void) {
    login_active = true;
    login_selection = 0;
    login_message = "";
    field_reset(&login_password, ACCOUNT_PASSWORD_MAX, true);
}

static void login_select(size_t index) {
    if (index < accounts_count() && index != login_selection) {
        login_selection = index;
        field_reset(&login_password, ACCOUNT_PASSWORD_MAX, true);
        login_message = "";
    }
}

static void login_attempt(void) {
    if (accounts_login(login_selection, login_password.text)) {
        field_reset(&login_password, ACCOUNT_PASSWORD_MAX, true);
        login_active = false;
        desktop_session_start();
    } else {
        field_reset(&login_password, ACCOUNT_PASSWORD_MAX, true);
        login_message = "WRONG PASSWORD";
    }
}

static void draw_login(void) {
    struct rect panel = login_panel_rect();
    fill(panel, 110, 160, 255);
    fill((struct rect){panel.x + 2, panel.y + 2, panel.width - 4,
                       panel.height - 4}, 24, 30, 46);
    draw_text_clipped(panel.x + 24, panel.y + 20, panel.width - 48,
                      "OPOS", 151, 190, 255);
    draw_text_clipped(panel.x + 24, panel.y + 50, panel.width - 48,
                      "CHOOSE YOUR ACCOUNT", 200, 214, 255);
    for (size_t i = 0; i < accounts_count(); i++) {
        struct rect row = login_row_rect(i);
        bool selected = i == login_selection;
        fill(row, selected ? 70 : 32, selected ? 100 : 40,
             selected ? 160 : 62);
        draw_text_clipped(row.x + 10, row.y + (row.height - CHAR_HEIGHT) / 2,
                          row.width - 20 - 9 * CHAR_WIDTH, accounts_name(i),
                          240, 245, 255);
        if (accounts_has_password(i)) {
            draw_text_clipped(row.x + row.width - 9 * CHAR_WIDTH - 10,
                              row.y + (row.height - CHAR_HEIGHT) / 2,
                              9 * CHAR_WIDTH, "PASSWORD", 170, 185, 215);
        }
    }
    struct rect field = login_field_rect();
    if (accounts_has_password(login_selection)) {
        draw_text_clipped(field.x, field.y - 28, field.width,
                          "PASSWORD:", 200, 214, 255);
        draw_field(field, &login_password, true);
    }
    draw_text_clipped(field.x, field.y + 44, field.width, login_message,
                      255, 120, 120);
    draw_button(login_button_rect(0), "LOG IN", true);
    draw_button(login_button_rect(1), "RESTART", false);
    draw_button(login_button_rect(2), "SHUT DOWN", false);
}

static void login_key(char character) {
    if (character == KEY_UP && login_selection > 0) {
        login_select(login_selection - 1);
    } else if (character == KEY_DOWN) {
        login_select(login_selection + 1);
    } else if (character == '\n') {
        login_attempt();
    } else if (character == KEY_ESCAPE) {
        field_reset(&login_password, ACCOUNT_PASSWORD_MAX, true);
    } else if (accounts_has_password(login_selection)) {
        login_message = "";
        (void)field_key(&login_password, character);
    }
}

static void login_press(int32_t x, int32_t y) {
    for (size_t i = 0; i < accounts_count(); i++) {
        if (rect_contains(login_row_rect(i), x, y)) {
            login_select(i);
            return;
        }
    }
    if (rect_contains(login_button_rect(0), x, y)) {
        login_attempt();
    } else if (rect_contains(login_button_rect(1), x, y)) {
        power_reboot();
    } else if (rect_contains(login_button_rect(2), x, y)) {
        if (!power_shutdown()) {
            login_message = "ACPI SHUTDOWN UNAVAILABLE";
        }
    }
}

/* ------------------------------------------------------ accounts window */

enum accounts_action {
    ACCOUNTS_NEW,
    ACCOUNTS_RENAME,
    ACCOUNTS_PASSWORD,
    ACCOUNTS_ADMIN,
    ACCOUNTS_DELETE,
    ACCOUNTS_ACTION_COUNT
};

static const char *const accounts_action_labels[ACCOUNTS_ACTION_COUNT] = {
    "NEW", "RENAME", "PASSWORD", "ADMIN", "DELETE",
};

enum accounts_prompt {
    ACCOUNTS_PROMPT_NONE,
    ACCOUNTS_PROMPT_NEW_NAME,
    ACCOUNTS_PROMPT_NEW_PASSWORD,
    ACCOUNTS_PROMPT_RENAME,
    ACCOUNTS_PROMPT_PASSWORD,
    ACCOUNTS_PROMPT_DELETE,
};

static size_t accounts_selection;
static enum accounts_prompt accounts_prompt;
static struct text_field accounts_field;
static char accounts_new_name[ACCOUNT_NAME_MAX + 1];
static const char *accounts_message = "";
static bool accounts_message_error;

static bool accounts_live_session(void) {
    return installer_available();
}

/* Administrators manage every account; others only their own. With no
   accounts yet, anyone may create the first (administrator) account. */
static bool accounts_action_allowed(enum accounts_action action) {
    if (accounts_live_session() || !fat16_system_mounted()) {
        return false;
    }
    size_t me = accounts_current();
    bool admin = me != ACCOUNT_NONE && accounts_is_admin(me);
    bool self = me != ACCOUNT_NONE && accounts_selection == me;
    bool selected = accounts_selection < accounts_count();
    switch (action) {
    case ACCOUNTS_NEW:
        return accounts_count() == 0 || admin;
    case ACCOUNTS_RENAME:
    case ACCOUNTS_PASSWORD:
        return selected && (admin || self);
    case ACCOUNTS_ADMIN:
        return selected && admin;
    case ACCOUNTS_DELETE:
        return selected && admin && !self;
    default:
        return false;
    }
}

static void accounts_report(enum account_result result, const char *done) {
    accounts_message_error = result != ACCOUNT_OK;
    accounts_message = result == ACCOUNT_OK ? done
                                            : accounts_result_text(result);
}

static void accounts_start(enum accounts_action action) {
    if (!accounts_action_allowed(action)) {
        return;
    }
    accounts_message = "";
    switch (action) {
    case ACCOUNTS_NEW:
        accounts_prompt = ACCOUNTS_PROMPT_NEW_NAME;
        field_reset(&accounts_field, ACCOUNT_NAME_MAX, false);
        break;
    case ACCOUNTS_RENAME:
        accounts_prompt = ACCOUNTS_PROMPT_RENAME;
        field_reset(&accounts_field, ACCOUNT_NAME_MAX, false);
        field_set(&accounts_field, accounts_name(accounts_selection));
        break;
    case ACCOUNTS_PASSWORD:
        accounts_prompt = ACCOUNTS_PROMPT_PASSWORD;
        field_reset(&accounts_field, ACCOUNT_PASSWORD_MAX, true);
        break;
    case ACCOUNTS_ADMIN:
        accounts_report(accounts_set_admin(accounts_selection,
                                           !accounts_is_admin(
                                               accounts_selection)),
                        "ADMINISTRATOR SETTING CHANGED");
        break;
    case ACCOUNTS_DELETE:
        accounts_prompt = ACCOUNTS_PROMPT_DELETE;
        break;
    default:
        break;
    }
}

static void accounts_commit(void) {
    enum accounts_prompt prompt = accounts_prompt;
    accounts_prompt = ACCOUNTS_PROMPT_NONE;
    switch (prompt) {
    case ACCOUNTS_PROMPT_NEW_NAME:
        if (!accounts_name_valid(accounts_field.text)) {
            accounts_report(ACCOUNT_BAD_NAME, "");
            accounts_prompt = ACCOUNTS_PROMPT_NEW_NAME;
            return;
        }
        if (accounts_find(accounts_field.text) != ACCOUNT_NONE) {
            accounts_report(ACCOUNT_NAME_TAKEN, "");
            accounts_prompt = ACCOUNTS_PROMPT_NEW_NAME;
            return;
        }
        text_copy(accounts_new_name, sizeof(accounts_new_name),
                  accounts_field.text);
        accounts_prompt = ACCOUNTS_PROMPT_NEW_PASSWORD;
        field_reset(&accounts_field, ACCOUNT_PASSWORD_MAX, true);
        return;
    case ACCOUNTS_PROMPT_NEW_PASSWORD: {
        bool first = accounts_count() == 0;
        enum account_result result = accounts_create(
            accounts_new_name, accounts_field.text, false);
        accounts_report(result, "ACCOUNT CREATED");
        if (result == ACCOUNT_OK) {
            accounts_selection = accounts_find(accounts_new_name);
            /* The first account starts a session right away. */
            if (first && accounts_login(accounts_selection, accounts_field.text)) {
                desktop_session_start();
            }
        }
        break;
    }
    case ACCOUNTS_PROMPT_RENAME: {
        bool self = accounts_selection == accounts_current();
        accounts_report(accounts_rename(accounts_selection,
                                        accounts_field.text),
                        "ACCOUNT RENAMED");
        if (self) {
            files_places_rebuild();
        }
        break;
    }
    case ACCOUNTS_PROMPT_PASSWORD:
        accounts_report(accounts_set_password(accounts_selection,
                                              accounts_field.text),
                        accounts_field.length == 0 ? "PASSWORD REMOVED"
                                                   : "PASSWORD CHANGED");
        break;
    default:
        break;
    }
    field_reset(&accounts_field, ACCOUNT_PASSWORD_MAX, true);
}

static void accounts_key(char character) {
    if (accounts_prompt == ACCOUNTS_PROMPT_DELETE) {
        if (character == 'y' || character == 'Y') {
            accounts_prompt = ACCOUNTS_PROMPT_NONE;
            accounts_report(accounts_delete(accounts_selection),
                            "ACCOUNT DELETED (FILES KEPT)");
            if (accounts_selection >= accounts_count()
                && accounts_selection != 0) {
                accounts_selection--;
            }
        } else if (character == 'n' || character == 'N'
                   || character == KEY_ESCAPE) {
            accounts_prompt = ACCOUNTS_PROMPT_NONE;
        }
        return;
    }
    if (accounts_prompt != ACCOUNTS_PROMPT_NONE) {
        if (character == '\n') {
            accounts_commit();
        } else if (character == KEY_ESCAPE) {
            accounts_prompt = ACCOUNTS_PROMPT_NONE;
            field_reset(&accounts_field, ACCOUNT_PASSWORD_MAX, true);
        } else {
            (void)field_key(&accounts_field, character);
        }
        return;
    }
    if (character == KEY_UP && accounts_selection > 0) {
        accounts_selection--;
    } else if (character == KEY_DOWN
               && accounts_selection + 1 < accounts_count()) {
        accounts_selection++;
    } else if (character == 'n' || character == 'N') {
        accounts_start(ACCOUNTS_NEW);
    } else if (character == 'r' || character == 'R') {
        accounts_start(ACCOUNTS_RENAME);
    } else if (character == 'p' || character == 'P') {
        accounts_start(ACCOUNTS_PASSWORD);
    } else if (character == 'a' || character == 'A') {
        accounts_start(ACCOUNTS_ADMIN);
    } else if (character == KEY_DELETE) {
        accounts_start(ACCOUNTS_DELETE);
    }
}

static struct rect accounts_row_rect(struct rect area, size_t index) {
    return (struct rect){area.x + 16, area.y + 70 + (int64_t)index * 34,
                         area.width - 32, 30};
}

static struct rect accounts_action_rect(struct rect area, size_t index) {
    int64_t width = (area.width - 32 - 4 * 8) / ACCOUNTS_ACTION_COUNT;
    return (struct rect){area.x + 16 + (int64_t)index * (width + 8),
                         area.y + area.height - 48, width, 34};
}

static struct rect accounts_field_rect(struct rect area) {
    return (struct rect){area.x + 16, area.y + area.height - 128,
                         area.width - 32, 34};
}

static void draw_accounts(struct rect area, bool focused) {
    fill(area, 24, 30, 46);
    char heading[64] = "SIGNED IN AS ";
    size_t me = accounts_current();
    if (me != ACCOUNT_NONE) {
        append_text(heading, sizeof(heading), text_length(heading),
                    accounts_name(me));
    } else {
        text_copy(heading, sizeof(heading), "NOT SIGNED IN");
    }
    draw_text_clipped(area.x + 16, area.y + 16, area.width - 32, heading,
                      151, 190, 255);
    if (accounts_live_session()) {
        draw_text_clipped(area.x + 16, area.y + 70, area.width - 32,
                          "ACCOUNTS ARE MANAGED ON AN INSTALLED SYSTEM.",
                          200, 214, 255);
        return;
    }
    if (!fat16_system_mounted()) {
        draw_text_clipped(area.x + 16, area.y + 70, area.width - 32,
                          "NO DISK: ACCOUNTS NEED A FORMATTED DISK.",
                          200, 214, 255);
        return;
    }
    if (accounts_count() == 0) {
        draw_text_clipped(area.x + 16, area.y + 70, area.width - 32,
                          "NO ACCOUNTS YET. CHOOSE NEW TO CREATE ONE;",
                          200, 214, 255);
        draw_text_clipped(area.x + 16, area.y + 98, area.width - 32,
                          "THE FIRST ACCOUNT IS AN ADMINISTRATOR.",
                          200, 214, 255);
    }
    for (size_t i = 0; i < accounts_count(); i++) {
        struct rect row = accounts_row_rect(area, i);
        bool selected = i == accounts_selection;
        fill(row, selected ? (focused ? 70 : 46) : 32,
             selected ? (focused ? 100 : 62) : 40,
             selected ? (focused ? 160 : 96) : 62);
        char line[96] = "";
        size_t length = append_text(line, sizeof(line), 0, accounts_name(i));
        if (i == me) {
            length = append_text(line, sizeof(line), length, " (YOU)");
        }
        draw_text_clipped(row.x + 10, row.y + 4, row.width / 2, line,
                          240, 245, 255);
        char details[48] = "";
        size_t details_length = append_text(
            details, sizeof(details), 0,
            accounts_is_admin(i) ? "ADMIN" : "STANDARD");
        append_text(details, sizeof(details), details_length,
                    accounts_has_password(i) ? ", PASSWORD" : ", NO PASSWORD");
        draw_text_clipped(row.x + row.width / 2, row.y + 4,
                          row.width / 2 - 10, details, 170, 185, 215);
    }

    struct rect field = accounts_field_rect(area);
    const char *label = NULL;
    switch (accounts_prompt) {
    case ACCOUNTS_PROMPT_NEW_NAME:
        label = "NEW ACCOUNT NAME (ENTER: NEXT, ESC: CANCEL):";
        break;
    case ACCOUNTS_PROMPT_NEW_PASSWORD:
        label = "PASSWORD (EMPTY: NONE):";
        break;
    case ACCOUNTS_PROMPT_RENAME:
        label = "NEW NAME:";
        break;
    case ACCOUNTS_PROMPT_PASSWORD:
        label = "NEW PASSWORD (EMPTY: REMOVE PASSWORD):";
        break;
    case ACCOUNTS_PROMPT_DELETE:
        draw_text_clipped(field.x, field.y + 6, field.width,
                          "DELETE THIS ACCOUNT? FILES ARE KEPT. (Y/N)",
                          240, 205, 120);
        break;
    default:
        break;
    }
    if (label != NULL) {
        draw_text_clipped(field.x, field.y - 28, field.width, label,
                          200, 214, 255);
        draw_field(field, &accounts_field, focused);
    }
    if (accounts_message[0] != '\0') {
        draw_text_clipped(field.x, field.y + 42, field.width,
                          accounts_message,
                          accounts_message_error ? 255 : 150,
                          accounts_message_error ? 120 : 220,
                          accounts_message_error ? 120 : 150);
    }
    for (size_t i = 0; i < ACCOUNTS_ACTION_COUNT; i++) {
        draw_text_button(accounts_action_rect(area, i),
                         accounts_action_labels[i],
                         accounts_prompt == ACCOUNTS_PROMPT_NONE
                         && accounts_action_allowed(
                             (enum accounts_action)i));
    }
}

static void accounts_click(struct rect area, int32_t x, int32_t y) {
    if (accounts_prompt != ACCOUNTS_PROMPT_NONE) {
        return;
    }
    for (size_t i = 0; i < accounts_count(); i++) {
        if (rect_contains(accounts_row_rect(area, i), x, y)) {
            accounts_selection = i;
            return;
        }
    }
    for (size_t i = 0; i < ACCOUNTS_ACTION_COUNT; i++) {
        if (rect_contains(accounts_action_rect(area, i), x, y)) {
            accounts_start((enum accounts_action)i);
            return;
        }
    }
}

/* -------------------------------------------------------------- session */

/* After login: the account's places and home folder, default windows. */
static void desktop_session_start(void) {
    files_places_rebuild();
    char home[96];
    size_t length = accounts_home_path(NULL, home, sizeof(home));
    if (length != 0) {
        files_set_path(home, length);
    }
    layout_load();
    if (z_count == 0) {
        welcome_open_first_time();
        window_open(WINDOW_TERMINAL);
    }
    char greeting[64] = "WELCOME, ";
    append_text(greeting, sizeof(greeting), text_length(greeting),
                accounts_name(accounts_current()));
    set_status(greeting);
}

/* Log out: close every window (the editor warns once about unsaved
   changes) and show the login screen. */
static void desktop_session_end(void) {
    if (!editor_may_discard()) {
        return;
    }
    apps_stop_all(); /* nothing keeps running as the old account */
    while (z_count != 0) {
        window_close(z_order[z_count - 1]);
    }
    accounts_logout();
    files_places_rebuild();
    files_set_path("/", 1);
    status_text[0] = '\0';
    login_show();
}

/* ------------------------------------------------------------ installer */

/*
 * OPOS Installer wizard, opened on boot from installation media. Closing it
 * leaves a live session ("try OPOS"). The engine is in installer.c.
 */
enum installer_page {
    INSTALLER_WELCOME,
    INSTALLER_DISK,
    INSTALLER_MODE,
    INSTALLER_PARTITIONS,
    INSTALLER_BOOT,
    INSTALLER_ACCOUNT,
    INSTALLER_CONFIRM,
    INSTALLER_RUNNING,
    INSTALLER_DONE,
    INSTALLER_FAILED,
};

#define INSTALLER_BUTTON_WIDTH (16 * CHAR_WIDTH + 20)
#define INSTALLER_BUTTON_HEIGHT 36

static enum installer_page installer_page;
static const char *installer_step = "";
static unsigned installer_percent;
static const char *installer_error = "";
/* Account page: name, password, password again. */
static struct text_field installer_fields[3];
static size_t installer_focus;
static const char *installer_account_error = "";
/* Disk page: the chosen block disk (BLOCK_NO_DISK until the user picks
   one, unless only one disk can take OPOS), and what each disk holds. */
static size_t installer_disk = BLOCK_NO_DISK;
static size_t installer_disk_scroll;
static struct installer_disk_summary installer_summaries[BLOCK_MAX_DISKS];

#define INSTALLER_DISK_ROW_HEIGHT 60
#define INSTALLER_DISK_LIST_TOP 98

/* Reads every disk's contents and drops a choice that became unusable. */
static void installer_scan_disks(void) {
    size_t usable = 0;
    size_t only_usable = 0;
    for (size_t i = 0; i < block_disk_count() && i < BLOCK_MAX_DISKS; i++) {
        (void)installer_disk_summary(i, &installer_summaries[i]);
        if (installer_disk_unusable(i) == NULL) {
            usable++;
            only_usable = i;
        }
    }
    if (installer_disk != BLOCK_NO_DISK
        && (installer_disk >= block_disk_count()
            || installer_disk_unusable(installer_disk) != NULL)) {
        installer_disk = BLOCK_NO_DISK;
    }
    if (installer_disk == BLOCK_NO_DISK && usable == 1) {
        installer_disk = only_usable;
    }
    installer_disk_scroll = 0;
}

static void installer_open(void) {
    if (!windows[WINDOW_INSTALLER].open) {
        installer_page = INSTALLER_WELCOME;
        installer_disk = BLOCK_NO_DISK;
        field_reset(&installer_fields[0], ACCOUNT_NAME_MAX, false);
        field_reset(&installer_fields[1], ACCOUNT_PASSWORD_MAX, true);
        field_reset(&installer_fields[2], ACCOUNT_PASSWORD_MAX, true);
        installer_focus = 0;
        installer_account_error = "";
    }
    window_open(WINDOW_INSTALLER);
}

/* Checks the account page; sets installer_account_error on failure. */
static bool installer_account_valid(void) {
    if (!accounts_name_valid(installer_fields[0].text)) {
        installer_account_error = accounts_result_text(ACCOUNT_BAD_NAME);
        installer_focus = 0;
        return false;
    }
    if (installer_fields[1].length != installer_fields[2].length
        || !text_equals(installer_fields[1].text, installer_fields[1].length,
                        installer_fields[2].text)) {
        installer_account_error = "THE PASSWORDS DO NOT MATCH";
        installer_focus = 2;
        return false;
    }
    installer_account_error = "";
    return true;
}

static struct rect installer_field_rect(struct rect area, size_t index) {
    int64_t width = area.width - 48 < 520 ? area.width - 48 : 520;
    return (struct rect){area.x + 24, area.y + 170 + (int64_t)index * 76,
                         width, 36};
}

static void draw_scene(void);
static void draw_button(struct rect area, const char *label, bool active);

static bool installer_disk_chosen(void) {
    return installer_disk != BLOCK_NO_DISK
        && installer_disk_unusable(installer_disk) == NULL;
}

static bool installer_disk_has_data(size_t disk) {
    return disk >= BLOCK_MAX_DISKS || !installer_summaries[disk].blank;
}

static size_t append_disk_size(char *buffer, size_t capacity, size_t offset,
                               uint64_t sectors) {
    uint64_t megabytes = sectors / 2048;
    if (megabytes >= 10240) {
        offset = append_decimal(buffer, capacity, offset, megabytes / 1024);
        return append_text(buffer, capacity, offset, " GB");
    }
    offset = append_decimal(buffer, capacity, offset, megabytes);
    return append_text(buffer, capacity, offset, " MB");
}

/* "20 GB  NVME NAMESPACE 1  VMware Virtual NVMe Disk" */
static void installer_disk_title(size_t disk, char *buffer, size_t capacity) {
    struct block_disk_info info;
    buffer[0] = '\0';
    if (!block_disk_info(disk, &info)) {
        return;
    }
    size_t length = append_disk_size(buffer, capacity, 0, info.sectors);
    length = append_text(buffer, capacity, length, "  ");
    length = append_text(buffer, capacity, length, info.location);
    if (info.model[0] != '\0') {
        length = append_text(buffer, capacity, length, "  ");
        append_text(buffer, capacity, length, info.model);
    }
}

/* "MBR: FAT32 19 GB, LINUX 1 GB" and the like. */
static void installer_disk_contents(size_t disk, char *buffer,
                                    size_t capacity) {
    const struct installer_disk_summary *summary = &installer_summaries[disk];
    size_t length = 0;
    buffer[0] = '\0';
    if (!summary->readable) {
        append_text(buffer, capacity, 0, "CANNOT BE READ");
    } else if (summary->blank) {
        append_text(buffer, capacity, 0, "EMPTY");
    } else if (summary->whole_disk_fat) {
        append_text(buffer, capacity, 0, "FAT FILESYSTEM, NO PARTITION TABLE");
    } else if (summary->gpt) {
        length = append_text(buffer, capacity, 0, "GPT: ");
        length = append_decimal(buffer, capacity, length,
                                summary->partition_count);
        append_text(buffer, capacity, length,
                    summary->partition_count == 1 ? " PARTITION"
                                                  : " PARTITIONS");
    } else if (summary->partition_count != 0) {
        length = append_text(buffer, capacity, 0, "MBR: ");
        for (size_t i = 0; i < summary->partition_count; i++) {
            if (i != 0) {
                length = append_text(buffer, capacity, length, ", ");
            }
            length = append_text(buffer, capacity, length,
                                 installer_partition_type_name(
                                     summary->mbr_types[i]));
            length = append_text(buffer, capacity, length, " ");
            length = append_disk_size(buffer, capacity, length,
                                      summary->mbr_sectors[i]);
        }
    } else {
        append_text(buffer, capacity, 0, "UNKNOWN DATA");
    }
}

static struct rect installer_disk_row(struct rect area, size_t row) {
    return (struct rect){area.x + 24,
                         area.y + INSTALLER_DISK_LIST_TOP
                             + (int64_t)row * INSTALLER_DISK_ROW_HEIGHT,
                         area.width - 48, INSTALLER_DISK_ROW_HEIGHT - 6};
}

/* Rows that fit between the list top and the buttons. */
static size_t installer_disk_rows(struct rect area) {
    int64_t space = area.height - INSTALLER_DISK_LIST_TOP
        - INSTALLER_BUTTON_HEIGHT - 70;
    return space < INSTALLER_DISK_ROW_HEIGHT
        ? 1 : (size_t)(space / INSTALLER_DISK_ROW_HEIGHT);
}

static void installer_disk_move(int direction) {
    size_t count = block_disk_count();
    if (count == 0) {
        return;
    }
    if (installer_disk == BLOCK_NO_DISK) {
        installer_disk = direction > 0 ? 0 : count - 1;
    } else if (direction > 0 && installer_disk + 1 < count) {
        installer_disk++;
    } else if (direction < 0 && installer_disk > 0) {
        installer_disk--;
    }
}

/* Mode page: erase the whole disk, or put OPOS into one partition. The
   partition plan is loaded when the mode page opens for a new disk and is
   kept while the user moves between the pages. */
static bool installer_manual;
static struct installer_layout installer_layout;
static size_t installer_layout_disk = BLOCK_NO_DISK;
/* Why manual partitioning is not offered for this disk, or NULL. */
static const char *installer_layout_error;
/* Partition page: selected row, size prompt for a new partition. */
static size_t installer_part_selection;
static bool installer_size_prompt;
static struct text_field installer_size_field;
static struct installer_region installer_size_region;
static const char *installer_part_message = "";
/* Boot page (partition installs): other systems found, and how OPOS is
   started on BIOS computers. The scan is also shown on the confirm page. */
static struct installer_boot_scan installer_boot;
static enum installer_boot_mode installer_boot_mode;

struct installer_part_row {
    bool free;
    size_t slot;
    struct installer_region region;
};

#define INSTALLER_PART_ROWS_MAX (INSTALLER_SLOTS * 2 + 1)

/* Partitions and free space in disk order. */
static size_t installer_part_rows(struct installer_part_row *rows) {
    struct installer_region free_regions[INSTALLER_SLOTS + 1];
    size_t free_count = installer_layout_free(&installer_layout,
                                              free_regions,
                                              INSTALLER_SLOTS + 1);
    size_t count = 0;
    for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
        const struct installer_slot *slot = &installer_layout.slots[i];
        if (slot->type != 0) {
            rows[count++] = (struct installer_part_row){
                false, i, {slot->first, slot->sectors}};
        }
    }
    for (size_t i = 0; i < free_count; i++) {
        rows[count++] = (struct installer_part_row){
            true, INSTALLER_NO_SLOT, free_regions[i]};
    }
    for (size_t i = 1; i < count; i++) {
        struct installer_part_row row = rows[i];
        size_t at = i;
        while (at > 0 && rows[at - 1].region.first > row.region.first) {
            rows[at] = rows[at - 1];
            at--;
        }
        rows[at] = row;
    }
    return count;
}

static void installer_load_layout(void) {
    if (installer_layout_disk == installer_disk) {
        return;
    }
    installer_layout_disk = installer_disk;
    installer_layout_error = NULL;
    const char *error = "THE DISK CANNOT BE READ";
    if (!installer_layout_load(installer_disk, &installer_layout, &error)) {
        installer_layout_error = error;
        installer_manual = false;
    }
    installer_part_selection = 0;
    installer_size_prompt = false;
    installer_part_message = "";
}

static bool installer_layout_ready(void) {
    return installer_layout.target != INSTALLER_NO_SLOT
        && installer_layout_slot_problem(&installer_layout,
                                         installer_layout.target) == NULL;
}

static bool installer_boot_mode_ok(enum installer_boot_mode mode) {
    return installer_boot_mode_problem(&installer_layout, &installer_boot,
                                       mode) == NULL;
}

/* Scans for other systems; a partition install also gets the default
   boot choice for what it found. */
static void installer_scan_boot(bool choose_default) {
    installer_boot_scan(installer_disk,
                        installer_manual ? &installer_layout : NULL,
                        &installer_boot);
    if (choose_default && installer_manual) {
        installer_boot_mode =
            installer_boot_default_mode(&installer_layout, &installer_boot);
    }
}

static void installer_boot_move(int direction) {
    int mode = (int)installer_boot_mode;
    for (int step = 0; step < INSTALLER_BOOT_MODES; step++) {
        mode += direction;
        if (mode < 0 || mode >= INSTALLER_BOOT_MODES) {
            return;
        }
        if (installer_boot_mode_ok((enum installer_boot_mode)mode)) {
            installer_boot_mode = (enum installer_boot_mode)mode;
            return;
        }
    }
}

/* "menuentry ..." line for GRUB, to start the OPOS partition. */
static void installer_grub_entry(char *buffer, size_t capacity) {
    /* GPT disks normally boot with UEFI: GRUB starts OPOS's own Limine
       from the OPOS partition. */
    size_t length = append_text(buffer, capacity, 0,
                                installer_layout.gpt
                                    ? "menuentry \"OPOS\" { set root=(hd0,gpt"
                                    : "menuentry \"OPOS\" { set root=(hd0,msdos");
    length = append_decimal(buffer, capacity, length,
                            installer_layout.slots[installer_layout.target]
                                .number);
    append_text(buffer, capacity, length,
                installer_layout.gpt
                    ? "); chainloader /EFI/BOOT/BOOTX64.EFI }"
                    : "); chainloader +1 }");
}

static void installer_part_new(const struct installer_part_row *row) {
    installer_size_region = row->region;
    field_reset(&installer_size_field, 7, false);
    size_t length = append_decimal(installer_size_field.text,
                                   sizeof(installer_size_field.text), 0,
                                   row->region.sectors / 2048);
    installer_size_field.length = length;
    installer_size_prompt = true;
    installer_part_message = "";
}

static void installer_part_create(void) {
    uint64_t megabytes = 0;
    for (size_t i = 0; i < installer_size_field.length; i++) {
        char digit = installer_size_field.text[i];
        if (digit < '0' || digit > '9') {
            installer_part_message = "TYPE THE SIZE IN MB (DIGITS ONLY)";
            return;
        }
        megabytes = megabytes * 10 + (uint64_t)(digit - '0');
    }
    uint64_t sectors = megabytes * 2048;
    if (sectors > UINT32_MAX) {
        sectors = UINT32_MAX;
    }
    const char *error = "";
    if (!installer_layout_create(&installer_layout, &installer_size_region,
                                 (uint32_t)sectors, &error)) {
        installer_part_message = error;
        return;
    }
    installer_size_prompt = false;
    installer_part_message = "THE NEW PARTITION WILL HOLD OPOS";
    /* Select the new partition's row. */
    struct installer_part_row rows[INSTALLER_PART_ROWS_MAX];
    size_t count = installer_part_rows(rows);
    for (size_t i = 0; i < count; i++) {
        if (!rows[i].free && rows[i].slot == installer_layout.target) {
            installer_part_selection = i;
        }
    }
}

/* action: 'N' new, 'D' delete, 'U' use for OPOS. */
static void installer_part_action(char action) {
    struct installer_part_row rows[INSTALLER_PART_ROWS_MAX];
    size_t count = installer_part_rows(rows);
    if (installer_part_selection >= count) {
        return;
    }
    const struct installer_part_row *row = &rows[installer_part_selection];
    installer_part_message = "";
    if (action == 'N') {
        if (!row->free) {
            installer_part_message = "SELECT FREE SPACE TO MAKE A PARTITION";
        } else {
            installer_part_new(row);
        }
    } else if (action == 'D') {
        if (row->free) {
            installer_part_message = "SELECT A PARTITION TO DELETE";
        } else {
            (void)installer_layout_delete(&installer_layout, row->slot);
            installer_part_message = "DELETED IN THE PLAN; NOTHING IS WRITTEN YET";
            size_t remaining = installer_part_rows(rows);
            if (installer_part_selection >= remaining && remaining != 0) {
                installer_part_selection = remaining - 1;
            }
        }
    } else if (action == 'U') {
        if (row->free) {
            installer_part_new(row);
            return;
        }
        const char *problem =
            installer_layout_slot_problem(&installer_layout, row->slot);
        if (problem != NULL) {
            installer_part_message = problem;
        } else {
            installer_layout.target = row->slot;
            installer_part_message = "OPOS WILL BE INSTALLED HERE";
        }
    }
}

/* "PARTITION 2  NTFS/EXFAT  40 GB" */
static size_t installer_slot_text(size_t slot, char *buffer, size_t capacity,
                                  bool original) {
    const struct installer_slot *entry = &installer_layout.slots[slot];
    uint8_t type = original ? entry->original_type : entry->type;
    uint32_t sectors = original ? entry->original_sectors : entry->sectors;
    size_t length = append_text(buffer, capacity, 0, "PARTITION ");
    length = append_decimal(buffer, capacity, length,
                            original ? entry->original_number
                                     : entry->number);
    length = append_text(buffer, capacity, length, "  ");
    length = append_text(buffer, capacity, length,
                         !original && entry->created
                             ? "NEW"
                             : installer_partition_type_name(type));
    length = append_text(buffer, capacity, length, "  ");
    return append_disk_size(buffer, capacity, length, sectors);
}

/* Buttons under the partition list: NEW, DELETE, INSTALL HERE. */
static struct rect installer_part_button(struct rect area, size_t index) {
    int64_t width = INSTALLER_BUTTON_WIDTH;
    return (struct rect){area.x + 24 + (int64_t)index * (width + 12),
                         area.y + area.height - 2 * INSTALLER_BUTTON_HEIGHT
                             - 40,
                         width, INSTALLER_BUTTON_HEIGHT};
}

static struct rect installer_size_rect(struct rect area) {
    return (struct rect){area.x + 24,
                         area.y + area.height - 3 * INSTALLER_BUTTON_HEIGHT
                             - 70,
                         200, 36};
}

/* Right button is the primary action, left button goes back. */
static struct rect installer_button_rect(struct rect area, bool primary) {
    int64_t y = area.y + area.height - INSTALLER_BUTTON_HEIGHT - 16;
    int64_t x = primary
        ? area.x + area.width - INSTALLER_BUTTON_WIDTH - 16
        : area.x + area.width - 2 * INSTALLER_BUTTON_WIDTH - 32;
    return (struct rect){x, y, INSTALLER_BUTTON_WIDTH, INSTALLER_BUTTON_HEIGHT};
}

static const char *installer_primary_label(void) {
    switch (installer_page) {
    case INSTALLER_WELCOME:
    case INSTALLER_DISK:
    case INSTALLER_MODE:
    case INSTALLER_PARTITIONS:
    case INSTALLER_BOOT:
    case INSTALLER_ACCOUNT:
        return "NEXT";
    case INSTALLER_CONFIRM:
        return installer_manual ? "FORMAT + INSTALL" : "ERASE + INSTALL";
    case INSTALLER_DONE:
        return "RESTART";
    case INSTALLER_FAILED:
        return "TRY AGAIN";
    default:
        return NULL;
    }
}

static const char *installer_back_label(void) {
    switch (installer_page) {
    case INSTALLER_WELCOME:
        return "TRY OPOS";
    case INSTALLER_DISK:
    case INSTALLER_MODE:
    case INSTALLER_PARTITIONS:
    case INSTALLER_BOOT:
    case INSTALLER_ACCOUNT:
    case INSTALLER_CONFIRM:
        return "BACK";
    case INSTALLER_DONE:
    case INSTALLER_FAILED:
        return "CLOSE";
    default:
        return NULL;
    }
}

static bool installer_primary_enabled(void) {
    switch (installer_page) {
    case INSTALLER_DISK:
    case INSTALLER_MODE:
        return installer_disk_chosen();
    case INSTALLER_PARTITIONS:
        return installer_disk_chosen() && !installer_size_prompt
            && installer_layout_ready();
    case INSTALLER_BOOT:
        return installer_disk_chosen() && installer_layout_ready()
            && installer_boot_mode_ok(installer_boot_mode);
    case INSTALLER_CONFIRM:
        return installer_disk_chosen()
            && (!installer_manual
                || (installer_layout_ready()
                    && installer_boot_mode_ok(installer_boot_mode)));
    default:
        return installer_primary_label() != NULL;
    }
}

static void installer_progress_update(const char *step, unsigned percent) {
    installer_step = step;
    installer_percent = percent > 100 ? 100 : percent;
    draw_scene();
}

static void installer_start(void) {
    installer_page = INSTALLER_RUNNING;
    installer_step = "STARTING";
    installer_percent = 0;
    draw_scene();
    const char *error = "UNKNOWN ERROR";
    bool installed = installer_manual
        ? installer_install_partition(&installer_layout, installer_boot_mode,
                                      installer_progress_update, &error)
        : installer_install(installer_disk, installer_progress_update,
                            &error);
    if (installed) {
        /* The fresh volume has no accounts; create the first one. */
        installer_progress_update("CREATING YOUR ACCOUNT", 100);
        accounts_load();
        enum account_result result = accounts_create(
            installer_fields[0].text, installer_fields[1].text, true);
        if (result == ACCOUNT_OK) {
            installer_page = INSTALLER_DONE;
        } else {
            installer_error = accounts_result_text(result);
            installer_page = INSTALLER_FAILED;
        }
    } else {
        installer_error = error;
        installer_page = INSTALLER_FAILED;
    }
    field_reset(&installer_fields[1], ACCOUNT_PASSWORD_MAX, true);
    field_reset(&installer_fields[2], ACCOUNT_PASSWORD_MAX, true);
    /* The disk changed: re-read its table next time, drop old listings. */
    installer_layout_disk = BLOCK_NO_DISK;
    files_refresh();
}

static void installer_primary(void) {
    if (!installer_primary_enabled()) {
        return;
    }
    switch (installer_page) {
    case INSTALLER_WELCOME:
        installer_page = INSTALLER_DISK;
        installer_scan_disks();
        break;
    case INSTALLER_DISK:
        installer_load_layout();
        installer_page = INSTALLER_MODE;
        break;
    case INSTALLER_MODE:
        installer_page = installer_manual ? INSTALLER_PARTITIONS
                                          : INSTALLER_ACCOUNT;
        break;
    case INSTALLER_PARTITIONS:
        installer_scan_boot(true);
        installer_page = INSTALLER_BOOT;
        break;
    case INSTALLER_BOOT:
        installer_page = INSTALLER_ACCOUNT;
        break;
    case INSTALLER_ACCOUNT:
        if (installer_account_valid()) {
            installer_scan_boot(false);
            installer_page = INSTALLER_CONFIRM;
        }
        break;
    case INSTALLER_CONFIRM:
        installer_start();
        break;
    case INSTALLER_DONE:
        power_reboot();
        break;
    case INSTALLER_FAILED:
        installer_page = INSTALLER_DISK;
        installer_scan_disks();
        break;
    default:
        break;
    }
}

static void installer_back(void) {
    switch (installer_page) {
    case INSTALLER_DISK:
        installer_page = INSTALLER_WELCOME;
        break;
    case INSTALLER_MODE:
        installer_page = INSTALLER_DISK;
        break;
    case INSTALLER_PARTITIONS:
        installer_size_prompt = false;
        installer_page = INSTALLER_MODE;
        break;
    case INSTALLER_BOOT:
        installer_page = INSTALLER_PARTITIONS;
        break;
    case INSTALLER_ACCOUNT:
        installer_page = installer_manual ? INSTALLER_BOOT : INSTALLER_MODE;
        break;
    case INSTALLER_CONFIRM:
        installer_page = INSTALLER_ACCOUNT;
        break;
    case INSTALLER_WELCOME:
    case INSTALLER_DONE:
    case INSTALLER_FAILED:
        window_close(WINDOW_INSTALLER);
        break;
    default:
        break;
    }
}

static void installer_partitions_key(char character) {
    if (installer_size_prompt) {
        if (character == '\n') {
            installer_part_create();
        } else if (character == KEY_ESCAPE) {
            installer_size_prompt = false;
            installer_part_message = "";
        } else if (character == '\b'
                   || (character >= '0' && character <= '9')) {
            installer_part_message = "";
            (void)field_key(&installer_size_field, character);
        }
        return;
    }
    struct installer_part_row rows[INSTALLER_PART_ROWS_MAX];
    size_t count = installer_part_rows(rows);
    if (character == KEY_UP && installer_part_selection > 0) {
        installer_part_selection--;
    } else if (character == KEY_DOWN && installer_part_selection + 1 < count) {
        installer_part_selection++;
    } else if (character == 'n' || character == 'N') {
        installer_part_action('N');
    } else if (character == 'd' || character == 'D'
               || character == KEY_DELETE) {
        installer_part_action('D');
    } else if (character == 'u' || character == 'U') {
        installer_part_action('U');
    } else if (character == '\n') {
        installer_primary();
    } else if (character == KEY_ESCAPE || character == '\b') {
        installer_back();
    }
}

static void installer_key(char character) {
    if (installer_page == INSTALLER_ACCOUNT) {
        if (character == '\t' || character == KEY_DOWN) {
            installer_focus = (installer_focus + 1) % 3;
        } else if (character == KEY_UP) {
            installer_focus = (installer_focus + 2) % 3;
        } else if (character == '\n') {
            /* Enter moves on from the name field; the password fields
               may stay empty, so Enter there tries NEXT. */
            if (installer_focus == 0) {
                installer_focus = 1;
            } else {
                installer_primary();
            }
        } else if (character == KEY_ESCAPE) {
            installer_back();
        } else {
            installer_account_error = "";
            (void)field_key(&installer_fields[installer_focus], character);
        }
        return;
    }
    if (installer_page == INSTALLER_PARTITIONS) {
        installer_partitions_key(character);
        return;
    }
    if (installer_page == INSTALLER_DISK
        && (character == KEY_UP || character == KEY_DOWN)) {
        installer_disk_move(character == KEY_UP ? -1 : 1);
    } else if (installer_page == INSTALLER_BOOT
               && (character == KEY_UP || character == KEY_DOWN)) {
        installer_boot_move(character == KEY_UP ? -1 : 1);
    } else if (installer_page == INSTALLER_MODE
               && (character == KEY_UP || character == KEY_DOWN)) {
        installer_manual = character == KEY_DOWN
            && installer_layout_error == NULL;
    } else if (character == '\n') {
        installer_primary();
    } else if (character == KEY_ESCAPE || character == '\b') {
        installer_back();
    }
}

#define INSTALLER_PART_ROW_HEIGHT 34

static struct rect installer_part_row_rect(struct rect area, size_t row) {
    return (struct rect){area.x + 24,
                         area.y + INSTALLER_DISK_LIST_TOP
                             + (int64_t)row * INSTALLER_PART_ROW_HEIGHT,
                         area.width - 48, INSTALLER_PART_ROW_HEIGHT - 4};
}

static void installer_partitions_click(struct rect area, int32_t x,
                                       int32_t y) {
    if (installer_size_prompt) {
        if (rect_contains(installer_part_button(area, 0), x, y)) {
            installer_part_create();
        } else if (rect_contains(installer_part_button(area, 1), x, y)) {
            installer_size_prompt = false;
            installer_part_message = "";
        }
        return;
    }
    struct installer_part_row rows[INSTALLER_PART_ROWS_MAX];
    size_t count = installer_part_rows(rows);
    for (size_t i = 0; i < count; i++) {
        if (rect_contains(installer_part_row_rect(area, i), x, y)) {
            installer_part_selection = i;
            return;
        }
    }
    static const char actions[3] = {'N', 'D', 'U'};
    for (size_t i = 0; i < 3; i++) {
        if (rect_contains(installer_part_button(area, i), x, y)) {
            installer_part_action(actions[i]);
            return;
        }
    }
}

static void installer_click(struct rect area, int32_t x, int32_t y) {
    if (installer_page == INSTALLER_DISK) {
        size_t rows = installer_disk_rows(area);
        for (size_t row = 0; row < rows; row++) {
            size_t disk = installer_disk_scroll + row;
            if (disk < block_disk_count()
                && rect_contains(installer_disk_row(area, row), x, y)) {
                installer_disk = disk;
                return;
            }
        }
    }
    if (installer_page == INSTALLER_MODE) {
        for (size_t option = 0; option < 2; option++) {
            if (rect_contains(installer_disk_row(area, option), x, y)) {
                installer_manual = option == 1
                    && installer_layout_error == NULL;
                return;
            }
        }
    }
    if (installer_page == INSTALLER_PARTITIONS) {
        installer_partitions_click(area, x, y);
    }
    if (installer_page == INSTALLER_BOOT) {
        for (size_t option = 0; option < INSTALLER_BOOT_MODES; option++) {
            if (rect_contains(installer_disk_row(area, option), x, y)) {
                if (installer_boot_mode_ok((enum installer_boot_mode)option)) {
                    installer_boot_mode = (enum installer_boot_mode)option;
                }
                return;
            }
        }
    }
    if (installer_page == INSTALLER_ACCOUNT) {
        for (size_t i = 0; i < 3; i++) {
            if (rect_contains(installer_field_rect(area, i), x, y)) {
                installer_focus = i;
                return;
            }
        }
    }
    if (installer_primary_label() != NULL
        && rect_contains(installer_button_rect(area, true), x, y)) {
        installer_primary();
    } else if (installer_back_label() != NULL
               && rect_contains(installer_button_rect(area, false), x, y)) {
        installer_back();
    }
}

static void installer_line(struct rect area, size_t line, const char *text,
                           uint8_t red, uint8_t green, uint8_t blue) {
    draw_text_clipped(area.x + 24, area.y + 70 + (int64_t)line * 28,
                      area.width - 48, text, red, green, blue);
}

/* Lists the other systems the boot menu will offer, from screen line
   `line`, at most `limit` lines; returns the next free line. */
static size_t installer_boot_entries(struct rect area, size_t line,
                                     size_t limit) {
    if (installer_boot.entry_count == 0) {
        installer_line(area, line++,
                       "No other systems were found for the boot menu.",
                       130, 138, 155);
        return line;
    }
    installer_line(area, line++, "THE OPOS BOOT MENU WILL ALSO OFFER:", 200,
                   214, 255);
    for (size_t i = 0; i < installer_boot.entry_count; i++) {
        if (i + 2 > limit && i + 1 < installer_boot.entry_count) {
            char more[48] = "  ... AND ";
            size_t length = append_decimal(more, sizeof(more),
                                           text_length(more),
                                           installer_boot.entry_count - i);
            append_text(more, sizeof(more), length, " MORE");
            installer_line(area, line++, more, 170, 186, 225);
            break;
        }
        char text[64] = "  ";
        append_text(text, sizeof(text), 2, installer_boot.entries[i].title);
        installer_line(area, line++, text, 170, 186, 225);
    }
    return line;
}

/* What a boot mode does, for the boot and confirm pages. */
static const char *installer_boot_mode_detail(enum installer_boot_mode mode,
                                              bool *danger) {
    *danger = false;
    const char *problem = installer_boot_mode_problem(
        &installer_layout, &installer_boot, mode);
    if (problem != NULL) {
        return problem;
    }
    enum installer_boot_code code = installer_boot.code;
    switch (mode) {
    case INSTALLER_BOOT_LIMINE_MBR:
        if (code == INSTALLER_BOOT_CODE_GRUB) {
            *danger = true;
            return "REPLACES GRUB: LINUX WILL NOT START ON BIOS COMPUTERS.";
        }
        if (installer_boot.gap_used) {
            *danger = true;
            return "OVERWRITES BOOT DATA AFTER THE MBR (SAVED IN "
                   "/OPOS/BOOTBAK.BIN).";
        }
        if (code == INSTALLER_BOOT_CODE_STANDARD
            || code == INSTALLER_BOOT_CODE_UNKNOWN) {
            return "Replaces the boot code (saved in /OPOS/BOOTBAK.BIN); "
                   "other systems get menu entries.";
        }
        return "Limine starts first and shows OPOS and the other systems.";
    case INSTALLER_BOOT_ACTIVE:
        return "Marks the OPOS partition active; its menu offers the other "
               "systems.";
    case INSTALLER_BOOT_KEEP:
        if (installer_layout.gpt) {
            return "Start OPOS with GRUB (shown before installing) or by "
                   "choosing its loader in the firmware menu.";
        }
        if (code == INSTALLER_BOOT_CODE_GRUB) {
            return "GRUB stays. Add OPOS to the GRUB menu (shown before "
                   "installing).";
        }
        if (code == INSTALLER_BOOT_CODE_NONE) {
            *danger = true;
            return "Nothing starts OPOS on BIOS computers; UEFI only.";
        }
        return "Start OPOS from your boot manager or the UEFI boot menu.";
    case INSTALLER_BOOT_UEFI_DEFAULT:
        return "Limine replaces \\EFI\\BOOT\\BOOTX64.EFI (old one kept as "
               "BOOTX64.OLD) and offers all systems.";
    }
    return "";
}

/* GRUB needs an entry for OPOS: kept boot code on an MBR disk with GRUB,
   or any GPT disk left unchanged. */
static bool installer_shows_grub_entry(void) {
    return installer_manual && installer_boot_mode == INSTALLER_BOOT_KEEP
        && (installer_layout.gpt
            || installer_boot.code == INSTALLER_BOOT_CODE_GRUB);
}

static void draw_installer(struct rect area, bool focused) {
    (void)focused;
    fill(area, 24, 30, 46);
    static const char *const headings[] = {
        "WELCOME TO OPOS", "INSTALLATION DISK", "HOW TO USE THE DISK",
        "PARTITIONS", "STARTING OPOS AND OTHER SYSTEMS",
        "CREATE YOUR ACCOUNT",
        "READY TO INSTALL",
        "INSTALLING OPOS", "INSTALLATION COMPLETE", "INSTALLATION FAILED",
    };
    draw_text_clipped(area.x + 24, area.y + 24, area.width - 48,
                      headings[installer_page], 151, 190, 255);
    fill((struct rect){area.x + 24, area.y + 54, area.width - 48, 1},
         58, 66, 88);

    char disk[128] = "";
    char contents[128] = "";
    if (installer_disk != BLOCK_NO_DISK
        && installer_disk < block_disk_count()) {
        size_t length = append_text(disk, sizeof(disk), 0, "DISK: ");
        installer_disk_title(installer_disk, disk + length,
                             sizeof(disk) - length);
        length = append_text(contents, sizeof(contents), 0, "NOW HOLDS: ");
        installer_disk_contents(installer_disk, contents + length,
                                sizeof(contents) - length);
    }

    switch (installer_page) {
    case INSTALLER_WELCOME:
        installer_line(area, 0, "This wizard installs OPOS on this computer.",
                       230, 238, 255);
        installer_line(area, 2, "Choose TRY OPOS (or close this window) to use",
                       200, 214, 255);
        installer_line(area, 3, "OPOS from the installation media without",
                       200, 214, 255);
        installer_line(area, 4, "installing. Nothing is written to the disk.",
                       200, 214, 255);
        installer_line(area, 6, "You can reopen the installer from the Start menu.",
                       200, 214, 255);
        break;
    case INSTALLER_DISK: {
        size_t count = block_disk_count();
        if (count == 0) {
            installer_line(area, 0, "No supported disk was found.", 255, 120,
                           120);
            installer_line(area, 2, "OPOS supports IDE, SATA, NVMe, PVSCSI, LSI Logic and BusLogic disks.",
                           200, 214, 255);
            break;
        }
        installer_line(area, 0,
                       "Choose the disk for OPOS (UP/DOWN or click).",
                       230, 238, 255);
        size_t rows = installer_disk_rows(area);
        if (installer_disk != BLOCK_NO_DISK) {
            if (installer_disk < installer_disk_scroll) {
                installer_disk_scroll = installer_disk;
            } else if (installer_disk >= installer_disk_scroll + rows) {
                installer_disk_scroll = installer_disk + 1 - rows;
            }
        }
        size_t shown = 0;
        for (size_t row = 0; row < rows; row++) {
            size_t index = installer_disk_scroll + row;
            if (index >= count) {
                break;
            }
            shown++;
            struct rect box = installer_disk_row(area, row);
            bool selected = index == installer_disk;
            const char *unusable = installer_disk_unusable(index);
            if (selected) {
                fill(box, 52, 72, 120);
            } else {
                fill(box, 34, 40, 58);
            }
            char title[128];
            char line[160];
            installer_disk_title(index, title, sizeof(title));
            installer_disk_contents(index, line, sizeof(line));
            if (unusable != NULL) {
                size_t length = text_length(line);
                length = append_text(line, sizeof(line), length, "  - ");
                append_text(line, sizeof(line), length, unusable);
            }
            draw_text_clipped(box.x + 12, box.y + 8, box.width - 24, title,
                              unusable != NULL ? 130 : 230,
                              unusable != NULL ? 138 : 238,
                              unusable != NULL ? 155 : 255);
            draw_text_clipped(box.x + 12, box.y + 30, box.width - 24, line,
                              unusable != NULL ? 200 : 170,
                              unusable != NULL ? 150 : 186,
                              unusable != NULL ? 110 : 225);
        }
        int64_t below = area.y + INSTALLER_DISK_LIST_TOP
            + (int64_t)shown * INSTALLER_DISK_ROW_HEIGHT + 8;
        if (count > rows) {
            char more[48] = "";
            size_t length = append_decimal(more, sizeof(more), 0, count);
            append_text(more, sizeof(more), length,
                        " DISKS - SCROLL WITH UP/DOWN");
            draw_text_clipped(area.x + 24, below, area.width - 48, more, 130,
                              138, 155);
            below += 28;
        }
        const char *status;
        uint8_t red = 200;
        uint8_t green = 214;
        uint8_t blue = 255;
        if (installer_disk == BLOCK_NO_DISK) {
            status = "Select a disk to continue.";
        } else if (installer_disk_unusable(installer_disk) != NULL) {
            status = "OPOS cannot be installed on the selected disk.";
            red = 255;
            green = 120;
            blue = 120;
        } else if (installer_disk_has_data(installer_disk)) {
            status = "ALL DATA ON THE SELECTED DISK WILL BE ERASED.";
            red = 240;
            green = 205;
            blue = 120;
        } else {
            status = "The selected disk is empty.";
        }
        draw_text_clipped(area.x + 24, below, area.width - 48, status, red,
                          green, blue);
        draw_text_clipped(area.x + 24, below + 28, area.width - 48,
                          "OPOS uses the whole disk: one FAT partition that "
                          "boots on BIOS and UEFI.",
                          130, 138, 155);
        break;
    }
    case INSTALLER_ACCOUNT: {
        installer_line(area, 0, "This account is an administrator. The password",
                       200, 214, 255);
        installer_line(area, 1, "is optional. TAB or ARROWS switch fields.",
                       200, 214, 255);
        static const char *const labels[] = {
            "NAME", "PASSWORD (OPTIONAL)", "PASSWORD AGAIN",
        };
        for (size_t i = 0; i < 3; i++) {
            struct rect field = installer_field_rect(area, i);
            draw_text_clipped(field.x, field.y - 26, field.width, labels[i],
                              230, 238, 255);
            draw_field(field, &installer_fields[i], installer_focus == i);
        }
        struct rect last = installer_field_rect(area, 2);
        draw_text_clipped(last.x, last.y + 50, area.width - 48,
                          installer_account_error, 255, 120, 120);
        break;
    }
    case INSTALLER_MODE: {
        installer_line(area, 0, disk, 230, 238, 255);
        static const char *const titles[2] = {
            "ERASE THE WHOLE DISK",
            "INSTALL INTO A PARTITION (KEEP THE OTHERS)",
        };
        const char *details[2] = {
            "OPOS gets one FAT partition that fills the disk.",
            installer_layout_error != NULL
                ? installer_layout_error
                : "Choose, make or delete partitions on the next page.",
        };
        for (size_t option = 0; option < 2; option++) {
            struct rect box = installer_disk_row(area, option);
            bool selected = installer_manual == (option == 1);
            bool usable = option == 0 || installer_layout_error == NULL;
            fill(box, selected ? 52 : 34, selected ? 72 : 40,
                 selected ? 120 : 58);
            draw_text_clipped(box.x + 12, box.y + 8, box.width - 24,
                              titles[option], usable ? 230 : 130,
                              usable ? 238 : 138, usable ? 255 : 155);
            draw_text_clipped(box.x + 12, box.y + 30, box.width - 24,
                              details[option], usable ? 170 : 200,
                              usable ? 186 : 150, usable ? 225 : 110);
        }
        draw_text_clipped(area.x + 24,
                          area.y + INSTALLER_DISK_LIST_TOP
                              + 2 * INSTALLER_DISK_ROW_HEIGHT + 8,
                          area.width - 48, contents, 200, 214, 255);
        break;
    }
    case INSTALLER_PARTITIONS: {
        installer_line(area, 0, disk, 230, 238, 255);
        struct installer_part_row rows[INSTALLER_PART_ROWS_MAX];
        size_t count = installer_part_rows(rows);
        if (installer_part_selection >= count && count != 0) {
            installer_part_selection = count - 1;
        }
        for (size_t i = 0; i < count; i++) {
            struct rect box = installer_part_row_rect(area, i);
            bool selected = i == installer_part_selection;
            char text[128];
            uint8_t red = 230;
            uint8_t green = 238;
            uint8_t blue = 255;
            if (rows[i].free) {
                size_t length = append_text(text, sizeof(text), 0,
                                            "FREE SPACE  ");
                append_disk_size(text, sizeof(text), length,
                                 rows[i].region.sectors);
                red = 150;
                green = 200;
                blue = 150;
            } else {
                size_t length = installer_slot_text(rows[i].slot, text,
                                                    sizeof(text), false);
                if (rows[i].slot == installer_layout.target) {
                    append_text(text, sizeof(text), length,
                                installer_layout.slots[rows[i].slot].created
                                    ? "  <- OPOS"
                                    : "  <- OPOS (WILL BE ERASED)");
                    red = 255;
                    green = 214;
                    blue = 120;
                }
            }
            fill(box, selected ? 52 : 34, selected ? 72 : 40,
                 selected ? 120 : 58);
            draw_text_clipped(box.x + 12,
                              box.y + (box.height - CHAR_HEIGHT) / 2,
                              box.width - 24, text, red, green, blue);
        }
        if (count == 0) {
            installer_line(area, 1, "The disk has no room for partitions.",
                           255, 120, 120);
        }
        int64_t below = area.y + INSTALLER_DISK_LIST_TOP
            + (int64_t)count * INSTALLER_PART_ROW_HEIGHT + 8;
        draw_text_clipped(area.x + 24, below, area.width - 48,
                          installer_part_message, 240, 205, 120);
        draw_text_clipped(area.x + 24, below + 28, area.width - 48,
                          "N: NEW  D: DELETE  U: INSTALL HERE  ENTER: NEXT",
                          130, 138, 155);
        if (installer_size_prompt) {
            struct rect field = installer_size_rect(area);
            draw_text_clipped(field.x, field.y - 26, area.width - 48,
                              "SIZE OF THE NEW PARTITION IN MB "
                              "(ENTER: CREATE, ESC: CANCEL)",
                              230, 238, 255);
            draw_field(field, &installer_size_field, true);
            draw_button(installer_part_button(area, 0), "CREATE", true);
            draw_button(installer_part_button(area, 1), "CANCEL", true);
        } else {
            static const char *const labels[3] = {
                "NEW", "DELETE", "INSTALL HERE",
            };
            for (size_t i = 0; i < 3; i++) {
                draw_button(installer_part_button(area, i), labels[i], true);
            }
        }
        break;
    }
    case INSTALLER_BOOT: {
        installer_line(area, 0, disk, 230, 238, 255);
        static const char *const titles[INSTALLER_BOOT_MODES] = {
            "OPOS BOOT MENU IN THE MBR (LIMINE)",
            "KEEP THE MBR CODE, MAKE THE OPOS PARTITION ACTIVE",
            "CHANGE NOTHING OUTSIDE THE OPOS PARTITION",
            "OPOS BOOT MENU AS THE DISK'S UEFI LOADER (EFI SYSTEM PARTITION)",
        };
        for (size_t option = 0; option < INSTALLER_BOOT_MODES; option++) {
            struct rect box = installer_disk_row(area, option);
            enum installer_boot_mode mode = (enum installer_boot_mode)option;
            bool selected = installer_boot_mode == mode;
            bool usable = installer_boot_mode_ok(mode);
            bool danger;
            const char *detail = installer_boot_mode_detail(mode, &danger);
            fill(box, selected ? 52 : 34, selected ? 72 : 40,
                 selected ? 120 : 58);
            draw_text_clipped(box.x + 12, box.y + 8, box.width - 24,
                              titles[option], usable ? 230 : 130,
                              usable ? 238 : 138, usable ? 255 : 155);
            draw_text_clipped(box.x + 12, box.y + 30, box.width - 24, detail,
                              !usable || danger ? 255 : 170,
                              !usable ? 150 : danger ? 120 : 186,
                              !usable ? 110 : danger ? 120 : 225);
        }
        /* Screen lines below the option rows. */
        size_t line = (size_t)(INSTALLER_DISK_LIST_TOP - 70
                               + INSTALLER_BOOT_MODES
                                   * INSTALLER_DISK_ROW_HEIGHT
                               + 27) / 28;
        char code[96] = "THE MBR NOW HOLDS: ";
        if (installer_layout.gpt) {
            size_t length = append_text(code, sizeof(code), 0,
                                        "GPT DISK. EFI SYSTEM PARTITION: ");
            if (installer_boot.esp_number != 0) {
                append_decimal(code, sizeof(code), length,
                               installer_boot.esp_number);
            } else {
                append_text(code, sizeof(code), length, "NONE");
            }
        } else {
            size_t length = append_text(code, sizeof(code), text_length(code),
                                        installer_boot_code_name(
                                            installer_boot.code));
            if (installer_boot.gap_used) {
                append_text(code, sizeof(code), length, " + DATA AFTER IT");
            }
        }
        installer_line(area, line++, code, 230, 238, 255);
        line = installer_boot_entries(area, line, 5);
        installer_line(area, line, "UP/DOWN or click to choose.", 130, 138,
                       155);
        break;
    }
    case INSTALLER_CONFIRM: {
        char account[64] = "ACCOUNT: ";
        size_t account_length = append_text(account, sizeof(account),
                                            text_length(account),
                                            installer_fields[0].text);
        append_text(account, sizeof(account), account_length,
                    installer_fields[1].length != 0 ? " (WITH PASSWORD)"
                                                    : " (NO PASSWORD)");
        installer_line(area, 0, disk, 230, 238, 255);
        installer_line(area, 1, account, 230, 238, 255);
        if (!installer_manual) {
            installer_line(area, 2, contents, 200, 214, 255);
            installer_line(area, 4, "ALL DATA ON THIS DISK WILL BE ERASED.",
                           255, 120, 120);
            installer_line(area, 5, "This cannot be undone.", 255, 120, 120);
            size_t next = installer_boot.entry_count != 0
                ? installer_boot_entries(area, 7, 4) + 1 : 7;
            installer_line(area, next, "Choose ERASE + INSTALL to start.",
                           200, 214, 255);
            break;
        }
        size_t line = 3;
        char text[160];
        if (installer_layout.target != INSTALLER_NO_SLOT) {
            size_t target = installer_layout.target;
            size_t length = append_text(text, sizeof(text), 0, "OPOS GOES INTO ");
            length += installer_slot_text(target, text + length,
                                          sizeof(text) - length, false);
            if (!installer_layout.slots[target].created) {
                append_text(text, sizeof(text), length,
                            ": ITS DATA WILL BE ERASED");
            }
            installer_line(area, line++, text,
                           installer_layout.slots[target].created ? 230 : 255,
                           installer_layout.slots[target].created ? 238 : 120,
                           installer_layout.slots[target].created ? 255 : 120);
        }
        for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
            const struct installer_slot *slot = &installer_layout.slots[i];
            bool replaced = slot->original_type != 0
                && (slot->type == 0 || slot->created);
            if (!replaced) {
                continue;
            }
            size_t length = installer_slot_text(i, text, sizeof(text), true);
            append_text(text, sizeof(text), length,
                        " WILL BE DELETED WITH ITS DATA");
            installer_line(area, line++, text, 255, 120, 120);
        }
        bool others = false;
        for (size_t i = 0; i < INSTALLER_SLOTS; i++) {
            others = others
                || (installer_layout.slots[i].type != 0
                    && i != installer_layout.target);
        }
        line++;
        if (others) {
            installer_line(area, line++, "Other partitions are kept.",
                           200, 214, 255);
        }
        static const char *const boot_names[INSTALLER_BOOT_MODES] = {
            "BIOS BOOT: OPOS BOOT MENU (LIMINE) IN THE MBR",
            "BIOS BOOT: MBR CODE KEPT, OPOS PARTITION MADE ACTIVE",
            "BOOT: NOTHING CHANGES OUTSIDE THE OPOS PARTITION",
            "UEFI BOOT: OPOS BOOT MENU ON THE EFI SYSTEM PARTITION",
        };
        bool danger;
        const char *detail =
            installer_boot_mode_detail(installer_boot_mode, &danger);
        installer_line(area, line++, boot_names[installer_boot_mode], 230,
                       238, 255);
        installer_line(area, line++, detail, danger ? 255 : 200,
                       danger ? 120 : 214, danger ? 120 : 255);
        if (installer_shows_grub_entry()) {
            char entry[96];
            installer_grub_entry(entry, sizeof(entry));
            installer_line(area, line++,
                           "Then add to /etc/grub.d/40_custom (hd0 = the "
                           "first disk) and run update-grub:",
                           240, 205, 120);
            installer_line(area, line++, entry, 240, 205, 120);
        }
        if (installer_boot.entry_count != 0) {
            char count[64] = "OTHER SYSTEMS IN THE BOOT MENU: ";
            append_decimal(count, sizeof(count), text_length(count),
                           installer_boot.entry_count);
            installer_line(area, line++, count, 200, 214, 255);
        }
        line++;
        installer_line(area, line, "Choose FORMAT + INSTALL to start.", 200,
                       214, 255);
        break;
    }
    case INSTALLER_RUNNING: {
        installer_line(area, 0, installer_step, 230, 238, 255);
        struct rect bar = {area.x + 24, area.y + 120, area.width - 48, 24};
        fill(bar, 58, 66, 88);
        fill((struct rect){bar.x + 2, bar.y + 2,
                           (bar.width - 4) * installer_percent / 100,
                           bar.height - 4},
             110, 160, 255);
        char percent[16] = "";
        append_text(percent, sizeof(percent),
                    append_decimal(percent, sizeof(percent), 0,
                                   installer_percent),
                    "%");
        installer_line(area, 4, percent, 200, 214, 255);
        installer_line(area, 6, "Do not turn off the computer.", 240, 205,
                       120);
        break;
    }
    case INSTALLER_DONE:
        installer_line(area, 0, "OPOS is installed.", 230, 238, 255);
        installer_line(area, 2, "Remove the installation media, then choose",
                       200, 214, 255);
        installer_line(area, 3, "RESTART to boot OPOS from the disk.", 200,
                       214, 255);
        installer_line(area, 5, "CLOSE keeps using this live session.", 200,
                       214, 255);
        if (installer_manual
            && installer_boot_mode == INSTALLER_BOOT_UEFI_DEFAULT) {
            installer_line(area, 7,
                           "If another system still starts first, choose "
                           "this disk itself (not its", 240, 205, 120);
            installer_line(area, 8,
                           "boot manager) in the firmware boot menu or move "
                           "it first in the boot order.", 240, 205, 120);
        }
        if (installer_shows_grub_entry()) {
            char entry[96];
            installer_grub_entry(entry, sizeof(entry));
            installer_line(area, 7,
                           "To start OPOS from GRUB, add this to "
                           "/etc/grub.d/40_custom and run update-grub:",
                           240, 205, 120);
            installer_line(area, 8, entry, 240, 205, 120);
        }
        break;
    case INSTALLER_FAILED:
        installer_line(area, 0, installer_error, 255, 120, 120);
        installer_line(area, 2, "Choose TRY AGAIN to go back to the disk page.",
                       200, 214, 255);
        break;
    }

    const char *primary = installer_primary_label();
    if (primary != NULL) {
        struct rect button = installer_button_rect(area, true);
        bool enabled = installer_primary_enabled();
        if (installer_page == INSTALLER_CONFIRM && enabled) {
            fill(button, 170, 60, 70);
            draw_text_clipped(button.x + 10,
                              button.y + (button.height - CHAR_HEIGHT) / 2,
                              button.width - 20, primary, 255, 255, 255);
        } else if (enabled) {
            draw_button(button, primary, true);
        } else {
            fill(button, 43, 48, 60);
            draw_text_clipped(button.x + 10,
                              button.y + (button.height - CHAR_HEIGHT) / 2,
                              button.width - 20, primary, 110, 118, 135);
        }
    }
    const char *back = installer_back_label();
    if (back != NULL) {
        draw_button(installer_button_rect(area, false), back, false);
    }
}

static void draw_window(enum window_kind kind) {
    struct rect frame = window_rect(kind);
    bool focused = window_focused(kind);
    /* Content never spills out of a window, however small it is resized. */
    int64_t clip_x = frame.x < 0 ? 0 : frame.x;
    int64_t clip_y = frame.y < 0 ? 0 : frame.y;
    int64_t clip_right = frame.x + frame.width;
    int64_t clip_bottom = frame.y + frame.height;
    if (clip_right <= clip_x || clip_bottom <= clip_y) {
        return;
    }
    console_set_clip((uint64_t)clip_x, (uint64_t)clip_y,
                     (uint64_t)(clip_right - clip_x),
                     (uint64_t)(clip_bottom - clip_y));
    if (focused) {
        fill(frame, 110, 160, 255);
    } else {
        fill(frame, 58, 66, 88);
    }
    struct rect title = title_rect(frame);
    fill(title, focused ? 36 : 30, focused ? 52 : 36, focused ? 86 : 52);
    draw_text_clipped(title.x + 10, title.y + 5,
                      title.width - TITLE_HEIGHT - 20, window_title(kind),
                      245, 248, 255);
    struct rect close = close_rect(frame);
    fill(close, focused ? 190 : 80, focused ? 70 : 60, focused ? 80 : 70);
    draw_text_clipped(close.x + 8, close.y + 5, CHAR_WIDTH, "X",
                      255, 255, 255);

    struct rect content = content_rect(frame);
    switch (kind) {
    case WINDOW_TERMINAL:
        draw_terminal(content, focused);
        break;
    case WINDOW_FILES:
        draw_files(content, focused);
        break;
    case WINDOW_SYSTEM_INFO:
        draw_system_info(content);
        break;
    case WINDOW_TASKS:
        draw_tasks(content, focused);
        break;
    case WINDOW_SETTINGS:
        draw_settings(content, focused);
        break;
    case WINDOW_EDITOR:
        draw_editor(content, focused);
        break;
    case WINDOW_INSTALLER:
        draw_installer(content, focused);
        break;
    case WINDOW_ACCOUNTS:
        draw_accounts(content, focused);
        break;
    default:
        if (window_is_app(kind)) {
            draw_app(kind - WINDOW_APP, content);
        } else {
            draw_welcome(content);
        }
        break;
    }
    console_clear_clip();
}

/* -------------------------------------------------------------- taskbar */

static struct rect taskbar_rect(void) {
    return (struct rect){0, screen_height() - TASKBAR_HEIGHT, screen_width(),
                         TASKBAR_HEIGHT};
}

static struct rect start_button_rect(void) {
    struct rect bar = taskbar_rect();
    return (struct rect){bar.x + 6, bar.y + 5, START_BUTTON_WIDTH,
                         TASKBAR_HEIGHT - 10};
}

static struct rect layout_button_rect(void) {
    struct rect start = start_button_rect();
    return (struct rect){start.x + start.width + 8, start.y,
                         LAYOUT_BUTTON_WIDTH, start.height};
}

static struct rect task_button_rect(size_t index) {
    struct rect layout = layout_button_rect();
    return (struct rect){
        layout.x + layout.width + 16 + (int64_t)index * (TASK_BUTTON_WIDTH + 6),
        layout.y, TASK_BUTTON_WIDTH, layout.height
    };
}

/* INSTALL OPOS is listed only on installation media, LOG OUT only while
   an account is logged in. */
static bool menu_action_visible(enum menu_action action) {
    if (action == MENU_INSTALL) {
        return installer_available();
    }
    if (action == MENU_LOG_OUT) {
        return accounts_current() != ACCOUNT_NONE;
    }
    return true;
}

static size_t menu_visible_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < MENU_COUNT; i++) {
        count += menu_action_visible((enum menu_action)i) ? 1 : 0;
    }
    return count;
}

static enum menu_action menu_action_at(size_t index) {
    for (size_t i = 0; i < MENU_COUNT; i++) {
        if (!menu_action_visible((enum menu_action)i)) {
            continue;
        }
        if (index-- == 0) {
            return (enum menu_action)i;
        }
    }
    return MENU_COUNT;
}

static struct rect menu_rect(void) {
    int64_t height = (int64_t)menu_visible_count() * MENU_ITEM_HEIGHT + 12;
    return (struct rect){6, screen_height() - TASKBAR_HEIGHT - height - 4,
                         MENU_WIDTH, height};
}

static struct rect menu_item_rect(size_t index) {
    struct rect menu = menu_rect();
    return (struct rect){menu.x + 6,
                         menu.y + 6 + (int64_t)index * MENU_ITEM_HEIGHT,
                         menu.width - 12, MENU_ITEM_HEIGHT};
}

static void draw_button(struct rect area, const char *label, bool active) {
    fill(area, active ? 70 : 43, active ? 100 : 56, active ? 160 : 84);
    draw_text_clipped(area.x + 10, area.y + (area.height - CHAR_HEIGHT) / 2,
                      area.width - 20, label, 240, 245, 255);
}

static void draw_taskbar(void) {
    struct rect bar = taskbar_rect();
    fill(bar, 20, 26, 40);
    fill((struct rect){bar.x, bar.y, bar.width, 1}, 70, 90, 130);
    draw_button(start_button_rect(), "START", start_menu_open);
    draw_button(layout_button_rect(), tiled_layout ? "TILED" : "FLOAT",
                tiled_layout);
    for (size_t i = 0; i < tile_count; i++) {
        struct rect button = task_button_rect(i);
        if (button.x + button.width > bar.width - CLOCK_WIDTH - 10) {
            break;
        }
        draw_button(button, window_title(tile_order[i]),
                    window_focused(tile_order[i]));
    }

    char clock[32] = "";
    size_t length = 0;
    struct rtc_time now;
    if (rtc_read(&now)) {
        length = append_date(clock, sizeof(clock), 0, &now);
        length = append_text(clock, sizeof(clock), length, " ");
        append_time(clock, sizeof(clock), length, &now);
    } else {
        uint64_t seconds = timer_ticks() / 100;
        length = append_two_digits(clock, sizeof(clock), 0, seconds / 3600);
        length = append_text(clock, sizeof(clock), length, ":");
        length = append_two_digits(clock, sizeof(clock), length,
                                   seconds / 60 % 60);
        length = append_text(clock, sizeof(clock), length, ":");
        append_two_digits(clock, sizeof(clock), length, seconds % 60);
    }
    draw_text_clipped(bar.width - CLOCK_WIDTH, bar.y + 11, CLOCK_WIDTH - 10,
                      clock, 230, 238, 255);
}

static void draw_start_menu(void) {
    struct rect menu = menu_rect();
    fill(menu, 110, 160, 255);
    fill((struct rect){menu.x + 2, menu.y + 2, menu.width - 4,
                       menu.height - 4}, 26, 34, 54);
    for (size_t i = 0; i < menu_visible_count(); i++) {
        struct rect item = menu_item_rect(i);
        enum menu_action action = menu_action_at(i);
        if (action == MENU_TOGGLE_LAYOUT || action == MENU_REBOOT) {
            fill((struct rect){item.x, item.y, item.width, 1}, 58, 66, 88);
        }
        if (i == menu_selection) {
            fill((struct rect){item.x, item.y + 2, item.width, item.height - 3},
                 70, 100, 160);
        }
        draw_text_clipped(item.x + 10, item.y + 7, item.width - 20,
                          menu_labels[action], 230, 238, 255);
    }
}

static void draw_wallpaper(void) {
    int64_t height = screen_height() - TASKBAR_HEIGHT;
    const int64_t band = 8;
    for (int64_t y = 0; y < height; y += band) {
        uint64_t shade = (uint64_t)(y * 64 / (height == 0 ? 1 : height));
        fill((struct rect){0, y, screen_width(), band},
             (uint8_t)(14 + shade / 4), (uint8_t)(20 + shade / 3),
             (uint8_t)(38 + shade / 2));
    }
    if (z_count == 0) {
        draw_text_clipped(screen_width() / 2 - 9 * CHAR_WIDTH,
                          height / 2 - 20, 18 * CHAR_WIDTH,
                          "OPOS DESKTOP", 151, 190, 235);
        draw_text_clipped(screen_width() / 2 - 15 * CHAR_WIDTH,
                          height / 2 + 14, 32 * CHAR_WIDTH,
                          "CLICK START TO OPEN AN APP", 110, 140, 180);
    }
}

static void draw_scene(void) {
    bool buffered = console_frame_begin();
    draw_wallpaper();
    if (login_active) {
        /* No taskbar on the login screen; cover its strip too. */
        fill(taskbar_rect(), 14, 20, 38);
        draw_login();
        if (buffered) {
            console_frame_present();
        } else {
            console_cursor_refresh();
        }
        return;
    }
    for (size_t i = 0; i < z_count; i++) {
        draw_window(z_order[i]);
    }
    draw_files_menu();
    draw_taskbar();
    if (status_text[0] != '\0') {
        int64_t x = task_button_rect(tile_count).x;
        int64_t width = screen_width() - CLOCK_WIDTH - 10 - x;
        draw_text_clipped(x, taskbar_rect().y + 11, width, status_text,
                          240, 205, 120);
    }
    if (start_menu_open) {
        draw_start_menu();
    }
    if (buffered) {
        console_frame_present();
    } else {
        console_cursor_refresh();
    }
}

/* --------------------------------------------------------------- events */

enum desktop_result {
    DESKTOP_CONTINUE,
    DESKTOP_EXIT
};

/* Tiling is a personal setting, saved for the logged-in account. */
static void layout_toggle(void) {
    tiled_layout = !tiled_layout;
    settings_set(SETTING_FLOATING_LAYOUT, !tiled_layout);
}

static void layout_load(void) {
    tiled_layout = !settings_get(SETTING_FLOATING_LAYOUT);
}

/* Welcome opens only the first time each account (or the machine, without
   accounts) reaches the desktop. The Start menu does not list it. */
static void welcome_open_first_time(void) {
    if (!settings_get(SETTING_WELCOME_SEEN)) {
        window_open(WINDOW_WELCOME);
        settings_set(SETTING_WELCOME_SEEN, true);
    }
}

static enum desktop_result menu_activate(enum menu_action action) {
    start_menu_open = false;
    switch (action) {
    case MENU_TERMINAL:
        window_open(WINDOW_TERMINAL);
        break;
    case MENU_FILES:
        window_open(WINDOW_FILES);
        break;
    case MENU_TEXT_EDITOR:
        if (!notepad_launch(NULL, 0)) {
            editor_new();
        }
        break;
    case MENU_TASK_MANAGER:
        tasks_open();
        break;
    case MENU_SYSTEM_INFO:
        window_open(WINDOW_SYSTEM_INFO);
        break;
    case MENU_TOGGLE_LAYOUT:
        layout_toggle();
        break;
    case MENU_SETTINGS:
        window_open(WINDOW_SETTINGS);
        break;
    case MENU_INSTALL:
        installer_open();
        break;
    case MENU_ACCOUNTS:
        window_open(WINDOW_ACCOUNTS);
        break;
    case MENU_LOG_OUT:
        desktop_session_end();
        break;
    case MENU_REBOOT:
        /* Ctrl+Reboot boots into the shell once instead of the desktop. */
        if (keyboard_ctrl_held()) {
            settings_set(SETTING_BOOT_SHELL_ONCE, true);
        }
        power_reboot();
        break;
    case MENU_SHUTDOWN:
        if (!power_shutdown()) {
            set_status("ACPI SHUTDOWN UNAVAILABLE");
        }
        break;
    default:
        break;
    }
    return DESKTOP_CONTINUE;
}

static void files_click(struct rect area, int32_t x, int32_t y) {
    struct rect list = files_list_area(area);
    if (list.x != area.x && x < list.x) {
        size_t slot = 0;
        for (size_t i = 0; i < files_place_count; i++) {
            if (!files_place_visible(i)) {
                continue;
            }
            if (rect_contains(files_place_rect(area, slot++), x, y)) {
                const char *path = files_places[i].path;
                (void)files_visit(path, text_length(path));
                return;
            }
        }
        return;
    }
    for (size_t i = 0; i < FILES_NAV_BUTTONS; i++) {
        if (rect_contains(files_nav_rect(list, i), x, y)) {
            files_address_editing = false;
            if (i == 2) {
                files_go_up();
            } else {
                files_history_step(i == 1);
            }
            return;
        }
    }
    if (rect_contains(files_address_rect(list), x, y)) {
        if (!files_address_editing) {
            files_address_start();
        }
        return;
    }
    files_address_editing = false;
    files_clamp(list);
    size_t rows = files_row_count();
    size_t visible = files_visible_rows(list);
    for (size_t row = files_scroll; row < rows
         && row < files_scroll + visible; row++) {
        if (!rect_contains(files_line_rect(list, row - files_scroll), x, y)) {
            continue;
        }
        uint64_t now = timer_ticks();
        bool double_click = row == files_last_click_row
            && now - files_last_click_tick <= FILES_DOUBLE_CLICK_TICKS;
        files_selection = row;
        files_last_click_row = double_click ? (size_t)-1 : row;
        files_last_click_tick = now;
        if (double_click) {
            files_open_row(row);
        }
        return;
    }
}

static void files_key_navigate(char character) {
    size_t count = files_row_count();
    if (character == '\b' || character == KEY_LEFT) {
        files_go_up();
    } else if (count == 0) {
        return;
    } else if (character == KEY_UP) {
        files_selection = (files_selection + count - 1) % count;
    } else if (character == KEY_DOWN) {
        files_selection = (files_selection + 1) % count;
    } else if (character == '\n' || character == KEY_RIGHT) {
        files_open_row(files_selection < count ? files_selection : 0);
    }
}

/* ------------------------------------------- Files: rename, delete, new */

enum files_action {
    FILES_ACTION_OPEN,
    FILES_ACTION_RENAME,
    FILES_ACTION_DELETE,
    FILES_ACTION_NEW_FOLDER,
    FILES_ACTION_NEW_FILE,
};

static const char *const files_action_labels[] = {
    "OPEN", "RENAME", "DELETE", "NEW FOLDER", "NEW FILE",
};

#define FILES_MENU_WIDTH (14 * CHAR_WIDTH + 16)
#define FILES_DELETE_DEPTH 8

static bool files_location_read_only(void) {
    return vfs_path_read_only(files_path, files_path_length)
        || !permission_ui_allows(files_path, files_path_length,
                                 PERMISSION_WRITE);
}

/* Entry index of the selected row, or false for ".." / nothing. */
static bool files_selected_entry(size_t *index) {
    size_t row = files_selection;
    if (!files_at_root()) {
        if (row == 0) {
            return false;
        }
        row--;
    }
    if (row >= files_count) {
        return false;
    }
    *index = row;
    return true;
}

static void files_select_name(const char *name, size_t length) {
    for (size_t i = 0; i < files_count; i++) {
        const char *entry = files_entries[i].name;
        if (text_length(entry) != length) {
            continue;
        }
        /* Case-insensitive: FAT may store the name in upper case. */
        bool same = true;
        for (size_t j = 0; j < length; j++) {
            char a = entry[j];
            char b = name[j];
            if (a >= 'a' && a <= 'z') {
                a = (char)(a - 32);
            }
            if (b >= 'a' && b <= 'z') {
                b = (char)(b - 32);
            }
            same = same && a == b;
        }
        if (same) {
            files_selection = i + (files_at_root() ? 0 : 1);
            return;
        }
    }
}

static void files_start_prompt(enum files_prompt_kind kind) {
    files_menu_open = false;
    size_t index = 0;
    bool has_entry = files_selected_entry(&index);
    if ((kind == FILES_PROMPT_RENAME || kind == FILES_PROMPT_DELETE)
        && !has_entry) {
        set_status("SELECT A FILE OR FOLDER FIRST");
        return;
    }
    if (files_location_read_only()) {
        set_status("THIS LOCATION IS READ-ONLY - USE THE DISK");
        return;
    }
    files_prompt_kind = kind;
    files_prompt_target = index;
    files_prompt_length = 0;
    files_prompt[0] = '\0';
    if (kind == FILES_PROMPT_RENAME) {
        files_prompt_length = append_text(files_prompt, sizeof(files_prompt),
                                          0, files_entries[index].name);
    }
}

/* Builds files_path + "/" + name into path; false if too long. */
static bool files_child_path(const char *name, size_t name_length,
                             char *path, size_t *length) {
    if (files_path_length + 1 + name_length >= FILES_PATH_CAPACITY) {
        return false;
    }
    size_t used = 0;
    for (size_t i = 0; i < files_path_length; i++) {
        path[used++] = files_path[i];
    }
    if (!files_at_root()) {
        path[used++] = '/';
    }
    for (size_t i = 0; i < name_length; i++) {
        path[used++] = name[i];
    }
    path[used] = '\0';
    *length = used;
    return true;
}

/* Deletes a file, or a folder with everything in it. path is modified
   temporarily and must have room for FILES_PATH_CAPACITY bytes. */
static bool files_delete_tree(char *path, size_t length, size_t depth) {
    struct vfs_metadata metadata;
    bool not_empty;
    if (!vfs_stat(path, length, &metadata)) {
        return false;
    }
    if (metadata.type != VFS_NODE_DIRECTORY) {
        return vfs_remove(path, length, false, &not_empty);
    }
    if (depth >= FILES_DELETE_DEPTH) {
        return false;
    }
    for (;;) {
        size_t count;
        if (!vfs_directory_count(path, length, &count)) {
            return false;
        }
        if (count == 0) {
            break;
        }
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(path, length, 0, &entry)
            || length + 1 + entry.name_length >= FILES_PATH_CAPACITY) {
            return false;
        }
        path[length] = '/';
        for (size_t i = 0; i < entry.name_length; i++) {
            path[length + 1 + i] = entry.name[i];
        }
        size_t child_length = length + 1 + entry.name_length;
        path[child_length] = '\0';
        bool removed = files_delete_tree(path, child_length, depth + 1);
        path[length] = '\0';
        if (!removed) {
            return false;
        }
    }
    return vfs_remove(path, length, true, &not_empty);
}

static bool files_name_valid(const char *name, size_t length) {
    if (length == 0) {
        set_status("ENTER A NAME");
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/'
            || c == '\\' || c == '|' || c == '?' || c == '*') {
            set_status("NAMES CANNOT CONTAIN < > : \" / \\ | ? *");
            return false;
        }
    }
    if ((length == 1 && name[0] == '.')
        || (length == 2 && name[0] == '.' && name[1] == '.')) {
        set_status("THAT NAME IS RESERVED");
        return false;
    }
    if (name[length - 1] == ' ' || name[length - 1] == '.') {
        set_status("NAMES CANNOT END WITH A SPACE OR DOT");
        return false;
    }
    if (length > FAT16_LONG_NAME_MAX) {
        set_status("NAME IS TOO LONG");
        return false;
    }
    return true;
}

/* Runs the confirmed prompt. */
static void files_prompt_commit(void) {
    enum files_prompt_kind kind = files_prompt_kind;
    files_prompt_kind = FILES_PROMPT_NONE;
    char path[FILES_PATH_CAPACITY];
    size_t length;
    if (kind == FILES_PROMPT_DELETE) {
        if (files_prompt_target >= files_count) {
            return;
        }
        const struct files_entry *entry = &files_entries[files_prompt_target];
        if (!files_child_path(entry->name, text_length(entry->name), path,
                              &length)) {
            return;
        }
        if (!permission_ui_allows(path, length, PERMISSION_REMOVE)) {
            set_status("ACCESS DENIED");
            return;
        }
        bool removed = files_delete_tree(path, length, 0);
        size_t selection = files_selection;
        files_refresh();
        files_selection = selection;
        set_status(removed ? "DELETED" : "COULD NOT DELETE EVERYTHING");
        return;
    }

    if (!files_name_valid(files_prompt, files_prompt_length)
        || !files_child_path(files_prompt, files_prompt_length, path,
                             &length)) {
        if (files_prompt_length != 0) {
            files_prompt_kind = kind; /* let the user fix the name */
        }
        return;
    }
    struct vfs_metadata metadata;
    bool exists = vfs_stat(path, length, &metadata);
    const char *failure = NULL;
    if (!permission_ui_allows(path, length, kind == FILES_PROMPT_RENAME
                              ? PERMISSION_REMOVE : PERMISSION_WRITE)) {
        set_status("ACCESS DENIED");
        return;
    }
    if (kind == FILES_PROMPT_RENAME) {
        if (files_prompt_target >= files_count) {
            return;
        }
        const struct files_entry *entry = &files_entries[files_prompt_target];
        char old_path[FILES_PATH_CAPACITY];
        size_t old_length;
        if (!files_child_path(entry->name, text_length(entry->name),
                              old_path, &old_length)) {
            return;
        }
        if (!permission_ui_allows(old_path, old_length, PERMISSION_REMOVE)) {
            set_status("ACCESS DENIED");
            return;
        }
        enum vfs_rename_result result =
            vfs_rename(old_path, old_length, path, length);
        if (result == VFS_RENAME_OK) {
            /* Keep an open editor pointing at the file. */
            if (editor_path_length == old_length) {
                bool same = true;
                for (size_t i = 0; i < old_length; i++) {
                    same = same && editor_path[i] == old_path[i];
                }
                if (same) {
                    editor_set_path(path, length);
                }
            }
        } else if (result == VFS_RENAME_NAME_TOO_LONG
                   || result == VFS_RENAME_INVALID) {
            failure = "INVALID NAME";
        } else if (exists) {
            failure = "NAME ALREADY EXISTS";
        } else {
            failure = "COULD NOT RENAME";
        }
    } else if (exists) {
        failure = "NAME ALREADY EXISTS";
    } else if (kind == FILES_PROMPT_NEW_FOLDER) {
        if (!vfs_create_directory(path, length)) {
            failure = "COULD NOT CREATE - DISK FULL OR FOLDER FULL";
        }
    } else if (!vfs_create_file(path, length, false)) {
        failure = "COULD NOT CREATE - DISK FULL OR FOLDER FULL";
    }
    if (failure != NULL) {
        set_status(failure);
        files_prompt_kind = kind;
        return;
    }
    files_refresh();
    files_select_name(files_prompt, files_prompt_length);
}

static void files_prompt_key(char character) {
    if (character == KEY_ESCAPE) {
        files_prompt_kind = FILES_PROMPT_NONE;
    } else if (character == '\n') {
        files_prompt_commit();
    } else if (files_prompt_kind == FILES_PROMPT_DELETE) {
        if (keyboard_last_ctrl()) {
            return;
        }
        if (character == 'y' || character == 'Y') {
            files_prompt_commit();
        } else if (character == 'n' || character == 'N') {
            files_prompt_kind = FILES_PROMPT_NONE;
        }
    } else if (line_ctrl_key(files_prompt, &files_prompt_length,
                             sizeof(files_prompt) - 1, false, character)) {
        return;
    } else if (character == '\b') {
        if (files_prompt_length > 0) {
            files_prompt[--files_prompt_length] = '\0';
        }
    } else if (character >= ' ' && character <= '~'
               && files_prompt_length + 1 < sizeof(files_prompt)) {
        files_prompt[files_prompt_length++] = character;
        files_prompt[files_prompt_length] = '\0';
    }
}

static void files_draw_prompt(struct rect list) {
    struct rect line = {list.x + 6, list.y + list.height - LINE_HEIGHT - 1,
                        list.width - 12, LINE_HEIGHT};
    fill(line, 46, 62, 96);
    char text[FILES_NAME_CAPACITY + 64];
    size_t length = 0;
    switch (files_prompt_kind) {
    case FILES_PROMPT_RENAME:
        length = append_text(text, sizeof(text), 0, "RENAME TO: ");
        break;
    case FILES_PROMPT_NEW_FOLDER:
        length = append_text(text, sizeof(text), 0, "NEW FOLDER: ");
        break;
    case FILES_PROMPT_NEW_FILE:
        length = append_text(text, sizeof(text), 0, "NEW FILE: ");
        break;
    default: {
        const struct files_entry *entry = &files_entries[files_prompt_target];
        length = append_text(text, sizeof(text), 0, "DELETE ");
        length = append_text(text, sizeof(text), length, entry->name);
        append_text(text, sizeof(text), length, entry->directory
                    ? " AND EVERYTHING IN IT? (Y/N)" : "? (Y/N)");
        draw_text_clipped(line.x + 6, line.y + 3, line.width - 12, text,
                          255, 190, 150);
        return;
    }
    }
    length = append_text(text, sizeof(text), length, files_prompt);
    append_text(text, sizeof(text), length, "_");
    draw_text_clipped(line.x + 6, line.y + 3, line.width - 12, text,
                      230, 238, 255);
}

/* Menu items for the current menu, in display order. */
static size_t files_menu_items(enum files_action *items) {
    size_t count = 0;
    if (files_menu_on_item) {
        items[count++] = FILES_ACTION_OPEN;
        items[count++] = FILES_ACTION_RENAME;
        items[count++] = FILES_ACTION_DELETE;
    }
    items[count++] = FILES_ACTION_NEW_FOLDER;
    items[count++] = FILES_ACTION_NEW_FILE;
    return count;
}

static struct rect files_menu_rect(void) {
    enum files_action items[5];
    size_t count = files_menu_items(items);
    int64_t height = (int64_t)count * LINE_HEIGHT + 8;
    int64_t x = files_menu_x;
    int64_t y = files_menu_y;
    if (x + FILES_MENU_WIDTH > screen_width()) {
        x = screen_width() - FILES_MENU_WIDTH;
    }
    if (y + height > screen_height() - TASKBAR_HEIGHT) {
        y = screen_height() - TASKBAR_HEIGHT - height;
    }
    return (struct rect){x, y, FILES_MENU_WIDTH, height};
}

static struct rect files_menu_item_rect(struct rect menu, size_t index) {
    return (struct rect){menu.x + 4, menu.y + 4 + (int64_t)index * LINE_HEIGHT,
                         menu.width - 8, LINE_HEIGHT};
}

static bool files_action_enabled(enum files_action action) {
    return action == FILES_ACTION_OPEN || !files_location_read_only();
}

/* Opens the right-click menu for the Files content area at (x, y). */
static void files_context(struct rect area, int32_t x, int32_t y) {
    files_prompt_kind = FILES_PROMPT_NONE;
    struct rect list = files_list_area(area);
    if (x < list.x) {
        files_menu_open = false;
        return;
    }
    files_clamp(list);
    files_menu_on_item = false;
    size_t rows = files_row_count();
    size_t visible = files_visible_rows(list);
    for (size_t row = files_scroll; row < rows
         && row < files_scroll + visible; row++) {
        if (rect_contains(files_line_rect(list, row - files_scroll), x, y)) {
            files_selection = row;
            files_menu_on_item = files_at_root() || row != 0;
            break;
        }
    }
    files_menu_x = x;
    files_menu_y = y;
    files_menu_open = true;
}

static void files_menu_activate(enum files_action action) {
    files_menu_open = false;
    if (!files_action_enabled(action)) {
        set_status("THIS LOCATION IS READ-ONLY - USE THE DISK");
        return;
    }
    switch (action) {
    case FILES_ACTION_OPEN:
        files_open_row(files_selection);
        break;
    case FILES_ACTION_RENAME:
        files_start_prompt(FILES_PROMPT_RENAME);
        break;
    case FILES_ACTION_DELETE:
        files_start_prompt(FILES_PROMPT_DELETE);
        break;
    case FILES_ACTION_NEW_FOLDER:
        files_start_prompt(FILES_PROMPT_NEW_FOLDER);
        break;
    case FILES_ACTION_NEW_FILE:
        files_start_prompt(FILES_PROMPT_NEW_FILE);
        break;
    }
}

/* Left click while the menu is open: true if the click was consumed. */
static bool files_menu_press(int32_t x, int32_t y) {
    if (!files_menu_open) {
        return false;
    }
    files_menu_open = false;
    if (!windows[WINDOW_FILES].open) {
        return false;
    }
    struct rect menu = files_menu_rect();
    enum files_action items[5];
    size_t count = files_menu_items(items);
    for (size_t i = 0; i < count; i++) {
        if (rect_contains(files_menu_item_rect(menu, i), x, y)) {
            files_menu_activate(items[i]);
            return true;
        }
    }
    return rect_contains(menu, x, y);
}

static void draw_files_menu(void) {
    if (!files_menu_open || !windows[WINDOW_FILES].open) {
        return;
    }
    struct rect menu = files_menu_rect();
    fill(menu, 70, 90, 130);
    fill((struct rect){menu.x + 1, menu.y + 1, menu.width - 2,
                       menu.height - 2}, 28, 35, 54);
    enum files_action items[5];
    size_t count = files_menu_items(items);
    for (size_t i = 0; i < count; i++) {
        struct rect line = files_menu_item_rect(menu, i);
        bool enabled = files_action_enabled(items[i]);
        draw_text_clipped(line.x + 8, line.y + 3, line.width - 16,
                          files_action_labels[items[i]],
                          enabled ? 230 : 100, enabled ? 238 : 110,
                          enabled ? 255 : 130);
    }
}

static void files_key(char character) {
    if (files_menu_open) {
        files_menu_open = false;
        if (character == KEY_ESCAPE) {
            return;
        }
    }
    if (files_address_editing) {
        files_address_key(character);
        return;
    }
    if (files_prompt_kind != FILES_PROMPT_NONE) {
        files_prompt_key(character);
        return;
    }
    if (keyboard_last_ctrl()) {
        if (character == 'l' || character == 'L') {
            files_address_start();
        } else if (character == 'r' || character == 'R') {
            files_start_prompt(FILES_PROMPT_RENAME);
        } else if (character == 'N') {
            /* Ctrl+Shift+N, as in Windows Explorer. */
            files_start_prompt(FILES_PROMPT_NEW_FOLDER);
        } else if (character == 'n') {
            files_start_prompt(FILES_PROMPT_NEW_FILE);
        }
        return;
    }
    if (character == KEY_DELETE) {
        files_start_prompt(FILES_PROMPT_DELETE);
        return;
    }
    files_key_navigate(character);
}

enum drag_mode {
    DRAG_MOVE,
    DRAG_RESIZE,
    DRAG_SPLIT
};

#define EDGE_LEFT 1
#define EDGE_RIGHT 2
#define EDGE_TOP 4
#define EDGE_BOTTOM 8
/* Grab band around a floating window: outside the frame plus the border. */
#define RESIZE_OUTSIDE 6
#define RESIZE_INSIDE (BORDER + 2)
/* Near a corner both edges are grabbed, over this length. */
#define RESIZE_CORNER 18

struct drag_state {
    bool active;
    enum drag_mode mode;
    enum window_kind kind;
    int32_t offset_x;
    int32_t offset_y;
    unsigned edges;
    struct rect start;
    int32_t start_x;
    int32_t start_y;
    /* DRAG_SPLIT: split index and the area it divides. */
    size_t split;
    struct rect split_area;
    bool split_vertical;
};

/* Edges of a floating window's frame under the pointer, or 0. */
static unsigned resize_edges(struct rect frame, int32_t x, int32_t y) {
    int64_t left = frame.x;
    int64_t top = frame.y;
    int64_t right = frame.x + frame.width;
    int64_t bottom = frame.y + frame.height;
    if (x < left - RESIZE_OUTSIDE || x >= right + RESIZE_OUTSIDE
        || y < top - RESIZE_OUTSIDE || y >= bottom + RESIZE_OUTSIDE) {
        return 0;
    }
    bool near_left = x < left + RESIZE_INSIDE;
    bool near_right = x >= right - RESIZE_INSIDE;
    /* The top edge stays thin so the title bar and close button work. */
    bool near_top = y < top + BORDER;
    bool near_bottom = y >= bottom - RESIZE_INSIDE;
    unsigned edges = 0;
    if (near_left || near_right) {
        edges |= near_left ? EDGE_LEFT : EDGE_RIGHT;
        if (y < top + RESIZE_CORNER) {
            edges |= EDGE_TOP;
        } else if (y >= bottom - RESIZE_CORNER) {
            edges |= EDGE_BOTTOM;
        }
    }
    if (near_top || near_bottom) {
        edges |= near_top ? EDGE_TOP : EDGE_BOTTOM;
        if (x < left + RESIZE_CORNER) {
            edges |= EDGE_LEFT;
        } else if (x >= right - RESIZE_CORNER) {
            edges |= EDGE_RIGHT;
        }
    }
    return edges;
}

/*
 * Starts resizing the topmost floating window whose edge is under the
 * pointer. A window covering the point with its inner area wins instead.
 */
static bool resize_start(int32_t x, int32_t y, struct drag_state *drag) {
    if (tiled_layout) {
        return false;
    }
    for (size_t i = z_count; i != 0; i--) {
        enum window_kind kind = z_order[i - 1];
        struct rect frame = windows[kind].floating;
        unsigned edges = resize_edges(frame, x, y);
        if (edges != 0) {
            window_raise(kind);
            drag->active = true;
            drag->mode = DRAG_RESIZE;
            drag->kind = kind;
            drag->edges = edges;
            drag->start = frame;
            drag->start_x = x;
            drag->start_y = y;
            return true;
        }
        if (rect_contains(frame, x, y)) {
            return false;
        }
    }
    return false;
}

static bool handle_resize(const struct drag_state *drag, int32_t x, int32_t y) {
    struct rect frame = drag->start;
    int64_t dx = x - drag->start_x;
    int64_t dy = y - drag->start_y;
    int64_t right = frame.x + frame.width;
    int64_t bottom = frame.y + frame.height;
    int64_t max_bottom = screen_height() - TASKBAR_HEIGHT;
    if (drag->edges & EDGE_LEFT) {
        int64_t left = frame.x + dx;
        if (left > right - MIN_WINDOW_WIDTH) {
            left = right - MIN_WINDOW_WIDTH;
        }
        if (left < 0) {
            left = 0;
        }
        frame.x = left;
        frame.width = right - left;
    }
    if (drag->edges & EDGE_RIGHT) {
        int64_t new_right = right + dx;
        if (new_right < frame.x + MIN_WINDOW_WIDTH) {
            new_right = frame.x + MIN_WINDOW_WIDTH;
        }
        if (new_right > screen_width()) {
            new_right = screen_width();
        }
        frame.width = new_right - frame.x;
    }
    if (drag->edges & EDGE_TOP) {
        int64_t top = frame.y + dy;
        if (top > bottom - MIN_WINDOW_HEIGHT) {
            top = bottom - MIN_WINDOW_HEIGHT;
        }
        if (top < 0) {
            top = 0;
        }
        frame.y = top;
        frame.height = bottom - top;
    }
    if (drag->edges & EDGE_BOTTOM) {
        int64_t new_bottom = bottom + dy;
        if (new_bottom < frame.y + MIN_WINDOW_HEIGHT) {
            new_bottom = frame.y + MIN_WINDOW_HEIGHT;
        }
        if (new_bottom > max_bottom) {
            new_bottom = max_bottom;
        }
        frame.height = new_bottom - frame.y;
    }
    struct rect *current = &windows[drag->kind].floating;
    if (current->x == frame.x && current->y == frame.y
        && current->width == frame.width && current->height == frame.height) {
        return false;
    }
    *current = frame;
    return true;
}

/*
 * Tiled layout: pressing in the gap between two windows grabs the dwindle
 * split there, so dragging moves the boundary.
 */
static bool split_start(int32_t x, int32_t y, struct drag_state *drag) {
    if (!tiled_layout || tile_count < 2) {
        return false;
    }
    struct rect area = workspace_area();
    for (size_t i = 0; i + 1 < tile_count; i++) {
        bool vertical = area.width >= area.height;
        struct rect gap;
        struct rect next = area;
        if (vertical) {
            int64_t first = (area.width - GAP) * split_ratio[i] / 1000;
            gap = (struct rect){area.x + first, area.y, GAP, area.height};
            next.x += first + GAP;
            next.width -= first + GAP;
        } else {
            int64_t first = (area.height - GAP) * split_ratio[i] / 1000;
            gap = (struct rect){area.x, area.y + first, area.width, GAP};
            next.y += first + GAP;
            next.height -= first + GAP;
        }
        if (rect_contains(gap, x, y)) {
            drag->active = true;
            drag->mode = DRAG_SPLIT;
            drag->split = i;
            drag->split_area = area;
            drag->split_vertical = vertical;
            return true;
        }
        area = next;
    }
    return false;
}

static bool handle_split(const struct drag_state *drag, int32_t x, int32_t y) {
    struct rect area = drag->split_area;
    int64_t span = drag->split_vertical ? area.width : area.height;
    int64_t offset = drag->split_vertical ? x - area.x : y - area.y;
    if (span <= GAP) {
        return false;
    }
    int64_t ratio = (offset - GAP / 2) * 1000 / (span - GAP);
    if (ratio < SPLIT_MIN) {
        ratio = SPLIT_MIN;
    }
    if (ratio > SPLIT_MAX) {
        ratio = SPLIT_MAX;
    }
    if (split_ratio[drag->split] == ratio) {
        return false;
    }
    split_ratio[drag->split] = (uint16_t)ratio;
    return true;
}

static enum desktop_result handle_press(int32_t x, int32_t y,
                                        struct drag_state *drag) {
    if (login_active) {
        login_press(x, y);
        return DESKTOP_CONTINUE;
    }
    if (files_menu_press(x, y)) {
        return DESKTOP_CONTINUE;
    }
    if (start_menu_open) {
        for (size_t i = 0; i < menu_visible_count(); i++) {
            if (rect_contains(menu_item_rect(i), x, y)) {
                return menu_activate(menu_action_at(i));
            }
        }
        start_menu_open = false;
        if (rect_contains(menu_rect(), x, y)
            || rect_contains(start_button_rect(), x, y)) {
            return DESKTOP_CONTINUE;
        }
    }

    if (rect_contains(taskbar_rect(), x, y)) {
        if (rect_contains(start_button_rect(), x, y)) {
            start_menu_open = true;
            menu_selection = 0;
        } else if (rect_contains(layout_button_rect(), x, y)) {
            layout_toggle();
        } else {
            for (size_t i = 0; i < tile_count; i++) {
                if (rect_contains(task_button_rect(i), x, y)) {
                    window_raise(tile_order[i]);
                    break;
                }
            }
        }
        return DESKTOP_CONTINUE;
    }

    if (resize_start(x, y, drag) || split_start(x, y, drag)) {
        return DESKTOP_CONTINUE;
    }
    enum window_kind kind = window_at(x, y);
    if (kind == WINDOW_COUNT) {
        return DESKTOP_CONTINUE;
    }
    window_raise(kind);
    struct rect frame = window_rect(kind);
    if (rect_contains(close_rect(frame), x, y)) {
        window_close(kind);
    } else if (rect_contains(title_rect(frame), x, y)) {
        if (!tiled_layout) {
            drag->active = true;
            drag->mode = DRAG_MOVE;
            drag->kind = kind;
            drag->offset_x = x - (int32_t)frame.x;
            drag->offset_y = y - (int32_t)frame.y;
        }
    } else if (kind == WINDOW_FILES) {
        files_click(content_rect(frame), x, y);
    } else if (kind == WINDOW_EDITOR) {
        editor_click(content_rect(frame), x, y);
    } else if (kind == WINDOW_INSTALLER) {
        installer_click(content_rect(frame), x, y);
    } else if (kind == WINDOW_ACCOUNTS) {
        accounts_click(content_rect(frame), x, y);
    } else if (kind == WINDOW_TASKS) {
        tasks_click(content_rect(frame), x, y);
    } else if (window_is_app(kind)) {
        app_mouse_press(kind - WINDOW_APP, x, y, 1);
    } else if (kind == WINDOW_SETTINGS) {
        struct rect area = content_rect(frame);
        for (size_t i = 0; i < SETTING_ROW_COUNT; i++) {
            struct rect row = setting_row_rect(area, i);
            if (row.y + row.height <= area.y + area.height
                && rect_contains(row, x, y)) {
                /* Left half of the value box steps down, the rest up. */
                struct rect value = setting_value_rect(row);
                bool down = rect_contains(value, x, y)
                          && x < value.x + value.width / 2;
                settings_selection = i;
                settings_change(i, down ? -1 : 1);
            }
        }
    }
    return DESKTOP_CONTINUE;
}

static bool handle_drag(const struct drag_state *drag, int32_t x, int32_t y) {
    struct rect *frame = &windows[drag->kind].floating;
    int64_t new_x = x - drag->offset_x;
    int64_t new_y = y - drag->offset_y;
    int64_t max_x = screen_width() - 60;
    int64_t max_y = screen_height() - TASKBAR_HEIGHT - TITLE_HEIGHT;
    if (new_x < 60 - frame->width) {
        new_x = 60 - frame->width;
    }
    if (new_x > max_x) {
        new_x = max_x;
    }
    if (new_y < 0) {
        new_y = 0;
    }
    if (new_y > max_y) {
        new_y = max_y;
    }
    if (new_x == frame->x && new_y == frame->y) {
        return false;
    }
    frame->x = new_x;
    frame->y = new_y;
    return true;
}

/* Returns true when the focused terminal consumed the character. */
static bool terminal_key(char character) {
    if (keyboard_last_ctrl() && (character == 'c' || character == 'C')
        && app_kill_terminal_job()) {
        terminal_output_flush();
        terminal_print("^C");
        return true;
    }
    if (app_input_readers != 0) {
        /* A program waits for a line: typing goes to it (it echoes). */
        app_input_push(character);
        return true;
    }
    if (line_ctrl_key(terminal_line, &terminal_length,
                      sizeof(terminal_line) - 1, false, character)) {
        return true;
    }
    if (character == '\b') {
        if (terminal_length != 0) {
            terminal_line[--terminal_length] = '\0';
        }
    } else if (character == '\n') {
        bool keep_open = terminal_execute();
        terminal_length = 0;
        terminal_line[0] = '\0';
        if (!keep_open) {
            window_close(WINDOW_TERMINAL);
        }
    } else if (character >= ' ' && character <= '~'
               && terminal_length + 1 < sizeof(terminal_line)) {
        terminal_line[terminal_length++] = character;
        terminal_line[terminal_length] = '\0';
    } else {
        return false;
    }
    return true;
}

/* Focuses the nearest window from the focused one in an arrow direction. */
static void focus_direction(char direction) {
    if (z_count < 2) {
        return;
    }
    struct rect from = window_rect(z_order[z_count - 1]);
    int64_t from_x = from.x + from.width / 2;
    int64_t from_y = from.y + from.height / 2;
    enum window_kind best = WINDOW_COUNT;
    int64_t best_score = INT64_MAX;
    for (size_t i = 0; i + 1 < z_count; i++) {
        struct rect to = window_rect(z_order[i]);
        int64_t dx = to.x + to.width / 2 - from_x;
        int64_t dy = to.y + to.height / 2 - from_y;
        int64_t along = direction == KEY_LEFT ? -dx
                      : direction == KEY_RIGHT ? dx
                      : direction == KEY_UP ? -dy : dy;
        int64_t across = direction == KEY_LEFT || direction == KEY_RIGHT
                       ? dy : dx;
        if (along <= 0) {
            continue;
        }
        int64_t score = along + 2 * (across < 0 ? -across : across);
        if (score < best_score) {
            best_score = score;
            best = z_order[i];
        }
    }
    if (best != WINDOW_COUNT) {
        window_raise(best);
    }
}

#define RESIZE_KEY_STEP 40
#define SPLIT_KEY_STEP 50

/*
 * Super+Shift+arrow: Right/Down grow the focused window, Left/Up shrink it.
 * Floating windows change size; tiled ones move their split.
 */
static void resize_focused(char direction) {
    if (z_count == 0) {
        return;
    }
    enum window_kind kind = z_order[z_count - 1];
    bool grow = direction == KEY_RIGHT || direction == KEY_DOWN;
    if (!tiled_layout) {
        struct drag_state drag = {0};
        drag.kind = kind;
        drag.start = windows[kind].floating;
        int32_t step = grow ? RESIZE_KEY_STEP : -RESIZE_KEY_STEP;
        if (direction == KEY_LEFT || direction == KEY_RIGHT) {
            drag.edges = EDGE_RIGHT;
            (void)handle_resize(&drag, step, 0);
        } else {
            drag.edges = EDGE_BOTTOM;
            (void)handle_resize(&drag, 0, step);
        }
        return;
    }
    size_t index = 0;
    while (index < tile_count && tile_order[index] != kind) {
        index++;
    }
    if (index == tile_count || tile_count < 2) {
        return;
    }
    /* The last window has no split; it grows when the one before shrinks. */
    size_t split = index + 1 < tile_count ? index : index - 1;
    bool larger = index + 1 < tile_count ? grow : !grow;
    int ratio = split_ratio[split] + (larger ? SPLIT_KEY_STEP : -SPLIT_KEY_STEP);
    if (ratio < SPLIT_MIN) {
        ratio = SPLIT_MIN;
    }
    if (ratio > SPLIT_MAX) {
        ratio = SPLIT_MAX;
    }
    split_ratio[split] = (uint16_t)ratio;
}

/* Super+key shortcuts, Hyprland style. */
static enum desktop_result handle_super_key(char character) {
    start_menu_open = false;
    switch (character) {
    case KEY_LEFT:
    case KEY_RIGHT:
    case KEY_UP:
    case KEY_DOWN:
        if (keyboard_last_shift()) {
            resize_focused(character);
        } else {
            focus_direction(character);
        }
        break;
    case 'q':
    case 'Q':
        if (z_count != 0) {
            window_close(z_order[z_count - 1]);
        }
        break;
    case '\n':
        window_open(WINDOW_TERMINAL);
        break;
    case 'e':
    case 'E':
        window_open(WINDOW_FILES);
        break;
    case 't':
    case 'T':
    case 'v':
    case 'V':
        layout_toggle();
        break;
    case '\t':
        if (z_count > 1) {
            window_raise(z_order[0]);
        }
        break;
    default:
        break;
    }
    return DESKTOP_CONTINUE;
}

static enum desktop_result handle_key(char character, bool super) {
    if (login_active) {
        if (!super) {
            login_key(character);
        }
        return DESKTOP_CONTINUE;
    }
    if (super) {
        return handle_super_key(character);
    }
    if (character == KEY_SUPER) {
        start_menu_open = !start_menu_open;
        menu_selection = 0;
        return DESKTOP_CONTINUE;
    }
    if (character == KEY_ESCAPE && keyboard_last_ctrl()
        && keyboard_last_shift()) {
        start_menu_open = false;
        tasks_open();
        return DESKTOP_CONTINUE;
    }
    if (start_menu_open) {
        if (character == KEY_UP) {
            menu_selection = (menu_selection + menu_visible_count() - 1)
                           % menu_visible_count();
        } else if (character == KEY_DOWN) {
            menu_selection = (menu_selection + 1) % menu_visible_count();
        } else if (character == '\n') {
            return menu_activate(menu_action_at(menu_selection));
        } else if (character == KEY_ESCAPE) {
            start_menu_open = false;
        }
        return DESKTOP_CONTINUE;
    }
    if (z_count != 0 && z_order[z_count - 1] == WINDOW_TERMINAL) {
        (void)terminal_key(character);
    } else if (z_count != 0 && z_order[z_count - 1] == WINDOW_FILES) {
        files_key(character);
    } else if (z_count != 0 && z_order[z_count - 1] == WINDOW_EDITOR) {
        editor_key(character);
    } else if (z_count != 0 && z_order[z_count - 1] == WINDOW_INSTALLER) {
        installer_key(character);
    } else if (z_count != 0 && z_order[z_count - 1] == WINDOW_ACCOUNTS) {
        accounts_key(character);
    } else if (z_count != 0 && z_order[z_count - 1] == WINDOW_TASKS) {
        tasks_key(character);
    } else if (z_count != 0 && window_is_app(z_order[z_count - 1])) {
        app_key(z_order[z_count - 1] - WINDOW_APP, character);
    } else if (z_count != 0 && z_order[z_count - 1] == WINDOW_SETTINGS) {
        if (character == KEY_UP) {
            settings_selection = (settings_selection + SETTING_ROW_COUNT - 1)
                               % SETTING_ROW_COUNT;
        } else if (character == KEY_DOWN) {
            settings_selection = (settings_selection + 1) % SETTING_ROW_COUNT;
        } else if (character == '\n' || character == ' '
                   || character == KEY_RIGHT) {
            settings_change(settings_selection, 1);
        } else if (character == KEY_LEFT) {
            settings_change(settings_selection, -1);
        }
    }
    return DESKTOP_CONTINUE;
}

/* Right click: raises the window under the pointer; Files gets a menu. */
static void handle_right_press(int32_t x, int32_t y) {
    if (login_active) {
        return;
    }
    start_menu_open = false;
    files_menu_open = false;
    enum window_kind kind = window_at(x, y);
    if (kind == WINDOW_COUNT) {
        return;
    }
    window_raise(kind);
    struct rect frame = window_rect(kind);
    struct rect content = content_rect(frame);
    if (kind == WINDOW_FILES && rect_contains(content, x, y)) {
        files_context(content, x, y);
    } else if (window_is_app(kind)) {
        app_mouse_press(kind - WINDOW_APP, x, y, 2);
    }
}


/* ------------------------------------------------------- program windows */

/*
 * Programs run on scheduler tasks of their own (app jobs, below), next to
 * the desktop and to each other. Their system calls reach the desktop
 * through the desktop_app_* functions and the Terminal's output and input
 * queues, all of which run on the program's task. The desktop task is
 * never preempted in the middle of a pass of its loop (see
 * scheduler_set_kernel_preemption), so those calls only ever find it
 * asleep between two passes. They fill in mailboxes (a window's pixels,
 * its event queue, the Terminal's queues) and wait; the next pass
 * (apps_poll) opens and closes windows and redraws.
 *
 * Each program may have one window, and APP_WINDOW_COUNT can be open at
 * once. A program draws into its own memory and presents it; the kernel
 * copies the pixels into the window's surface, shown at its top left.
 */
#define APP_SURFACE_WIDTH 1024
#define APP_SURFACE_HEIGHT 768
#define APP_EVENT_CAPACITY 64
#define APP_EVENT_KEY 1
#define APP_EVENT_MOUSE_DOWN 2
#define APP_EVENT_MOUSE_UP 3
#define APP_EVENT_MOUSE_MOVE 4
#define APP_EVENT_RESIZE 5
#define APP_EVENT_CLOSE 6
/* After its window closes a program has this long to exit (3 s). */
#define APP_CLOSE_GRACE_TICKS 300
/* Waiting programs look at least this often whether they were ended. */
#define APP_WAIT_RECHECK_TICKS 10

struct app_window {
    uint64_t owner;        /* process ID; 0: the slot is free */
    bool open_pending;     /* created; the desktop opens it next pass */
    bool opened;
    bool closed;           /* the user closed it */
    bool close_sent;       /* the program got its CLOSE event */
    volatile bool owner_exited;
    bool dirty;            /* new pixels to show */
    bool waiting;          /* the program waits for an event */
    uint64_t close_tick;
    uint32_t requested_width;
    uint32_t requested_height;
    uint32_t *surface;     /* kept for the slot's next program */
    uint32_t width;        /* presented size */
    uint32_t height;
    char title[32];
    struct desktop_app_event events[APP_EVENT_CAPACITY];
    size_t event_head;
    size_t event_count;
    uint32_t buttons_down;
    int32_t mouse_x;
    int32_t mouse_y;
    int64_t reported_width;
    int64_t reported_height;
};

static struct app_window app_windows[APP_WINDOW_COUNT];

static const char *app_window_title(size_t slot) {
    return app_windows[slot].title[0] != '\0' ? app_windows[slot].title
                                              : "PROGRAM";
}

static bool app_window_live(size_t slot) {
    const struct app_window *window = &app_windows[slot];
    return window->owner != 0 && window->opened && !window->closed
        && !window->owner_exited && windows[WINDOW_APP + slot].open;
}

static struct app_window *app_window_of(uint64_t owner) {
    if (owner == 0) {
        return NULL;
    }
    for (size_t i = 0; i < APP_WINDOW_COUNT; i++) {
        if (app_windows[i].owner == owner && !app_windows[i].owner_exited) {
            return &app_windows[i];
        }
    }
    return NULL;
}

static void app_push(size_t slot, const struct desktop_app_event *event) {
    struct app_window *window = &app_windows[slot];
    if (!app_window_live(slot)) {
        return;
    }
    if (event->type == APP_EVENT_MOUSE_MOVE && window->event_count != 0) {
        struct desktop_app_event *last = &window->events[
            (window->event_head + window->event_count - 1)
            % APP_EVENT_CAPACITY];
        if (last->type == APP_EVENT_MOUSE_MOVE) {
            *last = *event;
            return;
        }
    }
    if (window->event_count == APP_EVENT_CAPACITY) {
        return; /* the program is not keeping up; drop the newest */
    }
    window->events[(window->event_head + window->event_count)
                   % APP_EVENT_CAPACITY] = *event;
    window->event_count++;
    scheduler_wake_event(window);
}

static struct rect app_content(size_t slot) {
    return content_rect(window_rect(WINDOW_APP + slot));
}

static void app_key(size_t slot, char character) {
    /* For keys, buttons holds the modifiers: 1 Ctrl, 2 Shift. */
    struct desktop_app_event event = {
        .type = APP_EVENT_KEY, .key = (uint8_t)character,
        .buttons = (keyboard_last_ctrl() ? 1u : 0u)
                 | (keyboard_last_shift() ? 2u : 0u),
    };
    app_push(slot, &event);
}

static void app_mouse_press(size_t slot, int32_t x, int32_t y,
                            uint32_t button) {
    struct app_window *window = &app_windows[slot];
    struct rect area = app_content(slot);
    if (!app_window_live(slot) || !rect_contains(area, x, y)) {
        return;
    }
    window->buttons_down |= button;
    window->mouse_x = x - (int32_t)area.x;
    window->mouse_y = y - (int32_t)area.y;
    struct desktop_app_event event = {
        .type = APP_EVENT_MOUSE_DOWN, .x = window->mouse_x,
        .y = window->mouse_y, .button = button,
        .buttons = window->buttons_down,
    };
    app_push(slot, &event);
}

/* Releases and movement; presses come from handle_press. */
static void app_mouse_update(size_t slot, int32_t x, int32_t y,
                             uint8_t buttons) {
    struct app_window *window = &app_windows[slot];
    if (!app_window_live(slot)) {
        window->buttons_down = 0;
        return;
    }
    struct rect area = app_content(slot);
    int32_t local_x = x - (int32_t)area.x;
    int32_t local_y = y - (int32_t)area.y;
    for (uint32_t button = 1; button <= 2; button <<= 1) {
        if ((window->buttons_down & button) != 0 && (buttons & button) == 0) {
            window->buttons_down &= ~button;
            struct desktop_app_event event = {
                .type = APP_EVENT_MOUSE_UP, .x = local_x, .y = local_y,
                .button = button, .buttons = window->buttons_down,
            };
            app_push(slot, &event);
        }
    }
    enum window_kind kind = WINDOW_APP + slot;
    bool hovering = window_focused(kind) && rect_contains(area, x, y)
        && window_at(x, y) == kind;
    if ((window->buttons_down != 0 || hovering)
        && (local_x != window->mouse_x || local_y != window->mouse_y)) {
        window->mouse_x = local_x;
        window->mouse_y = local_y;
        struct desktop_app_event event = {
            .type = APP_EVENT_MOUSE_MOVE, .x = local_x, .y = local_y,
            .buttons = window->buttons_down,
        };
        app_push(slot, &event);
    }
}

static void apps_mouse_update(int32_t x, int32_t y, uint8_t buttons) {
    for (size_t slot = 0; slot < APP_WINDOW_COUNT; slot++) {
        app_mouse_update(slot, x, y, buttons);
    }
}

/* Tells a program when its content area changes size. */
static void app_check_resize(size_t slot) {
    struct app_window *window = &app_windows[slot];
    if (!app_window_live(slot)) {
        return;
    }
    struct rect area = app_content(slot);
    int64_t width = area.width < 0 ? 0 : area.width;
    int64_t height = area.height < 0 ? 0 : area.height;
    if (width != window->reported_width || height != window->reported_height) {
        window->reported_width = width;
        window->reported_height = height;
        struct desktop_app_event event = {
            .type = APP_EVENT_RESIZE,
            .width = (uint32_t)width, .height = (uint32_t)height,
        };
        app_push(slot, &event);
    }
}

static void apps_check_resize(void) {
    for (size_t slot = 0; slot < APP_WINDOW_COUNT; slot++) {
        app_check_resize(slot);
    }
}

static void draw_app(size_t slot, struct rect area) {
    const struct app_window *window = &app_windows[slot];
    fill(area, 12, 16, 26);
    if (window->surface == NULL || window->width == 0
        || window->height == 0) {
        return;
    }
    int64_t skip_x = area.x < 0 ? -area.x : 0;
    int64_t skip_y = area.y < 0 ? -area.y : 0;
    int64_t width = (int64_t)window->width < area.width
        ? (int64_t)window->width : area.width;
    int64_t height = (int64_t)window->height < area.height
        ? (int64_t)window->height : area.height;
    if (width <= skip_x || height <= skip_y) {
        return;
    }
    console_draw_pixels((uint64_t)(area.x + skip_x),
                        (uint64_t)(area.y + skip_y),
                        (uint64_t)(width - skip_x),
                        (uint64_t)(height - skip_y),
                        window->surface + skip_y * APP_SURFACE_WIDTH + skip_x,
                        APP_SURFACE_WIDTH);
}

/* The window went away (close button, Super+Q): the program gets CLOSE,
   and is ended if it has not exited after APP_CLOSE_GRACE_TICKS. */
static void app_window_closed(size_t slot) {
    struct app_window *window = &app_windows[slot];
    if (window->owner != 0 && !window->closed) {
        window->closed = true;
        window->close_tick = timer_ticks();
        window->event_count = 0;
        window->buttons_down = 0;
        scheduler_wake_event(window);
    }
}

/* Frees a slot whose program has ended. */
static void app_window_release(size_t slot) {
    struct app_window *window = &app_windows[slot];
    if (windows[WINDOW_APP + slot].open) {
        window_close(WINDOW_APP + slot);
    }
    uint32_t *surface = window->surface;
    *window = (struct app_window){0};
    window->surface = surface;
}

/* The desktop's pass: opens a newly created window. */
static void app_window_open(size_t slot) {
    struct app_window *window = &app_windows[slot];
    enum window_kind kind = WINDOW_APP + slot;
    window->open_pending = false;
    window_open(kind);
    struct rect area = workspace_area();
    int64_t frame_width = (int64_t)window->requested_width + 2 * BORDER;
    int64_t frame_height = (int64_t)window->requested_height + 2 * BORDER
        + TITLE_HEIGHT;
    if (frame_width > area.width) {
        frame_width = area.width;
    }
    if (frame_height > area.height) {
        frame_height = area.height;
    }
    /* Centred, each slot a little further down and right. */
    int64_t offset = 30 * (int64_t)slot;
    int64_t x = area.x + (area.width - frame_width) / 2 + offset;
    int64_t y = area.y + (area.height - frame_height) / 2 + offset;
    if (x + frame_width > area.x + area.width) {
        x = area.x + area.width - frame_width;
    }
    if (y + frame_height > area.y + area.height) {
        y = area.y + area.height - frame_height;
    }
    windows[kind].floating = (struct rect){x, y, frame_width, frame_height};
    window->opened = true;
    app_check_resize(slot);
    scheduler_wake_event(window);
}

int64_t desktop_app_window_create(uint64_t owner, const char *title,
                                  size_t title_length, uint32_t width,
                                  uint32_t height) {
    if (!desktop_active || login_active) {
        return -19; /* ENODEV: no desktop (kernel shell) */
    }
    if (owner == 0 || app_window_of(owner) != NULL) {
        return -16; /* EBUSY: one window per program */
    }
    size_t slot = 0;
    while (slot < APP_WINDOW_COUNT && app_windows[slot].owner != 0) {
        slot++;
    }
    if (slot == APP_WINDOW_COUNT) {
        return -16; /* EBUSY: every window slot is taken */
    }
    struct app_window *window = &app_windows[slot];
    if (window->surface == NULL) {
        uint64_t bytes = (uint64_t)APP_SURFACE_WIDTH * APP_SURFACE_HEIGHT
            * sizeof(uint32_t);
        uint64_t physical = physical_pages_alloc_contiguous(
            (bytes + 4095) / 4096);
        if (physical == 0) {
            return -12;
        }
        window->surface = physical_page_to_hhdm(physical);
    }
    uint32_t *surface = window->surface;
    *window = (struct app_window){
        .owner = owner,
        .surface = surface,
        .requested_width = width == 0 ? 320 : width > APP_SURFACE_WIDTH
            ? APP_SURFACE_WIDTH : width,
        .requested_height = height == 0 ? 240 : height > APP_SURFACE_HEIGHT
            ? APP_SURFACE_HEIGHT : height,
        .mouse_x = -1,
        .mouse_y = -1,
        .reported_width = -1,
        .reported_height = -1,
    };
    size_t used = 0;
    for (size_t i = 0; i < title_length && used + 1 < sizeof(window->title);
         i++) {
        char c = title[i];
        if (c >= ' ' && c <= '~') {
            window->title[used++] = c;
        }
    }
    window->title[used] = '\0';
    window->open_pending = true;
    /* The desktop opens it on its next pass. */
    while (!window->opened) {
        if (user_current_killed() || !desktop_active
            || window->owner != owner) {
            return -4; /* EINTR */
        }
        (void)scheduler_block_current_until_locked(
            window, timer_ticks() + APP_WAIT_RECHECK_TICKS);
    }
    return 0;
}

int64_t desktop_app_window_present(uint64_t owner, const uint32_t *pixels,
                                   uint32_t width, uint32_t height,
                                   uint32_t stride) {
    struct app_window *window = app_window_of(owner);
    if (window == NULL) {
        return -9; /* EBADF: no window */
    }
    if (window->closed || window->surface == NULL) {
        return 0;
    }
    uint32_t copy_width = width > APP_SURFACE_WIDTH ? APP_SURFACE_WIDTH
                                                    : width;
    uint32_t copy_height = height > APP_SURFACE_HEIGHT ? APP_SURFACE_HEIGHT
                                                       : height;
    for (uint32_t row = 0; row < copy_height; row++) {
        const uint32_t *source = pixels + (uint64_t)row * stride;
        uint32_t *target = window->surface
            + (uint64_t)row * APP_SURFACE_WIDTH;
        for (uint32_t column = 0; column < copy_width; column++) {
            target[column] = source[column];
        }
    }
    window->width = copy_width;
    window->height = copy_height;
    window->dirty = true;
    return 0;
}

uint64_t desktop_app_window_size(uint64_t owner) {
    struct app_window *window = app_window_of(owner);
    if (window == NULL || !app_window_live((size_t)(window - app_windows))
        || window->reported_width < 0 || window->reported_height < 0) {
        return 0;
    }
    return (uint64_t)window->reported_width << 32
        | (uint64_t)window->reported_height;
}

int64_t desktop_app_window_event(uint64_t owner,
                                 struct desktop_app_event *event,
                                 int64_t timeout_ms, bool *terminate) {
    *terminate = false;
    struct app_window *window = app_window_of(owner);
    if (window == NULL) {
        return -9;
    }
    if (window->waiting) {
        return -16; /* another thread of the program is already waiting */
    }
    uint64_t deadline = timeout_ms > 0
        ? timer_ticks() + ((uint64_t)timeout_ms + 9) / 10 : 0;
    window->waiting = true;
    int64_t result = 0;
    for (;;) {
        if (user_current_killed()) {
            break; /* the system call return ends the program */
        }
        if (window->closed) {
            if (window->close_sent) {
                /* Asked to close and still waiting: end it. */
                *terminate = true;
                break;
            }
            window->close_sent = true;
            *event = (struct desktop_app_event){.type = APP_EVENT_CLOSE};
            result = 1;
            break;
        }
        if (window->event_count != 0) {
            *event = window->events[window->event_head];
            window->event_head = (window->event_head + 1) % APP_EVENT_CAPACITY;
            window->event_count--;
            result = 1;
            break;
        }
        uint64_t now = timer_ticks();
        if (timeout_ms == 0 || (timeout_ms > 0 && now >= deadline)) {
            break;
        }
        uint64_t wake = now + APP_WAIT_RECHECK_TICKS;
        if (timeout_ms > 0 && deadline < wake) {
            wake = deadline;
        }
        (void)scheduler_block_current_until_locked(window, wake);
    }
    window->waiting = false;
    return result;
}

void desktop_app_process_exited(uint64_t process_id) {
    for (size_t i = 0; i < APP_WINDOW_COUNT; i++) {
        if (process_id != 0 && app_windows[i].owner == process_id) {
            app_windows[i].owner_exited = true;
        }
    }
}

/*
 * Standard output and error of every program go to the Terminal, through
 * this queue: a program waits while it is full, and the desktop empties it
 * on each pass.
 */
#define APP_OUTPUT_CAPACITY 4096
static char app_output[APP_OUTPUT_CAPACITY];
static size_t app_output_head;
static size_t app_output_count;
/* Output from a program without a window brings up the Terminal. */
static bool app_output_windowless;

static void app_output_sink(const char *data, size_t length, bool error) {
    (void)error;
    if (app_window_of(user_current_process_id()) == NULL) {
        app_output_windowless = true;
    }
    for (size_t i = 0; i < length; i++) {
        while (app_output_count == APP_OUTPUT_CAPACITY) {
            if (user_current_killed() || !desktop_active) {
                return;
            }
            (void)scheduler_sleep_until(timer_ticks() + 1);
        }
        app_output[(app_output_head + app_output_count)
                   % APP_OUTPUT_CAPACITY] = data[i];
        app_output_count++;
    }
}

/*
 * Standard input: while a program reads it, what is typed into the
 * Terminal goes to the program instead of the command line.
 */
#define APP_INPUT_CAPACITY 256
static char app_input[APP_INPUT_CAPACITY];
static size_t app_input_head;
static size_t app_input_count;
static size_t app_input_readers_drawn;

static void app_input_push(char character) {
    if ((character != '\n' && character != '\b'
         && (character < ' ' || character > '~'))
        || app_input_count == APP_INPUT_CAPACITY) {
        return;
    }
    app_input[(app_input_head + app_input_count) % APP_INPUT_CAPACITY] =
        character;
    app_input_count++;
    scheduler_wake_event(&app_input_readers);
}

static bool app_console_input(char *character) {
    for (;;) {
        if (user_current_killed() || !desktop_active) {
            return false;
        }
        if (app_input_count != 0) {
            *character = app_input[app_input_head];
            app_input_head = (app_input_head + 1) % APP_INPUT_CAPACITY;
            app_input_count--;
            return true;
        }
        app_input_readers++;
        (void)scheduler_block_current_until_locked(
            &app_input_readers, timer_ticks() + APP_WAIT_RECHECK_TICKS);
        app_input_readers--;
    }
}

/*
 * App jobs: each started program gets a scheduler task that loads it,
 * runs it to the end and records how it went. The desktop's pass reports
 * the result where the program was started from.
 */
#define APP_JOB_CAPACITY 8
#define APP_ARGUMENTS_CAPACITY 512

struct app_job {
    bool used;
    volatile bool finished;
    enum app_origin origin;
    bool builtin;
    enum user_program program;
    char path[256];
    size_t path_length;
    char arguments[APP_ARGUMENTS_CAPACITY];
    size_t arguments_length;
    /* Files: the file opened; Notepad: the file to edit. */
    char document[FILES_PATH_CAPACITY];
    size_t document_length;
    struct permission_identity identity;
    uint64_t task_id;
    uint64_t process_id;
    uint64_t exit_status;
    enum user_run_result result;
};

static struct app_job app_jobs[APP_JOB_CAPACITY];
/* Set while logging out: ending programs then opens nothing. */
static bool apps_stopping;

static void app_job_worker(void *argument) {
    struct app_job *job = argument;
    uint64_t process_id = 0;
    uint64_t exit_status = 1;
    enum user_run_result result;
    if (job->builtin) {
        result = user_process_run(job->program, &process_id, &exit_status)
            ? USER_RUN_STARTED : USER_RUN_FAILED;
    } else {
        result = user_process_run_file(job->path, job->path_length,
                                       job->arguments, job->arguments_length,
                                       &job->identity, &process_id,
                                       &exit_status);
    }
    job->process_id = process_id;
    job->exit_status = exit_status;
    job->result = result;
    __atomic_store_n(&job->finished, true, __ATOMIC_RELEASE);
}

static struct app_job *app_job_new(enum app_origin origin) {
    for (size_t i = 0; i < APP_JOB_CAPACITY; i++) {
        if (!app_jobs[i].used) {
            app_jobs[i] = (struct app_job){0};
            app_jobs[i].used = true;
            app_jobs[i].origin = origin;
            app_jobs[i].identity = permission_session_identity();
            return &app_jobs[i];
        }
    }
    return NULL;
}

static enum run_result app_job_start(struct app_job *job) {
    if (!scheduler_spawn(app_job_worker, job, &job->task_id)) {
        job->used = false;
        return RUN_BUSY;
    }
    return RUN_STARTED;
}

/*
 * Starts an ELF program from the initramfs (/bin, /lib) or a disk
 * (/disk...) as the logged-in account, after the same permission check
 * Files uses. RUN_STARTED means it is on its way: whether the file was
 * a program at all is known when its job ends.
 */
static enum run_result app_launch(enum app_origin origin, const char *typed,
                                  size_t typed_length, const char *arguments,
                                  size_t arguments_length,
                                  const char *document,
                                  size_t document_length) {
    char path[256];
    size_t length = permission_canonicalize(typed, typed_length, path,
                                            sizeof(path));
    if (length < 2) {
        return RUN_NOT_FOUND;
    }
    if (!permission_ui_allows(path, length, PERMISSION_READ)) {
        return RUN_DENIED;
    }
    if (arguments_length > APP_ARGUMENTS_CAPACITY
        || document_length > FILES_PATH_CAPACITY) {
        return RUN_FAILED;
    }
    struct app_job *job = app_job_new(origin);
    if (job == NULL) {
        return RUN_BUSY;
    }
    for (size_t i = 0; i < length; i++) {
        job->path[i] = path[i];
    }
    job->path_length = length;
    for (size_t i = 0; i < arguments_length; i++) {
        job->arguments[i] = arguments[i];
    }
    job->arguments_length = arguments_length;
    for (size_t i = 0; i < document_length; i++) {
        job->document[i] = document[i];
    }
    job->document_length = document_length;
    return app_job_start(job);
}

/* The Terminal's built-in test programs (elf, demo). */
static enum run_result app_launch_builtin(enum user_program program,
                                          const char *name) {
    struct app_job *job = app_job_new(APP_FROM_TERMINAL);
    if (job == NULL) {
        return RUN_BUSY;
    }
    job->builtin = true;
    job->program = program;
    job->path_length = append_text(job->path, sizeof(job->path), 0, name);
    return app_job_start(job);
}

static uint64_t app_job_process(const struct app_job *job) {
    return job->used && !job->finished ? user_task_process_id(job->task_id)
                                       : 0;
}

/* Ctrl+C in the Terminal: ends the newest program started there. */
static bool app_kill_terminal_job(void) {
    const struct app_job *newest = NULL;
    for (size_t i = 0; i < APP_JOB_CAPACITY; i++) {
        const struct app_job *job = &app_jobs[i];
        if (job->used && !job->finished && job->origin == APP_FROM_TERMINAL
            && (newest == NULL || job->task_id > newest->task_id)) {
            newest = job;
        }
    }
    return newest != NULL && user_process_kill(app_job_process(newest));
}

/* The Terminal's ps. */
static void apps_list(void) {
    bool any = false;
    for (size_t i = 0; i < APP_JOB_CAPACITY; i++) {
        const struct app_job *job = &app_jobs[i];
        if (!job->used || job->finished) {
            continue;
        }
        char line[TERMINAL_COLUMNS] = "PID ";
        uint64_t process_id = app_job_process(job);
        size_t length = process_id != 0
            ? append_decimal(line, sizeof(line), 4, process_id)
            : append_text(line, sizeof(line), 4, "-");
        length = append_text(line, sizeof(line), length, "  ");
        char path[sizeof(job->path) + 1];
        for (size_t j = 0; j < job->path_length; j++) {
            path[j] = job->path[j];
        }
        path[job->path_length] = '\0';
        append_text(line, sizeof(line), length, path);
        terminal_print(line);
        any = true;
    }
    if (!any) {
        terminal_print("NO PROGRAMS RUNNING");
    }
}

static enum run_result app_run_result(enum user_run_result result) {
    switch (result) {
    case USER_RUN_STARTED: return RUN_STARTED;
    case USER_RUN_NOT_FOUND: return RUN_NOT_FOUND;
    case USER_RUN_TOO_LARGE: return RUN_TOO_LARGE;
    case USER_RUN_NOT_PROGRAM: return RUN_NOT_PROGRAM;
    default: return RUN_FAILED;
    }
}

/* A job ended: say so where the program was started. */
static void app_job_report(struct app_job *job) {
    enum run_result result = app_run_result(job->result);
    bool started = result == RUN_STARTED;
    switch (job->origin) {
    case APP_FROM_TERMINAL:
        terminal_output_flush();
        if (started) {
            terminal_report_exit(true, job->exit_status);
        } else {
            terminal_print(run_result_text(result));
        }
        break;
    case APP_FROM_FILES:
        if (result == RUN_NOT_PROGRAM && !apps_stopping) {
            files_open_text(job->document, job->document_length);
        } else if (!started) {
            set_status(run_result_text(result));
        } else {
            set_status(job->exit_status == 0 ? "PROGRAM EXITED WITH STATUS 0"
                       : job->exit_status == 128 + 9 ? "PROGRAM WAS ENDED"
                                                     : "PROGRAM EXITED WITH ERROR");
        }
        break;
    case APP_FROM_NOTEPAD:
        if (!started && !apps_stopping) {
            if (job->document_length != 0) {
                (void)editor_open(job->document, job->document_length);
            } else {
                editor_new();
            }
        } else if (started && job->exit_status != 0
                   && job->exit_status != 128 + 9) {
            set_status("NOTEPAD EXITED WITH ERROR");
        }
        break;
    }
    if (windows[WINDOW_FILES].open) {
        files_refresh(); /* the program may have changed files */
    }
}

/*
 * Task Manager: the running programs with their CPU share over the last
 * second, memory and threads. End Program (or Delete) ends the selected
 * one, as the Terminal's kill does.
 */
#define TASKS_CAPACITY 16
#define TASKS_ROW_HEIGHT 24
#define TASKS_SAMPLE_TICKS 100

struct tasks_row {
    uint64_t id;
    char name[32];
    size_t threads;
    size_t memory_pages;
    uint64_t cpu_ticks;
    uint32_t cpu_percent;
};

static struct tasks_row tasks_rows[TASKS_CAPACITY];
static size_t tasks_row_count;
static uint64_t tasks_selected; /* process ID */
static uint64_t tasks_sample_tick;
static uint64_t tasks_idle_sample;
static uint32_t tasks_busy_percent;
static char tasks_message[48];

static uint32_t tasks_percent(uint64_t part, uint64_t whole) {
    if (whole == 0) {
        return 0;
    }
    uint64_t percent = (part * 100 + whole / 2) / whole;
    return percent > 100 ? 100 : (uint32_t)percent;
}

static void tasks_sample(void) {
    struct user_process_info info[TASKS_CAPACITY];
    size_t count = user_process_list(info, TASKS_CAPACITY);
    uint64_t now = timer_ticks();
    uint64_t idle = scheduler_idle_ticks();
    bool first = tasks_sample_tick == 0;
    uint64_t elapsed = now - tasks_sample_tick;
    struct tasks_row rows[TASKS_CAPACITY];
    for (size_t i = 0; i < count; i++) {
        rows[i] = (struct tasks_row){
            .id = info[i].id,
            .threads = info[i].threads,
            .memory_pages = info[i].memory_pages,
            .cpu_ticks = info[i].cpu_ticks,
        };
        for (size_t j = 0; j < sizeof(rows[i].name); j++) {
            rows[i].name[j] = info[i].name[j];
        }
        rows[i].name[sizeof(rows[i].name) - 1] = '\0';
        for (size_t j = 0; !first && j < tasks_row_count; j++) {
            if (tasks_rows[j].id == rows[i].id
                && rows[i].cpu_ticks >= tasks_rows[j].cpu_ticks) {
                rows[i].cpu_percent = tasks_percent(
                    rows[i].cpu_ticks - tasks_rows[j].cpu_ticks, elapsed);
            }
        }
    }
    tasks_busy_percent = first ? 0
        : 100 - tasks_percent(idle - tasks_idle_sample, elapsed);
    for (size_t i = 0; i < count; i++) {
        tasks_rows[i] = rows[i];
    }
    tasks_row_count = count;
    tasks_sample_tick = now;
    tasks_idle_sample = idle;
    bool selected = false;
    for (size_t i = 0; i < count; i++) {
        selected |= tasks_rows[i].id == tasks_selected;
    }
    if (!selected) {
        tasks_selected = count != 0 ? tasks_rows[0].id : 0;
    }
}

static void tasks_open(void) {
    window_open(WINDOW_TASKS);
    tasks_sample_tick = 0;
    tasks_row_count = 0;
    tasks_message[0] = '\0';
    tasks_sample();
}

static size_t tasks_selected_index(void) {
    for (size_t i = 0; i < tasks_row_count; i++) {
        if (tasks_rows[i].id == tasks_selected) {
            return i;
        }
    }
    return tasks_row_count;
}

static struct rect tasks_end_button(struct rect area) {
    return (struct rect){area.x + area.width - 16 - 15 * CHAR_WIDTH,
                         area.y + area.height - 40, 15 * CHAR_WIDTH, 28};
}

static struct rect tasks_row_rect(struct rect area, size_t index) {
    return (struct rect){area.x + 8, area.y + 72
                         + (int64_t)index * TASKS_ROW_HEIGHT,
                         area.width - 16, TASKS_ROW_HEIGHT};
}

static void tasks_end_selected(void) {
    size_t index = tasks_selected_index();
    if (index == tasks_row_count) {
        return;
    }
    size_t length = append_text(tasks_message, sizeof(tasks_message), 0,
                                user_process_kill(tasks_rows[index].id)
                                ? "ENDING " : "ALREADY ENDED: ");
    append_text(tasks_message, sizeof(tasks_message), length,
                tasks_rows[index].name);
}

static void draw_tasks(struct rect area, bool focused) {
    fill(area, 24, 30, 46);
    int64_t x = area.x + 16;
    int64_t width = area.width - 32;
    uint64_t total = memory_total_pages();
    uint64_t free = memory_free_pages();
    uint64_t used = total > free ? total - free : 0;
    char summary[80] = "CPU ";
    size_t length = append_decimal(summary, sizeof(summary), 4,
                                   tasks_busy_percent);
    length = append_text(summary, sizeof(summary), length, "%   MEMORY ");
    length = append_decimal(summary, sizeof(summary), length,
                            used * 4096 / (1024 * 1024));
    length = append_text(summary, sizeof(summary), length, " OF ");
    length = append_decimal(summary, sizeof(summary), length,
                            total * 4096 / (1024 * 1024));
    append_text(summary, sizeof(summary), length, " MB IN USE");
    draw_text_clipped(x, area.y + 14, width, summary, 151, 190, 255);

    static const char *const headings[] = {
        "PID", "NAME", "CPU", "MEMORY", "THREADS"
    };
    static const int64_t columns[] = {0, 7, 28, 35, 46};
    for (size_t i = 0; i < 5; i++) {
        draw_text_clipped(x + columns[i] * CHAR_WIDTH, area.y + 46,
                          width - columns[i] * CHAR_WIDTH, headings[i],
                          150, 160, 185);
    }
    struct rect button = tasks_end_button(area);
    size_t selected = tasks_selected_index();
    for (size_t i = 0; i < tasks_row_count; i++) {
        struct rect row = tasks_row_rect(area, i);
        if (row.y + row.height > button.y - 8) {
            break;
        }
        const struct tasks_row *task = &tasks_rows[i];
        if (i == selected) {
            fill(row, focused ? 52 : 40, focused ? 82 : 52,
                 focused ? 140 : 80);
        }
        char cells[5][32];
        append_decimal(cells[0], sizeof(cells[0]), 0, task->id);
        text_copy(cells[1], 20, task->name);
        length = append_decimal(cells[2], sizeof(cells[2]), 0,
                                task->cpu_percent);
        append_text(cells[2], sizeof(cells[2]), length, "%");
        uint64_t kib = (uint64_t)task->memory_pages * 4;
        length = kib >= 10240
            ? append_decimal(cells[3], sizeof(cells[3]), 0, kib / 1024)
            : append_decimal(cells[3], sizeof(cells[3]), 0, kib);
        append_text(cells[3], sizeof(cells[3]), length,
                    kib >= 10240 ? " MB" : " KB");
        append_decimal(cells[4], sizeof(cells[4]), 0, task->threads);
        for (size_t c = 0; c < 5; c++) {
            draw_text_clipped(x + columns[c] * CHAR_WIDTH, row.y + 5,
                              width - columns[c] * CHAR_WIDTH, cells[c],
                              230, 238, 255);
        }
    }
    if (tasks_row_count == 0) {
        draw_text_clipped(x, area.y + 77, width, "NO PROGRAMS RUNNING",
                          200, 214, 255);
    }
    draw_text_clipped(x, button.y + 7, button.x - x - 16,
                      tasks_message[0] != '\0' ? tasks_message
                      : "ARROWS: SELECT  DELETE: END PROGRAM",
                      tasks_message[0] != '\0' ? 240 : 150,
                      tasks_message[0] != '\0' ? 205 : 160,
                      tasks_message[0] != '\0' ? 100 : 185);
    bool enabled = selected != tasks_row_count;
    fill(button, enabled ? 150 : 50, enabled ? 60 : 56, enabled ? 70 : 72);
    draw_text_clipped(button.x + 12, button.y + 7, button.width - 24,
                      "END PROGRAM", enabled ? 255 : 130,
                      enabled ? 255 : 136, enabled ? 255 : 150);
}

static void tasks_click(struct rect area, int32_t x, int32_t y) {
    if (rect_contains(tasks_end_button(area), x, y)) {
        tasks_end_selected();
        return;
    }
    for (size_t i = 0; i < tasks_row_count; i++) {
        if (rect_contains(tasks_row_rect(area, i), x, y)) {
            tasks_selected = tasks_rows[i].id;
            tasks_message[0] = '\0';
        }
    }
}

static void tasks_key(char character) {
    size_t index = tasks_selected_index();
    if (character == KEY_UP && index != tasks_row_count && index != 0) {
        tasks_selected = tasks_rows[index - 1].id;
    } else if (character == KEY_DOWN && index + 1 < tasks_row_count) {
        tasks_selected = tasks_rows[index + 1].id;
    } else if (character == KEY_DELETE) {
        tasks_end_selected();
    }
}

/*
 * The desktop's side of programs, once per pass: opens and frees windows,
 * ends programs that ignore a close, shows output, reports ended jobs.
 * Returns true when the screen needs drawing.
 */
static bool apps_poll(void) {
    bool redraw = false;
    uint64_t now = timer_ticks();
    for (size_t slot = 0; slot < APP_WINDOW_COUNT; slot++) {
        struct app_window *window = &app_windows[slot];
        if (window->owner == 0) {
            continue;
        }
        if (window->owner_exited) {
            app_window_release(slot);
            redraw = true;
            continue;
        }
        if (window->open_pending) {
            app_window_open(slot);
            redraw = true;
        }
        if (window->closed && now - window->close_tick
                                  > APP_CLOSE_GRACE_TICKS) {
            (void)user_process_kill(window->owner);
        }
        if (window->dirty) {
            window->dirty = false;
            redraw = true;
        }
    }
    if (app_output_count != 0) {
        if (app_output_windowless && !windows[WINDOW_TERMINAL].open
            && !apps_stopping) {
            window_open(WINDOW_TERMINAL);
        }
        app_output_windowless = false;
        while (app_output_count != 0) {
            terminal_output_put(app_output[app_output_head]);
            app_output_head = (app_output_head + 1) % APP_OUTPUT_CAPACITY;
            app_output_count--;
        }
        redraw = true;
    }
    if (windows[WINDOW_TASKS].open
        && now - tasks_sample_tick >= TASKS_SAMPLE_TICKS) {
        tasks_sample();
        redraw = true;
    }
    if (app_input_readers != app_input_readers_drawn) {
        app_input_readers_drawn = app_input_readers;
        redraw = true;
    }
    for (size_t i = 0; i < APP_JOB_CAPACITY; i++) {
        struct app_job *job = &app_jobs[i];
        if (job->used && __atomic_load_n(&job->finished, __ATOMIC_ACQUIRE)) {
            app_job_report(job);
            job->used = false;
            redraw = true;
        }
    }
    return redraw;
}

/*
 * Log out, the shell: every program ends first (they run as the account
 * that is leaving). Ending wakes programs from any wait; this gives them
 * up to 3 seconds in case one is stuck in the kernel anyway.
 */
static void apps_stop_all(void) {
    apps_stopping = true;
    uint64_t deadline = timer_ticks() + APP_CLOSE_GRACE_TICKS;
    for (;;) {
        bool running = user_process_kill_all() != 0;
        for (size_t i = 0; i < APP_JOB_CAPACITY; i++) {
            if (app_jobs[i].used && !app_jobs[i].finished) {
                running = true;
            }
        }
        if (!running || timer_ticks() >= deadline) {
            break;
        }
        (void)scheduler_sleep_until(timer_ticks() + 1);
    }
    (void)apps_poll();
    for (size_t slot = 0; slot < APP_WINDOW_COUNT; slot++) {
        if (app_windows[slot].owner != 0 && app_windows[slot].owner_exited) {
            app_window_release(slot);
        }
    }
    app_input_count = 0;
    apps_stopping = false;
}

/*
 * One pass of the desktop: programs' requests, input, host resize, mouse,
 * clock. Returns true when the desktop should end (shell, log out to the
 * shell). Between passes the desktop sleeps, which is when programs run.
 */
static struct mouse_state loop_mouse;
static uint8_t loop_previous_buttons;
static struct drag_state loop_drag;
static uint64_t loop_drawn_tick;

static bool desktop_step(void) {
    bool redraw = apps_poll();
    char character;
    bool super_pending = false;
    while (keyboard_read_character(&character)) {
        redraw = true;
        if (character == KEY_SUPER_PREFIX) {
            super_pending = true;
            continue;
        }
        enum desktop_result result = handle_key(character, super_pending);
        super_pending = false;
        if (result == DESKTOP_EXIT || exit_to_shell) {
            return true;
        }
    }

    /* The host may resize the screen (VMware "Autofit Guest"). */
    int64_t width_before = screen_width();
    int64_t height_before = screen_height();
    vmware_tools_poll();
    if (screen_width() != width_before || screen_height() != height_before) {
        windows_fit_screen();
        redraw = true;
    }
    if (mouse_read_state(&loop_mouse)) {
        struct mouse_state mouse = loop_mouse;
        bool pressed = (mouse.buttons & 1) != 0;
        bool was_pressed = (loop_previous_buttons & 1) != 0;
        if (pressed && !was_pressed) {
            status_text[0] = '\0';
            if (handle_press(mouse.x, mouse.y, &loop_drag) == DESKTOP_EXIT) {
                return true;
            }
            redraw = true;
        } else if (pressed && loop_drag.active) {
            if (loop_drag.mode == DRAG_RESIZE) {
                redraw |= handle_resize(&loop_drag, mouse.x, mouse.y);
            } else if (loop_drag.mode == DRAG_SPLIT) {
                redraw |= handle_split(&loop_drag, mouse.x, mouse.y);
            } else {
                redraw |= handle_drag(&loop_drag, mouse.x, mouse.y);
            }
        } else if (!pressed) {
            loop_drag.active = false;
        }
        if ((mouse.buttons & 2) != 0 && (loop_previous_buttons & 2) == 0) {
            handle_right_press(mouse.x, mouse.y);
            redraw = true;
        }
        loop_previous_buttons = mouse.buttons;
        apps_mouse_update(mouse.x, mouse.y, mouse.buttons);
    }
    apps_check_resize();
    if (exit_to_shell) {
        return true;
    }

    /* Half-second tick drives the clock and the terminal caret blink. */
    uint64_t tick = timer_ticks() / 50;
    if (tick != loop_drawn_tick) {
        redraw = true;
    }
    if (redraw) {
        loop_drawn_tick = tick;
        draw_scene();
    } else {
        console_cursor_refresh();
        (void)scheduler_sleep_until(timer_ticks() + 1);
    }
    return false;
}

void desktop_run(void) {
    if (screen_width() < 640 || screen_height() < 480) {
        console_write_string("Desktop requires a 640x480 framebuffer.\n");
        return;
    }

    for (size_t i = 0; i < WINDOW_COUNT; i++) {
        windows[i].open = false;
        windows[i].floating.width = 0;
        split_ratio[i] = SPLIT_DEFAULT;
    }
    z_count = 0;
    tile_count = 0;
    start_menu_open = false;
    status_text[0] = '\0';
    exit_to_shell = false;
    login_active = false;
    if (installer_available()) {
        /* Live session: no accounts, the installer opens. */
        accounts_logout();
        files_places_rebuild();
        installer_open();
    } else {
        /* Returning from the shell keeps the session. */
        if (accounts_current() == ACCOUNT_NONE) {
            accounts_load();
        }
        files_places_rebuild();
        if (accounts_current() != ACCOUNT_NONE) {
            desktop_session_start();
        } else if (accounts_login_required()) {
            login_show();
        } else if (accounts_count() == 1 && accounts_login(0, "")) {
            desktop_session_start();
        } else {
            layout_load();
            welcome_open_first_time();
            window_open(WINDOW_TERMINAL);
        }
    }

    loop_mouse = (struct mouse_state){0};
    loop_previous_buttons = mouse_read_state(&loop_mouse)
        ? loop_mouse.buttons : 0;
    loop_drag = (struct drag_state){0};
    loop_drawn_tick = timer_ticks() / 50;
    /* From here the desktop gives up the processor only between passes,
       and programs' output and input go through the Terminal. */
    scheduler_set_kernel_preemption(false);
    user_set_console_sink(app_output_sink);
    user_set_console_input(app_console_input);
    desktop_active = true;
    draw_scene();

    while (!desktop_step()) {
    }
    apps_stop_all();
    desktop_active = false;
    user_set_console_input(NULL);
    user_set_console_sink(NULL);
    scheduler_set_kernel_preemption(true);
    console_clear();
}
