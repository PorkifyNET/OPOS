/*
 * Minesweeper for OPOS.
 *
 * Find the mines without stepping on one. A number tells how many of the
 * eight cells around it hold a mine.
 *
 * Mouse: left button opens a cell, right button flags it (and then marks
 * it "?"), a click on an opened number whose flags are all placed opens
 * the cells around it. The face starts a new game.
 * Keys: arrows move, Space or Enter open, F flags, N starts a new game,
 * 1, 2 and 3 pick the level (beginner, intermediate, expert).
 */
#include <stdbool.h>
#include <stdint.h>

#include <opos.h>
#include <opos_gfx.h>

#include "fit.h"

OPOS_MANIFEST("Minesweeper", "1.0", 0);

#define CELL 24
#define BORDER 12
#define BAR_HEIGHT 44
#define MAX_COLUMNS 30
#define MAX_ROWS 16
#define MAX_CELLS (MAX_COLUMNS * MAX_ROWS)

/* Colours */
#define BACKGROUND OPOS_RGB(36, 42, 58)
#define BAR OPOS_RGB(28, 33, 46)
#define CLOSED OPOS_RGB(92, 110, 150)
#define CLOSED_LIGHT OPOS_RGB(128, 146, 186)
#define CLOSED_DARK OPOS_RGB(58, 70, 100)
#define OPENED OPOS_RGB(214, 220, 232)
#define OPENED_LINE OPOS_RGB(170, 178, 196)
#define CURSOR OPOS_RGB(255, 210, 80)
#define TEXT OPOS_RGB(230, 238, 255)
#define DIGITS OPOS_RGB(255, 96, 80)

struct level {
    const char *name;
    int columns, rows, mines;
};

static const struct level levels[] = {
    {"BEGINNER", 9, 9, 10},
    {"INTERMEDIATE", 16, 16, 40},
    {"EXPERT", 30, 16, 99},
};

static const uint32_t number_colours[9] = {
    0,
    OPOS_RGB(30, 90, 220),  OPOS_RGB(30, 140, 50),  OPOS_RGB(210, 40, 40),
    OPOS_RGB(40, 30, 150),  OPOS_RGB(140, 30, 30),  OPOS_RGB(20, 140, 140),
    OPOS_RGB(20, 20, 20),   OPOS_RGB(110, 110, 110),
};

enum state { PLAYING, WON, LOST };
enum mark { MARK_NONE, MARK_FLAG, MARK_QUESTION };

static struct fit fit;
/* What the game draws on (fit.h shrinks it into a small window). */
static struct opos_canvas *const canvas = &fit.drawing;


static int level_index;
static int columns, rows, mines;
static bool mine[MAX_CELLS];
static bool opened[MAX_CELLS];
static uint8_t mark[MAX_CELLS];
static uint8_t near[MAX_CELLS];
static bool placed;      /* mines are placed at the first opening */
static int exploded = -1;
static enum state state;
static int opened_count, flag_count;
static long start_seconds = -1, end_seconds;
static int cursor_x, cursor_y;
static bool show_cursor; /* after a key; a click hides it */
static bool face_down;   /* the face is held down */
static int pressed = -1; /* cell held down by the left button */

/* ------------------------------------------------------------- random */

static uint64_t random_state;

static uint64_t random_next(void) {
    /* xorshift64* */
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return random_state * 0x2545f4914f6cdd1dull;
}

static void random_seed(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    struct opos_timespec now;
    opos_uptime(&now);
    random_state = ((uint64_t)high << 32 | low)
                 ^ (uint64_t)now.nanoseconds * 0x9e3779b97f4a7c15ull
                 ^ (uint64_t)now.seconds;
    if (random_state == 0) {
        random_state = 0x853c49e6748fea9bull;
    }
}

static int random_below(int limit) {
    return (int)(random_next() % (uint64_t)limit);
}

/* --------------------------------------------------------------- game */

