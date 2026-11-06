/*
 * Notepad: the OPOS text editor, built as an ordinary SDK program.
 *
 *     notepad [FILE]
 *
 * Typing, Enter, Tab, Backspace, Delete, arrows, Home, End, Page Up/Down.
 * Shift with movement keys, or dragging with the mouse, selects.
 * Ctrl+A selects all; Ctrl+C, Ctrl+X, Ctrl+V copy, cut and paste (inside
 * Notepad); Ctrl+S saves; Ctrl+Shift+S saves under a new name. Escape
 * quits, asking once first if there are unsaved changes. Files with CRLF
 * line ends are saved with CRLF again.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_gfx.h>

#define SCALE 2
#define CHAR_W opos_char_width(SCALE)
#define LINE_H (opos_char_height(SCALE) + 5)
#define MARGIN 8
#define STATUS_H 28
#define TAB_WIDTH 4
#define MAX_FILE (1024 * 1024)
#define PATH_MAX_LENGTH 255
#define NO_SELECTION ((size_t)-1)

#define COLOUR_BACK OPOS_RGB(14, 18, 28)
#define COLOUR_TEXT OPOS_RGB(225, 232, 245)
#define COLOUR_SELECT OPOS_RGB(52, 82, 140)
#define COLOUR_CARET OPOS_RGB(140, 190, 255)
#define COLOUR_BAR OPOS_RGB(30, 38, 58)
#define COLOUR_BAR_TEXT OPOS_RGB(200, 210, 230)
#define COLOUR_WARN OPOS_RGB(240, 205, 100)

static char *text;
static size_t length;
static size_t capacity;
static size_t caret;
static size_t anchor = NO_SELECTION;
static size_t goal_column = (size_t)-1;
static size_t scroll_pos;    /* first byte of the first visible line */
static size_t scroll_column; /* first visible display column */
static bool modified;
static bool crlf;
static bool quit_warned;
static char path[PATH_MAX_LENGTH + 1];
static char message[96];
static bool message_warn;

static char *clipboard;
static size_t clipboard_length;

static bool prompting; /* typing a file name for Save As */
static char prompt[PATH_MAX_LENGTH + 1];
static size_t prompt_length;

static struct opos_canvas canvas;
static int view_width;
static int view_height;
static bool dragging;

/* ------------------------------------------------------------ helpers */

static void set_message(const char *text_message, bool warn) {
    snprintf(message, sizeof(message), "%s", text_message);
    message_warn = warn;
}

static const char *error_text(long error) {
    switch (-error) {
    case OPOS_EACCES: return "ACCESS DENIED";
    case OPOS_ENOENT: return "FOLDER NOT FOUND";
    case OPOS_EISDIR: return "THAT IS A FOLDER";
    case OPOS_EROFS: return "READ-ONLY DISK";
    case OPOS_ENOMEM: return "OUT OF MEMORY";
    default: return "COULD NOT SAVE";
    }
}

static bool reserve(size_t needed) {
    if (needed <= capacity) {
        return true;
    }
    if (needed > MAX_FILE) {
        set_message("FILE IS FULL (1 MIB LIMIT)", true);
        return false;
    }
    size_t bigger = capacity == 0 ? 4096 : capacity;
    while (bigger < needed) {
        bigger *= 2;
    }
    if (bigger > MAX_FILE) {
        bigger = MAX_FILE;
    }
    char *grown = realloc(text, bigger);
    if (grown == NULL) {
        set_message("OUT OF MEMORY", true);
        return false;
    }
    text = grown;
    capacity = bigger;
    return true;
}

