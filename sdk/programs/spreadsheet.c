/*
 * Spreadsheet: the OPOS spreadsheet.
 *
 * Workbooks of sheets with cells holding numbers, text, dates and
 * formulas (about 90 functions, see sheet_formula.h), number formats,
 * bold, italic, underline, colours, fills, alignment and column widths.
 * Opens and saves Excel workbooks (.xlsx, see sheet_xlsx.h) and CSV
 * files. See docs/spreadsheet.md.
 *
 * Keys: arrows, Tab, Enter move (Shift goes back or extends); Ctrl+arrows
 * jump to the edge of the data; typing replaces a cell, F2 or a
 * double-click edits it; Delete clears; Ctrl+Z/Y undo, redo;
 * Ctrl+X/C/V cut, copy, paste; Ctrl+D and Ctrl+R fill down and right;
 * Ctrl+B/I/U bold, italic, underline; Ctrl+; today's date; Ctrl+N/O/S
 * new, open, save (Ctrl+Shift+S save as); Ctrl+Page Up/Down change sheet;
 * Ctrl++ and Ctrl+- zoom.
 */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_font.h>
#include <opos_gfx.h>
#include <opos_http.h>

#define ZIP_MAX_PARTS 72
#include "ooxml.h"

OPOS_MANIFEST("Spreadsheet", "1.0", OPOS_CAP_CLIPBOARD_READ);

static double sheet_trunc(double x) {
    return x < 0 ? ceil(x) : floor(x);
}
#define trunc sheet_trunc

/* ------------------------------------------------------------ the model */

#define MAX_ROWS 1048576
#define MAX_COLS 16384
#define MAX_SHEETS 64
#define SHEET_NAME_MAX 96   /* bytes; 31 characters like other programs */
#define DEFAULT_WIDTH 64    /* px at 100%, like 8.43 characters */
#define ROW_HEIGHT 20
#define INPUT_MAX 32767

enum { V_EMPTY, V_NUM, V_STR, V_BOOL, V_ERR };
enum { E_NULL = 1, E_DIV0, E_VALUE, E_REF, E_NAME, E_NUM, E_NA, E_CYCLE,
       E_DEEP };
static const char *const error_names[] = {
    "", "#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A",
    "#CYCLE!", "#CYCLE!",
};

struct formula;

struct cell {
    uint64_t key;           /* row * MAX_COLS + col + 1; 0 = a free slot */
    char *input;            /* as typed; NULL when only formatted */
    struct formula *code;   /* compiled, for formulas */
    double num;
    char *text;             /* the value when it is text */
    unsigned gen;           /* the recalculation the value is from */
    uint16_t style;
    uint8_t kind, err;
    uint8_t formula, busy;
    uint8_t cached;         /* a value from the file (for formulas with
                               unknown functions) */
};

struct sheet {
    char name[SHEET_NAME_MAX];
    struct cell *cells;
    uint32_t cap, count;
    uint16_t *widths;       /* MAX_COLS widths in px at 100%, 0 = default */
    int rows_used, cols_used;
    bool extent_dirty;
    /* the view */
    int cur_r, cur_c, anc_r, anc_c, top, left;
};

static struct sheet *sheets[MAX_SHEETS];
static int nsheets;
static int active; /* the sheet shown */

static int cell_row(const struct cell *c) {
    return (int)((c->key - 1) / MAX_COLS);
}

static int cell_col(const struct cell *c) {
    return (int)((c->key - 1) % MAX_COLS);
}

static uint32_t key_hash(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    return (uint32_t)key;
}

static struct cell *cell_find(struct sheet *s, int r, int c) {
    if (s->cap == 0 || r < 0 || c < 0 || r >= MAX_ROWS || c >= MAX_COLS) {
        return NULL;
    }
    uint64_t key = (uint64_t)r * MAX_COLS + (uint64_t)c + 1;
    uint32_t mask = s->cap - 1, i = key_hash(key) & mask;
    while (s->cells[i].key != 0) {
        if (s->cells[i].key == key) {
            return &s->cells[i];
        }
        i = (i + 1) & mask;
    }
    return NULL;
}

static bool sheet_rehash(struct sheet *s, uint32_t cap) {
    struct cell *cells = calloc(cap, sizeof(*cells));
    if (cells == NULL) {
        return false;
    }
    for (uint32_t k = 0; k < s->cap; k++) {
        if (s->cells[k].key == 0) {
            continue;
        }
        uint32_t i = key_hash(s->cells[k].key) & (cap - 1);
        while (cells[i].key != 0) {
            i = (i + 1) & (cap - 1);
        }
        cells[i] = s->cells[k];
    }
    free(s->cells);
    s->cells = cells;
    s->cap = cap;
    return true;
}

/* The cell, created when missing (NULL when out of memory). Creating
   cells moves others: do not keep cell pointers across this call. */
static struct cell *cell_get(struct sheet *s, int r, int c) {
    struct cell *found = cell_find(s, r, c);
    if (found != NULL) {
        return found;
    }
    if ((s->count + 1) * 4 > s->cap * 3
        && !sheet_rehash(s, s->cap == 0 ? 64 : s->cap * 2)) {
        return NULL;
    }
    uint64_t key = (uint64_t)r * MAX_COLS + (uint64_t)c + 1;
    uint32_t mask = s->cap - 1, i = key_hash(key) & mask;
    while (s->cells[i].key != 0) {
        i = (i + 1) & mask;
    }
    memset(&s->cells[i], 0, sizeof(s->cells[i]));
    s->cells[i].key = key;
    s->count++;
    if (r + 1 > s->rows_used) {
        s->rows_used = r + 1;
    }
    if (c + 1 > s->cols_used) {
        s->cols_used = c + 1;
    }
    return &s->cells[i];
}

static void formula_free(struct formula *f);

static void cell_free_contents(struct cell *c) {
    free(c->input);
    free(c->text);
    formula_free(c->code);
    c->input = NULL;
    c->text = NULL;
    c->code = NULL;
}

static void cell_delete(struct sheet *s, struct cell *c) {
    cell_free_contents(c);
    uint32_t mask = s->cap - 1;
    uint32_t i = (uint32_t)(c - s->cells);
    for (;;) {
        s->cells[i].key = 0;
        uint32_t j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (s->cells[j].key == 0) {
                s->count--;
                s->extent_dirty = true;
                return;
            }
            uint32_t k = key_hash(s->cells[j].key) & mask;
            bool movable = i <= j ? (k <= i || k > j) : (k <= i && k > j);
            if (movable) {
                s->cells[i] = s->cells[j];
                i = j;
                break;
            }
        }
    }
}

static void sheet_extent(struct sheet *s, int *rows, int *cols) {
    if (s->extent_dirty) {
        s->rows_used = s->cols_used = 0;
        for (uint32_t i = 0; i < s->cap; i++) {
            if (s->cells[i].key != 0) {
                int r = cell_row(&s->cells[i]), c = cell_col(&s->cells[i]);
                if (r + 1 > s->rows_used) {
                    s->rows_used = r + 1;
                }
                if (c + 1 > s->cols_used) {
                    s->cols_used = c + 1;
                }
            }
        }
        s->extent_dirty = false;
    }
    *rows = s->rows_used;
    *cols = s->cols_used;
}

static int sheet_find(const char *name) {
    for (int i = 0; i < nsheets; i++) {
        const char *a = sheets[i]->name, *b = name;
        while (*a != '\0' && tolower((unsigned char)*a)
               == tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return i;
        }
    }
    return -1;
}

static void sheet_free(struct sheet *s) {
    if (s == NULL) {
        return;
    }
    for (uint32_t i = 0; i < s->cap; i++) {
        if (s->cells[i].key != 0) {
            cell_free_contents(&s->cells[i]);
        }
    }
    free(s->cells);
    free(s->widths);
    free(s);
}

static struct sheet *sheet_new(const char *name) {
    struct sheet *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->widths = calloc(MAX_COLS, sizeof(uint16_t));
    if (s->widths == NULL) {
        free(s);
        return NULL;
    }
    snprintf(s->name, sizeof(s->name), "%s", name);
    return s;
}

/* Adds a sheet at the end; -1 when there is no room. */
static int sheet_add(const char *name) {
    if (nsheets == MAX_SHEETS) {
        return -1;
    }
    struct sheet *s = sheet_new(name);
    if (s == NULL) {
        return -1;
    }
    sheets[nsheets] = s;
    return nsheets++;
}

static int col_width(const struct sheet *s, int col) {
    int w = s->widths[col];
    return w == 0 ? DEFAULT_WIDTH : w;
}

/* --------------------------------------------------------------- styles */

#define F_BOLD 1u
#define F_ITALIC 2u
#define F_UNDER 4u
#define F_STRIKE 8u
#define NO_FILL 0xffffffffu
#define FORMAT_MAX 96

enum { AL_AUTO, AL_LEFT, AL_CENTER, AL_RIGHT };

struct style {
    uint8_t flags, align;
    uint32_t colour, fill;
    char format[FORMAT_MAX]; /* "" = General */
};

#define STYLE_MAX 4000
static struct style *styles;
static int nstyles;

static int style_intern(const struct style *want) {
    for (int i = 0; i < nstyles; i++) {
        const struct style *s = &styles[i];
        if (s->flags == want->flags && s->align == want->align
            && s->colour == want->colour && s->fill == want->fill
            && strcmp(s->format, want->format) == 0) {
            return i;
        }
    }
    if (nstyles == STYLE_MAX) {
        return 0;
    }
    if (styles == NULL) {
        styles = calloc(STYLE_MAX, sizeof(*styles));
        if (styles == NULL) {
            return 0;
        }
    }
    styles[nstyles] = *want;
    return nstyles++;
}

static void styles_reset(void) {
    nstyles = 0;
    struct style plain = {0, AL_AUTO, 0x000000, NO_FILL, ""};
    style_intern(&plain);
}

static const struct style *style_of(const struct cell *c) {
    return &styles[c != NULL && c->style < nstyles ? c->style : 0];
}

/* Numbers, dates and formats; formulas. */
#include "sheet_num.h"
#include "sheet_formula.h"

/* ----------------------------------------------------------- cell values */

/* Sets a constant's value from its input text. */
static void cell_constant_value(struct cell *c) {
    free(c->text);
    c->text = NULL;
    c->err = 0;
    c->num = 0;
    const char *in = c->input;
    if (in == NULL || in[0] == '\0') {
        c->kind = V_EMPTY;
        return;
    }
    const struct style *st = style_of(c);
    if (format_is_text(st->format) || in[0] == '\'') {
        c->kind = V_STR;
        c->text = strdup(in[0] == '\'' ? in + 1 : in);
        return;
    }
    size_t n = strlen(in);
    if ((n == 4 && fmt_word(in, n, 0, "true"))
        || (n == 5 && fmt_word(in, n, 0, "false"))) {
        c->kind = V_BOOL;
        c->num = n == 4;
        return;
    }
    for (int e = 1; e <= 7; e++) {
        if (strcmp(in, error_names[e]) == 0) {
            c->kind = V_ERR;
            c->err = (uint8_t)e;
            return;
        }
    }
    char format[FORMAT_MAX];
    double number;
    if (parse_typed_number(in, &number, format, sizeof(format))) {
        c->kind = V_NUM;
        c->num = number;
        return;
    }
    c->kind = V_STR;
    c->text = strdup(in);
}

/* Puts input and style into a cell exactly (no guessing of formats);
   input NULL or "" with style 0 removes the cell. */
static void cell_put(int si, int r, int col, const char *input, int style) {
    struct sheet *s = sheets[si];
    bool empty = input == NULL || input[0] == '\0';
    if (empty && style == 0) {
        struct cell *c = cell_find(s, r, col);
        if (c != NULL) {
            cell_delete(s, c);
        }
        return;
    }
    struct cell *c = cell_get(s, r, col);
    if (c == NULL) {
        return;
    }
    cell_free_contents(c);
    c->style = (uint16_t)style;
    c->cached = 0;
    c->busy = 0;
    c->formula = !empty && input[0] == '=' && input[1] != '\0';
    c->input = empty ? NULL : strdup(input);
    if (c->formula) {
        c->kind = V_EMPTY;
        c->gen = 0;
    } else {
        cell_constant_value(c);
    }
}

/* The input of a number for the formula bar, in a way that reads back
   the same with this format. */
static void input_for_number(double v, const char *format, char *out,
                             size_t capacity) {
    if (format_is_date(format) && v >= 0) {
        struct when w;
        if (date_from_serial(v, &w)) {
            bool time = v != floor(v);
            if (v < 1 && time) {
                snprintf(out, capacity, "%d:%02d:%02d", w.hour, w.minute,
                         w.second);
            } else if (time) {
                snprintf(out, capacity, "%04ld-%02d-%02d %d:%02d:%02d", w.year,
                         w.month, w.day, w.hour, w.minute, w.second);
            } else {
                snprintf(out, capacity, "%04ld-%02d-%02d", w.year, w.month,
                         w.day);
            }
            return;
        }
    }
    if (format != NULL && strchr(format, '%') != NULL
        && !format_is_date(format)) {
        char digits[48];
        num_full(v * 100, digits, sizeof(digits));
        snprintf(out, capacity, "%s%%", digits);
        return;
    }
    num_full(v, out, capacity);
}

/* What a cell shows: text, colour and alignment (width is General's
   width in characters). */
struct look {
    struct shown shown;
    int align;      /* AL_LEFT, AL_CENTER, AL_RIGHT */
    bool number;
};

static void cell_look(struct cell *c, int width, struct look *o) {
    const struct style *st = style_of(c);
    o->shown.text[0] = '\0';
    o->shown.has_colour = false;
    o->shown.too_wide_hash = false;
    o->number = false;
    o->align = AL_LEFT;
    if (c == NULL) {
        return;
    }
    if (c->formula && c->gen != calc_gen) {
        cell_compute((int)(active), c);
    }
    switch (c->kind) {
    case V_NUM:
        format_number(c->num, st->format, width, &o->shown);
        o->align = AL_RIGHT;
        o->number = true;
        break;
    case V_STR:
        format_text(c->text != NULL ? c->text : "", st->format, &o->shown);
        break;
    case V_BOOL:
        snprintf(o->shown.text, sizeof(o->shown.text), "%s",
                 c->num != 0 ? "TRUE" : "FALSE");
        o->align = AL_CENTER;
        break;
    case V_ERR:
        snprintf(o->shown.text, sizeof(o->shown.text), "%s",
                 error_names[c->err < 10 ? c->err : E_VALUE]);
        o->align = AL_CENTER;
        break;
    default:
        break;
    }
    if (st->align != AL_AUTO) {
        o->align = st->align;
    }
}

/* ---------------------------------------------------------- recalculation */

static int compare_cell_ptrs(const void *a, const void *b) {
    uint64_t x = (*(struct cell *const *)a)->key;
    uint64_t y = (*(struct cell *const *)b)->key;
    return x < y ? -1 : x > y;
}

/* Computes every formula, top to bottom (so most chains are short). */
static void recalc(void) {
    calc_gen++;
    if (calc_gen == 0) {
        calc_gen = 1;
    }
    struct cell **list = NULL;
    size_t capacity = 0;
    for (int pass = 0; pass < 20000; pass++) {
        bool stale = false, progress = false;
        for (int si = 0; si < nsheets; si++) {
            struct sheet *s = sheets[si];
            size_t count = 0;
            for (uint32_t i = 0; i < s->cap; i++) {
                struct cell *c = &s->cells[i];
                if (c->key == 0 || !c->formula || c->gen == calc_gen) {
                    continue;
                }
                if (count == capacity) {
                    size_t grown_cap = capacity == 0 ? 256 : capacity * 2;
                    struct cell **grown = realloc(list, grown_cap * sizeof(*grown));
                    if (grown == NULL) {
                        free(list);
                        return;
                    }
                    list = grown;
                    capacity = grown_cap;
                }
                list[count++] = c;
            }
            if (pass == 0) {
                qsort(list, count, sizeof(*list), compare_cell_ptrs);
            }
            for (size_t k = 0; k < count; k++) {
                calc_depth = 0;
                if (list[k]->gen != calc_gen) {
                    cell_compute(si, list[k]);
                }
                if (list[k]->gen == calc_gen) {
                    progress = true;
                } else {
                    stale = true;
                }
            }
        }
        if (!stale || !progress) {
            break;
        }
    }
    /* What could not be computed is part of a loop. */
    for (int si = 0; si < nsheets; si++) {
        struct sheet *s = sheets[si];
        for (uint32_t i = 0; i < s->cap; i++) {
            struct cell *c = &s->cells[i];
            if (c->key != 0 && c->formula && c->gen != calc_gen) {
                free(c->text);
                c->text = NULL;
                c->kind = V_ERR;
                c->err = E_CYCLE;
                c->gen = calc_gen;
            }
        }
    }
    free(list);
}

/* ----------------------------------------------------------- the document */

#define PATH_MAX_LENGTH 256
static char path[PATH_MAX_LENGTH];
static char file_name[64] = "Untitled";
static bool can_write;   /* path came from opos_save_file in this run */
static bool modified;
static char message[112];
static bool message_warn;
static bool close_removes_window; /* OPOS before 0.6.6 */
static bool headless;             /* --convert and --print */

static void say(const char *text, bool warn) {
    snprintf(message, sizeof(message), "%s", text);
    message_warn = warn;
}

static void update_title(void) {
    if (headless) {
        return;
    }
    char title[32];
    snprintf(title, sizeof(title), "Spreadsheet - %s%s", file_name,
             modified ? "*" : "");
    opos_window_title(title);
}

static void set_modified(bool value) {
    if (modified != value) {
        modified = value;
        update_title();
    }
}

static void workbook_clear(void) {
    for (int i = 0; i < nsheets; i++) {
        sheet_free(sheets[i]);
        sheets[i] = NULL;
    }
    nsheets = 0;
    active = 0;
    styles_reset();
    sheets_version++;
}

static void undo_clear_all(void);

static void new_workbook(void) {
    workbook_clear();
    sheet_add("Sheet1");
    undo_clear_all();
}

/* --------------------------------------------------------------- undo */

#define UNDO_MAX 200
#define UNDO_BYTES (64u * 1024 * 1024)

enum { U_CELL, U_WIDTH, U_SHEET };

struct snapcell {
    uint64_t key;
    char *input;
    uint16_t style;
};

struct uentry {
    int type, sheet, row, col;
    char *input;
    uint16_t style, width;
    bool exists;
    struct snapcell *cells; /* U_SHEET */
    uint32_t ncells;
    uint16_t *widths;
};

struct ustep {
    struct uentry *e;
    int n, cap;
    size_t bytes;
    int sheet, cur_r, cur_c, anc_r, anc_c;
};

static struct ustep undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int undo_count, redo_count;
static struct ustep building;
static bool recording;
static size_t undo_bytes;

static void entry_free(struct uentry *e) {
    free(e->input);
    for (uint32_t i = 0; i < e->ncells; i++) {
        free(e->cells[i].input);
    }
    free(e->cells);
    free(e->widths);
}

static void step_free(struct ustep *s) {
    for (int i = 0; i < s->n; i++) {
        entry_free(&s->e[i]);
    }
    free(s->e);
    memset(s, 0, sizeof(*s));
}

static void stack_clear(struct ustep *stack, int *count) {
    for (int i = 0; i < *count; i++) {
        undo_bytes -= stack[i].bytes;
        step_free(&stack[i]);
    }
    *count = 0;
}