static long seconds_now(void) {
    struct opos_timespec now;
    opos_uptime(&now);
    return now.seconds;
}

static int elapsed(void) {
    if (start_seconds < 0) {
        return 0;
    }
    long end = state == PLAYING ? seconds_now() : end_seconds;
    long seconds = end - start_seconds;
    return seconds > 999 ? 999 : (int)seconds;
}

static int content_width(void) {
    return columns * CELL + 2 * BORDER;
}

static int content_height(void) {
    return BAR_HEIGHT + rows * CELL + 2 * BORDER;
}

static void new_game(void) {
    const struct level *level = &levels[level_index];
    columns = level->columns;
    rows = level->rows;
    mines = level->mines;
    for (int i = 0; i < MAX_CELLS; i++) {
        mine[i] = opened[i] = false;
        mark[i] = MARK_NONE;
        near[i] = 0;
    }
    placed = false;
    exploded = -1;
    state = PLAYING;
    opened_count = flag_count = 0;
    start_seconds = -1;
    if (cursor_x >= columns) {
        cursor_x = columns - 1;
    }
    if (cursor_y >= rows) {
        cursor_y = rows - 1;
    }
}

static bool inside(int x, int y) {
    return x >= 0 && x < columns && y >= 0 && y < rows;
}

/* Places the mines anywhere but on and around the first cell opened. */
static void place_mines(int safe_x, int safe_y) {
    int left = mines;
    while (left > 0) {
        int x = random_below(columns), y = random_below(rows);
        int dx = x - safe_x, dy = y - safe_y;
        bool near_safe = dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1;
        /* A tiny board may not have room to keep the whole ring clear. */
        bool room = columns * rows - 9 >= mines;
        if (mine[y * columns + x] || (near_safe && (room || (dx == 0
                                                             && dy == 0)))) {
            continue;
        }
        mine[y * columns + x] = true;
        left--;
    }
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < columns; x++) {
            int count = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (inside(x + dx, y + dy)
                        && mine[(y + dy) * columns + x + dx]) {
                        count++;
                    }
                }
            }
            near[y * columns + x] = (uint8_t)count;
        }
    }
    placed = true;
    start_seconds = seconds_now();
}

static void finish(enum state result) {
    state = result;
    end_seconds = seconds_now();
    if (start_seconds < 0) {
        start_seconds = end_seconds;
    }
    if (result == WON) {
        /* Every mine gets its flag. */
        for (int i = 0; i < columns * rows; i++) {
            if (mine[i]) {
                mark[i] = MARK_FLAG;
            }
        }
        flag_count = mines;
    }
}

/* Opens a cell; an empty one opens its neighbours too. */
static void open_cell(int x, int y) {
    if (state != PLAYING || !inside(x, y)) {
        return;
    }
    if (!placed) {
        place_mines(x, y);
    }
    int index = y * columns + x;
    if (opened[index] || mark[index] == MARK_FLAG) {
        return;
    }
    if (mine[index]) {
        opened[index] = true;
        exploded = index;
        finish(LOST);
        return;
    }
    static int stack[MAX_CELLS];
    int depth = 0;
    stack[depth++] = index;
    opened[index] = true;
    opened_count++;
    if (mark[index] == MARK_QUESTION) {
        mark[index] = MARK_NONE;
    }
    while (depth > 0) {
        int cell = stack[--depth];
        if (near[cell] != 0) {
            continue;
        }
        int cx = cell % columns, cy = cell / columns;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (!inside(nx, ny)) {
                    continue;
                }
                int next = ny * columns + nx;
                if (opened[next] || mark[next] == MARK_FLAG || mine[next]) {
                    continue;
                }
                opened[next] = true;
                opened_count++;
                mark[next] = MARK_NONE;
                stack[depth++] = next;
            }
        }
    }
    if (opened_count == columns * rows - mines) {
        finish(WON);
    }
}