static const char *file_name(void) {
    if (path[0] == '\0') {
        return "UNTITLED";
    }
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

/* ------------------------------------------------------- text geometry */

static size_t line_start(size_t position) {
    while (position > 0 && text[position - 1] != '\n') {
        position--;
    }
    return position;
}

static size_t line_end(size_t position) {
    while (position < length && text[position] != '\n') {
        position++;
    }
    return position;
}

static size_t next_column(size_t column, char character) {
    return character == '\t' ? (column / TAB_WIDTH + 1) * TAB_WIDTH
                             : column + 1;
}

static size_t column_of(size_t position) {
    size_t column = 0;
    for (size_t i = line_start(position); i < position; i++) {
        column = next_column(column, text[i]);
    }
    return column;
}

/* The position in the line starting at start closest to a column. */
static size_t position_at_column(size_t start, size_t column) {
    size_t current = 0;
    size_t i = start;
    while (i < length && text[i] != '\n') {
        size_t next = next_column(current, text[i]);
        if (next > column) {
            if (column - current >= next - column) {
                i++;
            }
            break;
        }
        current = next;
        i++;
    }
    return i;
}

static size_t line_number(size_t position) {
    size_t line = 1;
    for (size_t i = 0; i < position; i++) {
        if (text[i] == '\n') {
            line++;
        }
    }
    return line;
}

static int visible_lines(void) {
    int lines = (view_height - STATUS_H - MARGIN) / LINE_H;
    return lines < 1 ? 1 : lines;
}

static int visible_columns(void) {
    int columns = (view_width - 2 * MARGIN) / CHAR_W;
    return columns < 1 ? 1 : columns;
}

static void keep_caret_visible(void) {
    if (caret < scroll_pos) {
        scroll_pos = line_start(caret);
    } else {
        /* Count lines from the top of the view to the caret's line. */
        int lines = 0;
        size_t caret_line = line_start(caret);
        for (size_t i = scroll_pos; i < caret_line; i++) {
            if (text[i] == '\n') {
                lines++;
            }
        }
        while (lines >= visible_lines()) {
            scroll_pos = line_end(scroll_pos) + 1;
            lines--;
        }
    }
    size_t column = column_of(caret);
    if (column < scroll_column) {
        scroll_column = column;
    } else if (column >= scroll_column + (size_t)visible_columns()) {
        scroll_column = column - (size_t)visible_columns() + 1;
    }
}

/* ------------------------------------------------------------- editing */

static bool has_selection(void) {
    return anchor != NO_SELECTION && anchor != caret;
}

static size_t selection_start(void) {
    return anchor < caret ? anchor : caret;
}

static size_t selection_end(void) {
    return anchor < caret ? caret : anchor;
}

static void delete_range(size_t start, size_t end) {
    memmove(text + start, text + end, length - end);
    length -= end - start;
    caret = start;
    anchor = NO_SELECTION;
    modified = true;
}

static bool delete_selection(void) {
    if (!has_selection()) {
        anchor = NO_SELECTION;
        return false;
    }
    delete_range(selection_start(), selection_end());
    return true;
}

static void insert(const char *data, size_t count) {
    delete_selection();
    if (!reserve(length + count)) {
        return;
    }
    memmove(text + caret + count, text + caret, length - caret);
    memcpy(text + caret, data, count);
    length += count;
    caret += count;
    modified = true;
}

static void copy_selection(void) {
    if (!has_selection()) {
        return;
    }
    size_t count = selection_end() - selection_start();
    char *copy = realloc(clipboard, count == 0 ? 1 : count);
    if (copy == NULL) {
        set_message("OUT OF MEMORY", true);
        return;
    }
    clipboard = copy;
    memcpy(clipboard, text + selection_start(), count);
    clipboard_length = count;
}

/* Moves the caret; with select (Shift) the selection follows. */
static void move_to(size_t position, bool select) {
    if (select) {
        if (anchor == NO_SELECTION) {
            anchor = caret;
        }
    } else {
        anchor = NO_SELECTION;
    }
    caret = position;
}

static void move_lines(int delta, bool select) {
    if (goal_column == (size_t)-1) {
        goal_column = column_of(caret);
    }
    size_t start = line_start(caret);
    for (; delta < 0 && start > 0; delta++) {
        start = line_start(start - 1);
    }
    for (; delta > 0; delta--) {
        size_t end = line_end(start);
        if (end >= length) {
            break;
        }
        start = end + 1;
    }
    size_t keep = goal_column;
    move_to(position_at_column(start, goal_column), select);
    goal_column = keep;
}

/* ---------------------------------------------------------------- files */

static void load(const char *name) {
    snprintf(path, sizeof(path), "%s", name);
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        set_message(fd == -OPOS_ENOENT ? "NEW FILE" : error_text(fd),
                    fd != -OPOS_ENOENT);
        if (fd == -OPOS_EACCES) {
            path[0] = '\0';
        }
        return;
    }
    char chunk[4096];
    for (;;) {
        long count = opos_read((int)fd, chunk, sizeof(chunk));
        if (count <= 0) {
            break;
        }
        if (!reserve(length + (size_t)count)) {
            break;
        }
        memcpy(text + length, chunk, (size_t)count);
        length += (size_t)count;
    }
    opos_close((int)fd);
    /* Keep LF inside; remember CRLF files to write them back alike. */
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '\r' && i + 1 < length && text[i + 1] == '\n') {
            crlf = true;
            continue;
        }
        text[out++] = text[i];
    }
    length = out;
}