static void undo_clear_all(void) {
    stack_clear(undo_stack, &undo_count);
    stack_clear(redo_stack, &redo_count);
    undo_bytes = 0;
}

static struct uentry *step_add(struct ustep *s) {
    if (s->n == s->cap) {
        int cap = s->cap < 16 ? 16 : s->cap * 2;
        struct uentry *grown = realloc(s->e, (size_t)cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        s->e = grown;
        s->cap = cap;
    }
    struct uentry *e = &s->e[s->n++];
    memset(e, 0, sizeof(*e));
    s->bytes += sizeof(*e);
    return e;
}

static void capture_cell(struct ustep *s, int si, int r, int c) {
    struct uentry *e = step_add(s);
    if (e == NULL) {
        return;
    }
    e->type = U_CELL;
    e->sheet = si;
    e->row = r;
    e->col = c;
    struct cell *cell = cell_find(sheets[si], r, c);
    e->exists = cell != NULL;
    if (cell != NULL) {
        e->style = cell->style;
        if (cell->input != NULL) {
            e->input = strdup(cell->input);
            s->bytes += strlen(cell->input) + 1;
        }
    }
}

static void capture_width(struct ustep *s, int si, int col) {
    struct uentry *e = step_add(s);
    if (e != NULL) {
        e->type = U_WIDTH;
        e->sheet = si;
        e->col = col;
        e->width = sheets[si]->widths[col];
    }
}

static void capture_sheet(struct ustep *s, int si) {
    struct uentry *e = step_add(s);
    if (e == NULL) {
        return;
    }
    struct sheet *sh = sheets[si];
    e->type = U_SHEET;
    e->sheet = si;
    e->cells = malloc((size_t)(sh->count + 1) * sizeof(*e->cells));
    e->widths = malloc(MAX_COLS * sizeof(uint16_t));
    if (e->cells == NULL || e->widths == NULL) {
        return;
    }
    memcpy(e->widths, sh->widths, MAX_COLS * sizeof(uint16_t));
    s->bytes += MAX_COLS * sizeof(uint16_t);
    for (uint32_t i = 0; i < sh->cap; i++) {
        struct cell *c = &sh->cells[i];
        if (c->key == 0) {
            continue;
        }
        struct snapcell *q = &e->cells[e->ncells++];
        q->key = c->key;
        q->style = c->style;
        q->input = c->input != NULL ? strdup(c->input) : NULL;
        s->bytes += sizeof(*q) + (c->input != NULL ? strlen(c->input) + 1 : 0);
    }
}

/* Puts an entry back; first records the present state into inverse. */
static void entry_apply(const struct uentry *e, struct ustep *inverse) {
    if (e->sheet >= nsheets) {
        return;
    }
    struct sheet *sh = sheets[e->sheet];
    if (e->type == U_CELL) {
        capture_cell(inverse, e->sheet, e->row, e->col);
        if (e->exists) {
            cell_put(e->sheet, e->row, e->col, e->input, e->style);
            if (e->input == NULL && e->style == 0) {
                /* an empty cell with the plain style: keep it gone */
            }
        } else {
            struct cell *c = cell_find(sh, e->row, e->col);
            if (c != NULL) {
                cell_delete(sh, c);
            }
        }
    } else if (e->type == U_WIDTH) {
        capture_width(inverse, e->sheet, e->col);
        sh->widths[e->col] = e->width;
    } else if (e->cells != NULL && e->widths != NULL) {
        capture_sheet(inverse, e->sheet);
        for (uint32_t i = 0; i < sh->cap; i++) {
            if (sh->cells[i].key != 0) {
                cell_free_contents(&sh->cells[i]);
            }
        }
        free(sh->cells);
        sh->cells = NULL;
        sh->cap = sh->count = 0;
        sh->rows_used = sh->cols_used = 0;
        for (uint32_t i = 0; i < e->ncells; i++) {
            const struct snapcell *q = &e->cells[i];
            cell_put(e->sheet, (int)((q->key - 1) / MAX_COLS),
                     (int)((q->key - 1) % MAX_COLS),
                     q->input, q->style == 0 && q->input == NULL ? 1 : q->style);
            if (q->style == 0 && q->input == NULL) {
                struct cell *c = cell_find(sh, (int)((q->key - 1) / MAX_COLS),
                                           (int)((q->key - 1) % MAX_COLS));
                if (c != NULL) {
                    c->style = 0;
                }
            }
        }
        memcpy(sh->widths, e->widths, MAX_COLS * sizeof(uint16_t));
        sh->extent_dirty = true;
    }
}

static void edit_begin(void) {
    step_free(&building);
    recording = true;
    struct sheet *s = sheets[active];
    building.sheet = active;
    building.cur_r = s->cur_r;
    building.cur_c = s->cur_c;
    building.anc_r = s->anc_r;
    building.anc_c = s->anc_c;
}

static void edit_cell(int si, int r, int c) {
    if (recording) {
        capture_cell(&building, si, r, c);
    }
}

static void edit_width(int si, int col) {
    if (recording) {
        capture_width(&building, si, col);
    }
}

static void edit_sheet(int si) {
    if (recording) {
        capture_sheet(&building, si);
    }
}

static void stack_push(struct ustep *stack, int *count, struct ustep *step) {
    if (*count == UNDO_MAX) {
        undo_bytes -= stack[0].bytes;
        step_free(&stack[0]);
        memmove(stack, stack + 1, (UNDO_MAX - 1) * sizeof(*stack));
        (*count)--;
    }
    stack[(*count)++] = *step;
    undo_bytes += step->bytes;
    memset(step, 0, sizeof(*step));
    while (undo_bytes > UNDO_BYTES && undo_count > 1) {
        undo_bytes -= undo_stack[0].bytes;
        step_free(&undo_stack[0]);
        memmove(undo_stack, undo_stack + 1,
                (size_t)(undo_count - 1) * sizeof(*undo_stack));
        undo_count--;
    }
}

static void edit_end(void) {
    recording = false;
    if (building.n == 0) {
        step_free(&building);
        return;
    }
    stack_clear(redo_stack, &redo_count);
    stack_push(undo_stack, &undo_count, &building);
    set_modified(true);
    recalc();
}

static void step_replay(struct ustep *from, int *from_count, struct ustep *to,
                        int *to_count) {
    if (*from_count == 0) {
        return;
    }
    struct ustep step = from[--(*from_count)];
    undo_bytes -= step.bytes;
    struct ustep inverse = {0};
    if (step.sheet < nsheets) {
        struct sheet *s = sheets[active];
        inverse.sheet = active;
        inverse.cur_r = s->cur_r;
        inverse.cur_c = s->cur_c;
        inverse.anc_r = s->anc_r;
        inverse.anc_c = s->anc_c;
    }
    for (int i = step.n - 1; i >= 0; i--) {
        entry_apply(&step.e[i], &inverse);
    }
    if (step.sheet < nsheets) {
        active = step.sheet;
        struct sheet *s = sheets[active];
        s->cur_r = step.cur_r;
        s->cur_c = step.cur_c;
        s->anc_r = step.anc_r;
        s->anc_c = step.anc_c;
    }
    step_free(&step);
    stack_push(to, to_count, &inverse);
    set_modified(true);
    recalc();
}

/* ---------------------------------------------------------- cell edits */

/* Commits typed text into a cell (formats follow what was typed). */
static void cell_type(int si, int r, int c, const char *text) {
    struct cell *cell = cell_find(sheets[si], r, c);
    int style = cell != NULL ? cell->style : 0;
    edit_cell(si, r, c);
    if (text[0] == '=' && text[1] != '\0') {
        struct rewrite rw = {RW_NORMAL, 0, 0, -1, 0, 0, NULL, si};
        char *normal = formula_rewrite(text + 1, &rw);
        if (normal != NULL) {
            size_t n = strlen(normal);
            char *full = malloc(n + 2);
            if (full != NULL) {
                full[0] = '=';
                memcpy(full + 1, normal, n + 1);
                cell_put(si, r, c, full, style);
                free(full);
            }
            free(normal);
        }
        return;
    }
    /* A number typed like 12%, $5 or 2026-11-16 gets that format when
       the cell has General. */
    struct style st = styles[style];
    double number;
    char format[FORMAT_MAX];
    if (st.format[0] == '\0' && parse_typed_number(text, &number, format,
                                                  sizeof(format))
        && format[0] != '\0') {
        snprintf(st.format, sizeof(st.format), "%s", format);
        style = style_intern(&st);
    }
    cell_put(si, r, c, text, style);
}

/* -------------------------------------------------------- file formats */

static void cell_name(int r, int c, char *out) {
    char col[4];
    col_name(c, col);
    sprintf(out, "%s%d", col, r + 1);
}

/* The first character of text that is in set, or NULL. */
static const char *any_of(const char *text, const char *set) {
    for (; *text != '\0'; text++) {
        if (strchr(set, *text) != NULL) {
            return text;
        }
    }
    return NULL;
}

#include "sheet_xlsx.h"

/* =============================================================== the UI */

#define ROW_H 40
#define TOOLBAR_H (2 * ROW_H)
#define BAR_H 34       /* the formula bar */
#define TABS_H 30
#define STATUS_H 26
#define SCROLLBAR_W 14
#define NAME_W 110

#define UI_BACK OPOS_RGB(30, 34, 46)
#define UI_PANEL OPOS_RGB(40, 46, 62)
#define UI_BUTTON OPOS_RGB(56, 64, 86)
#define UI_HOVER OPOS_RGB(72, 82, 110)
#define UI_ACTIVE OPOS_RGB(70, 110, 190)
#define UI_TEXT OPOS_RGB(230, 236, 248)
#define UI_DIM OPOS_RGB(130, 140, 165)
#define UI_WARN OPOS_RGB(245, 205, 110)
#define GRID_LINE OPOS_RGB(218, 220, 224)
#define HEAD_BACK OPOS_RGB(240, 241, 243)
#define HEAD_SEL OPOS_RGB(211, 227, 253)
#define HEAD_TEXT OPOS_RGB(70, 74, 82)
#define SEL_BORDER OPOS_RGB(26, 115, 232)
#define SEL_TINT_R 26
#define SEL_TINT_G 115
#define SEL_TINT_B 232

static int zoom = 100;
static int view_width, view_height;
static struct opos_canvas canvas;
static struct opos_canvas *screen = &canvas;
static int mouse_x, mouse_y;
static bool shift_down, ctrl_down;

static struct opos_font *fonts[4];
static bool fonts_tried[4];

static struct opos_font *font_for(uint32_t flags) {
    int variant = ((flags & F_BOLD) ? 1 : 0) | ((flags & F_ITALIC) ? 2 : 0);
    if (!fonts_tried[variant]) {
        static const char *const names[4] = {
            "/share/fonts/NotoSans-Regular.ttf",
            "/share/fonts/NotoSans-Bold.ttf",
            "/share/fonts/NotoSans-Italic.ttf",
            "/share/fonts/NotoSans-BoldItalic.ttf",
        };
        fonts_tried[variant] = true;
        fonts[variant] = opos_font_load(names[variant]);
    }
    if (fonts[variant] == NULL && variant != 0) {
        return font_for(0);
    }
    return fonts[variant];
}

static int px(int value) {
    return value * zoom / 100;
}

static int text_size(void) { /* 11 points */
    int size = (11 * 4 * zoom + 150) / 300;
    return size < 6 ? 6 : size;
}

static int row_h(void) {
    int h = px(ROW_HEIGHT);
    return h < 8 ? 8 : h;
}

static int col_px(int col) {
    int w = px(col_width(sheets[active], col));
    return w < 2 ? 2 : w;
}

static struct sheet *sh(void) {
    return sheets[active];
}

/* --- the grid area */

static int grid_top(void) {
    return TOOLBAR_H + BAR_H;
}

static int head_h(void) {
    return row_h() + 2;
}

static int row_head_w(void) {
    struct sheet *s = sh();
    char digits[16];
    int last = s->top + 100;
    snprintf(digits, sizeof(digits), "%d", last);
    struct opos_font *font = font_for(0);
    int w = font != NULL
        ? opos_font_text_width(font, text_size(), digits, strlen(digits))
        : (int)strlen(digits) * 8;
    return w + px(16) < 34 ? 34 : w + px(16);
}

static int cells_x(void) {
    return row_head_w();
}

static int cells_y(void) {
    return grid_top() + head_h();
}

static int cells_w(void) {
    return view_width - SCROLLBAR_W - cells_x();
}

static int cells_h(void) {
    return view_height - TABS_H - STATUS_H - cells_y();
}

static int visible_rows(void) {
    return cells_h() / row_h() + 1;
}

/* Columns from left that fit (at least 1). */
static int visible_cols(void) {
    int x = 0, n = 0;
    for (int c = sh()->left; c < MAX_COLS && x < cells_w(); c++) {
        x += col_px(c);
        n++;
    }
    return n < 1 ? 1 : n;
}

static int col_x(int col) { /* screen x of a column's left edge */
    struct sheet *s = sh();
    int x = cells_x();
    if (col >= s->left) {
        for (int c = s->left; c < col && x < view_width + 4000; c++) {
            x += col_px(c);
        }
    } else {
        for (int c = col; c < s->left && x > -4000; c++) {
            x -= col_px(c);
        }
    }
    return x;
}

static int row_y(int row) {
    return cells_y() + (row - sh()->top) * row_h();
}

/* Cell at a screen point (may be outside the visible area). */
static void cell_at(int x, int y, int *row, int *col) {
    struct sheet *s = sh();
    int r = s->top + (int)floor((double)(y - cells_y()) / row_h());
    int c = s->left;
    int cx = cells_x();
    if (x >= cx) {
        while (c < MAX_COLS - 1 && cx + col_px(c) <= x) {
            cx += col_px(c);
            c++;
        }
    } else {
        while (c > 0 && cx > x) {
            c--;
            cx -= col_px(c);
        }
    }
    *row = r < 0 ? 0 : r >= MAX_ROWS ? MAX_ROWS - 1 : r;
    *col = c;
}

static void scroll_into_view(int r, int c) {
    struct sheet *s = sh();
    int rows = cells_h() / row_h();
    if (rows < 1) {
        rows = 1;
    }
    if (r < s->top) {
        s->top = r;
    } else if (r >= s->top + rows) {
        s->top = r - rows + 1;
    }
    if (c < s->left) {
        s->left = c;
    } else {
        /* scroll right until the column's right edge fits */
        while (s->left < c && col_x(c) + col_px(c) > cells_x() + cells_w()) {
            s->left++;
        }
    }
    if (s->top < 0) {
        s->top = 0;
    }
}

/* --- selection */

static void sel_box(int *r1, int *c1, int *r2, int *c2) {
    struct sheet *s = sh();
    *r1 = s->cur_r < s->anc_r ? s->cur_r : s->anc_r;
    *r2 = s->cur_r < s->anc_r ? s->anc_r : s->cur_r;
    *c1 = s->cur_c < s->anc_c ? s->cur_c : s->anc_c;
    *c2 = s->cur_c < s->anc_c ? s->anc_c : s->cur_c;
}

static bool single_cell(void) {
    return sh()->cur_r == sh()->anc_r && sh()->cur_c == sh()->anc_c;
}

static int tab_start = -1; /* where a row of Tabs began, for Enter */

static void select_cell(int r, int c, bool extend) {
    struct sheet *s = sh();
    tab_start = -1;
    s->cur_r = r < 0 ? 0 : r >= MAX_ROWS ? MAX_ROWS - 1 : r;
    s->cur_c = c < 0 ? 0 : c >= MAX_COLS ? MAX_COLS - 1 : c;
    if (!extend) {
        s->anc_r = s->cur_r;
        s->anc_c = s->cur_c;
    }
    scroll_into_view(s->cur_r, s->cur_c);
}

/* Enter after typing across a row with Tab goes back to where it began,
   one row down, like other spreadsheets. */
static void move_after_enter(bool up) {
    int c = tab_start >= 0 && !up ? tab_start : sh()->cur_c;
    select_cell(sh()->cur_r + (up ? -1 : 1), c, false);
}

static void move_after_tab(bool back) {
    int start = tab_start >= 0 ? tab_start : sh()->cur_c;
    select_cell(sh()->cur_r, sh()->cur_c + (back ? -1 : 1), false);
    tab_start = start;
}

static bool cell_has_value(int r, int c) {
    struct cell *cell = cell_find(sh(), r, c);
    return cell != NULL && cell->input != NULL;
}

/* Ctrl+arrow: to the edge of the data. */
static void jump(int dr, int dc, bool extend) {
    struct sheet *s = sh();
    int r = s->cur_r, c = s->cur_c;
    int rows, cols;
    sheet_extent(s, &rows, &cols);
    bool here = cell_has_value(r, c);
    bool next = cell_has_value(r + dr, c + dc);
    if (here && next) {
        while (r + dr >= 0 && c + dc >= 0 && r + dr < MAX_ROWS
               && c + dc < MAX_COLS && cell_has_value(r + dr, c + dc)) {
            r += dr;
            c += dc;
        }
    } else {
        r += dr;
        c += dc;
        while (r >= 0 && c >= 0 && r < MAX_ROWS && c < MAX_COLS
               && !cell_has_value(r, c)) {
            if ((dr > 0 && r >= rows) || (dc > 0 && c >= cols)) {
                r = dr > 0 ? MAX_ROWS - 1 : r;
                c = dc > 0 ? MAX_COLS - 1 : c;
                break;
            }
            r += dr;
            c += dc;
        }
        if (r < 0) {
            r = 0;
        }
        if (c < 0) {
            c = 0;
        }
    }
    select_cell(r, c, extend);
}

/* --- editing a cell (or a sheet name) */

enum edit_kind { EDIT_NONE, EDIT_CELL, EDIT_TAB };
static enum edit_kind editing;
static bool enter_mode;   /* started by typing: arrows leave the cell */
static char *edit_text;   /* INPUT_MAX + 1 */
static int edit_len, edit_caret;
static int edit_r, edit_c, edit_sheet_index;
/* Pointing at cells while typing a formula. */
static bool pointing;
static int point_start, point_len, point_r, point_c, point_r2, point_c2;

static void edit_set(const char *text) {
    edit_len = (int)strlen(text);
    if (edit_len > INPUT_MAX) {
        edit_len = INPUT_MAX;
    }
    memcpy(edit_text, text, (size_t)edit_len);
    edit_text[edit_len] = '\0';
    edit_caret = edit_len;
    pointing = false;
}

static void edit_start(bool replace, const char *initial) {
    struct sheet *s = sh();
    editing = EDIT_CELL;
    edit_r = s->cur_r;
    edit_c = s->cur_c;
    edit_sheet_index = active;
    enter_mode = replace;
    if (replace) {
        edit_set(initial != NULL ? initial : "");
    } else {
        struct cell *c = cell_find(s, s->cur_r, s->cur_c);
        edit_set(c != NULL && c->input != NULL ? c->input : "");
    }
    scroll_into_view(edit_r, edit_c);
}

static void edit_insert(const char *text, int length) {
    if (edit_len + length > INPUT_MAX) {
        length = INPUT_MAX - edit_len;
    }
    if (length <= 0) {
        return;
    }
    memmove(edit_text + edit_caret + length, edit_text + edit_caret,
            (size_t)(edit_len - edit_caret + 1));
    memcpy(edit_text + edit_caret, text, (size_t)length);
    edit_len += length;
    edit_caret += length;
}

static void edit_remove(int from, int to) {
    if (from < 0) {
        from = 0;
    }
    if (to > edit_len) {
        to = edit_len;
    }
    if (to <= from) {
        return;
    }
    memmove(edit_text + from, edit_text + to, (size_t)(edit_len - to + 1));
    edit_len -= to - from;
    if (edit_caret > to) {
        edit_caret -= to - from;
    } else if (edit_caret > from) {
        edit_caret = from;
    }
}

static int edit_prev(int at) {
    if (at <= 0) {
        return 0;
    }
    at--;
    while (at > 0 && ((unsigned char)edit_text[at] & 0xc0) == 0x80) {
        at--;
    }
    return at;
}

static int edit_next(int at) {
    if (at >= edit_len) {
        return edit_len;
    }
    at++;
    while (at < edit_len && ((unsigned char)edit_text[at] & 0xc0) == 0x80) {
        at++;
    }
    return at;
}

/* Can a reference go at the caret? (after = ( , and operators) */
static bool point_allowed(void) {
    if (edit_len == 0 || edit_text[0] != '=') {
        return false;
    }
    if (pointing && edit_caret == point_start + point_len) {
        return true;
    }
    int i = edit_caret - 1;
    while (i > 0 && edit_text[i] == ' ') {
        i--;
    }
    return i >= 0 && strchr("=(,;+-*/^&<>:", edit_text[i]) != NULL;
}

/* Puts the pointed-at cell or range at the caret. */
static void point_write(void) {
    char ref[48];
    int r1 = point_r < point_r2 ? point_r : point_r2;
    int r2 = point_r < point_r2 ? point_r2 : point_r;
    int c1 = point_c < point_c2 ? point_c : point_c2;
    int c2 = point_c < point_c2 ? point_c2 : point_c;
    char a[24], b[24];
    cell_name(r1, c1, a);
    cell_name(r2, c2, b);
    if (r1 == r2 && c1 == c2) {
        snprintf(ref, sizeof(ref), "%s", a);
    } else {
        snprintf(ref, sizeof(ref), "%s:%s", a, b);
    }
    char prefix[SHEET_NAME_MAX + 8] = "";
    if (active != edit_sheet_index) {
        struct buf name = {0};
        put_sheet_name(&name, sh()->name);
        snprintf(prefix, sizeof(prefix), "%s!", name.data != NULL ? name.data : "");
        free(name.data);
    }
    char full[SHEET_NAME_MAX + 64];
    snprintf(full, sizeof(full), "%s%s", prefix, ref);
    if (pointing) {
        edit_remove(point_start, point_start + point_len);
        edit_caret = point_start;
    }
    point_start = edit_caret;
    edit_insert(full, (int)strlen(full));
    point_len = (int)strlen(full);
    pointing = true;
}

static void edit_cancel(void) {
    if (editing == EDIT_CELL) {
        active = edit_sheet_index;
    }
    editing = EDIT_NONE;
    pointing = false;
}

static bool sheet_name_ok(const char *name, int except) {
    if (name[0] == '\0' || utf8_count(name) > 31) {
        return false;
    }
    if (any_of(name, "\\/?*[]:") != NULL || name[0] == '\''
        || name[strlen(name) - 1] == '\'') {
        return false;
    }
    int found = sheet_find(name);
    return found < 0 || found == except;
}

static void rename_sheet(int si, const char *name);

/* Enter: puts the text into the cell. false if the formula is wrong
   (the editor stays open). */
static bool edit_commit(void) {
    if (editing == EDIT_TAB) {
        if (!sheet_name_ok(edit_text, edit_sheet_index)) {
            say("SHEET NAMES: 1-31 CHARACTERS, NOT \\ / ? * [ ] : AND NOT USED",
                true);
            return false;
        }
        rename_sheet(edit_sheet_index, edit_text);
        editing = EDIT_NONE;
        return true;
    }
    if (editing != EDIT_CELL) {
        return true;
    }
    active = edit_sheet_index;
    if (edit_text[0] == '=' && edit_len > 1) {
        /* Close brackets left open, like other spreadsheets do. */
        int depth = 0;
        bool quote = false;
        for (int i = 1; i < edit_len; i++) {
            if (edit_text[i] == '"') {
                quote = !quote;
            } else if (!quote && edit_text[i] == '(') {
                depth++;
            } else if (!quote && edit_text[i] == ')') {
                depth--;
            }
        }
        while (depth > 0 && edit_len < INPUT_MAX) {
            edit_text[edit_len++] = ')';
            edit_text[edit_len] = '\0';
            depth--;
        }
        if (!formula_valid(edit_text + 1)) {
            say("THAT FORMULA HAS A MISTAKE; FIX IT OR PRESS ESC", true);
            return false;
        }
    }
    struct cell *c = cell_find(sh(), edit_r, edit_c);
    const char *old = c != NULL && c->input != NULL ? c->input : "";
    if (strcmp(old, edit_text) != 0) {
        edit_begin();
        cell_type(active, edit_r, edit_c, edit_text);
        edit_end();
    }
    editing = EDIT_NONE;
    pointing = false;
    say("", false);
    return true;
}

/* --- changes to the selection */

/* Calls change for each cell in the selection (only cells that exist,
   when skip_missing). */
static void each_selected(void (*change)(int r, int c, void *data),
                          void *data, bool skip_missing) {
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    struct sheet *s = sh();
    int rows, cols;
    sheet_extent(s, &rows, &cols);
    long area = (long)(r2 - r1 + 1) * (c2 - c1 + 1);
    if (skip_missing && area > (long)s->count * 2) {
        /* Big selections (whole columns): visit the cells there are. */
        uint64_t *keys = malloc((size_t)(s->count + 1) * sizeof(uint64_t));
        if (keys == NULL) {
            return;
        }
        uint32_t n = 0;
        for (uint32_t i = 0; i < s->cap; i++) {
            struct cell *c = &s->cells[i];
            if (c->key != 0 && cell_row(c) >= r1 && cell_row(c) <= r2
                && cell_col(c) >= c1 && cell_col(c) <= c2) {
                keys[n++] = c->key;
            }
        }
        for (uint32_t i = 0; i < n; i++) {
            change((int)((keys[i] - 1) / MAX_COLS),
                   (int)((keys[i] - 1) % MAX_COLS), data);
        }
        free(keys);
        return;
    }
    for (int r = r1; r <= r2; r++) {
        for (int c = c1; c <= c2; c++) {
            if (skip_missing && cell_find(s, r, c) == NULL) {
                continue;
            }
            change(r, c, data);
        }
    }
}

struct style_change {
    int what; /* 0 flag on, 1 flag off, 2 colour, 3 fill, 4 align,
                 5 format, 6 decimals, 7 clear formats */
    uint32_t value;
    const char *format;
};

static void restyle_one(int r, int c, void *data) {
    struct style_change *ch = data;
    struct cell *cell = cell_find(sh(), r, c);
    struct style st = *style_of(cell);
    switch (ch->what) {
    case 0: st.flags |= (uint8_t)ch->value; break;
    case 1: st.flags &= (uint8_t)~ch->value; break;
    case 2: st.colour = ch->value; break;
    case 3: st.fill = ch->value; break;
    case 4: st.align = (uint8_t)ch->value; break;
    case 5: snprintf(st.format, sizeof(st.format), "%s", ch->format); break;
    case 6: {
        char changed[FORMAT_MAX];
        const char *base = st.format;
        /* General numbers start from what they show. */
        if (base[0] == '\0' && cell != NULL && cell->kind == V_NUM) {
            char shown[48];
            num_general(cell->num, 11, shown, sizeof(shown));
            char *dot = strchr(shown, '.');
            int places = dot != NULL && strchr(shown, 'E') == NULL
                ? (int)strlen(dot + 1) : 0;
            char guess[FORMAT_MAX] = "0";
            if (places > 0) {
                snprintf(guess, sizeof(guess), "0.%.*s", places,
                         "000000000000000");
            }
            format_change_decimals(guess, (int)ch->value - 1, changed,
                                   sizeof(changed));
        } else {
            format_change_decimals(base, (int)ch->value - 1, changed,
                                   sizeof(changed));
        }
        snprintf(st.format, sizeof(st.format), "%s", changed);
        break;
    }
    default:
        st = styles[0];
        break;
    }
    int index = style_intern(&st);
    if (cell != NULL && cell->style == index) {
        return;
    }
    if (cell == NULL && index == 0) {
        return;
    }
    edit_cell(active, r, c);
    cell = cell_get(sh(), r, c);
    if (cell == NULL) {
        return;
    }
    cell->style = (uint16_t)index;
    if (!cell->formula) {
        cell_constant_value(cell); /* "@" makes numbers text */
    }
    if (cell->input == NULL && index == 0) {
        cell_delete(sh(), cell);
    }
}

static void restyle(int what, uint32_t value, const char *format) {
    struct style_change ch = {what, value, format};
    edit_begin();
    /* Whole rows or columns: only the cells there are. */
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    long area = (long)(r2 - r1 + 1) * (c2 - c1 + 1);
    each_selected(restyle_one, &ch, area > 200000);
    edit_end();
}

static void toggle_flag(uint32_t flag) {
    struct cell *c = cell_find(sh(), sh()->cur_r, sh()->cur_c);
    bool on = (style_of(c)->flags & flag) != 0;
    restyle(on ? 1 : 0, flag, NULL);
}

static void clear_one(int r, int c, void *data) {
    (void)data;
    struct cell *cell = cell_find(sh(), r, c);
    if (cell == NULL || cell->input == NULL) {
        return;
    }
    edit_cell(active, r, c);
    cell_put(active, r, c, NULL, cell->style);
}

static void clear_selection(void) {
    edit_begin();
    each_selected(clear_one, NULL, true);
    edit_end();
}

/* --- clipboard */

struct clip_cell {
    char *input;
    uint16_t style;
    bool exists;
};

static struct clip_cell *clip;
static int clip_rows, clip_cols, clip_r, clip_c, clip_sheet;
static bool clip_cut;
static char *clip_text;   /* what was put on the system clipboard */

static void clip_free(void) {
    for (long i = 0; i < (long)clip_rows * clip_cols; i++) {
        free(clip[i].input);
    }
    free(clip);
    clip = NULL;
    clip_rows = clip_cols = 0;
    free(clip_text);
    clip_text = NULL;
}

static void copy_selection(bool cut) {
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    int rows, cols;
    sheet_extent(sh(), &rows, &cols);
    /* Whole columns or rows: only the used part. */
    if (r2 >= rows && r2 - r1 > 10000) {
        r2 = rows > r1 ? rows - 1 : r1;
    }
    if (c2 >= cols && c2 - c1 > 1000) {
        c2 = cols > c1 ? cols - 1 : c1;
    }
    long area = (long)(r2 - r1 + 1) * (c2 - c1 + 1);
    if (area > 2000000) {
        say("THAT IS TOO MUCH TO COPY AT ONCE", true);
        return;
    }
    clip_free();
    clip = calloc((size_t)area, sizeof(*clip));
    if (clip == NULL) {
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    clip_rows = r2 - r1 + 1;
    clip_cols = c2 - c1 + 1;
    clip_r = r1;
    clip_c = c1;
    clip_sheet = active;
    clip_cut = cut;
    struct buf text = {0};
    for (int r = r1; r <= r2; r++) {
        for (int c = c1; c <= c2; c++) {
            struct cell *cell = cell_find(sh(), r, c);
            struct clip_cell *q = &clip[(long)(r - r1) * clip_cols + (c - c1)];
            if (cell != NULL) {
                q->exists = true;
                q->style = cell->style;
                q->input = cell->input != NULL ? strdup(cell->input) : NULL;
                struct look look;
                cell_look(cell, 17, &look);
                buf_puts(&text, look.shown.text);
            }
            if (c < c2) {
                buf_byte(&text, '\t');
            }
        }
        buf_puts(&text, "\n");
    }
    if (!text.failed && text.data != NULL) {
        if (text.length > 65536) {
            say("ONLY THE FIRST 64 KB REACH OTHER PROGRAMS", true);
        }
        opos_clipboard_set(text.data, text.length > 65536 ? 65536 : text.length);
        clip_text = text.data;
    } else {
        free(text.data);
    }
    say(cut ? "CUT; CHOOSE WHERE TO PASTE" : "COPIED", false);
}

/* Pastes what Spreadsheet copied (formulas move with the cells). */
static void paste_cells(void) {
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    /* A selection that is a multiple of the copied size is filled. */
    int times_r = (r2 - r1 + 1) % clip_rows == 0 ? (r2 - r1 + 1) / clip_rows : 1;
    int times_c = (c2 - c1 + 1) % clip_cols == 0 ? (c2 - c1 + 1) / clip_cols : 1;
    if (clip_cut) {
        times_r = times_c = 1;
    }
    if (r1 + clip_rows * times_r > MAX_ROWS || c1 + clip_cols * times_c > MAX_COLS) {
        say("THAT DOES NOT FIT ON THE SHEET THERE", true);
        return;
    }
    edit_begin();
    if (clip_cut && clip_sheet < nsheets) {
        /* Clear the source first (what is pasted over it wins). */
        for (int r = 0; r < clip_rows; r++) {
            for (int c = 0; c < clip_cols; c++) {
                if (cell_find(sheets[clip_sheet], clip_r + r, clip_c + c) != NULL) {
                    edit_cell(clip_sheet, clip_r + r, clip_c + c);
                    cell_put(clip_sheet, clip_r + r, clip_c + c, NULL, 0);
                }
            }
        }
    }
    for (int tr = 0; tr < times_r; tr++) {
        for (int tc = 0; tc < times_c; tc++) {
            int top = r1 + tr * clip_rows, left = c1 + tc * clip_cols;
            for (int r = 0; r < clip_rows; r++) {
                for (int c = 0; c < clip_cols; c++) {
                    const struct clip_cell *q = &clip[(long)r * clip_cols + c];
                    int tr2 = top + r, tc2 = left + c;
                    if (!q->exists && cell_find(sh(), tr2, tc2) == NULL) {
                        continue;
                    }
                    edit_cell(active, tr2, tc2);
                    if (q->input != NULL && q->input[0] == '=' && !clip_cut) {
                        struct rewrite rw = {RW_SHIFT, tr2 - (clip_r + r),
                                             tc2 - (clip_c + c), -1, 0, 0,
                                             NULL, active};
                        char *moved = formula_rewrite(q->input + 1, &rw);
                        if (moved != NULL) {
                            size_t n = strlen(moved);
                            char *full = malloc(n + 2);
                            if (full != NULL) {
                                full[0] = '=';
                                memcpy(full + 1, moved, n + 1);
                                cell_put(active, tr2, tc2, full, q->style);
                                free(full);
                            }
                            free(moved);
                        }
                    } else {
                        cell_put(active, tr2, tc2, q->input,
                                 q->exists ? q->style : 0);
                    }
                }
            }
        }
    }
    edit_end();
    sh()->cur_r = r1;
    sh()->cur_c = c1;
    sh()->anc_r = r1 + clip_rows * times_r - 1;
    sh()->anc_c = c1 + clip_cols * times_c - 1;
    if (clip_cut) {
        clip_free();
    }
}

/* Pastes text from another program: tabs between cells, lines between
   rows; each piece is read like typing. */
static void paste_text(const char *text, size_t length) {
    int r = sh()->cur_r, c = sh()->cur_c;
    int max_c = c;
    edit_begin();
    size_t start = 0;
    char *piece = malloc(INPUT_MAX + 1);
    if (piece == NULL) {
        return;
    }
    for (size_t i = 0; i <= length; i++) {
        bool end_line = i == length || text[i] == '\n';
        if (i < length && text[i] != '\t' && !end_line) {
            continue;
        }
        size_t n = i - start;
        if (n > 0 && text[start + n - 1] == '\r') {
            n--;
        }
        if (n > INPUT_MAX) {
            n = INPUT_MAX;
        }
        memcpy(piece, text + start, n);
        piece[n] = '\0';
        if (!(i == length && n == 0 && c == sh()->cur_c)) {
            if (r < MAX_ROWS && c < MAX_COLS) {
                if (n > 0 || cell_find(sh(), r, c) != NULL) {
                    cell_type(active, r, c, piece);
                }
            }
            if (c > max_c) {
                max_c = c;
            }
        }
        start = i + 1;
        if (end_line) {
            if (i == length || (i + 1 == length)) {
                break;
            }
            r++;
            c = sh()->cur_c;
        } else {
            c++;
        }
    }
    free(piece);
    edit_end();
    sh()->anc_r = r;
    sh()->anc_c = max_c;
}

static void paste(void) {
    static char buffer[65537];
    long length = opos_clipboard_get(buffer, sizeof(buffer) - 1);
    bool ours = clip != NULL;
    if (length >= 0 && ours && clip_text != NULL) {
        size_t n = strlen(clip_text);
        if (n > 65536) {
            n = 65536;
        }
        ours = (size_t)length == n && memcmp(buffer, clip_text, n) == 0;
    }
    if (length == -OPOS_EACCES) {
        if (clip != NULL) {
            paste_cells();
            return;
        }
        say("SPREADSHEET MAY NOT READ THE CLIPBOARD (SECURITY CENTER)", true);
        return;
    }
    if (length < 0) {
        if (clip != NULL) {
            paste_cells();
            return;
        }
        say("COULD NOT READ THE CLIPBOARD", true);
        return;
    }
    if (ours) {
        paste_cells();
        return;
    }
    if (length == 0) {
        return;
    }
    buffer[length] = '\0';
    paste_text(buffer, (size_t)length);
}

/* Ctrl+D and Ctrl+R: the first row (column) of the selection copied
   into the rest. */
static void fill(bool down) {
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    if (down && r1 == r2 && r1 > 0) {
        r1--; /* one row: copy from above */
    }
    if (!down && c1 == c2 && c1 > 0) {
        c1--;
    }
    edit_begin();
    for (int r = r1; r <= r2; r++) {
        for (int c = c1; c <= c2; c++) {
            int sr = down ? r1 : r, sc = down ? c : c1;
            if ((down && r == r1) || (!down && c == c1)) {
                continue;
            }
            struct cell *src = cell_find(sh(), sr, sc);
            if (src == NULL && cell_find(sh(), r, c) == NULL) {
                continue;
            }
            char *input = src != NULL && src->input != NULL
                ? strdup(src->input) : NULL;
            int style = src != NULL ? src->style : 0;
            edit_cell(active, r, c);
            if (input != NULL && input[0] == '=') {
                struct rewrite rw = {RW_SHIFT, r - sr, c - sc, -1, 0, 0, NULL,
                                     active};
                char *moved = formula_rewrite(input + 1, &rw);
                if (moved != NULL) {
                    size_t n = strlen(moved);
                    char *full = malloc(n + 2);
                    if (full != NULL) {
                        full[0] = '=';
                        memcpy(full + 1, moved, n + 1);
                    }
                    free(input);
                    input = full;
                    free(moved);
                }
            }
            cell_put(active, r, c, input, style);
            free(input);
        }
    }
    edit_end();
}

/* --- rows and columns */

/* Every formula in the workbook rewritten (rw.own_sheet set per cell);
   changed cells are recorded for undo. */
static void rewrite_all(struct rewrite rw) {
    for (int si = 0; si < nsheets; si++) {
        struct sheet *s = sheets[si];
        rw.own_sheet = si;
        for (uint32_t i = 0; i < s->cap; i++) {
            struct cell *c = &s->cells[i];
            if (c->key == 0 || !c->formula) {
                continue;
            }
            char *changed = formula_rewrite(c->input + 1, &rw);
            if (changed == NULL || strcmp(changed, c->input + 1) == 0) {
                free(changed);
                continue;
            }
            size_t n = strlen(changed);
            char *full = malloc(n + 2);
            if (full != NULL) {
                full[0] = '=';
                memcpy(full + 1, changed, n + 1);
                int r = cell_row(c), col = cell_col(c);
                edit_cell(si, r, col);
                c = cell_find(s, r, col); /* edit_cell does not move cells */
                free(c->input);
                c->input = full;
                formula_free(c->code);
                c->code = NULL;
                c->gen = 0;
                c->cached = 0;
            }
            free(changed);
        }
    }
}

/* Inserts (count > 0) or deletes (count < 0) rows or columns at at. */
static void shift_lines(bool rows, int at, int count) {
    struct sheet *s = sh();
    int used_r, used_c;
    sheet_extent(s, &used_r, &used_c);
    if (count > 0 && (rows ? used_r + count > MAX_ROWS : used_c + count > MAX_COLS)) {
        say("THE SHEET IS FULL THERE; DELETE SOMETHING AT THE END FIRST", true);
        return;
    }
    edit_begin();
    edit_sheet(active);
    struct rewrite rw = {rows ? RW_ROWS : RW_COLS, 0, 0, active, at, count,
                         NULL, active};
    rewrite_all(rw);
    /* Move the cells: take them all out and put them back. */
    uint32_t n = 0;
    struct cell *moved = malloc((size_t)(s->count + 1) * sizeof(*moved));
    if (moved == NULL) {
        edit_end();
        return;
    }
    for (uint32_t i = 0; i < s->cap; i++) {
        if (s->cells[i].key != 0) {
            moved[n++] = s->cells[i];
        }
    }
    memset(s->cells, 0, (size_t)s->cap * sizeof(*s->cells));
    s->count = 0;
    s->rows_used = s->cols_used = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct cell *c = &moved[i];
        int r = cell_row(c), col = cell_col(c);
        int *line = rows ? &r : &col;
        bool keep = shift_line(line, at, count, rows ? MAX_ROWS : MAX_COLS);
        if (!keep) {
            cell_free_contents(c);
            continue;
        }
        struct cell *slot = cell_get(s, r, col);
        if (slot == NULL) {
            cell_free_contents(c);
            continue;
        }
        uint64_t key = slot->key;
        *slot = *c;
        slot->key = key;
        slot->gen = 0;
    }
    free(moved);
    if (!rows) {
        /* Column widths move too. */
        if (count > 0) {
            memmove(s->widths + at + count, s->widths + at,
                    (size_t)(MAX_COLS - at - count) * sizeof(uint16_t));
            memset(s->widths + at, 0, (size_t)count * sizeof(uint16_t));
        } else {
            int removed = -count;
            memmove(s->widths + at, s->widths + at + removed,
                    (size_t)(MAX_COLS - at - removed) * sizeof(uint16_t));
            memset(s->widths + MAX_COLS - removed, 0,
                   (size_t)removed * sizeof(uint16_t));
        }
    }
    s->extent_dirty = true;
    edit_end();
}

static void insert_or_delete(bool rows, bool insert) {
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    int at = rows ? r1 : c1;
    int count = rows ? r2 - r1 + 1 : c2 - c1 + 1;
    shift_lines(rows, at, insert ? count : -count);
    if (!insert) {
        select_cell(sh()->cur_r < (rows ? MAX_ROWS - 1 : MAX_ROWS)
                    ? r1 : r1, c1, false);
    }
}

/* --- sorting */

struct sort_row {
    int row;
    struct look key;
    uint8_t kind;
    double num;
    char *text;
};

static bool sort_descending;

static int sort_compare(const void *a, const void *b) {
    const struct sort_row *x = a, *y = b;
    /* empty cells always last */
    if (x->kind == V_EMPTY || y->kind == V_EMPTY) {
        if (x->kind == y->kind) {
            return x->row - y->row;
        }
        return x->kind == V_EMPTY ? 1 : -1;
    }
    struct val p = {0}, q = {0};
    p.kind = x->kind;
    p.num = x->num;
    p.str = x->text;
    q.kind = y->kind;
    q.num = y->num;
    q.str = y->text;
    if (p.kind == V_ERR) {
        p.kind = V_BOOL;
        p.num = 2;
    }
    if (q.kind == V_ERR) {
        q.kind = V_BOOL;
        q.num = 2;
    }
    int c = compare_values(&p, &q);
    if (sort_descending) {
        c = -c;
    }
    return c != 0 ? c : x->row - y->row;
}

/* Sorts the rows of the selection (or the data around the cell) by the
   active cell's column. */
static void sort_rows(bool descending) {
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    struct sheet *s = sh();
    int rows, cols;
    sheet_extent(s, &rows, &cols);
    if (single_cell()) {
        /* The block of data around the cell. */
        while (r1 > 0 && cell_has_value(r1 - 1, c1)) {
            r1--;
        }
        while (r2 + 1 < rows && cell_has_value(r2 + 1, c1)) {
            r2++;
        }
        while (c1 > 0 && cell_has_value(r1, c1 - 1)) {
            c1--;
        }
        while (c2 + 1 < cols && cell_has_value(r1, c2 + 1)) {
            c2++;
        }
        /* A header row of text over numbers stays on top. */
        struct cell *first = cell_find(s, r1, s->cur_c);
        struct cell *second = cell_find(s, r1 + 1, s->cur_c);
        if (r2 > r1 && first != NULL && first->kind == V_STR
            && second != NULL && second->kind != V_STR) {
            r1++;
        }
    }
    if (r2 >= rows) {
        r2 = rows - 1;
    }
    if (c2 >= cols) {
        c2 = cols - 1;
    }
    if (r2 <= r1 || c2 < c1) {
        return;
    }
    int n = r2 - r1 + 1, width = c2 - c1 + 1;
    struct sort_row *order = calloc((size_t)n, sizeof(*order));
    struct clip_cell *saved = calloc((size_t)n * (size_t)width, sizeof(*saved));
    if (order == NULL || saved == NULL) {
        free(order);
        free(saved);
        say("NOT ENOUGH MEMORY", true);
        return;
    }
    int key_col = s->cur_c >= c1 && s->cur_c <= c2 ? s->cur_c : c1;
    for (int i = 0; i < n; i++) {
        struct cell *c = cell_find(s, r1 + i, key_col);
        order[i].row = i;
        if (c != NULL) {
            if (c->formula && c->gen != calc_gen) {
                cell_compute(active, c);
            }
            order[i].kind = c->kind;
            order[i].num = c->num;
            order[i].text = c->kind == V_STR ? c->text : NULL;
            if (c->kind == V_STR && order[i].text == NULL) {
                order[i].text = (char *)"";
            }
        }
        for (int k = 0; k < width; k++) {
            struct cell *cell = cell_find(s, r1 + i, c1 + k);
            struct clip_cell *q = &saved[(long)i * width + k];
            if (cell != NULL) {
                q->exists = true;
                q->style = cell->style;
                q->input = cell->input != NULL ? strdup(cell->input) : NULL;
            }
        }
    }
    sort_descending = descending;
    qsort(order, (size_t)n, sizeof(*order), sort_compare);
    edit_begin();
    for (int i = 0; i < n; i++) {
        int from = order[i].row;
        for (int k = 0; k < width; k++) {
            struct clip_cell *q = &saved[(long)from * width + k];
            int r = r1 + i, c = c1 + k;
            if (!q->exists && cell_find(s, r, c) == NULL) {
                continue;
            }
            edit_cell(active, r, c);
            if (q->input != NULL && q->input[0] == '=' && from != i) {
                struct rewrite rw = {RW_SHIFT, i - from, 0, -1, 0, 0, NULL,
                                     active};
                char *moved = formula_rewrite(q->input + 1, &rw);
                char *full = moved != NULL ? malloc(strlen(moved) + 2) : NULL;
                if (full != NULL) {
                    full[0] = '=';
                    strcpy(full + 1, moved);
                    cell_put(active, r, c, full, q->style);
                }
                free(full);
                free(moved);
            } else {
                cell_put(active, r, c, q->input, q->exists ? q->style : 0);
            }
        }
    }
    edit_end();
    for (long i = 0; i < (long)n * width; i++) {
        free(saved[i].input);
    }
    free(saved);
    free(order);
    s->anc_r = r1;
    s->anc_c = c1;
    s->cur_r = r2;
    s->cur_c = c2;
    say(descending ? "SORTED Z TO A" : "SORTED A TO Z", false);
}

/* --- sheets */

static void rename_sheet(int si, const char *name) {
    if (strcmp(sheets[si]->name, name) == 0) {
        return;
    }
    struct rewrite rw = {RW_RENAME, 0, 0, si, 0, 0, name, 0};
    recording = false;
    rewrite_all(rw);
    snprintf(sheets[si]->name, sizeof(sheets[si]->name), "%s", name);
    sheets_version++;
    undo_clear_all(); /* earlier steps name the old sheet */
    set_modified(true);
    recalc();
}

static void new_sheet(void) {
    char name[32];
    for (int k = nsheets + 1;; k++) {
        snprintf(name, sizeof(name), "Sheet%d", k);
        if (sheet_find(name) < 0) {
            break;
        }
    }
    int si = sheet_add(name);
    if (si < 0) {
        say("A WORKBOOK HAS AT MOST 64 SHEETS HERE", true);
        return;
    }
    active = si;
    sheets_version++;
    set_modified(true);
    recalc();
}

static void delete_sheet(int si) {
    if (nsheets <= 1) {
        say("A WORKBOOK NEEDS AT LEAST ONE SHEET", true);
        return;
    }
    struct rewrite rw = {RW_DROP_SHEET, 0, 0, si, 0, 0, NULL, 0};
    recording = false;
    rewrite_all(rw);
    sheet_free(sheets[si]);
    memmove(sheets + si, sheets + si + 1,
            (size_t)(nsheets - si - 1) * sizeof(*sheets));
    nsheets--;
    sheets[nsheets] = NULL;
    if (active >= nsheets) {
        active = nsheets - 1;
    } else if (active > si) {
        active--;
    }
    sheets_version++;
    /* Formulas on the other sheets kept their text; recompile them. */
    undo_clear_all();
    set_modified(true);
    recalc();
}

/* --- column widths */

static void set_width(int col, int width) {
    if (width < 2) {
        width = 2;
    }
    if (width > 2000) {
        width = 2000;
    }
    sh()->widths[col] = (uint16_t)(width == DEFAULT_WIDTH ? 0 : width);
}

/* The width that fits a column's contents (at 100%). */
static int fit_width(int col) {
    struct sheet *s = sh();
    int rows, cols;
    sheet_extent(s, &rows, &cols);
    int best = 0;
    int size = (11 * 4 + 1) / 3; /* 100% */
    for (int r = 0; r < rows; r++) {
        struct cell *c = cell_find(s, r, col);
        if (c == NULL) {
            continue;
        }
        struct look look;
        cell_look(c, 17, &look);
        struct opos_font *font = font_for(style_of(c)->flags);
        if (font == NULL) {
            continue;
        }
        int w = opos_font_text_width(font, size, look.shown.text,
                                     strlen(look.shown.text));
        if (w > best) {
            best = w;
        }
    }
    return best == 0 ? DEFAULT_WIDTH : best + 10;
}

/* =========================================================== drawing */

struct box {
    int x, y, w, h;
};

static bool inside(struct box b, int x, int y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

static void fill_box(struct box b, uint32_t colour) {
    opos_fill_rect(screen, b.x, b.y, b.w, b.h, colour);
}

/* A canvas that draws only inside a box. */
static struct opos_canvas clip_view(struct box b) {
    struct opos_canvas v = {0};
    if (b.x < 0) {
        b.w += b.x;
        b.x = 0;
    }
    if (b.y < 0) {
        b.h += b.y;
        b.y = 0;
    }
    if (b.x + b.w > canvas.width) {
        b.w = canvas.width - b.x;
    }
    if (b.y + b.h > canvas.height) {
        b.h = canvas.height - b.y;
    }
    if (b.w <= 0 || b.h <= 0) {
        return v;
    }
    v.pixels = canvas.pixels + (size_t)b.y * (size_t)canvas.stride + (size_t)b.x;
    v.width = b.w;
    v.height = b.h;
    v.stride = canvas.stride;
    return v;
}

/* Text in a TrueType face, clipped to clip; x and baseline are screen
   positions. */
static void draw_tt(struct box clip, int x, int baseline, uint32_t flags,
                    int size, const char *text, size_t length,
                    uint32_t colour) {
    struct opos_font *font = font_for(flags);
    struct opos_canvas v = clip_view(clip);
    if (font == NULL || v.pixels == NULL) {
        return;
    }
    int cx = clip.x < 0 ? 0 : clip.x, cy = clip.y < 0 ? 0 : clip.y;
    int end = opos_font_draw(font, &v, x - cx, baseline - cy, size, text,
                             length, colour);
    int w = end - (x - cx);
    if (flags & F_UNDER) {
        opos_fill_rect(&v, x - cx, baseline - cy + 2, w, 1, colour);
    }
    if (flags & F_STRIKE) {
        opos_fill_rect(&v, x - cx, baseline - cy - size / 3, w, 1, colour);
    }
}

static void draw_label(struct box b, const char *label, uint32_t colour) {
    int width = opos_text_width(label, 2);
    opos_draw_text(screen, b.x + (b.w - width) / 2 + 1,
                   b.y + (b.h - opos_char_height(2)) / 2, label, colour, 2);
}

/* --- toolbar */

enum button {
    B_NEW, B_OPEN, B_SAVE, B_SAVE_AS, B_UNDO, B_REDO, B_CUT, B_COPY,
    B_PASTE, B_ZOOM_OUT, B_ZOOM, B_ZOOM_IN,
    B_BOLD, B_ITALIC, B_UNDER, B_STRIKE, B_COLOUR, B_FILL, B_LEFT,
    B_CENTER, B_RIGHT, B_FORMAT, B_DEC_LESS, B_DEC_MORE, B_SUM, B_INSERT,
    B_DELETE, B_SORT, B_COUNT,
};

static struct box boxes[B_COUNT];

enum menu {
    MENU_NONE, MENU_FORMAT, MENU_COLOUR, MENU_FILL, MENU_INSERT, MENU_DELETE,
    MENU_SORT, MENU_CONTEXT, MENU_TAB,
};
static enum menu menu;
static int menu_x, menu_y;
static int tab_menu_sheet;

enum pending {
    PENDING_NONE, PENDING_CLOSE, PENDING_NEW, PENDING_OPEN,
    PENDING_DELETE_SHEET,
};
static enum pending pending;

static void layout_toolbar(void) {
    static const struct {
        int button, width;
    } row1[] = {
        {B_NEW, 52}, {B_OPEN, 60}, {B_SAVE, 60}, {B_SAVE_AS, 92}, {-1, 10},
        {B_UNDO, 60}, {B_REDO, 60}, {-1, 10}, {B_CUT, 50}, {B_COPY, 60},
        {B_PASTE, 70},
    };
    int x = 8, y = 6, h = ROW_H - 12;
    for (size_t i = 0; i < sizeof(row1) / sizeof(row1[0]); i++) {
        if (row1[i].button < 0) {
            x += row1[i].width;
            continue;
        }
        boxes[row1[i].button] = (struct box){x, y, row1[i].width, h};
        x += row1[i].width + 4;
    }
    int zx = view_width - 8 - (30 + 4 + 64 + 4 + 30);
    if (zx < x + 10) {
        zx = x + 10;
    }
    boxes[B_ZOOM_OUT] = (struct box){zx, y, 30, h};
    boxes[B_ZOOM] = (struct box){zx + 34, y, 64, h};
    boxes[B_ZOOM_IN] = (struct box){zx + 102, y, 30, h};
    static const struct {
        int button, width;
    } row2[] = {
        {B_BOLD, 30}, {B_ITALIC, 30}, {B_UNDER, 30}, {B_STRIKE, 30}, {-1, 10},
        {B_COLOUR, 50}, {B_FILL, 50}, {-1, 10}, {B_LEFT, 30}, {B_CENTER, 30},
        {B_RIGHT, 30}, {-1, 10}, {B_FORMAT, 120}, {B_DEC_LESS, 40},
        {B_DEC_MORE, 40}, {-1, 10}, {B_SUM, 40}, {-1, 10}, {B_INSERT, 96},
        {B_DELETE, 96}, {B_SORT, 80},
    };
    x = 8;
    y = ROW_H + 6;
    for (size_t i = 0; i < sizeof(row2) / sizeof(row2[0]); i++) {
        if (row2[i].button < 0) {
            x += row2[i].width;
            continue;
        }
        boxes[row2[i].button] = (struct box){x, y, row2[i].width, h};
        x += row2[i].width + 4;
    }
}

static void draw_button_base(struct box b, bool on) {
    bool hover = inside(b, mouse_x, mouse_y) && pending == PENDING_NONE;
    fill_box(b, on ? UI_ACTIVE : hover ? UI_HOVER : UI_BUTTON);
}

static void draw_button(int index, const char *label, bool on, bool enabled) {
    draw_button_base(boxes[index], on);
    draw_label(boxes[index], label, enabled ? UI_TEXT : UI_DIM);
}

static void draw_face(int index, const char *letter, uint32_t flags, bool on) {
    struct box b = boxes[index];
    draw_button_base(b, on);
    int size = 17;
    struct opos_font *font = font_for(flags);
    if (font == NULL) {
        draw_label(b, letter, UI_TEXT);
        return;
    }
    int w = opos_font_text_width(font, size, letter, strlen(letter));
    int base = b.y + b.h / 2 + opos_font_ascent(font, size) / 2 - 1;
    draw_tt(b, b.x + (b.w - w) / 2, base, flags, size, letter, strlen(letter),
            UI_TEXT);
}

static void draw_align_icon(int index, int align, bool on) {
    struct box b = boxes[index];
    draw_button_base(b, on);
    int widths[4] = {16, 10, 16, 12};
    int x0 = b.x + (b.w - 16) / 2, y0 = b.y + (b.h - 13) / 2;
    for (int i = 0; i < 4; i++) {
        int w = widths[i];
        int x = align == AL_LEFT ? x0 : align == AL_RIGHT ? x0 + 16 - w
                                                          : x0 + (16 - w) / 2;
        opos_fill_rect(screen, x, y0 + i * 4, w, 2, UI_TEXT);
    }
}

static uint32_t last_colour = 0xc00000, last_fill = 0xffff00;

static const uint32_t palette[20] = {
    0x000000, 0x595959, 0x808080, 0xbfbfbf, 0xffffff,
    0xc00000, 0xff0000, 0xffc000, 0xffff00, 0x92d050,
    0x00b050, 0x00b0f0, 0x0070c0, 0x002060, 0x7030a0,
    0xfce4d6, 0xfff2cc, 0xe2efda, 0xddebf7, 0xededed,
};

static void draw_toolbar(void) {
    fill_box((struct box){0, 0, view_width, TOOLBAR_H}, UI_BACK);
    opos_fill_rect(screen, 0, ROW_H - 1, view_width, 1, UI_PANEL);
    draw_button(B_NEW, "NEW", false, true);
    draw_button(B_OPEN, "OPEN", false, true);
    draw_button(B_SAVE, "SAVE", false, true);
    draw_button(B_SAVE_AS, "SAVE AS", false, true);
    draw_button(B_UNDO, "UNDO", false, undo_count > 0);
    draw_button(B_REDO, "REDO", false, redo_count > 0);
    draw_button(B_CUT, "CUT", false, true);
    draw_button(B_COPY, "COPY", false, true);
    draw_button(B_PASTE, "PASTE", false, true);
    draw_button(B_ZOOM_OUT, "-", false, zoom > 50);
    char z[16];
    snprintf(z, sizeof(z), "%d%%", zoom);
    draw_button(B_ZOOM, z, false, true);
    draw_button(B_ZOOM_IN, "+", false, zoom < 200);

    struct cell *c = cell_find(sh(), sh()->cur_r, sh()->cur_c);
    const struct style *st = style_of(c);
    draw_face(B_BOLD, "B", F_BOLD, (st->flags & F_BOLD) != 0);
    draw_face(B_ITALIC, "I", F_ITALIC, (st->flags & F_ITALIC) != 0);
    draw_face(B_UNDER, "U", F_UNDER, (st->flags & F_UNDER) != 0);
    draw_face(B_STRIKE, "S", F_STRIKE, (st->flags & F_STRIKE) != 0);
    /* Text colour: an A over a bar; fill: a box. Click the left part to
       apply the last one, the arrow for the palette. */
    struct box b = boxes[B_COLOUR];
    draw_button_base(b, menu == MENU_COLOUR);
    draw_tt(b, b.x + 8, b.y + b.h - 10, F_BOLD, 16, "A", 1, UI_TEXT);
    opos_fill_rect(screen, b.x + 6, b.y + b.h - 7, 16, 4, last_colour);
    opos_draw_text(screen, b.x + 34, b.y + (b.h - 9) / 2, "v", UI_TEXT, 1);
    opos_fill_rect(screen, b.x + 28, b.y + 4, 1, b.h - 8, UI_PANEL);
    b = boxes[B_FILL];
    draw_button_base(b, menu == MENU_FILL);
    opos_draw_rect(screen, b.x + 6, b.y + 5, 16, 12, UI_TEXT);
    opos_fill_rect(screen, b.x + 6, b.y + b.h - 7, 16, 4, last_fill);
    opos_draw_text(screen, b.x + 34, b.y + (b.h - 9) / 2, "v", UI_TEXT, 1);
    opos_fill_rect(screen, b.x + 28, b.y + 4, 1, b.h - 8, UI_PANEL);
    draw_align_icon(B_LEFT, AL_LEFT, st->align == AL_LEFT);
    draw_align_icon(B_CENTER, AL_CENTER, st->align == AL_CENTER);
    draw_align_icon(B_RIGHT, AL_RIGHT, st->align == AL_RIGHT);
    const char *format_label = "GENERAL";
    const char *f = st->format;
    if (f[0] == '\0') {
        format_label = "GENERAL";
    } else if (format_is_text(f)) {
        format_label = "TEXT";
    } else if (format_is_date(f)) {
        format_label = strchr(f, 'y') != NULL || strchr(f, 'd') != NULL
            ? "DATE" : "TIME";
    } else if (strchr(f, '%') != NULL) {
        format_label = "PERCENT";
    } else if (strchr(f, '$') != NULL || strstr(f, "\xe2\x82\xac") != NULL) {
        format_label = "CURRENCY";
    } else {
        format_label = "NUMBER";
    }
    char fl[24];
    snprintf(fl, sizeof(fl), "%s v", format_label);
    draw_button(B_FORMAT, fl, menu == MENU_FORMAT, true);
    draw_button(B_DEC_LESS, ".0-", false, true);
    draw_button(B_DEC_MORE, ".0+", false, true);
    /* Sigma */
    draw_button_base(boxes[B_SUM], false);
    struct box s = boxes[B_SUM];
    int sx = s.x + 13, sy = s.y + 6;
    opos_fill_rect(screen, sx, sy, 14, 2, UI_TEXT);
    opos_fill_rect(screen, sx, sy + s.h - 14, 14, 2, UI_TEXT);
    for (int k = 0; k < 7; k++) {
        opos_fill_rect(screen, sx + 1 + k, sy + 2 + k, 2, 1, UI_TEXT);
        opos_fill_rect(screen, sx + 1 + k, sy + s.h - 15 - k, 2, 1, UI_TEXT);
    }
    draw_button(B_INSERT, "INSERT v", menu == MENU_INSERT, true);
    draw_button(B_DELETE, "DELETE v", menu == MENU_DELETE, true);
    draw_button(B_SORT, "SORT v", menu == MENU_SORT, true);
}

/* --- the formula bar */

static struct box name_box(void) {
    return (struct box){8, TOOLBAR_H + 4, NAME_W, BAR_H - 8};
}

static struct box input_box(void) {
    int x = 8 + NAME_W + 36;
    return (struct box){x, TOOLBAR_H + 4, view_width - x - 8, BAR_H - 8};
}

static int bar_text_x; /* the formula bar's text start, for clicks */

static void draw_formula_bar(void) {
    fill_box((struct box){0, TOOLBAR_H, view_width, BAR_H}, UI_BACK);
    struct box nb = name_box();
    fill_box(nb, UI_PANEL);
    char name[48];
    struct sheet *s = sh();
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    if (editing == EDIT_CELL) {
        cell_name(edit_r, edit_c, name);
    } else if (single_cell()) {
        cell_name(s->cur_r, s->cur_c, name);
    } else {
        snprintf(name, sizeof(name), "%dR X %dC", r2 - r1 + 1, c2 - c1 + 1);
    }
    opos_draw_text(screen, nb.x + 8, nb.y + (nb.h - 18) / 2, name, UI_TEXT, 2);
    opos_draw_text(screen, nb.x + nb.w + 8, nb.y + (nb.h - 18) / 2, "FX",
                   UI_DIM, 2);
    struct box ib = input_box();
    fill_box(ib, OPOS_RGB(255, 255, 255));
    const char *text;
    int length;
    if (editing == EDIT_CELL) {
        text = edit_text;
        length = edit_len;
    } else {
        struct cell *c = cell_find(s, s->cur_r, s->cur_c);
        text = c != NULL && c->input != NULL ? c->input : "";
        length = (int)strlen(text);
    }
    int size = 15;
    struct opos_font *font = font_for(0);
    if (font == NULL) {
        return;
    }
    int base = ib.y + ib.h / 2 + opos_font_ascent(font, size) / 2 - 1;
    /* Keep the caret visible: scroll the text left when needed. */
    int x = ib.x + 6;
    if (editing == EDIT_CELL) {
        int caret_w = opos_font_text_width(font, size, text, (size_t)edit_caret);
        if (caret_w > ib.w - 20) {
            x -= caret_w - (ib.w - 20);
        }
    }
    bar_text_x = x;
    /* One line: stop at a line break. */
    int shown = 0;
    while (shown < length && text[shown] != '\n') {
        shown++;
    }
    draw_tt(ib, x, base, 0, size, text, (size_t)shown, 0x202124);
    if (editing == EDIT_CELL) {
        int cx = x + opos_font_text_width(font, size, text, (size_t)edit_caret);
        opos_fill_rect(screen, cx, ib.y + 4, 1, ib.h - 8, 0x202124);
    }
}

/* --- the grid */

static uint32_t blend(uint32_t base, int r, int g, int b, int alpha) {
    int br = (int)(base >> 16 & 0xff), bg = (int)(base >> 8 & 0xff),
        bb = (int)(base & 0xff);
    br += (r - br) * alpha / 255;
    bg += (g - bg) * alpha / 255;
    bb += (b - bb) * alpha / 255;
    return OPOS_RGB(br, bg, bb);
}

/* General's width in characters for a column. */
static int general_chars(int col) {
    struct opos_font *font = font_for(0);
    int digit = font != NULL
        ? opos_font_advance(font, text_size(), '0') / 64 : 8;
    if (digit < 1) {
        digit = 1;
    }
    int chars = (col_px(col) - px(6)) / digit;
    return chars < 1 ? 1 : chars;
}

static void draw_cell_text(struct cell *c, int r, int col, int x, int y,
                           int w, int clip_right) {
    struct look look;
    cell_look(c, general_chars(col), &look);
    const char *text = look.shown.text;
    if (text[0] == '\0') {
        return;
    }
    const struct style *st = style_of(c);
    struct opos_font *font = font_for(st->flags);
    if (font == NULL) {
        return;
    }
    int size = text_size();
    int tw = opos_font_text_width(font, size, text, strlen(text));
    char hashes[64];
    if (look.number && tw > w - px(4)) {
        /* Numbers that do not fit show ###. */
        int hw = opos_font_advance(font, size, '#') / 64;
        int count = hw > 0 ? (w - px(4)) / hw : 1;
        if (count < 1) {
            count = 1;
        }
        if (count > 60) {
            count = 60;
        }
        memset(hashes, '#', (size_t)count);
        hashes[count] = '\0';
        text = hashes;
        tw = count * hw;
    }
    int pad = px(3) < 2 ? 2 : px(3);
    int tx;
    struct box clip = {x + 1, y, w - 1, row_h() - 1};
    if (look.align == AL_RIGHT) {
        tx = x + w - pad - tw;
    } else if (look.align == AL_CENTER) {
        tx = x + (w - tw) / 2;
    } else {
        tx = x + pad;
        /* Text may run on over empty cells to the right. */
        if (!look.number && tx + tw > x + w) {
            clip.w = clip_right - clip.x;
        }
    }
    if (look.align == AL_RIGHT && !look.number && tx < x + pad) {
        /* Long right-aligned text runs to the left: keep it inside. */
    }
    uint32_t colour = look.shown.has_colour ? look.shown.colour : st->colour;
    int base = y + (row_h() + opos_font_ascent(font, size)
                    - opos_font_descent(font, size)) / 2;
    (void)r;
    draw_tt(clip, tx, base, st->flags, size, text, strlen(text), colour);
}

static void draw_grid(void) {
    struct sheet *s = sh();
    int gx = cells_x(), gy = cells_y();
    int gw = cells_w(), gh = cells_h();
    int rh = row_h();
    fill_box((struct box){0, grid_top(), view_width - SCROLLBAR_W,
                          gh + head_h()}, OPOS_RGB(255, 255, 255));
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    int nrows = visible_rows();
    /* Column x positions. */
    int xs[1100];
    int ncols = 0;
    {
        int x = gx;
        for (int c = s->left; c < MAX_COLS && x < gx + gw && ncols < 1099; c++) {
            xs[ncols++] = x;
            x += col_px(c);
        }
        xs[ncols] = x;
    }
    /* Fills and the selection tint. */
    for (int i = 0; i < nrows; i++) {
        int r = s->top + i;
        if (r >= MAX_ROWS) {
            break;
        }
        int y = gy + i * rh;
        for (int k = 0; k < ncols; k++) {
            int c = s->left + k;
            struct cell *cell = cell_find(s, r, c);
            uint32_t back = 0xffffff;
            const struct style *st = style_of(cell);
            if (st->fill != NO_FILL) {
                back = st->fill;
            }
            bool selected = r >= r1 && r <= r2 && c >= c1 && c <= c2
                && !(r == s->cur_r && c == s->cur_c);
            if (selected) {
                back = blend(back, SEL_TINT_R, SEL_TINT_G, SEL_TINT_B, 40);
            }
            if (back != 0xffffff) {
                opos_fill_rect(screen, xs[k], y, xs[k + 1] - xs[k], rh, back);
            }
        }
    }
    /* Grid lines. */
    for (int k = 0; k <= ncols; k++) {
        opos_fill_rect(screen, xs[k] - 1, gy, 1, gh, GRID_LINE);
    }
    for (int i = 0; i <= nrows; i++) {
        opos_fill_rect(screen, gx, gy + i * rh - 1, gw, 1, GRID_LINE);
    }
    /* Fills hide the grid lines inside them (like other spreadsheets). */
    /* Text. */
    struct box grid_clip = {gx, gy, gw, gh};
    for (int i = 0; i < nrows; i++) {
        int r = s->top + i;
        if (r >= MAX_ROWS) {
            break;
        }
        int y = gy + i * rh;
        /* Text running in from cells left of the view. */
        for (int c = s->left - 1; c >= 0 && c >= s->left - 30; c--) {
            struct cell *cell = cell_find(s, r, c);
            if (cell == NULL || cell->input == NULL) {
                continue;
            }
            struct look look;
            cell_look(cell, general_chars(c), &look);
            if (!look.number && look.align == AL_LEFT) {
                int right = xs[0];
                for (int k = 0; k < ncols; k++) {
                    if (cell_has_value(r, s->left + k)) {
                        right = xs[k];
                        break;
                    }
                    right = xs[k + 1];
                }
                draw_cell_text(cell, r, c, col_x(c), y, col_px(c), right);
                (void)grid_clip;
            }
            break;
        }
        for (int k = 0; k < ncols; k++) {
            int c = s->left + k;
            struct cell *cell = cell_find(s, r, c);
            if (cell == NULL || cell->input == NULL) {
                continue;
            }
            if (editing == EDIT_CELL && edit_sheet_index == active
                && r == edit_r && c == edit_c) {
                continue;
            }
            /* How far may text run on? Until the next cell with a
               value. */
            int right = xs[k + 1];
            for (int j = k + 1; j < ncols; j++) {
                if (cell_has_value(r, s->left + j)) {
                    break;
                }
                right = xs[j + 1];
            }
            if (right > gx + gw) {
                right = gx + gw;
            }
            draw_cell_text(cell, r, c, xs[k], y, xs[k + 1] - xs[k], right);
        }
    }
    /* Headers. */
    int hy = grid_top();
    int hh = head_h();
    fill_box((struct box){0, hy, view_width - SCROLLBAR_W, hh}, HEAD_BACK);
    fill_box((struct box){0, gy, gx, gh}, HEAD_BACK);
    struct opos_font *font = font_for(0);
    int size = text_size() - 1;
    for (int k = 0; k < ncols; k++) {
        int c = s->left + k;
        if (c >= c1 && c <= c2) {
            opos_fill_rect(screen, xs[k], hy, xs[k + 1] - xs[k], hh, HEAD_SEL);
        }
        opos_fill_rect(screen, xs[k + 1] - 1, hy, 1, hh, GRID_LINE);
        char name[4];
        col_name(c, name);
        if (font != NULL) {
            int w = opos_font_text_width(font, size, name, strlen(name));
            int base = hy + (hh + opos_font_ascent(font, size)) / 2 - 1;
            draw_tt((struct box){xs[k], hy, xs[k + 1] - xs[k], hh},
                    xs[k] + (xs[k + 1] - xs[k] - w) / 2, base, 0, size, name,
                    strlen(name), HEAD_TEXT);
        }
    }
    for (int i = 0; i < nrows; i++) {
        int r = s->top + i;
        if (r >= MAX_ROWS) {
            break;
        }
        int y = gy + i * rh;
        if (r >= r1 && r <= r2) {
            opos_fill_rect(screen, 0, y, gx, rh, HEAD_SEL);
        }
        opos_fill_rect(screen, 0, y + rh - 1, gx, 1, GRID_LINE);
        char number[16];
        snprintf(number, sizeof(number), "%d", r + 1);
        if (font != NULL) {
            int w = opos_font_text_width(font, size, number, strlen(number));
            int base = y + (rh + opos_font_ascent(font, size)) / 2 - 1;
            draw_tt((struct box){0, y, gx, rh}, (gx - w) / 2, base, 0, size,
                    number, strlen(number), HEAD_TEXT);
        }
    }
    opos_fill_rect(screen, 0, hy + hh - 1, view_width - SCROLLBAR_W, 1,
                   OPOS_RGB(190, 192, 196));
    opos_fill_rect(screen, gx - 1, hy, 1, gh + hh, OPOS_RGB(190, 192, 196));
    /* The selection border and the active cell. */
    if (editing != EDIT_TAB) {
        int sx1 = col_x(c1), sx2 = col_x(c2) + col_px(c2);
        int sy1 = row_y(r1), sy2 = row_y(r2) + rh;
        struct opos_canvas v = clip_view(grid_clip);
        int ox = gx, oy = gy;
        if (v.pixels != NULL) {
            opos_draw_rect(&v, sx1 - 1 - ox, sy1 - 1 - oy, sx2 - sx1 + 1,
                           sy2 - sy1 + 1, SEL_BORDER);
            opos_draw_rect(&v, sx1 - 2 - ox, sy1 - 2 - oy, sx2 - sx1 + 3,
                           sy2 - sy1 + 3, SEL_BORDER);
            int cx1 = col_x(s->cur_c), cy1 = row_y(s->cur_r);
            if (!single_cell()) {
                opos_draw_rect(&v, cx1 - ox, cy1 - oy,
                               col_px(s->cur_c) - 1, rh - 1, SEL_BORDER);
            }
            /* the fill handle square */
            opos_fill_rect(&v, sx2 - 4 - ox, sy2 - 4 - oy, 6, 6, SEL_BORDER);
        }
    }
    /* Cut or copied cells: a dashed outline. */
    if (clip != NULL && clip_sheet == active) {
        int x1 = col_x(clip_c), x2 = col_x(clip_c + clip_cols - 1)
            + col_px(clip_c + clip_cols - 1);
        int y1 = row_y(clip_r), y2 = row_y(clip_r + clip_rows - 1) + rh;
        struct opos_canvas v = clip_view(grid_clip);
        if (v.pixels != NULL) {
            for (int x = x1; x < x2; x += 8) {
                opos_fill_rect(&v, x - gx, y1 - 1 - gy, 4, 2, SEL_BORDER);
                opos_fill_rect(&v, x - gx, y2 - 1 - gy, 4, 2, SEL_BORDER);
            }
            for (int y = y1; y < y2; y += 8) {
                opos_fill_rect(&v, x1 - 1 - gx, y - gy, 2, 4, SEL_BORDER);
                opos_fill_rect(&v, x2 - 1 - gx, y - gy, 2, 4, SEL_BORDER);
            }
        }
    }
    /* The cell editor. */
    if (editing == EDIT_CELL && edit_sheet_index == active) {
        int x = col_x(edit_c), y = row_y(edit_r);
        struct opos_font *ef = font_for(style_of(cell_find(s, edit_r, edit_c))->flags);
        if (ef != NULL && y >= gy && y < gy + gh) {
            int tsize = text_size();
            int caret_w = opos_font_text_width(ef, tsize, edit_text,
                                               (size_t)edit_caret);
            int line = 0;
            while (line < edit_len && edit_text[line] != '\n') {
                line++;
            }
            int tw = opos_font_text_width(ef, tsize, edit_text, (size_t)line);
            int w = col_px(edit_c);
            while (w < tw + px(10) && x + w < gx + gw) {
                w += px(DEFAULT_WIDTH);
            }
            struct box eb = {x, y, w, rh};
            fill_box(eb, 0xffffff);
            opos_draw_rect(screen, x - 1, y - 1, w + 1, rh + 1, SEL_BORDER);
            opos_draw_rect(screen, x - 2, y - 2, w + 3, rh + 3, SEL_BORDER);
            int base = y + (rh + opos_font_ascent(ef, tsize)
                            - opos_font_descent(ef, tsize)) / 2;
            int shift = caret_w > w - px(8) ? caret_w - (w - px(8)) : 0;
            draw_tt(eb, x + px(3) - shift, base,
                    style_of(cell_find(s, edit_r, edit_c))->flags, tsize,
                    edit_text, (size_t)line, 0x202124);
            opos_fill_rect(screen, x + px(3) - shift + caret_w, y + 3, 1,
                           rh - 6, 0x202124);
        }
    }
    /* Corner. */
    fill_box((struct box){0, hy, gx - 1, hh - 1}, HEAD_BACK);
}

/* --- scrollbars, tabs, status */

static int scroll_rows_total(void) {
    int rows, cols;
    sheet_extent(sh(), &rows, &cols);
    int total = rows > sh()->cur_r + 1 ? rows : sh()->cur_r + 1;
    total += visible_rows() * 2;
    if (total < 200) {
        total = 200;
    }
    return total > MAX_ROWS ? MAX_ROWS : total;
}

static struct box vscroll_track(void) {
    return (struct box){view_width - SCROLLBAR_W, cells_y(), SCROLLBAR_W,
                        cells_h()};
}

static struct box vscroll_thumb(void) {
    struct box t = vscroll_track();
    int total = scroll_rows_total();
    int shown = visible_rows();
    int h = (int)((long)t.h * shown / total);
    if (h < 24) {
        h = 24;
    }
    if (h > t.h) {
        h = t.h;
    }
    int y = t.y + (int)((long)(t.h - h) * sh()->top
                        / (total - shown > 0 ? total - shown : 1));
    if (y > t.y + t.h - h) {
        y = t.y + t.h - h;
    }
    return (struct box){t.x + 2, y, SCROLLBAR_W - 4, h};
}

static int tabs_y(void) {
    return view_height - TABS_H - STATUS_H;
}

static struct box hscroll_track(void) {
    int w = view_width / 3;
    return (struct box){view_width - w - 8, tabs_y() + 8, w, TABS_H - 16};
}

static int scroll_cols_total(void) {
    int rows, cols;
    sheet_extent(sh(), &rows, &cols);
    int total = cols > sh()->cur_c + 1 ? cols : sh()->cur_c + 1;
    total += 26;
    if (total < 52) {
        total = 52;
    }
    return total > MAX_COLS ? MAX_COLS : total;
}

static struct box hscroll_thumb(void) {
    struct box t = hscroll_track();
    int total = scroll_cols_total();
    int shown = visible_cols();
    int w = (int)((long)t.w * shown / total);
    if (w < 24) {
        w = 24;
    }
    if (w > t.w) {
        w = t.w;
    }
    int x = t.x + (int)((long)(t.w - w) * sh()->left
                        / (total - shown > 0 ? total - shown : 1));
    if (x > t.x + t.w - w) {
        x = t.x + t.w - w;
    }
    return (struct box){x, t.y + 2, w, t.h - 4};
}

static struct box tab_box(int si) {
    int x = 8;
    for (int i = 0; i < si; i++) {
        int w = opos_text_width(sheets[i]->name, 2) + 24;
        if (w < 80) {
            w = 80;
        }
        x += w + 2;
    }
    int w = (editing == EDIT_TAB && edit_sheet_index == si)
        ? opos_text_width(edit_text, 2) + 40 : opos_text_width(sheets[si]->name, 2) + 24;
    if (w < 80) {
        w = 80;
    }
    return (struct box){x, tabs_y() + 2, w, TABS_H - 4};
}

static struct box plus_box(void) {
    struct box last = tab_box(nsheets - 1);
    return (struct box){last.x + last.w + 6, tabs_y() + 2, 30, TABS_H - 4};
}

static void draw_tabs(void) {
    int y = tabs_y();
    fill_box((struct box){0, y, view_width, TABS_H}, UI_BACK);
    for (int i = 0; i < nsheets; i++) {
        struct box b = tab_box(i);
        if (b.x > hscroll_track().x - 40) {
            break;
        }
        bool on = i == active;
        fill_box(b, on ? OPOS_RGB(255, 255, 255) : UI_BUTTON);
        const char *label = editing == EDIT_TAB && edit_sheet_index == i
            ? edit_text : sheets[i]->name;
        opos_draw_text(screen, b.x + 12, b.y + (b.h - 18) / 2, label,
                       on ? OPOS_RGB(26, 115, 232) : UI_TEXT, 2);
        if (editing == EDIT_TAB && edit_sheet_index == i) {
            int cx = b.x + 12 + opos_text_width(label, 2) * edit_caret
                / (edit_len > 0 ? edit_len : 1);
            if (edit_len == 0) {
                cx = b.x + 12;
            }
            opos_fill_rect(screen, cx, b.y + 4, 2, b.h - 8, SEL_BORDER);
        }
    }
    struct box p = plus_box();
    if (p.x < hscroll_track().x - 34) {
        fill_box(p, inside(p, mouse_x, mouse_y) ? UI_HOVER : UI_BUTTON);
        draw_label(p, "+", UI_TEXT);
    }
    fill_box(hscroll_track(), UI_PANEL);
    fill_box(hscroll_thumb(), UI_HOVER);
}

/* The status bar: a message, or the sum of the selected numbers. */
static void draw_status(void) {
    int y = view_height - STATUS_H;
    fill_box((struct box){0, y, view_width, STATUS_H}, UI_PANEL);
    if (message[0] != '\0') {
        opos_draw_text(screen, 10, y + 4, message,
                       message_warn ? UI_WARN : UI_TEXT, 2);
        return;
    }
    if (single_cell()) {
        return;
    }
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    struct val range = {0};
    range.kind = V_RANGE;
    range.sheet = active;
    range.r1 = r1;
    range.c1 = c1;
    range.r2 = r2;
    range.c2 = c2;
    struct agg g = aggregate(&range, 1, false);
    struct counter counted = {0, 1};
    each_value(&range, 1, false, count_visit, &counted);
    char line[160], sum[48], average[48];
    if (g.count > 0) {
        num_general(g.sum, 12, sum, sizeof(sum));
        num_general(g.sum / (double)g.count, 12, average, sizeof(average));
        snprintf(line, sizeof(line), "SUM %s   AVERAGE %s   COUNT %ld", sum,
                 average, counted.count);
    } else {
        snprintf(line, sizeof(line), "COUNT %ld", counted.count);
    }
    int w = opos_text_width(line, 2);
    opos_draw_text(screen, view_width - w - 12, y + 4, line, UI_TEXT, 2);
}

/* --- menus */

struct mitem {
    const char *label;
    int action;
};

enum action {
    A_GENERAL = 1, A_NUMBER, A_CURRENCY_DOLLAR, A_CURRENCY_EURO, A_PERCENT,
    A_DATE, A_TIME, A_DATE_TIME, A_TEXT, A_INSERT_ROWS, A_INSERT_COLS,
    A_INSERT_SHEET, A_DELETE_ROWS, A_DELETE_COLS, A_DELETE_SHEET, A_SORT_UP,
    A_SORT_DOWN, A_CUT, A_COPY, A_PASTE, A_CLEAR, A_CLEAR_FORMATS, A_RENAME,
    A_FIT, A_NO_COLOUR,
};

static const struct mitem format_items[] = {
    {"GENERAL", A_GENERAL}, {"NUMBER  1,234.50", A_NUMBER},
    {"CURRENCY  $1,234.50", A_CURRENCY_DOLLAR},
    {"CURRENCY  1,234.50 EUR", A_CURRENCY_EURO},
    {"PERCENT  12%", A_PERCENT}, {"DATE  2026-11-16", A_DATE},
    {"TIME  13:30:00", A_TIME}, {"DATE AND TIME", A_DATE_TIME},
    {"TEXT", A_TEXT},
};
static const struct mitem insert_items[] = {
    {"ROWS ABOVE", A_INSERT_ROWS}, {"COLUMNS LEFT", A_INSERT_COLS},
    {"SHEET", A_INSERT_SHEET},
};
static const struct mitem delete_items[] = {
    {"ROWS", A_DELETE_ROWS}, {"COLUMNS", A_DELETE_COLS},
    {"SHEET", A_DELETE_SHEET},
};
static const struct mitem sort_items[] = {
    {"A TO Z  (SMALLEST FIRST)", A_SORT_UP},
    {"Z TO A  (LARGEST FIRST)", A_SORT_DOWN},
};
static const struct mitem context_items[] = {
    {"CUT", A_CUT}, {"COPY", A_COPY}, {"PASTE", A_PASTE},
    {"INSERT ROWS ABOVE", A_INSERT_ROWS}, {"INSERT COLUMNS LEFT", A_INSERT_COLS},
    {"DELETE ROWS", A_DELETE_ROWS}, {"DELETE COLUMNS", A_DELETE_COLS},
    {"CLEAR CONTENTS", A_CLEAR}, {"CLEAR FORMATTING", A_CLEAR_FORMATS},
    {"SORT A TO Z", A_SORT_UP}, {"SORT Z TO A", A_SORT_DOWN},
    {"FIT COLUMN WIDTH", A_FIT},
};
static const struct mitem tab_items[] = {
    {"RENAME", A_RENAME}, {"DELETE SHEET", A_DELETE_SHEET},
    {"NEW SHEET", A_INSERT_SHEET},
};

static const struct mitem *menu_list(int *count) {
    switch (menu) {
    case MENU_FORMAT:
        *count = (int)(sizeof(format_items) / sizeof(format_items[0]));
        return format_items;
    case MENU_INSERT:
        *count = 3;
        return insert_items;
    case MENU_DELETE:
        *count = 3;
        return delete_items;
    case MENU_SORT:
        *count = 2;
        return sort_items;
    case MENU_CONTEXT:
        *count = (int)(sizeof(context_items) / sizeof(context_items[0]));
        return context_items;
    case MENU_TAB:
        *count = 3;
        return tab_items;
    default:
        *count = 0;
        return NULL;
    }
}

#define MENU_ITEM_H 30
#define SWATCH 28

static struct box menu_frame(void) {
    if (menu == MENU_COLOUR || menu == MENU_FILL) {
        struct box b = {menu_x, menu_y, 5 * (SWATCH + 6) + 10,
                        4 * (SWATCH + 6) + 10 + MENU_ITEM_H};
        return b;
    }
    int count;
    const struct mitem *items = menu_list(&count);
    int w = 120;
    for (int i = 0; i < count; i++) {
        int lw = opos_text_width(items[i].label, 2) + 24;
        if (lw > w) {
            w = lw;
        }
    }
    struct box b = {menu_x, menu_y, w, count * MENU_ITEM_H + 8};
    if (b.x + b.w > view_width) {
        b.x = view_width - b.w;
    }
    if (b.y + b.h > view_height) {
        b.y = view_height - b.h;
    }
    return b;
}

static struct box menu_item_box(int i) {
    struct box f = menu_frame();
    if (menu == MENU_COLOUR || menu == MENU_FILL) {
        if (i == 20) {
            return (struct box){f.x + 5, f.y + 5, f.w - 10, MENU_ITEM_H - 4};
        }
        return (struct box){f.x + 5 + (i % 5) * (SWATCH + 6),
                            f.y + 5 + MENU_ITEM_H + (i / 5) * (SWATCH + 6),
                            SWATCH, SWATCH};
    }
    return (struct box){f.x + 4, f.y + 4 + i * MENU_ITEM_H, f.w - 8,
                        MENU_ITEM_H};
}

static int menu_count(void) {
    if (menu == MENU_COLOUR || menu == MENU_FILL) {
        return 21;
    }
    int count;
    menu_list(&count);
    return count;
}

static void draw_menu(void) {
    if (menu == MENU_NONE) {
        return;
    }
    struct box f = menu_frame();
    fill_box((struct box){f.x - 1, f.y - 1, f.w + 2, f.h + 2}, UI_ACTIVE);
    fill_box(f, UI_PANEL);
    if (menu == MENU_COLOUR || menu == MENU_FILL) {
        struct box first = menu_item_box(20);
        if (inside(first, mouse_x, mouse_y)) {
            fill_box(first, UI_HOVER);
        }
        draw_label(first, menu == MENU_FILL ? "NO FILL" : "AUTOMATIC", UI_TEXT);
        for (int i = 0; i < 20; i++) {
            struct box b = menu_item_box(i);
            if (inside(b, mouse_x, mouse_y)) {
                fill_box((struct box){b.x - 2, b.y - 2, b.w + 4, b.h + 4},
                         UI_TEXT);
            }
            fill_box(b, palette[i]);
        }
        return;
    }
    int count;
    const struct mitem *items = menu_list(&count);
    for (int i = 0; i < count; i++) {
        struct box b = menu_item_box(i);
        if (inside(b, mouse_x, mouse_y)) {
            fill_box(b, UI_HOVER);
        }
        opos_draw_text(screen, b.x + 8, b.y + (b.h - 18) / 2, items[i].label,
                       UI_TEXT, 2);
    }
}

/* --- the prompt */

static struct box prompt_button(int index) {
    int width = 150, gap = 12;
    int count = pending == PENDING_DELETE_SHEET ? 2 : 3;
    int total = count * width + (count - 1) * gap;
    return (struct box){(view_width - total) / 2 + index * (width + gap),
                        view_height / 2 + 6, width, 32};
}

static void draw_prompt(void) {
    if (pending == PENDING_NONE) {
        return;
    }
    int width = 3 * 150 + 2 * 12 + 40;
    struct box panel = {(view_width - width) / 2, view_height / 2 - 50, width,
                        100};
    fill_box((struct box){panel.x - 2, panel.y - 2, panel.w + 4, panel.h + 4},
             UI_ACTIVE);
    fill_box(panel, UI_PANEL);
    char question[120];
    const char *name = pending == PENDING_DELETE_SHEET
        ? sheets[tab_menu_sheet]->name : file_name;
    char upper[41];
    size_t n = 0;
    for (; name[n] != '\0' && n < 40; n++) {
        upper[n] = (char)toupper((unsigned char)name[n]);
    }
    upper[n] = '\0';
    if (pending == PENDING_DELETE_SHEET) {
        snprintf(question, sizeof(question), "DELETE THE SHEET %s?", upper);
    } else {
        snprintf(question, sizeof(question), "SAVE CHANGES TO %s?", upper);
    }
    opos_draw_text(screen, panel.x + 20, panel.y + 16, question, UI_TEXT, 2);
    if (pending == PENDING_DELETE_SHEET) {
        static const char *const labels[2] = {"DELETE", "CANCEL"};
        for (int i = 0; i < 2; i++) {
            struct box b = prompt_button(i);
            fill_box(b, i == 0 ? UI_ACTIVE : inside(b, mouse_x, mouse_y)
                     ? UI_HOVER : UI_BUTTON);
            draw_label(b, labels[i], UI_TEXT);
        }
        return;
    }
    static const char *const labels[3] = {"SAVE", "DON'T SAVE", "CANCEL"};
    for (int i = 0; i < 3; i++) {
        struct box b = prompt_button(i);
        fill_box(b, i == 0 ? UI_ACTIVE : inside(b, mouse_x, mouse_y)
                 ? UI_HOVER : UI_BUTTON);
        draw_label(b, labels[i], UI_TEXT);
    }
}

static void draw(void) {
    if (canvas.pixels == NULL) {
        return;
    }
    draw_grid();
    fill_box(vscroll_track(), UI_BACK);
    fill_box(vscroll_thumb(), UI_BUTTON);
    fill_box((struct box){view_width - SCROLLBAR_W, grid_top(), SCROLLBAR_W,
                          head_h()}, UI_BACK);
    draw_toolbar();
    draw_formula_bar();
    draw_tabs();
    draw_status();
    draw_menu();
    draw_prompt();
    opos_canvas_present_area(&canvas, view_width, view_height);
}

/* ========================================================== files */

static void set_name_from_path(void) {
    const char *slash = strrchr(path, '/');
    snprintf(file_name, sizeof(file_name), "%s",
             slash != NULL ? slash + 1 : path);
}

static bool ends_with(const char *text, const char *suffix) {
    size_t a = strlen(text), b = strlen(suffix);
    if (b > a) {
        return false;
    }
    for (size_t i = 0; i < b; i++) {
        if (tolower((unsigned char)text[a - b + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

static void do_new(void) {
    new_workbook();
    path[0] = '\0';
    snprintf(file_name, sizeof(file_name), "Untitled");
    can_write = false;
    modified = true;
    set_modified(false);
    clip_free();
    recalc();
    say("", false);
}

static void open_path(const char *chosen) {
    size_t size;
    uint8_t *data = read_whole_file(chosen, &size);
    if (data == NULL) {
        say("COULD NOT READ THAT FILE", true);
        return;
    }
    if (size >= 8 && memcmp(data, "\xd0\xcf\x11\xe0", 4) == 0) {
        free(data);
        say("OLD EXCEL FILES (.XLS) DO NOT OPEN YET; SAVE AS .XLSX ELSEWHERE",
            true);
        return;
    }
    bool zip = size >= 4 && memcmp(data, "PK\3\4", 4) == 0;
    if (zip && !ends_with(chosen, ".xlsx") && !ends_with(chosen, ".xlsm")
        && !ends_with(chosen, ".xltx")) {
        free(data);
        say("SPREADSHEET OPENS EXCEL (.XLSX) AND CSV FILES", true);
        return;
    }
    if (!zip && (ends_with(chosen, ".ods") || ends_with(chosen, ".pdf")
                 || ends_with(chosen, ".docx"))) {
        free(data);
        say("SPREADSHEET OPENS EXCEL (.XLSX) AND CSV FILES", true);
        return;
    }
    workbook_clear();
    bool lossy = false;
    const char *problem = zip ? xlsx_load(data, size, &lossy)
                              : csv_load(data, size);
    free(data);
    if (nsheets == 0) {
        sheet_add("Sheet1");
    }
    undo_clear_all();
    clip_free();
    if (problem != NULL) {
        new_workbook();
        recalc();
        say(problem, true);
        return;
    }
    if (!zip) {
        /* The sheet takes the file's name, like other programs. */
        const char *slash = strrchr(chosen, '/');
        char name[SHEET_NAME_MAX];
        snprintf(name, sizeof(name), "%s", slash != NULL ? slash + 1 : chosen);
        char *dot = strrchr(name, '.');
        if (dot != NULL && dot != name) {
            *dot = '\0';
        }
        if (sheet_name_ok(name, 0)) {
            snprintf(sheets[0]->name, sizeof(sheets[0]->name), "%s", name);
        }
    }
    snprintf(path, sizeof(path), "%s", chosen);
    set_name_from_path();
    can_write = false; /* reading a file does not allow writing it */
    modified = true;
    set_modified(false);
    sheets_version++;
    recalc();
    say(lossy ? "OPENED; CHARTS, PICTURES, MERGED CELLS AND SOME OTHER PARTS "
                "ARE NOT SHOWN YET" : "OPENED", lossy);
}

static void do_open(void) {
    char chosen[PATH_MAX_LENGTH];
    long picked = opos_pick_file(chosen, sizeof(chosen));
    if (picked == -OPOS_ECANCELED) {
        return;
    }
    if (picked < 0) {
        say("COULD NOT CHOOSE A FILE", true);
        return;
    }
    open_path(chosen);
}

static bool write_workbook(const char *file) {
    bool csv = ends_with(file, ".csv") || ends_with(file, ".txt");
    bool ok = csv ? csv_save(file, active) : xlsx_save(file);
    if (!ok) {
        say("COULD NOT SAVE THE WORKBOOK", true);
    } else if (csv) {
        say(nsheets > 1 ? "SAVED THIS SHEET AS CSV; OTHER SHEETS, FORMULAS AND "
                          "FORMATTING ARE NOT KEPT"
                        : "SAVED AS CSV; FORMULAS AND FORMATTING ARE NOT KEPT",
            false);
    } else {
        say("SAVED", false);
    }
    return ok;
}

static bool do_save_as(void) {
    char suggested[PATH_MAX_LENGTH + 8];
    char start[PATH_MAX_LENGTH] = "Workbook.xlsx";
    struct opos_user_info user;
    user.size = sizeof(user);
    if (path[0] != '/' && opos_user_info(&user) == 0 && user.home[0] == '/'
        && strlen(user.home) + 26 < sizeof(start)) {
        snprintf(start, sizeof(start), "%s/Documents/Workbook.xlsx", user.home);
    }
    snprintf(suggested, sizeof(suggested), "%s", path[0] == '/' ? path : start);
    if (!ends_with(suggested, ".xlsx") && !ends_with(suggested, ".csv")) {
        char *dot = strrchr(suggested, '.');
        char *slash = strrchr(suggested, '/');
        if (dot != NULL && dot != suggested && (slash == NULL || dot > slash + 1)) {
            *dot = '\0';
        }
        size_t length = strlen(suggested);
        if (length + 6 < sizeof(suggested)) {
            memcpy(suggested + length, ".xlsx", 6);
        }
    }
    char chosen[PATH_MAX_LENGTH];
    if (pending != PENDING_NONE) {
        /* The picker waits: show the sheet without the prompt meanwhile. */
        enum pending keep = pending;
        pending = PENDING_NONE;
        draw();
        pending = keep;
    }
    long result = opos_save_file(suggested, chosen, sizeof(chosen));
    if (result == -OPOS_ECANCELED) {
        return false;
    }
    if (result == -OPOS_ENOSYS) {
        say("THIS OPOS CANNOT SAVE FOR PROGRAMS YET; PLEASE UPDATE", true);
        return false;
    }
    if (result < 0) {
        say("COULD NOT CHOOSE WHERE TO SAVE", true);
        return false;
    }
    if (!write_workbook(chosen)) {
        return false;
    }
    snprintf(path, sizeof(path), "%s", chosen);
    set_name_from_path();
    can_write = true;
    modified = true;
    set_modified(false);
    return true;
}

static bool do_save(void) {
    if (!can_write) {
        return do_save_as();
    }
    if (!write_workbook(path)) {
        return false;
    }
    set_modified(false);
    return true;
}

/* ========================================================== input */

static void finish_pending(bool go) {
    enum pending was = pending;
    pending = PENDING_NONE;
    if (!go) {
        return;
    }
    if (was == PENDING_CLOSE) {
        opos_exit(0);
    } else if (was == PENDING_NEW) {
        do_new();
    } else if (was == PENDING_OPEN) {
        draw(); /* the picker waits; do not leave the prompt showing */
        do_open();
    } else if (was == PENDING_DELETE_SHEET) {
        delete_sheet(tab_menu_sheet);
    }
}

static void prompt_answer(int answer) {
    if (pending == PENDING_DELETE_SHEET) {
        finish_pending(answer == 0);
        return;
    }
    if (answer == 0) {
        finish_pending(do_save());
    } else if (answer == 1) {
        finish_pending(true);
    } else {
        finish_pending(false);
    }
}

static void request(enum pending what) {
    if (editing != EDIT_NONE && !edit_commit()) {
        edit_cancel();
    }
    pending = what;
    if (!modified && what != PENDING_DELETE_SHEET) {
        finish_pending(true);
    }
}

static void set_zoom(int value) {
    if (value < 50) {
        value = 50;
    }
    if (value > 200) {
        value = 200;
    }
    zoom = value;
    scroll_into_view(sh()->cur_r, sh()->cur_c);
}

static void switch_sheet(int si) {
    if (si < 0 || si >= nsheets) {
        return;
    }
    if (editing == EDIT_CELL && !point_allowed()) {
        if (!edit_commit()) {
            return;
        }
    }
    if (editing == EDIT_TAB) {
        edit_commit();
    }
    active = si;
    pointing = false;
}

/* Σ: a SUM of the numbers above (or to the left). */
static void auto_sum(void) {
    struct sheet *s = sh();
    int r1, c1, r2, c2;
    sel_box(&r1, &c1, &r2, &c2);
    if (!single_cell()) {
        /* Sums under each selected column. */
        edit_begin();
        for (int c = c1; c <= c2; c++) {
            char a[24], b[24], text[64];
            cell_name(r1, c, a);
            cell_name(r2, c, b);
            snprintf(text, sizeof(text), "=SUM(%s:%s)", a, b);
            if (r2 + 1 < MAX_ROWS) {
                cell_type(active, r2 + 1, c, text);
            }
        }
        edit_end();
        return;
    }
    int r = s->cur_r, c = s->cur_c;
    int top = r, left = c;
    while (top > 0) {
        struct cell *x = cell_find(s, top - 1, c);
        if (x == NULL || x->kind != V_NUM) {
            break;
        }
        top--;
    }
    char text[64] = "=SUM()";
    if (top < r) {
        char a[24], b[24];
        cell_name(top, c, a);
        cell_name(r - 1, c, b);
        snprintf(text, sizeof(text), "=SUM(%s:%s)", a, b);
    } else {
        while (left > 0) {
            struct cell *x = cell_find(s, r, left - 1);
            if (x == NULL || x->kind != V_NUM) {
                break;
            }
            left--;
        }
        if (left < c) {
            char a[24], b[24];
            cell_name(r, left, a);
            cell_name(r, c - 1, b);
            snprintf(text, sizeof(text), "=SUM(%s:%s)", a, b);
        }
    }
    edit_start(true, text);
    if (strcmp(text, "=SUM()") == 0) {
        edit_caret = edit_len - 1;
    }
}

static void do_action(int action) {
    menu = MENU_NONE;
    switch (action) {
    case A_GENERAL: restyle(5, 0, ""); break;
    case A_NUMBER: restyle(5, 0, "#,##0.00"); break;
    case A_CURRENCY_DOLLAR: restyle(5, 0, "$#,##0.00"); break;
    case A_CURRENCY_EURO: restyle(5, 0, "#,##0.00 \"\xe2\x82\xac\""); break;
    case A_PERCENT: restyle(5, 0, "0%"); break;
    case A_DATE: restyle(5, 0, "yyyy-mm-dd"); break;
    case A_TIME: restyle(5, 0, "h:mm:ss"); break;
    case A_DATE_TIME: restyle(5, 0, "yyyy-mm-dd h:mm"); break;
    case A_TEXT: restyle(5, 0, "@"); break;
    case A_INSERT_ROWS: insert_or_delete(true, true); break;
    case A_INSERT_COLS: insert_or_delete(false, true); break;
    case A_DELETE_ROWS: insert_or_delete(true, false); break;
    case A_DELETE_COLS: insert_or_delete(false, false); break;
    case A_INSERT_SHEET: new_sheet(); break;
    case A_DELETE_SHEET:
        if (menu_x >= 0 && tab_menu_sheet >= nsheets) {
            tab_menu_sheet = active;
        }
        if (nsheets <= 1) {
            say("A WORKBOOK NEEDS AT LEAST ONE SHEET", true);
        } else {
            pending = PENDING_DELETE_SHEET;
        }
        break;
    case A_SORT_UP: sort_rows(false); break;
    case A_SORT_DOWN: sort_rows(true); break;
    case A_CUT: copy_selection(true); break;
    case A_COPY: copy_selection(false); break;
    case A_PASTE: paste(); break;
    case A_CLEAR: clear_selection(); break;
    case A_CLEAR_FORMATS: restyle(7, 0, NULL); break;
    case A_RENAME:
        editing = EDIT_TAB;
        edit_sheet_index = tab_menu_sheet;
        edit_set(sheets[tab_menu_sheet]->name);
        break;
    case A_FIT: {
        int r1, c1, r2, c2;
        sel_box(&r1, &c1, &r2, &c2);
        edit_begin();
        for (int c = c1; c <= c2 && c - c1 < 200; c++) {
            edit_width(active, c);
            set_width(c, fit_width(c));
        }
        edit_end();
        break;
    }
    default:
        break;
    }
}

static void press_button(int index) {
    if (editing != EDIT_NONE && index != B_SUM) {
        if (!edit_commit()) {
            return;
        }
    }
    enum menu was = menu;
    menu = MENU_NONE;
    struct box b = boxes[index];
    switch (index) {
    case B_NEW: request(PENDING_NEW); break;
    case B_OPEN: request(PENDING_OPEN); break;
    case B_SAVE: do_save(); break;
    case B_SAVE_AS: do_save_as(); break;
    case B_UNDO: step_replay(undo_stack, &undo_count, redo_stack, &redo_count); break;
    case B_REDO: step_replay(redo_stack, &redo_count, undo_stack, &undo_count); break;
    case B_CUT: copy_selection(true); break;
    case B_COPY: copy_selection(false); break;
    case B_PASTE: paste(); break;
    case B_ZOOM_OUT: set_zoom(zoom - 10); break;
    case B_ZOOM: set_zoom(100); break;
    case B_ZOOM_IN: set_zoom(zoom + 10); break;
    case B_BOLD: toggle_flag(F_BOLD); break;
    case B_ITALIC: toggle_flag(F_ITALIC); break;
    case B_UNDER: toggle_flag(F_UNDER); break;
    case B_STRIKE: toggle_flag(F_STRIKE); break;
    case B_COLOUR:
        if (mouse_x < b.x + 28) {
            restyle(2, last_colour, NULL);
        } else if (was != MENU_COLOUR) {
            menu = MENU_COLOUR;
            menu_x = b.x;
            menu_y = b.y + b.h + 2;
        }
        break;
    case B_FILL:
        if (mouse_x < b.x + 28) {
            restyle(3, last_fill, NULL);
        } else if (was != MENU_FILL) {
            menu = MENU_FILL;
            menu_x = b.x;
            menu_y = b.y + b.h + 2;
        }
        break;
    case B_LEFT: case B_CENTER: case B_RIGHT: {
        int align = index == B_LEFT ? AL_LEFT : index == B_CENTER ? AL_CENTER
                                                                  : AL_RIGHT;
        struct cell *c = cell_find(sh(), sh()->cur_r, sh()->cur_c);
        restyle(4, style_of(c)->align == align ? AL_AUTO : (uint32_t)align,
                NULL);
        break;
    }
    case B_FORMAT: case B_INSERT: case B_DELETE: case B_SORT: {
        enum menu which = index == B_FORMAT ? MENU_FORMAT : index == B_INSERT
            ? MENU_INSERT : index == B_DELETE ? MENU_DELETE : MENU_SORT;
        if (was != which) {
            menu = which;
            menu_x = b.x;
            menu_y = b.y + b.h + 2;
            tab_menu_sheet = active;
        }
        break;
    }
    case B_DEC_LESS: restyle(6, 0, NULL); break;
    case B_DEC_MORE: restyle(6, 2, NULL); break;
    case B_SUM:
        if (editing == EDIT_NONE) {
            auto_sum();
        }
        break;
    default:
        break;
    }
}

/* Mouse state. */
static bool selecting, thumb_drag_v, thumb_drag_h, head_drag_rows,
    head_drag_cols, point_drag;
static int resize_col = -1, resize_start_x, resize_start_w;
static int thumb_grab;
static long last_click_ms;
static int last_click_x, last_click_y, click_count;

static long now_ms(void) {
    struct opos_timespec t;
    opos_uptime(&t);
    return t.seconds * 1000 + t.nanoseconds / 1000000;
}

/* The column border near x in the header, or -1. */
static int border_at(int x) {
    struct sheet *s = sh();
    int cx = cells_x();
    for (int c = s->left; c < MAX_COLS && cx < view_width; c++) {
        cx += col_px(c);
        if (x >= cx - 4 && x <= cx + 3) {
            return c;
        }
    }
    return -1;
}

static int caret_from_x(const char *text, int length, int x0, int x, int size,
                        uint32_t flags) {
    struct opos_font *font = font_for(flags);
    if (font == NULL) {
        return length;
    }
    int best = 0;
    for (int i = 0; i <= length;) {
        int w = opos_font_text_width(font, size, text, (size_t)i);
        if (x0 + w <= x + 3) {
            best = i;
        }
        if (i == length) {
            break;
        }
        int next = i + 1;
        while (next < length && ((unsigned char)text[next] & 0xc0) == 0x80) {
            next++;
        }
        i = next;
    }
    return best;
}

static void mouse_down(const struct opos_event *event) {
    int x = event->x, y = event->y;
    bool right = (event->button & OPOS_BUTTON_RIGHT) != 0
        && !(event->button & OPOS_BUTTON_LEFT);
    long now = now_ms();
    if (now - last_click_ms < 450 && abs(x - last_click_x) < 5
        && abs(y - last_click_y) < 5) {
        click_count++;
    } else {
        click_count = 1;
    }
    last_click_ms = now;
    last_click_x = x;
    last_click_y = y;
    if (pending != PENDING_NONE) {
        int count = pending == PENDING_DELETE_SHEET ? 2 : 3;
        for (int i = 0; i < count; i++) {
            if (inside(prompt_button(i), x, y)) {
                prompt_answer(i);
            }
        }
        return;
    }
    if (menu != MENU_NONE) {
        struct box f = menu_frame();
        if (inside(f, x, y)) {
            for (int i = 0; i < menu_count(); i++) {
                if (!inside(menu_item_box(i), x, y)) {
                    continue;
                }
                if (menu == MENU_COLOUR || menu == MENU_FILL) {
                    bool text = menu == MENU_COLOUR;
                    uint32_t value = i == 20 ? (text ? 0x000000 : NO_FILL)
                                             : palette[i];
                    if (i < 20) {
                        if (text) {
                            last_colour = value;
                        } else {
                            last_fill = value;
                        }
                    }
                    menu = MENU_NONE;
                    restyle(text ? 2 : 3, value, NULL);
                    return;
                }
                int count;
                const struct mitem *items = menu_list(&count);
                do_action(items[i].action);
                return;
            }
            return;
        }
        enum menu was = menu;
        menu = MENU_NONE;
        for (int b = 0; b < B_COUNT; b++) {
            if (inside(boxes[b], x, y)
                && ((was == MENU_FORMAT && b == B_FORMAT)
                    || (was == MENU_INSERT && b == B_INSERT)
                    || (was == MENU_DELETE && b == B_DELETE)
                    || (was == MENU_SORT && b == B_SORT)
                    || (was == MENU_COLOUR && b == B_COLOUR && x >= boxes[b].x + 28)
                    || (was == MENU_FILL && b == B_FILL && x >= boxes[b].x + 28))) {
                return;
            }
        }
    }
    if (y < TOOLBAR_H) {
        for (int b = 0; b < B_COUNT; b++) {
            if (inside(boxes[b], x, y)) {
                press_button(b);
                return;
            }
        }
        return;
    }
    /* The formula bar. */
    if (y < grid_top()) {
        struct box ib = input_box();
        if (inside(ib, x, y)) {
            if (editing == EDIT_TAB) {
                edit_commit();
            }
            if (editing != EDIT_CELL) {
                edit_start(false, NULL);
            }
            enter_mode = false;
            edit_caret = caret_from_x(edit_text, edit_len, bar_text_x, x, 15, 0);
            pointing = false;
        }
        return;
    }
    /* Sheet tabs and the horizontal scrollbar. */
    if (y >= tabs_y() && y < view_height - STATUS_H) {
        if (inside(hscroll_track(), x, y)) {
            struct box t = hscroll_thumb();
            if (inside(t, x, y)) {
                thumb_drag_h = true;
                thumb_grab = x - t.x;
            } else {
                sh()->left += x < t.x ? -visible_cols() : visible_cols();
                if (sh()->left < 0) {
                    sh()->left = 0;
                }
            }
            return;
        }
        if (inside(plus_box(), x, y)) {
            if (editing != EDIT_NONE && !edit_commit()) {
                return;
            }
            new_sheet();
            return;
        }
        for (int i = 0; i < nsheets; i++) {
            if (!inside(tab_box(i), x, y)) {
                continue;
            }
            if (editing == EDIT_TAB && edit_sheet_index == i) {
                return;
            }
            switch_sheet(i);
            if (right) {
                tab_menu_sheet = i;
                menu = MENU_TAB;
                menu_x = x;
                menu_y = tabs_y() - 3 * MENU_ITEM_H - 10;
            } else if (click_count == 2 && editing == EDIT_NONE) {
                editing = EDIT_TAB;
                edit_sheet_index = i;
                edit_set(sheets[i]->name);
            }
            return;
        }
        if (editing == EDIT_TAB) {
            edit_commit();
        }
        return;
    }
    if (y >= view_height - STATUS_H) {
        return;
    }
    if (editing == EDIT_TAB) {
        edit_commit();
    }
    /* The vertical scrollbar. */
    if (x >= view_width - SCROLLBAR_W) {
        struct box t = vscroll_thumb();
        if (inside(t, x, y)) {
            thumb_drag_v = true;
            thumb_grab = y - t.y;
        } else if (y > cells_y()) {
            sh()->top += y < t.y ? -(visible_rows() - 1) : visible_rows() - 1;
            if (sh()->top < 0) {
                sh()->top = 0;
            }
        }
        return;
    }
    int hy = grid_top();
    /* Column headers: select, or resize at a border. */
    if (y < cells_y()) {
        if (x < cells_x()) {
            if (editing == EDIT_CELL && !edit_commit()) {
                return;
            }
            sh()->anc_r = 0;
            sh()->anc_c = 0;
            sh()->cur_r = MAX_ROWS - 1;
            sh()->cur_c = MAX_COLS - 1;
            return;
        }
        int border = border_at(x);
        if (border >= 0 && y >= hy) {
            if (editing == EDIT_CELL && !edit_commit()) {
                return;
            }
            if (click_count == 2) {
                edit_begin();
                edit_width(active, border);
                set_width(border, fit_width(border));
                edit_end();
                return;
            }
            edit_begin();
            edit_width(active, border);
            resize_col = border;
            resize_start_x = x;
            resize_start_w = col_width(sh(), border);
            return;
        }
        int r, c;
        cell_at(x, cells_y(), &r, &c);
        if (editing == EDIT_CELL && !edit_commit()) {
            return;
        }
        if (shift_down) {
            sh()->cur_c = c;
            sh()->cur_r = MAX_ROWS - 1;
            sh()->anc_r = 0;
        } else {
            sh()->anc_r = 0;
            sh()->anc_c = c;
            sh()->cur_r = MAX_ROWS - 1;
            sh()->cur_c = c;
        }
        head_drag_cols = !right;
        if (right) {
            menu = MENU_CONTEXT;
            menu_x = x;
            menu_y = y;
        }
        return;
    }
    /* Row headers. */
    if (x < cells_x()) {
        int r, c;
        cell_at(cells_x(), y, &r, &c);
        if (editing == EDIT_CELL && !edit_commit()) {
            return;
        }
        if (shift_down) {
            sh()->cur_r = r;
            sh()->cur_c = MAX_COLS - 1;
            sh()->anc_c = 0;
        } else {
            sh()->anc_r = r;
            sh()->anc_c = 0;
            sh()->cur_r = r;
            sh()->cur_c = MAX_COLS - 1;
        }
        head_drag_rows = !right;
        if (right) {
            menu = MENU_CONTEXT;
            menu_x = x;
            menu_y = y;
        }
        return;
    }
    /* The cells. */
    int r, c;
    cell_at(x, y, &r, &c);
    if (editing == EDIT_CELL) {
        if (active == edit_sheet_index && r == edit_r && c == edit_c) {
            /* A click in the cell being edited moves the caret. */
            struct opos_font *font = font_for(0);
            (void)font;
            edit_caret = caret_from_x(edit_text, edit_len, col_x(c) + px(3), x,
                                      text_size(), 0);
            enter_mode = false;
            pointing = false;
            return;
        }
        if (point_allowed()) {
            if (!(pointing && edit_caret == point_start + point_len)) {
                pointing = false;
            }
            point_r = point_r2 = r;
            point_c = point_c2 = c;
            point_write();
            point_drag = true;
            return;
        }
        if (!edit_commit()) {
            return;
        }
    }
    if (right) {
        int r1, c1, r2, c2;
        sel_box(&r1, &c1, &r2, &c2);
        if (!(r >= r1 && r <= r2 && c >= c1 && c <= c2)) {
            select_cell(r, c, false);
        }
        menu = MENU_CONTEXT;
        menu_x = x;
        menu_y = y;
        return;
    }
    select_cell(r, c, shift_down);
    if (click_count == 2) {
        edit_start(false, NULL);
        edit_caret = caret_from_x(edit_text, edit_len, col_x(c) + px(3), x,
                                  text_size(), 0);
        return;
    }
    selecting = true;
}

static void mouse_move(int x, int y) {
    if (resize_col >= 0) {
        int w = resize_start_w + (x - resize_start_x) * 100 / zoom;
        set_width(resize_col, w);
        return;
    }
    if (thumb_drag_v) {
        struct box t = vscroll_track();
        struct box thumb = vscroll_thumb();
        int total = scroll_rows_total();
        int range = t.h - thumb.h;
        if (range > 0) {
            long top = (long)(y - thumb_grab - t.y) * (total - visible_rows()) / range;
            sh()->top = top < 0 ? 0 : (int)top;
        }
        return;
    }
    if (thumb_drag_h) {
        struct box t = hscroll_track();
        struct box thumb = hscroll_thumb();
        int total = scroll_cols_total();
        int range = t.w - thumb.w;
        if (range > 0) {
            long left = (long)(x - thumb_grab - t.x) * (total - visible_cols()) / range;
            sh()->left = left < 0 ? 0 : left >= MAX_COLS ? MAX_COLS - 1 : (int)left;
        }
        return;
    }
    if (!selecting && !head_drag_rows && !head_drag_cols && !point_drag) {
        return;
    }
    /* Scroll when dragging past the edges. */
    struct sheet *s = sh();
    if (y < cells_y() && s->top > 0 && (selecting || head_drag_rows || point_drag)) {
        s->top--;
    } else if (y > cells_y() + cells_h()) {
        s->top++;
    }
    if (x < cells_x() && s->left > 0 && (selecting || head_drag_cols || point_drag)) {
        s->left--;
    } else if (x > cells_x() + cells_w()) {
        s->left++;
    }
    int r, c;
    cell_at(x < cells_x() ? cells_x() : x, y < cells_y() ? cells_y() : y, &r, &c);
    if (point_drag) {
        point_r2 = r;
        point_c2 = c;
        point_write();
        return;
    }
    if (head_drag_rows) {
        s->cur_r = r;
        return;
    }
    if (head_drag_cols) {
        s->cur_c = c;
        return;
    }
    s->cur_r = r;
    s->cur_c = c;
}

static void mouse_up(void) {
    if (resize_col >= 0) {
        resize_col = -1;
        edit_end();
    }
    selecting = thumb_drag_v = thumb_drag_h = head_drag_rows = head_drag_cols
        = point_drag = false;
}

/* Keys while editing a cell or a sheet name. */
static void edit_key(uint32_t k, bool ctrl, bool shift) {
    if (ctrl) {
        uint32_t c = k < 0x80 ? (uint32_t)tolower((int)k) : k;
        if (c == 'a') {
            edit_caret = edit_len;
        } else if (c == 'v') {
            static char buffer[4097];
            long length = opos_clipboard_get(buffer, sizeof(buffer) - 1);
            if (length > 0) {
                for (long i = 0; i < length; i++) {
                    if (buffer[i] == '\n' || buffer[i] == '\r' || buffer[i] == '\t') {
                        buffer[i] = ' ';
                    }
                }
                edit_insert(buffer, (int)length);
            }
            pointing = false;
        }
        return;
    }
    bool formula_point = editing == EDIT_CELL && enter_mode && point_allowed();
    switch (k) {
    case OPOS_KEY_ESCAPE:
        edit_cancel();
        say("", false);
        return;
    case '\n':
        if (edit_commit() && editing == EDIT_NONE) {
            move_after_enter(shift);
        }
        return;
    case '\t':
        if (editing == EDIT_TAB) {
            edit_commit();
            return;
        }
        if (edit_commit()) {
            move_after_tab(shift);
        }
        return;
    case OPOS_KEY_LEFT:
    case OPOS_KEY_RIGHT:
    case OPOS_KEY_UP:
    case OPOS_KEY_DOWN: {
        int dr = k == OPOS_KEY_UP ? -1 : k == OPOS_KEY_DOWN ? 1 : 0;
        int dc = k == OPOS_KEY_LEFT ? -1 : k == OPOS_KEY_RIGHT ? 1 : 0;
        if (formula_point) {
            /* Arrows point at cells for the formula. */
            if (!pointing || edit_caret != point_start + point_len) {
                pointing = false;
                point_r = edit_r;
                point_c = edit_c;
            }
            if (shift && pointing) {
                point_r2 += dr;
                point_c2 += dc;
            } else {
                point_r += dr;
                point_c += dc;
                point_r2 = point_r;
                point_c2 = point_c;
            }
            if (point_r < 0) point_r = 0;
            if (point_c < 0) point_c = 0;
            if (point_r2 < 0) point_r2 = 0;
            if (point_c2 < 0) point_c2 = 0;
            point_write();
            scroll_into_view(point_r2, point_c2);
            return;
        }
        if (editing == EDIT_CELL && enter_mode) {
            if (edit_commit()) {
                select_cell(sh()->cur_r + dr, sh()->cur_c + dc, false);
            }
            return;
        }
        if (dc < 0) {
            edit_caret = edit_prev(edit_caret);
        } else if (dc > 0) {
            edit_caret = edit_next(edit_caret);
        } else if (editing == EDIT_CELL) {
            if (edit_commit()) {
                select_cell(sh()->cur_r + dr, sh()->cur_c, false);
            }
        }
        pointing = false;
        return;
    }
    case OPOS_KEY_HOME:
        edit_caret = 0;
        pointing = false;
        return;
    case OPOS_KEY_END:
        edit_caret = edit_len;
        pointing = false;
        return;
    case '\b':
        edit_remove(edit_prev(edit_caret), edit_caret);
        pointing = false;
        return;
    case OPOS_KEY_DELETE:
        edit_remove(edit_caret, edit_next(edit_caret));
        pointing = false;
        return;
    default:
        if (k >= 0x20 && k < 0x7f) {
            char ch = (char)k;
            edit_insert(&ch, 1);
            pointing = false;
        }
        return;
    }
}

static void key(const struct opos_event *event) {
    uint32_t k = event->key;
    bool ctrl = (event->buttons & OPOS_MOD_CTRL) != 0;
    bool shift = (event->buttons & OPOS_MOD_SHIFT) != 0;
    if (pending != PENDING_NONE) {
        if (k == OPOS_KEY_ESCAPE) {
            finish_pending(false);
        } else if (k == '\n') {
            prompt_answer(0);
        }
        return;
    }
    if (menu != MENU_NONE) {
        if (k == OPOS_KEY_ESCAPE) {
            menu = MENU_NONE;
        }
        return;
    }
    if (editing != EDIT_NONE) {
        if (ctrl && (k == 's' || k == 'S')) {
            if (edit_commit()) {
                if (shift) {
                    do_save_as();
                } else {
                    do_save();
                }
            }
            return;
        }
        edit_key(k, ctrl, shift);
        return;
    }
    struct sheet *s = sh();
    if (ctrl) {
        uint32_t c = k < 0x80 ? (uint32_t)tolower((int)k) : k;
        switch (c) {
        case 'n': request(PENDING_NEW); return;
        case 'o': request(PENDING_OPEN); return;
        case 's': if (shift) { do_save_as(); } else { do_save(); } return;
        case 'z': step_replay(undo_stack, &undo_count, redo_stack, &redo_count); return;
        case 'y': step_replay(redo_stack, &redo_count, undo_stack, &undo_count); return;
        case 'c': copy_selection(false); return;
        case 'x': copy_selection(true); return;
        case 'v': paste(); return;
        case 'a':
            s->anc_r = 0;
            s->anc_c = 0;
            s->cur_r = MAX_ROWS - 1;
            s->cur_c = MAX_COLS - 1;
            return;
        case 'b': toggle_flag(F_BOLD); return;
        case 'i': toggle_flag(F_ITALIC); return;
        case 'u': toggle_flag(F_UNDER); return;
        case 'd': fill(true); return;
        case 'r': fill(false); return;
        case ';': {
            char today[16];
            struct when w;
            date_from_serial(floor(now_serial()), &w);
            snprintf(today, sizeof(today), "%04ld-%02d-%02d", w.year, w.month,
                     w.day);
            edit_begin();
            cell_type(active, s->cur_r, s->cur_c, today);
            edit_end();
            return;
        }
        case '=': case '+': set_zoom(zoom + 10); return;
        case '-': set_zoom(zoom - 10); return;
        case '0': set_zoom(100); return;
        case OPOS_KEY_PAGE_UP: switch_sheet(active - 1); return;
        case OPOS_KEY_PAGE_DOWN: switch_sheet(active + 1); return;
        case OPOS_KEY_UP: jump(-1, 0, shift); return;
        case OPOS_KEY_DOWN: jump(1, 0, shift); return;
        case OPOS_KEY_LEFT: jump(0, -1, shift); return;
        case OPOS_KEY_RIGHT: jump(0, 1, shift); return;
        case OPOS_KEY_HOME: select_cell(0, 0, shift); return;
        case OPOS_KEY_END: {
            int rows, cols;
            sheet_extent(s, &rows, &cols);
            select_cell(rows > 0 ? rows - 1 : 0, cols > 0 ? cols - 1 : 0, shift);
            return;
        }
        default:
            return;
        }
    }
    switch (k) {
    case OPOS_KEY_UP: select_cell(s->cur_r - 1, s->cur_c, shift); return;
    case OPOS_KEY_DOWN: select_cell(s->cur_r + 1, s->cur_c, shift); return;
    case OPOS_KEY_LEFT: select_cell(s->cur_r, s->cur_c - 1, shift); return;
    case OPOS_KEY_RIGHT: select_cell(s->cur_r, s->cur_c + 1, shift); return;
    case OPOS_KEY_PAGE_UP:
        s->top -= visible_rows() - 1;
        if (s->top < 0) {
            s->top = 0;
        }
        select_cell(s->cur_r - (visible_rows() - 1), s->cur_c, shift);
        return;
    case OPOS_KEY_PAGE_DOWN:
        s->top += visible_rows() - 1;
        select_cell(s->cur_r + (visible_rows() - 1), s->cur_c, shift);
        return;
    case OPOS_KEY_HOME: select_cell(s->cur_r, 0, shift); return;
    case OPOS_KEY_END: {
        int c = s->cur_c;
        while (c + 1 < MAX_COLS && cell_has_value(s->cur_r, c + 1)) {
            c++;
        }
        select_cell(s->cur_r, c, shift);
        return;
    }
    case '\n': {
        /* Inside a selection, Enter moves within it. */
        int r1, c1, r2, c2;
        sel_box(&r1, &c1, &r2, &c2);
        if (!single_cell() && r2 - r1 < 100000 && c2 - c1 < 1000) {
            int r = s->cur_r + (shift ? -1 : 1), c = s->cur_c;
            if (r > r2) {
                r = r1;
                c = c + 1 > c2 ? c1 : c + 1;
            } else if (r < r1) {
                r = r2;
                c = c - 1 < c1 ? c2 : c - 1;
            }
            s->cur_r = r;
            s->cur_c = c;
            int ar = s->anc_r, ac = s->anc_c;
            (void)ar;
            (void)ac;
            /* keep the selection: anchor and cursor are its corners */
            s->anc_r = r1 == r ? r2 : r1;
            s->anc_c = c1 == c ? c2 : c1;
            if (r1 == r2) {
                s->anc_r = r;
            }
            if (c1 == c2) {
                s->anc_c = c;
            }
            scroll_into_view(r, c);
            return;
        }
        move_after_enter(shift);
        return;
    }
    case '\t':
        move_after_tab(shift);
        return;
    case OPOS_KEY_DELETE:
        clear_selection();
        return;
    case '\b':
        edit_begin();
        clear_one(s->cur_r, s->cur_c, NULL);
        edit_end();
        edit_start(true, "");
        return;
    case OPOS_KEY_ESCAPE:
        if (clip != NULL) {
            clip_free();
        }
        say("", false);
        return;
    default:
        if (k >= 0x20 && k < 0x7f) {
            char text[2] = {(char)k, '\0'};
            edit_start(true, text);
        }
        return;
    }
}

static void wheel(const struct opos_event *event) {
    int notches = OPOS_WHEEL_NOTCHES(*event);
    if (ctrl_down) {
        set_zoom(zoom + 10 * notches);
        return;
    }
    if (menu != MENU_NONE || pending != PENDING_NONE) {
        return;
    }
    struct sheet *s = sh();
    if (shift_down) {
        s->left -= notches * 2;
        if (s->left < 0) {
            s->left = 0;
        }
        if (s->left >= MAX_COLS) {
            s->left = MAX_COLS - 1;
        }
    } else {
        s->top -= notches * 3;
        if (s->top < 0) {
            s->top = 0;
        }
        if (s->top >= MAX_ROWS) {
            s->top = MAX_ROWS - 1;
        }
    }
}

static void handle(const struct opos_event *event) {
    switch (event->type) {
    case OPOS_EVENT_RESIZE:
        view_width = (int)event->width;
        view_height = (int)event->height;
        if (canvas.pixels == NULL || canvas.width < view_width
            || canvas.height < view_height) {
            opos_canvas_destroy(&canvas);
            canvas = opos_canvas_create(view_width, view_height);
            if (canvas.pixels == NULL) {
                opos_print("spreadsheet: out of memory\n");
                opos_exit(1);
            }
        }
        layout_toolbar();
        break;
    case OPOS_EVENT_MOUSE_DOWN:
        mouse_x = event->x;
        mouse_y = event->y;
        if (message_warn || message[0] != '\0') {
            if (!(event->y >= tabs_y())) {
                say("", false);
            }
        }
        mouse_down(event);
        break;
    case OPOS_EVENT_MOUSE_MOVE:
        mouse_x = event->x;
        mouse_y = event->y;
        mouse_move(event->x, event->y);
        break;
    case OPOS_EVENT_MOUSE_UP:
        mouse_up();
        break;
    case OPOS_EVENT_WHEEL:
        wheel(event);
        break;
    case OPOS_EVENT_KEY:
        if (!(message_warn && editing != EDIT_NONE)) {
            if (event->key != 0 && editing == EDIT_NONE) {
                say("", false);
            }
        }
        key(event);
        break;
    case OPOS_EVENT_KEY_STATE: {
        bool down = event->button != 0;
        if (event->key == 0x2a || event->key == 0x36) {
            shift_down = down;
        } else if (event->key == 0x1d || event->key == 0x9d) {
            ctrl_down = down;
        } else if (event->key == 0x3c && down && pending == PENDING_NONE
                   && menu == MENU_NONE && editing == EDIT_NONE) {
            edit_start(false, NULL); /* F2 */
        }
        break;
    }
    case OPOS_EVENT_CLOSE:
        if (close_removes_window) {
            opos_exit(0);
        }
        mouse_up();
        menu = MENU_NONE;
        request(PENDING_CLOSE);
        break;
    default:
        break;
    }
}

/* ======================================================= headless */

/* "spreadsheet --convert IN OUT": opens IN and saves OUT (.xlsx or .csv)
   without a window. "spreadsheet --print IN": every cell's address,
   input and shown value. For tests; both run on Linux too. */
static int convert(const char *in, const char *out) {
    open_path(in);
    opos_print(message);
    opos_print("\n");
    if (message_warn && strncmp(message, "OPENED", 6) != 0) {
        return 1;
    }
    if (!write_workbook(out)) {
        opos_print(message);
        opos_print("\n");
        return 1;
    }
    return 0;
}

static int print_cells(const char *in) {
    open_path(in);
    if (message_warn && strncmp(message, "OPENED", 6) != 0) {
        opos_print(message);
        opos_print("\n");
        return 1;
    }
    for (int si = 0; si < nsheets; si++) {
        struct sheet *s = sheets[si];
        active = si;
        printf("[%s]\n", s->name);
        int rows, cols;
        sheet_extent(s, &rows, &cols);
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                struct cell *cell = cell_find(s, r, c);
                if (cell == NULL || cell->input == NULL) {
                    continue;
                }
                struct look look;
                cell_look(cell, 11, &look);
                char name[24];
                cell_name(r, c, name);
                printf("%s\t%s\t%s\n", name, cell->input, look.shown.text);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    edit_text = malloc(INPUT_MAX + 1);
    if (edit_text == NULL) {
        return 1;
    }
    edit_text[0] = '\0';
    new_workbook();
    if (argc == 4 && strcmp(argv[1], "--convert") == 0) {
        headless = true;
        return convert(argv[2], argv[3]);
    }
    if (argc == 3 && strcmp(argv[1], "--print") == 0) {
        headless = true;
        return print_cells(argv[2]);
    }
    if (opos_window_create("Spreadsheet", 1100, 800) < 0) {
        opos_print("spreadsheet: no window; run it from the desktop\n");
        return 1;
    }
    /* Older OPOS versions have no ASK_CLOSE: their close button removes
       the window before CLOSE arrives, so Spreadsheet cannot ask then. */
    if (opos_window_options(OPOS_WINDOW_KEY_STATE | OPOS_WINDOW_ASK_CLOSE)
        < 0) {
        opos_window_options(OPOS_WINDOW_KEY_STATE);
        close_removes_window = true;
    }
    if (font_for(0) == NULL) {
        say("COULD NOT LOAD THE FONTS IN /SHARE/FONTS", true);
    }
    update_title();
    if (argc > 1) {
        open_path(argv[1]);
    }
    recalc();
    struct opos_event event;
    for (;;) {
        bool dragging = selecting || head_drag_rows || head_drag_cols || point_drag;
        bool outside = mouse_y < cells_y() || mouse_y > cells_y() + cells_h()
            || mouse_x < cells_x() || mouse_x > cells_x() + cells_w();
        long timeout = dragging && outside ? 60 : -1;
        long got = opos_window_event(&event, timeout);
        if (got < 0) {
            continue;
        }
        if (got == 0) {
            if (dragging) {
                mouse_move(mouse_x, mouse_y); /* keep scrolling */
                draw();
            }
            continue;
        }
        handle(&event);
        int more = 0;
        while (more < 64 && opos_window_event(&event, 0) == 1) {
            handle(&event);
            more++;
        }
        draw();
    }
}