/* On an opened number with as many flags around it: opens the rest. */
static void open_around(int x, int y) {
    int index = y * columns + x;
    if (state != PLAYING || !opened[index] || near[index] == 0) {
        return;
    }
    int flags = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (inside(x + dx, y + dy)
                && mark[(y + dy) * columns + x + dx] == MARK_FLAG) {
                flags++;
            }
        }
    }
    if (flags != near[index]) {
        return;
    }
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            open_cell(x + dx, y + dy);
        }
    }
}

static void toggle_mark(int x, int y) {
    int index = y * columns + x;
    if (state != PLAYING || !inside(x, y) || opened[index]) {
        return;
    }
    if (mark[index] == MARK_NONE) {
        mark[index] = MARK_FLAG;
        flag_count++;
    } else if (mark[index] == MARK_FLAG) {
        mark[index] = MARK_QUESTION;
        flag_count--;
    } else {
        mark[index] = MARK_NONE;
    }
}

/* Opening a cell: a number opens around it, anything else opens it. */
static void activate(int x, int y) {
    int index = y * columns + x;
    if (opened[index]) {
        open_around(x, y);
    } else {
        open_cell(x, y);
    }
}

/* ------------------------------------------------------------ drawing */

/* Where the board starts: centred in the window. */
static int board_x(void) {
    int x = (fit.width - columns * CELL) / 2;
    return x < BORDER ? BORDER : x;
}

static int board_y(void) {
    int spare = fit.height - content_height();
    return BAR_HEIGHT + BORDER + (spare > 0 ? spare / 2 : 0);
}

static void draw_counter(int x, int y, int value) {
    char text[4];
    if (value < -99) {
        value = -99;
    }
    if (value < 0) {
        text[0] = '-';
        value = -value;
        text[1] = (char)('0' + value / 10);
        text[2] = (char)('0' + value % 10);
    } else {
        text[0] = (char)('0' + value / 100 % 10);
        text[1] = (char)('0' + value / 10 % 10);
        text[2] = (char)('0' + value % 10);
    }
    text[3] = '\0';
    opos_fill_rect(canvas, x, y, 3 * opos_char_width(3) + 6, 34,
                   OPOS_RGB(10, 10, 14));
    opos_draw_text(canvas, x + 4, y + 4, text, DIGITS, 3);
}

/* A little face: smiling, surprised (while a cell is held), cool (won)
   or crossed out (lost). */
static void draw_face(void) {
    int size = 32;
    int x = (fit.width - size) / 2;
    int y = board_y() - BORDER - BAR_HEIGHT + (BAR_HEIGHT - size) / 2;
    opos_fill_rect(canvas, x, y, size, size,
                   face_down ? CLOSED_DARK : CLOSED_LIGHT);
    opos_fill_rect(canvas, x + 2, y + 2, size - 4, size - 4, CLOSED);
    uint32_t yellow = OPOS_RGB(250, 210, 60), ink = OPOS_RGB(30, 30, 30);
    int cx = x + size / 2, cy = y + size / 2;
    for (int dy = -11; dy <= 11; dy++) {
        for (int dx = -11; dx <= 11; dx++) {
            if (dx * dx + dy * dy <= 121) {
                opos_fill_rect(canvas, cx + dx, cy + dy, 1, 1, yellow);
            }
        }
    }
    if (state == LOST) {
        for (int i = -2; i <= 2; i++) {
            opos_fill_rect(canvas, cx - 5 + i, cy - 4 + i, 1, 1, ink);
            opos_fill_rect(canvas, cx - 5 + i, cy - 4 - i, 1, 1, ink);
            opos_fill_rect(canvas, cx + 5 + i, cy - 4 + i, 1, 1, ink);
            opos_fill_rect(canvas, cx + 5 + i, cy - 4 - i, 1, 1, ink);
        }
    } else if (state == WON) {
        opos_fill_rect(canvas, cx - 9, cy - 5, 18, 2, ink);
        opos_fill_rect(canvas, cx - 8, cy - 5, 6, 4, ink);
        opos_fill_rect(canvas, cx + 2, cy - 5, 6, 4, ink);
    } else {
        opos_fill_rect(canvas, cx - 5, cy - 5, 3, 3, ink);
        opos_fill_rect(canvas, cx + 3, cy - 5, 3, 3, ink);
    }
    if (pressed >= 0 && state == PLAYING) {
        /* An "o" mouth. */
        opos_draw_rect(canvas, cx - 2, cy + 3, 5, 5, ink);
    } else if (state == LOST) {
        opos_fill_rect(canvas, cx - 4, cy + 5, 9, 2, ink);
        opos_fill_rect(canvas, cx - 5, cy + 7, 2, 2, ink);
        opos_fill_rect(canvas, cx + 4, cy + 7, 2, 2, ink);
    } else {
        opos_fill_rect(canvas, cx - 5, cy + 4, 2, 2, ink);
        opos_fill_rect(canvas, cx + 4, cy + 4, 2, 2, ink);
        opos_fill_rect(canvas, cx - 3, cy + 6, 7, 2, ink);
    }
}