static bool write_all(int fd, const char *data, size_t count) {
    while (count != 0) {
        size_t chunk = count > 4096 ? 4096 : count;
        long written = opos_write(fd, data, chunk);
        if (written <= 0) {
            return false;
        }
        data += written;
        count -= (size_t)written;
    }
    return true;
}

static void save(void) {
    long fd = opos_open(path, OPOS_O_WRONLY | OPOS_O_CREAT | OPOS_O_TRUNC);
    if (fd < 0) {
        set_message(error_text(fd), true);
        return;
    }
    bool ok = true;
    if (crlf) {
        char buffer[4096];
        size_t used = 0;
        for (size_t i = 0; ok && i < length; i++) {
            if (text[i] == '\n') {
                buffer[used++] = '\r';
            }
            buffer[used++] = text[i];
            if (used >= sizeof(buffer) - 1) {
                ok = write_all((int)fd, buffer, used);
                used = 0;
            }
        }
        ok = ok && write_all((int)fd, buffer, used);
    } else {
        ok = write_all((int)fd, text, length);
    }
    opos_close((int)fd);
    if (ok) {
        modified = false;
        quit_warned = false;
        char saved[96];
        snprintf(saved, sizeof(saved), "SAVED %s", file_name());
        set_message(saved, false);
    } else {
        set_message("COULD NOT WRITE THE WHOLE FILE", true);
    }
}

static void start_save_as(void) {
    prompting = true;
    if (path[0] != '\0') {
        snprintf(prompt, sizeof(prompt), "%s", path);
    } else {
        struct opos_user_info user;
        if (opos_user_info(&user) == 0 && (user.flags & OPOS_USER_ACCOUNT)) {
            snprintf(prompt, sizeof(prompt), "%s/Documents/untitled.txt",
                     user.home);
        } else {
            snprintf(prompt, sizeof(prompt), "/disk/untitled.txt");
        }
    }
    prompt_length = strlen(prompt);
}

static void prompt_key(unsigned key) {
    if (key == OPOS_KEY_ESCAPE) {
        prompting = false;
    } else if (key == '\n') {
        if (prompt_length != 0) {
            prompting = false;
            snprintf(path, sizeof(path), "%s", prompt);
            save();
        }
    } else if (key == '\b') {
        if (prompt_length != 0) {
            prompt[--prompt_length] = '\0';
        }
    } else if (key >= ' ' && key <= '~' && prompt_length < PATH_MAX_LENGTH) {
        prompt[prompt_length++] = (char)key;
        prompt[prompt_length] = '\0';
    }
}

/* -------------------------------------------------------------- input */