static void draw_mine(int x, int y) {
    uint32_t ink = OPOS_RGB(20, 20, 24);
    int cx = x + CELL / 2, cy = y + CELL / 2;
    opos_fill_rect(canvas, cx - 9, cy - 1, 19, 3, ink);
    opos_fill_rect(canvas, cx - 1, cy - 9, 3, 19, ink);
    for (int dy = -6; dy <= 6; dy++) {
        for (int dx = -6; dx <= 6; dx++) {
            if (dx * dx + dy * dy <= 36) {
                opos_fill_rect(canvas, cx + dx, cy + dy, 1, 1, ink);
            }
        }
    }
    for (int i = -6; i <= 6; i++) {
        opos_fill_rect(canvas, cx + i, cy + i, 2, 2, ink);
        opos_fill_rect(canvas, cx + i, cy - i, 2, 2, ink);
    }
    opos_fill_rect(canvas, cx - 3, cy - 3, 3, 3, OPOS_RGB(240, 240, 240));
}

static void draw_flag(int x, int y) {
    uint32_t red = OPOS_RGB(220, 40, 40), ink = OPOS_RGB(20, 20, 24);
    int cx = x + CELL / 2;
    opos_fill_rect(canvas, cx, y + 5, 2, 12, ink);
    for (int i = 0; i < 6; i++) {
        opos_fill_rect(canvas, cx - 7 + i, y + 5 + i / 2, 7 - i, 1, red);
        opos_fill_rect(canvas, cx - 7 + i, y + 10 - i / 2, 7 - i, 1, red);
    }
    opos_fill_rect(canvas, cx - 7, y + 5, 7, 6, red);
    opos_fill_rect(canvas, cx - 4, y + 17, 10, 2, ink);
    opos_fill_rect(canvas, cx - 6, y + 19, 14, 2, ink);
}