/* Returns false when Notepad should quit. */
static bool key_pressed(unsigned key, unsigned modifiers) {
    bool ctrl = (modifiers & OPOS_MOD_CTRL) != 0;
    bool shift = (modifiers & OPOS_MOD_SHIFT) != 0;
    if (prompting) {
        prompt_key(key);
        return true;
    }
    message[0] = '\0';
    if (key != OPOS_KEY_UP && key != OPOS_KEY_DOWN
        && key != OPOS_KEY_PAGE_UP && key != OPOS_KEY_PAGE_DOWN) {
        goal_column = (size_t)-1;
    }
    if (key != OPOS_KEY_ESCAPE) {
        quit_warned = false;
    }
    if (ctrl) {
        switch (key) {
        case 'a':
        case 'A':
            anchor = 0;
            caret = length;
            break;
        case 'c':
        case 'C':
            copy_selection();
            break;
        case 'x':
        case 'X':
            copy_selection();
            delete_selection();
            break;
        case 'v':
        case 'V':
            if (clipboard_length != 0) {
                insert(clipboard, clipboard_length);
            }
            break;
        case 's':
            if (path[0] == '\0') {
                start_save_as();
            } else {
                save();
            }
            break;
        case 'S':
            start_save_as();
            break;
        default:
            break;
        }
        return true;
    }
    switch (key) {
    case OPOS_KEY_ESCAPE:
        if (modified && !quit_warned) {
            quit_warned = true;
            set_message("UNSAVED CHANGES: ESC AGAIN QUITS, CTRL+S SAVES",
                        true);
            return true;
        }
        return false;
    case OPOS_KEY_LEFT:
        if (has_selection() && !shift) {
            move_to(selection_start(), false);
        } else if (caret > 0) {
            move_to(caret - 1, shift);
        }
        break;
    case OPOS_KEY_RIGHT:
        if (has_selection() && !shift) {
            move_to(selection_end(), false);
        } else if (caret < length) {
            move_to(caret + 1, shift);
        }
        break;
    case OPOS_KEY_UP:
        move_lines(-1, shift);
        break;
    case OPOS_KEY_DOWN:
        move_lines(1, shift);
        break;
    case OPOS_KEY_PAGE_UP:
        move_lines(-visible_lines(), shift);
        break;
    case OPOS_KEY_PAGE_DOWN:
        move_lines(visible_lines(), shift);
        break;
    case OPOS_KEY_HOME:
        move_to(line_start(caret), shift);
        break;
    case OPOS_KEY_END:
        move_to(line_end(caret), shift);
        break;
    case '\b':
        if (!delete_selection() && caret > 0) {
            delete_range(caret - 1, caret);
        }
        break;
    case OPOS_KEY_DELETE:
        if (!delete_selection() && caret < length) {
            delete_range(caret, caret + 1);
        }
        break;
    case '\n': {
        /* Keep the indentation of the line above. */
        char indent[128] = "\n";
        size_t count = 1;
        for (size_t i = line_start(caret);
             i < caret && (text[i] == ' ' || text[i] == '\t')
             && count < sizeof(indent);
             i++) {
            indent[count++] = text[i];
        }
        insert(indent, count);
        break;
    }
    case '\t':
        insert("    ", TAB_WIDTH - column_of(caret) % TAB_WIDTH);
        break;
    default:
        if (key >= ' ' && key <= '~') {
            char character = (char)key;
            insert(&character, 1);
        }
        break;
    }
    return true;
}

static size_t position_at_point(int x, int y) {
    int row = y < MARGIN ? -1 : (y - MARGIN) / LINE_H;
    size_t start = scroll_pos;
    if (row < 0) {
        start = start > 0 ? line_start(start - 1) : 0;
    }
    for (int i = 0; i < row; i++) {
        size_t end = line_end(start);
        if (end >= length) {
            break;
        }
        start = end + 1;
    }
    int column = x < MARGIN ? 0 : (x - MARGIN + CHAR_W / 2) / CHAR_W;
    return position_at_column(start, scroll_column + (size_t)column);
}

/* ------------------------------------------------------------ drawing */

static void draw(void) {
    int width = view_width < canvas.width ? view_width : canvas.width;
    int height = view_height < canvas.height ? view_height : canvas.height;
    opos_fill_rect(&canvas, 0, 0, width, height, COLOUR_BACK);

    int text_bottom = height - STATUS_H;
    size_t select_from = has_selection() ? selection_start() : 0;
    size_t select_to = has_selection() ? selection_end() : 0;
    size_t position = scroll_pos;
    for (int row = 0; row < visible_lines() && position <= length; row++) {
        int y = MARGIN + row * LINE_H;
        if (y + LINE_H > text_bottom + LINE_H / 2) {
            break;
        }
        size_t column = 0;
        size_t end = line_end(position);
        for (size_t i = position; i <= end; i++) {
            size_t next = i < end ? next_column(column, text[i]) : column + 1;
            bool selected = i >= select_from && i < select_to;
            if (column >= scroll_column) {
                int x = MARGIN + (int)(column - scroll_column) * CHAR_W;
                int cell = (int)(next - column) * CHAR_W;
                if (x >= width) {
                    break;
                }
                if (selected) {
                    opos_fill_rect(&canvas, x, y - 2, i < end ? cell : CHAR_W / 2,
                                   LINE_H - 1, COLOUR_SELECT);
                }
                if (i < end && text[i] != '\t') {
                    char shown = text[i] >= ' ' && text[i] <= '~' ? text[i]
                                                                  : '?';
                    opos_draw_char(&canvas, x, y, shown, COLOUR_TEXT, SCALE);
                }
                if (i == caret && !prompting) {
                    opos_fill_rect(&canvas, x - 1, y - 2, 2, LINE_H - 1,
                                   COLOUR_CARET);
                }
            }
            column = next;
        }
        if (end >= length) {
            break;
        }
        position = end + 1;
    }

    /* Status bar: file, position, and a message or the Save As prompt. */
    int bar_y = height - STATUS_H;
    opos_fill_rect(&canvas, 0, bar_y, width, STATUS_H, COLOUR_BAR);
    int text_y = bar_y + (STATUS_H - opos_char_height(SCALE)) / 2;
    char line[160];
    if (prompting) {
        snprintf(line, sizeof(line), "SAVE AS: %s_", prompt);
        opos_draw_text(&canvas, MARGIN, text_y, line, COLOUR_WARN, SCALE);
        opos_canvas_present_area(&canvas, width, height);
        return;
    }
    snprintf(line, sizeof(line), "%s%s  LN %zu, COL %zu%s", file_name(),
             modified ? " *" : "", line_number(caret), column_of(caret) + 1,
             crlf ? "  CRLF" : "");
    int x = opos_draw_text(&canvas, MARGIN, text_y, line, COLOUR_BAR_TEXT,
                           SCALE);
    if (message[0] != '\0') {
        opos_draw_text(&canvas, x + 2 * CHAR_W, text_y, message,
                       message_warn ? COLOUR_WARN : COLOUR_BAR_TEXT, SCALE);
    }
    opos_canvas_present_area(&canvas, width, height);
}