static void draw_cell(int column, int row) {
    int index = row * columns + column;
    int x = board_x() + column * CELL, y = board_y() + row * CELL;
    bool shown_open = opened[index]
        || (state == LOST && mine[index] && mark[index] != MARK_FLAG);
    bool held = pressed == index && !opened[index]
             && mark[index] != MARK_FLAG && state == PLAYING;
    if (shown_open || held) {
        uint32_t back = index == exploded ? OPOS_RGB(230, 60, 50) : OPENED;
        opos_fill_rect(canvas, x, y, CELL, CELL, OPENED_LINE);
        opos_fill_rect(canvas, x + 1, y + 1, CELL - 1, CELL - 1, back);
        if (shown_open && mine[index]) {
            draw_mine(x, y);
        } else if (shown_open && near[index] != 0) {
            char digit = (char)('0' + near[index]);
            uint32_t colour = number_colours[near[index]];
            int dx = (CELL - opos_char_width(2) + 2) / 2;
            int dy = (CELL - opos_char_height(2)) / 2;
            opos_draw_char(canvas, x + dx, y + dy, digit, colour, 2);
            opos_draw_char(canvas, x + dx + 1, y + dy, digit, colour, 2);
        }
    } else {
        opos_fill_rect(canvas, x, y, CELL, CELL, CLOSED_DARK);
        opos_fill_rect(canvas, x, y, CELL - 2, CELL - 2, CLOSED_LIGHT);
        opos_fill_rect(canvas, x + 2, y + 2, CELL - 4, CELL - 4, CLOSED);
        if (mark[index] == MARK_FLAG) {
            draw_flag(x, y);
            if (state == LOST && !mine[index]) {
                /* A wrong flag. */
                uint32_t red = OPOS_RGB(230, 40, 40);
                for (int i = 3; i < CELL - 3; i++) {
                    opos_fill_rect(canvas, x + i, y + i, 2, 2, red);
                    opos_fill_rect(canvas, x + CELL - 1 - i, y + i, 2, 2,
                                   red);
                }
            }
        } else if (mark[index] == MARK_QUESTION) {
            int dx = (CELL - opos_char_width(2) + 2) / 2;
            opos_draw_char(canvas, x + dx, y + 3, '?', TEXT, 2);
        }
    }
    if (show_cursor && column == cursor_x && row == cursor_y) {
        opos_draw_rect(canvas, x, y, CELL, CELL, CURSOR);
        opos_draw_rect(canvas, x + 1, y + 1, CELL - 2, CELL - 2, CURSOR);
    }
}

static void draw(void) {
    if (canvas->pixels == 0) {
        return;
    }
    opos_fill_rect(canvas, 0, 0, fit.width, fit.height, BACKGROUND);
    int top = board_y() - BORDER - BAR_HEIGHT;
    int left = board_x();
    int width = columns * CELL;
    opos_fill_rect(canvas, left - BORDER, top, width + 2 * BORDER,
                   BAR_HEIGHT + rows * CELL + 2 * BORDER, BAR);
    draw_counter(left, top + 5, mines - flag_count);
    draw_counter(left + width - (3 * opos_char_width(3) + 6), top + 5,
                 elapsed());
    draw_face();
    for (int row = 0; row < rows; row++) {
        for (int column = 0; column < columns; column++) {
            draw_cell(column, row);
        }
    }
    /* The level, and what to do after a game. */
    const char *line = state == WON ? "YOU WON!  N: NEW GAME"
                     : state == LOST ? "BOOM!  N: NEW GAME"
                     : levels[level_index].name;
    int text_y = board_y() + rows * CELL + BORDER + 4;
    if (text_y + opos_char_height(2) <= fit.height) {
        int text_x = (fit.width - opos_text_width(line, 2)) / 2;
        opos_draw_text(canvas, text_x, text_y, line,
                       state == WON ? OPOS_RGB(120, 230, 140)
                       : state == LOST ? OPOS_RGB(255, 120, 110) : TEXT, 2);
    }
    fit_present(&fit);
}

/* ------------------------------------------------------------- events */

static bool cell_at(int x, int y, int *column, int *row) {
    int bx = board_x(), by = board_y();
    if (x < bx || y < by || x >= bx + columns * CELL
        || y >= by + rows * CELL) {
        return false;
    }
    *column = (x - bx) / CELL;
    *row = (y - by) / CELL;
    return true;
}

static bool on_face(int x, int y) {
    int size = 32;
    int fx = (fit.width - size) / 2;
    int fy = board_y() - BORDER - BAR_HEIGHT + (BAR_HEIGHT - size) / 2;
    return x >= fx && x < fx + size && y >= fy && y < fy + size;
}

/* The window size the level needs: the board, its bar and a text line. */
static int need_height(void) {
    return content_height() + 28;
}

/* Lays the game out for the window again; false when out of memory. */
static bool relayout(void) {
    return fit_layout(&fit, fit.window_width, fit.window_height,
                      content_width(), need_height());
}

static void set_level(int index) {
    level_index = index;
    new_game();
    /* The window follows if it can (a RESIZE event); a tiled or
       maximized one keeps its size, and the board shrinks to fit. */
    opos_window_resize((uint32_t)content_width(), (uint32_t)need_height());
    if (!relayout()) {
        opos_print("minesweeper: out of memory\n");
        opos_exit(1);
    }
}

static void key(uint32_t character) {
    if (character == 'n' || character == 'N') {
        new_game();
        return;
    }
    if (character >= '1' && character <= '3') {
        set_level((int)(character - '1'));
        return;
    }
    show_cursor = true;
    if (character == OPOS_KEY_LEFT && cursor_x > 0) {
        cursor_x--;
    } else if (character == OPOS_KEY_RIGHT && cursor_x + 1 < columns) {
        cursor_x++;
    } else if (character == OPOS_KEY_UP && cursor_y > 0) {
        cursor_y--;
    } else if (character == OPOS_KEY_DOWN && cursor_y + 1 < rows) {
        cursor_y++;
    } else if (character == ' ' || character == '\n') {
        activate(cursor_x, cursor_y);
    } else if (character == 'f' || character == 'F') {
        toggle_mark(cursor_x, cursor_y);
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    random_seed();
    level_index = 0;
    new_game();
    long created = opos_window_create("Minesweeper",
                                      (uint32_t)content_width(),
                                      (uint32_t)need_height());
    if (created < 0) {
        opos_print("minesweeper: no window; run it from the desktop\n");
        return 1;
    }
    struct opos_event event;
    for (;;) {
        /* While the clock runs, wake up to redraw it. */
        long timeout = state == PLAYING && start_seconds >= 0 ? 250 : -1;
        long got = opos_window_event(&event, timeout);
        if (got < 0) {
            continue;
        }
        if (got == 0) {
            draw();
            continue;
        }
        /* Mouse positions in the drawing's pixels. */
        event.x = fit_x(&fit, event.x);
        event.y = fit_y(&fit, event.y);
        switch (event.type) {
        case OPOS_EVENT_RESIZE:
            fit.window_width = (int)event.width;
            fit.window_height = (int)event.height;
            if (!relayout()) {
                opos_print("minesweeper: out of memory\n");
                return 1;
            }
            break;
        case OPOS_EVENT_MOUSE_DOWN: {
            int column, row;
            show_cursor = false;
            if (on_face(event.x, event.y)
                && event.button == OPOS_BUTTON_LEFT) {
                face_down = true;
            } else if (cell_at(event.x, event.y, &column, &row)) {
                cursor_x = column;
                cursor_y = row;
                if (event.button == OPOS_BUTTON_RIGHT) {
                    if (opened[row * columns + column]) {
                        open_around(column, row);
                    } else {
                        toggle_mark(column, row);
                    }
                } else if (event.button == OPOS_BUTTON_LEFT) {
                    pressed = row * columns + column;
                }
            }
            break;
        }
        case OPOS_EVENT_MOUSE_MOVE:
            if (pressed >= 0) {
                int column, row;
                pressed = cell_at(event.x, event.y, &column, &row)
                        ? row * columns + column : columns * rows;
            }
            break;
        case OPOS_EVENT_MOUSE_UP: {
            if (event.button != OPOS_BUTTON_LEFT) {
                break;
            }
            int column, row;
            if (face_down) {
                face_down = false;
                if (on_face(event.x, event.y)) {
                    new_game();
                }
            } else if (pressed >= 0
                       && cell_at(event.x, event.y, &column, &row)) {
                activate(column, row);
            }
            pressed = -1;
            break;
        }
        case OPOS_EVENT_KEY:
            key(event.key);
            break;
        case OPOS_EVENT_CLOSE:
            return 0;
        default:
            break;
        }
        draw();
    }
}