/* A bigger window needs a bigger canvas; it never shrinks. If memory runs
   out the old canvas stays and draws clip to it. */
static void grow_canvas(int width, int height) {
    if (width <= canvas.width && height <= canvas.height) {
        return;
    }
    if (width < canvas.width) {
        width = canvas.width;
    }
    if (height < canvas.height) {
        height = canvas.height;
    }
    struct opos_canvas bigger = opos_canvas_create(width, height);
    if (bigger.pixels == NULL) {
        return;
    }
    opos_canvas_destroy(&canvas);
    canvas = bigger;
}

/* --------------------------------------------------------------- main */

int main(int argc, char **argv, char **envp) {
    (void)envp;
    canvas = opos_canvas_create(1024, 768);
    if (canvas.pixels == NULL || !reserve(4096)) {
        eprintf("notepad: out of memory\n");
        return 1;
    }
    if (argc > 1) {
        load(argv[1]);
    }
    char title[32];
    snprintf(title, sizeof(title), "NOTEPAD - %s", file_name());
    long created = opos_window_create(title, 760, 520);
    if (created < 0) {
        eprintf("notepad: no window (%ld); run it from the desktop\n",
                created);
        return 1;
    }

    struct opos_event event;
    /* Handle every queued event, then draw once: a held key queues them
       faster than a redraw per key could keep up with. */
    long timeout = -1;
    for (;;) {
        long got = opos_window_event(&event, timeout);
        if (got < 0) {
            continue;
        }
        if (got == 0) {
            draw();
            timeout = -1;
            continue;
        }
        timeout = 0;
        switch (event.type) {
        case OPOS_EVENT_RESIZE:
            view_width = (int)event.width;
            view_height = (int)event.height;
            grow_canvas(view_width, view_height);
            break;
        case OPOS_EVENT_KEY:
            if (!key_pressed(event.key, event.buttons)) {
                return 0;
            }
            if (!prompting) {
                keep_caret_visible();
            }
            break;
        case OPOS_EVENT_MOUSE_DOWN:
            if (event.button == OPOS_BUTTON_LEFT && !prompting
                && event.y < view_height - STATUS_H) {
                move_to(position_at_point(event.x, event.y), false);
                anchor = caret;
                dragging = true;
                goal_column = (size_t)-1;
            }
            break;
        case OPOS_EVENT_MOUSE_MOVE:
            if (dragging && (event.buttons & OPOS_BUTTON_LEFT) != 0) {
                caret = position_at_point(event.x, event.y);
                keep_caret_visible();
            }
            break;
        case OPOS_EVENT_MOUSE_UP:
            dragging = false;
            break;
        case OPOS_EVENT_WHEEL: {
            /* Three lines per notch; the caret stays where it is. */
            int lines = OPOS_WHEEL_NOTCHES(event) * 3;
            for (; lines > 0 && scroll_pos > 0; lines--) {
                scroll_pos = line_start(scroll_pos - 1);
            }
            for (; lines < 0 && line_end(scroll_pos) < length; lines++) {
                scroll_pos = line_end(scroll_pos) + 1;
            }
            break;
        }
        case OPOS_EVENT_CLOSE:
            return 0;
        default:
            break;
        }
    }
}
