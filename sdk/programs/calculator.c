/*
 * Calculator: the OPOS calculator.
 *
 * Type a whole calculation, like 12 × (3 + 4)², and see the answer while
 * typing; = keeps it in the history. Standard mode has the four
 * operations, percent, square root and squares; Scientific mode adds
 * trigonometry in degrees or radians, logarithms, powers, factorials,
 * π and e. Results show 15 significant digits, like a spreadsheet, so
 * 0.1 + 0.2 is 0.3.
 *
 * Keys: digits, . or , for the decimal point, + - * / ^ ( ) % !, letters
 * for functions (sin, sqrt, ln, ...), Enter or = for the result,
 * Backspace, Esc to clear, Left/Right/Home/End to move, Ctrl+C copies the
 * result, Ctrl+V pastes, F9 changes the sign, Ctrl+H shows the history.
 *
 * "calculator --eval EXPRESSION" prints the result without a window (for
 * tests; it runs on Linux too).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_font.h>
#include <opos_gfx.h>

/* Calculator uses only the number functions of sheet_num.h. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "sheet_num.h"
#pragma GCC diagnostic pop

OPOS_MANIFEST("Calculator", "1.0", OPOS_CAP_CLIPBOARD_READ);

#define PI 3.14159265358979323846
#define E_CONST 2.71828182845904523536

/* UTF-8 of the symbols the keypad types. */
#define S_TIMES "\xc3\x97"      /* × */
#define S_DIVIDE "\xc3\xb7"     /* ÷ */
#define S_MINUS "\xe2\x88\x92"  /* − */
#define S_ROOT "\xe2\x88\x9a"   /* √ */
#define S_PI "\xcf\x80"         /* π */
#define S_SQUARE "\xc2\xb2"     /* ² */
#define S_CUBE "\xc2\xb3"       /* ³ */
#define S_INVERSE "\xe2\x81\xbb\xc2\xb9" /* ⁻¹ */

/* ================================================== the maths */

static bool degrees = true;

/* atan for any x: range reduction to |x| <= tan 15°, then the series. */
static double calc_atan(double x) {
    if (x != x) {
        return x;
    }
    if (x < 0) {
        return -calc_atan(-x);
    }
    if (x > 1) {
        return PI / 2 - calc_atan(1 / x);
    }
    double offset = 0;
    if (x > 0.26794919243112270) { /* tan 15° */
        const double r3 = 1.73205080756887729;
        x = (x * r3 - 1) / (x + r3);
        offset = PI / 6;
    }
    double x2 = x * x, term = x, sum = x;
    for (int k = 3; k < 61; k += 2) {
        term *= -x2;
        sum += term / k;
    }
    return offset + sum;
}

static double calc_asin(double x) {
    if (x >= 1) {
        return PI / 2;
    }
    if (x <= -1) {
        return -PI / 2;
    }
    return calc_atan(x / sqrt(1 - x * x));
}

/* Answers that are 0 except for rounding (sin 180°) are 0. */
static double snap(double x) {
    return fabs(x) < 1e-13 ? 0 : x;
}

enum problem {
    P_NONE, P_SYNTAX, P_DIV0, P_DOMAIN, P_BIG, P_UNKNOWN, P_EMPTY,
};

static const char *const problem_text[] = {
    "", "CHECK THE CALCULATION", "CANNOT DIVIDE BY ZERO",
    "NOT DEFINED FOR THAT NUMBER", "TOO BIG", "UNKNOWN NAME", "",
};

struct parser {
    const char *s;
    size_t n, i;
    enum problem problem;
    double ans;
};

static void skip_spaces(struct parser *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t')) {
        p->i++;
    }
}

/* Is the text at the cursor this (ASCII or UTF-8) symbol? Eats it. */
static bool take(struct parser *p, const char *symbol) {
    skip_spaces(p);
    size_t length = strlen(symbol);
    if (p->i + length <= p->n && memcmp(p->s + p->i, symbol, length) == 0) {
        p->i += length;
        return true;
    }
    return false;
}

static bool peek(struct parser *p, const char *symbol) {
    size_t at = p->i;
    bool found = take(p, symbol);
    p->i = at;
    return found;
}

static bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static double parse_sum(struct parser *p);
static double parse_unary(struct parser *p);

/* Can a value start here (for 2π, 3(4), 2sin 30)? */
static bool starts_value(struct parser *p) {
    skip_spaces(p);
    if (p->i >= p->n) {
        return false;
    }
    char c = p->s[p->i];
    return is_digit(c) || c == '.' || c == '(' || is_letter(c)
        || peek(p, S_PI) || peek(p, S_ROOT);
}

static double fail(struct parser *p, enum problem problem) {
    if (p->problem == P_NONE) {
        p->problem = problem;
    }
    return 0;
}

static double factorial(struct parser *p, double x) {
    if (x < 0 || x != floor(x)) {
        return fail(p, P_DOMAIN);
    }
    if (x > 170) {
        return fail(p, P_BIG);
    }
    double r = 1;
    for (int k = 2; k <= (int)x; k++) {
        r *= k;
    }
    return r;
}

/* ( sum ) with the ) optional at the end. */
static double parse_group(struct parser *p) {
    double v = parse_sum(p);
    if (!take(p, ")")) {
        skip_spaces(p);
        if (p->i < p->n) {
            return fail(p, P_SYNTAX);
        }
    }
    return v;
}

static double parse_postfix(struct parser *p);

static double call(struct parser *p, const char *name, size_t length) {
    static const char *const names[] = {
        "sin", "cos", "tan", "asin", "acos", "atan", "sqrt", "cbrt", "ln",
        "log", "exp", "abs", "round", "floor", "ceil", "fact", "int",
    };
    int f = -1;
    for (int k = 0; k < (int)(sizeof(names) / sizeof(names[0])); k++) {
        if (strlen(names[k]) == length && fmt_word(name, length, 0, names[k])) {
            f = k;
        }
    }
    if (f < 0) {
        return fail(p, P_UNKNOWN);
    }
    double x = take(p, "(") ? parse_group(p) : parse_postfix(p);
    if (p->problem != P_NONE) {
        return 0;
    }
    double angle = degrees ? x * PI / 180 : x;
    double to_angle = degrees ? 180 / PI : 1;
    switch (f) {
    case 0: return snap(sin(angle));
    case 1: return snap(cos(angle));
    case 2: {
        double c = cos(angle);
        if (fabs(c) < 1e-13) {
            return fail(p, P_DOMAIN);
        }
        return snap(sin(angle) / c);
    }
    case 3:
    case 4:
        if (x < -1 || x > 1) {
            return fail(p, P_DOMAIN);
        }
        return (f == 3 ? calc_asin(x) : PI / 2 - calc_asin(x)) * to_angle;
    case 5: return calc_atan(x) * to_angle;
    case 6:
        if (x < 0) {
            return fail(p, P_DOMAIN);
        }
        return sqrt(x);
    case 7: {
        double r = pow(fabs(x), 1.0 / 3);
        double rounded = floor(r + 0.5);
        if (fabs(rounded * rounded * rounded - fabs(x)) < 1e-9 * fabs(x) + 1e-300) {
            r = rounded;
        }
        return x < 0 ? -r : r;
    }
    case 8:
    case 9:
        if (x <= 0) {
            return fail(p, P_DOMAIN);
        }
        return f == 8 ? log(x) : log10(x);
    case 10: return exp(x);
    case 11: return fabs(x);
    case 12: return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
    case 13: return floor(x);
    case 14: return ceil(x);
    case 15: return factorial(p, x);
    default: return x < 0 ? ceil(x) : floor(x);
    }
}

static double parse_primary(struct parser *p) {
    skip_spaces(p);
    if (p->i >= p->n) {
        return fail(p, P_SYNTAX);
    }
    if (take(p, "(")) {
        return parse_group(p);
    }
    if (take(p, S_PI)) {
        return PI;
    }
    if (take(p, S_ROOT)) {
        double x = peek(p, "-") || peek(p, S_MINUS) ? parse_unary(p)
                                                     : parse_postfix(p);
        if (x < 0) {
            return fail(p, P_DOMAIN);
        }
        return sqrt(x);
    }
    char c = p->s[p->i];
    if (is_digit(c) || c == '.') {
        /* A number; an e only counts as an exponent before digits. */
        size_t end = p->i;
        while (end < p->n && (is_digit(p->s[end]) || p->s[end] == '.')) {
            end++;
        }
        if (end < p->n && (p->s[end] == 'e' || p->s[end] == 'E')) {
            size_t k = end + 1;
            if (k < p->n && (p->s[k] == '+' || p->s[k] == '-')) {
                k++;
            }
            if (k < p->n && is_digit(p->s[k])) {
                end = k;
                while (end < p->n && is_digit(p->s[end])) {
                    end++;
                }
            }
        }
        double v;
        size_t used = num_scan(p->s + p->i, end - p->i, &v);
        if (used == 0 || used != end - p->i) {
            return fail(p, P_SYNTAX);
        }
        p->i = end;
        return v;
    }
    if (is_letter(c)) {
        size_t start = p->i;
        while (p->i < p->n && is_letter(p->s[p->i])) {
            p->i++;
        }
        const char *name = p->s + start;
        size_t length = p->i - start;
        if (length == 2 && fmt_word(name, 2, 0, "pi")) {
            return PI;
        }
        if (length == 1 && (name[0] == 'e' || name[0] == 'E')) {
            return E_CONST;
        }
        if (length == 3 && fmt_word(name, 3, 0, "ans")) {
            return p->ans;
        }
        return call(p, name, length);
    }
    return fail(p, P_SYNTAX);
}

/* value followed by !, %, ² and ³ */
static double parse_postfix(struct parser *p) {
    double v = parse_primary(p);
    for (;;) {
        if (take(p, "!")) {
            v = factorial(p, v);
        } else if (take(p, "%")) {
            v /= 100;
        } else if (take(p, S_SQUARE)) {
            v *= v;
        } else if (take(p, S_CUBE)) {
            v = v * v * v;
        } else {
            return v;
        }
    }
}

static double parse_power(struct parser *p) {
    double base = parse_postfix(p);
    if (take(p, "^")) {
        double power = parse_unary(p); /* right to left: 2^3^2 = 2^9 */
        if (base == 0 && power < 0) {
            return fail(p, P_DIV0);
        }
        if (base < 0 && power != floor(power)) {
            /* Odd roots of negative numbers: (-8)^(1/3) = -2. */
            double inverse = 1 / power;
            if (fabs(inverse - floor(inverse + 0.5)) < 1e-9
                && ((long)floor(inverse + 0.5)) % 2 != 0) {
                return -pow(-base, power);
            }
            return fail(p, P_DOMAIN);
        }
        return pow(base, power);
    }
    return base;
}

static double parse_unary(struct parser *p) {
    if (take(p, "-") || take(p, S_MINUS)) {
        return -parse_unary(p);
    }
    if (take(p, "+")) {
        return parse_unary(p);
    }
    return parse_power(p);
}

static double parse_product(struct parser *p) {
    double v = parse_unary(p);
    for (;;) {
        if (take(p, "*") || take(p, S_TIMES) || take(p, "x")) {
            v *= parse_unary(p);
        } else if (take(p, "/") || take(p, S_DIVIDE)) {
            double d = parse_unary(p);
            if (d == 0 && p->problem == P_NONE) {
                return fail(p, P_DIV0);
            }
            v /= d;
        } else if (starts_value(p)) {
            v *= parse_power(p); /* 2π, 3(4+1) */
        } else {
            return v;
        }
        if (p->problem != P_NONE) {
            return 0;
        }
    }
}

static double parse_sum(struct parser *p) {
    double v = parse_product(p);
    for (;;) {
        if (take(p, "+")) {
            v += parse_product(p);
        } else if (take(p, "-") || take(p, S_MINUS)) {
            v -= parse_product(p);
        } else {
            return v;
        }
        if (p->problem != P_NONE) {
            return 0;
        }
    }
}

/* Computes text; P_NONE and the value, or what is wrong. */
static enum problem evaluate(const char *text, double ans, double *out) {
    struct parser p = {text, strlen(text), 0, P_NONE, ans};
    skip_spaces(&p);
    if (p.i == p.n) {
        return P_EMPTY;
    }
    double v = parse_sum(&p);
    skip_spaces(&p);
    if (p.problem == P_NONE && p.i < p.n) {
        p.problem = take(&p, ")") ? P_SYNTAX : P_SYNTAX;
    }
    if (p.problem != P_NONE) {
        return p.problem;
    }
    if (v != v) {
        return P_DOMAIN;
    }
    if (!num_finite(v)) {
        return P_BIG;
    }
    *out = v == 0 ? 0 : v; /* no -0 */
    return P_NONE;
}

/* A result as shown: 15 significant digits, thousands separated. */
static void show_number(double v, bool group, char *out, size_t capacity) {
    char plain[48];
    num_general(v, 17, plain, sizeof(plain));
    if (!group || strchr(plain, 'E') != NULL) {
        snprintf(out, capacity, "%s", plain);
        return;
    }
    const char *digits = plain[0] == '-' ? plain + 1 : plain;
    size_t whole = strcspn(digits, ".");
    size_t used = 0;
    if (plain[0] == '-' && used + 1 < capacity) {
        out[used++] = '-';
    }
    for (size_t k = 0; k < whole && used + 2 < capacity; k++) {
        if (k > 0 && (whole - k) % 3 == 0) {
            out[used++] = ',';
        }
        out[used++] = digits[k];
    }
    snprintf(out + used, capacity - used, "%s", digits + whole);
}

/* ================================================== the state */

#define EXPR_MAX 512
#define HISTORY_MAX 100

static char expr[EXPR_MAX];
static int expr_len, caret;
static double ans;
static bool have_result;     /* = was pressed and nothing typed since */
static double result;
static char result_problem[64];

struct entry {
    char expr[EXPR_MAX];
    double value;
};
static struct entry history[HISTORY_MAX];
static int history_count;
static int history_scroll;

static bool scientific;
static bool show_history = true;
static char message[80];

static void insert_text(const char *text) {
    int length = (int)strlen(text);
    if (expr_len + length >= EXPR_MAX) {
        return;
    }
    memmove(expr + caret + length, expr + caret, (size_t)(expr_len - caret + 1));
    memcpy(expr + caret, text, (size_t)length);
    expr_len += length;
    caret += length;
}

static void set_expr(const char *text) {
    snprintf(expr, sizeof(expr), "%s", text);
    expr_len = (int)strlen(expr);
    caret = expr_len;
}

/* The full-precision text of a number, to keep calculating with. */
static void number_text(double v, char *out, size_t capacity) {
    num_full(v, out, capacity);
}

static bool is_operator_text(const char *text) {
    return strcmp(text, "+") == 0 || strcmp(text, S_MINUS) == 0
        || strcmp(text, S_TIMES) == 0 || strcmp(text, S_DIVIDE) == 0
        || strcmp(text, "^") == 0 || strcmp(text, "!") == 0
        || strcmp(text, "%") == 0 || strcmp(text, S_SQUARE) == 0
        || strcmp(text, S_CUBE) == 0;
}

/* Typing after a result: operators go on from it, anything else starts
   a new calculation. */
static void type_text(const char *text) {
    message[0] = '\0';
    if (have_result) {
        have_result = false;
        if (is_operator_text(text)) {
            char number[48];
            number_text(result, number, sizeof(number));
            set_expr(number);
        } else {
            set_expr("");
        }
    }
    result_problem[0] = '\0';
    insert_text(text);
}

static void backspace(void) {
    if (have_result) {
        have_result = false;
        return; /* the expression is still there to change */
    }
    if (caret == 0) {
        return;
    }
    int start = caret - 1;
    while (start > 0 && ((unsigned char)expr[start] & 0xc0) == 0x80) {
        start--;
    }
    memmove(expr + start, expr + caret, (size_t)(expr_len - caret + 1));
    expr_len -= caret - start;
    caret = start;
    result_problem[0] = '\0';
}

static void clear_all(void) {
    set_expr("");
    have_result = false;
    result_problem[0] = '\0';
    message[0] = '\0';
}

static void equals(void) {
    if (have_result) {
        return;
    }
    double v;
    enum problem problem = evaluate(expr, ans, &v);
    if (problem == P_EMPTY) {
        return;
    }
    if (problem != P_NONE) {
        snprintf(result_problem, sizeof(result_problem), "%s",
                 problem_text[problem]);
        return;
    }
    /* Close brackets left open, so the history reads right. */
    int depth = 0;
    for (int k = 0; k < expr_len; k++) {
        depth += expr[k] == '(' ? 1 : expr[k] == ')' ? -1 : 0;
    }
    while (depth-- > 0 && expr_len + 1 < EXPR_MAX) {
        expr[expr_len++] = ')';
        expr[expr_len] = '\0';
    }
    caret = expr_len;
    if (history_count == HISTORY_MAX) {
        memmove(history, history + 1, (HISTORY_MAX - 1) * sizeof(*history));
        history_count--;
    }
    snprintf(history[history_count].expr, EXPR_MAX, "%s", expr);
    history[history_count].value = v;
    history_count++;
    history_scroll = 0;
    result = v;
    ans = v;
    have_result = true;
}

/* ± on the last number: -x <-> x, a + b <-> a - b, or -(whole). */
static void negate(void) {
    if (have_result) {
        char number[48];
        number_text(-result, number, sizeof(number));
        have_result = false;
        set_expr(number);
        return;
    }
    int end = caret;
    int start = end;
    while (start > 0 && (is_digit(expr[start - 1]) || expr[start - 1] == '.')) {
        start--;
    }
    if (start == end) {
        /* Not after a number: the whole calculation. */
        char wrapped[EXPR_MAX];
        if (strncmp(expr, S_MINUS "(", 4) == 0 && expr[expr_len - 1] == ')') {
            snprintf(wrapped, sizeof(wrapped), "%.*s", expr_len - 5, expr + 4);
        } else if (expr_len + 5 < EXPR_MAX) {
            snprintf(wrapped, sizeof(wrapped), S_MINUS "(%s)", expr);
        } else {
            return;
        }
        set_expr(wrapped);
        return;
    }
    /* What is before the number? */
    int before = start;
    bool minus = before >= 3 && memcmp(expr + before - 3, S_MINUS, 3) == 0;
    bool hyphen = before >= 1 && expr[before - 1] == '-';
    bool plus = before >= 1 && expr[before - 1] == '+';
    int sign_at = minus ? before - 3 : before - 1;
    int sign_len = minus ? 3 : 1;
    if (minus || hyphen) {
        /* Binary minus becomes plus; unary minus goes. */
        bool binary = sign_at > 0;
        if (binary) {
            char c = expr[sign_at - 1];
            binary = is_digit(c) || c == '.' || c == ')' || c == '!'
                || c == '%' || (unsigned char)c == 0xb2 || (unsigned char)c == 0xb3
                || (unsigned char)c == 0x80; /* end of π */
        }
        memmove(expr + sign_at, expr + sign_at + sign_len,
                (size_t)(expr_len - sign_at - sign_len + 1));
        expr_len -= sign_len;
        caret -= sign_len;
        if (binary) {
            int keep = caret;
            caret = sign_at;
            insert_text("+");
            caret = keep + 1;
        }
    } else if (plus) {
        memmove(expr + before - 1, expr + before, (size_t)(expr_len - before + 1));
        expr_len--;
        caret--;
        int keep = caret;
        caret = before - 1;
        insert_text(S_MINUS);
        caret = keep + 3;
    } else {
        int keep = caret;
        caret = start;
        insert_text(S_MINUS);
        caret = keep + 3;
    }
}

/* 1/x and |x| work on the whole calculation (or the result). */
static void wrap(const char *before, const char *after) {
    char inner[EXPR_MAX];
    if (have_result) {
        number_text(result, inner, sizeof(inner));
        have_result = false;
    } else {
        snprintf(inner, sizeof(inner), "%s", expr);
    }
    if (inner[0] == '\0') {
        set_expr(before);
        return;
    }
    char wrapped[EXPR_MAX + 16];
    snprintf(wrapped, sizeof(wrapped), "%s%s%s", before, inner, after);
    if (strlen(wrapped) < EXPR_MAX) {
        set_expr(wrapped);
    }
    result_problem[0] = '\0';
}

static void copy_result(void) {
    char text[64];
    double v;
    if (have_result) {
        v = result;
    } else if (evaluate(expr, ans, &v) != P_NONE) {
        snprintf(message, sizeof(message), "NOTHING TO COPY YET");
        return;
    }
    num_general(v, 17, text, sizeof(text));
    opos_clipboard_set(text, strlen(text));
    snprintf(message, sizeof(message), "COPIED %s", text);
}

static void paste(void) {
    char text[EXPR_MAX];
    long length = opos_clipboard_get(text, sizeof(text) - 1);
    if (length == -OPOS_EACCES) {
        snprintf(message, sizeof(message),
                 "CALCULATOR MAY NOT READ THE CLIPBOARD");
        return;
    }
    if (length <= 0) {
        return;
    }
    text[length] = '\0';
    /* 1.234,5 or 1,234.5 or 12,5: keep one decimal point. */
    bool dot = strchr(text, '.') != NULL;
    char clean[EXPR_MAX];
    size_t used = 0;
    for (long k = 0; k < length && used + 1 < sizeof(clean); k++) {
        char c = text[k];
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            continue;
        }
        if (c == ',') {
            if (dot) {
                continue;
            }
            c = '.';
        }
        clean[used++] = c;
    }
    clean[used] = '\0';
    if (used > 0) {
        type_text(clean);
    }
}

/* ================================================== the window */

#define BAR_H 40
#define GAP 4

#define UI_BACK OPOS_RGB(30, 34, 46)
#define UI_PANEL OPOS_RGB(40, 46, 62)
#define UI_BUTTON OPOS_RGB(56, 64, 86)
#define UI_DIGIT OPOS_RGB(72, 82, 108)
#define UI_HOVER OPOS_RGB(92, 104, 136)
#define UI_ACTIVE OPOS_RGB(70, 110, 190)
#define UI_ACTIVE_HOVER OPOS_RGB(92, 132, 214)
#define UI_TEXT OPOS_RGB(230, 236, 248)
#define UI_DIM OPOS_RGB(140, 150, 175)
#define UI_WARN OPOS_RGB(245, 205, 110)

static struct opos_canvas canvas;
static int view_width, view_height;
static int mouse_x = -1, mouse_y = -1;
static int pressed = -1;
static struct opos_font *font;

struct box {
    int x, y, w, h;
};

static bool inside(struct box b, int x, int y) {
    return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

enum kind { K_TEXT, K_DIGIT, K_OP, K_EQUALS, K_CLEAR, K_BACK, K_NEGATE,
            K_INVERSE, K_ABS, K_FUNC };

struct key {
    const char *label;
    const char *text; /* what it types */
    enum kind kind;
};

static const struct key standard_keys[6][4] = {
    {{"%", "%", K_OP}, {S_ROOT, "sqrt(", K_FUNC},
     {"x" S_SQUARE, S_SQUARE, K_OP}, {"", NULL, K_BACK}},
    {{"C", NULL, K_CLEAR}, {"(", "(", K_TEXT}, {")", ")", K_TEXT},
     {S_DIVIDE, S_DIVIDE, K_OP}},
    {{"7", "7", K_DIGIT}, {"8", "8", K_DIGIT}, {"9", "9", K_DIGIT},
     {S_TIMES, S_TIMES, K_OP}},
    {{"4", "4", K_DIGIT}, {"5", "5", K_DIGIT}, {"6", "6", K_DIGIT},
     {S_MINUS, S_MINUS, K_OP}},
    {{"1", "1", K_DIGIT}, {"2", "2", K_DIGIT}, {"3", "3", K_DIGIT},
     {"+", "+", K_OP}},
    {{"\xc2\xb1", NULL, K_NEGATE}, {"0", "0", K_DIGIT}, {".", ".", K_DIGIT},
     {"=", NULL, K_EQUALS}},
};

static const struct key science_keys[6][3] = {
    {{"sin", "sin(", K_FUNC}, {"cos", "cos(", K_FUNC},
     {"tan", "tan(", K_FUNC}},
    {{"sin" S_INVERSE, "asin(", K_FUNC}, {"cos" S_INVERSE, "acos(", K_FUNC},
     {"tan" S_INVERSE, "atan(", K_FUNC}},
    {{"ln", "ln(", K_FUNC}, {"log", "log(", K_FUNC},
     {"x" S_CUBE, S_CUBE, K_OP}},
    {{"e^x", "e^", K_FUNC}, {"10^x", "10^", K_FUNC}, {"x^y", "^", K_OP}},
    {{S_PI, S_PI, K_TEXT}, {"e", "e", K_TEXT}, {"n!", "!", K_OP}},
    {{"1/x", NULL, K_INVERSE}, {"|x|", NULL, K_ABS},
     {"Ans", "Ans", K_TEXT}},
};

/* Keys are numbered row * 7 + column, scientific columns first. */
static int key_columns(void) {
    return scientific ? 7 : 4;
}

static const struct key *key_at(int index) {
    int row = index / 7, column = index % 7;
    if (scientific) {
        if (column < 3) {
            return &science_keys[row][column];
        }
        return &standard_keys[row][column - 3];
    }
    return column < 4 ? &standard_keys[row][column] : NULL;
}

static int history_w(void) {
    if (!show_history || view_width < 560) {
        return 0;
    }
    int w = view_width * 2 / 5;
    return w > 340 ? 340 : w;
}

static int main_w(void) {
    return view_width - history_w();
}

static int display_h(void) {
    int h = view_height * 2 / 9;
    return h < 96 ? 96 : h > 170 ? 170 : h;
}

static struct box key_box(int index) {
    int columns = key_columns();
    int row = index / 7, column = index % 7;
    int top = BAR_H + display_h() + GAP;
    int width = main_w() - 2 * GAP, height = view_height - top - GAP;
    int kw = (width - (columns - 1) * GAP) / columns;
    int kh = (height - 5 * GAP) / 6;
    return (struct box){GAP + column * (kw + GAP), top + row * (kh + GAP), kw, kh};
}

/* The bar: STANDARD, SCIENTIFIC, DEG/RAD and HISTORY. */
enum { BAR_STANDARD = 100, BAR_SCIENCE, BAR_ANGLE, BAR_HISTORY, BAR_CLEAR };

static struct box bar_box(int which) {
    switch (which) {
    case BAR_STANDARD: return (struct box){6, 6, 104, BAR_H - 12};
    case BAR_SCIENCE: return (struct box){114, 6, 122, BAR_H - 12};
    case BAR_ANGLE: return (struct box){246, 6, 52, BAR_H - 12};
    case BAR_HISTORY: return (struct box){view_width - 104, 6, 98, BAR_H - 12};
    default: {
        int w = history_w();
        return (struct box){view_width - w + 8, view_height - 40, w - 16, 32};
    }
    }
}

static void fill(struct box b, uint32_t colour) {
    opos_fill_rect(&canvas, b.x, b.y, b.w, b.h, colour);
}

static void label_bitmap(struct box b, const char *text, uint32_t colour) {
    int width = opos_text_width(text, 2);
    opos_draw_text(&canvas, b.x + (b.w - width) / 2 + 1,
                   b.y + (b.h - opos_char_height(2)) / 2, text, colour, 2);
}

/* TrueType text clipped to a box. */
static void draw_text_in(struct box clip, int x, int baseline, int size,
                         const char *text, uint32_t colour) {
    if (font == NULL) {
        opos_draw_text(&canvas, x, baseline - 14, text, colour, 2);
        return;
    }
    if (clip.x < 0) {
        clip.w += clip.x;
        clip.x = 0;
    }
    if (clip.x + clip.w > canvas.width) {
        clip.w = canvas.width - clip.x;
    }
    if (clip.w <= 0 || clip.h <= 0) {
        return;
    }
    struct opos_canvas v = {canvas.pixels + (size_t)clip.y * (size_t)canvas.stride
                                + (size_t)clip.x,
                            clip.w, clip.h, canvas.stride};
    opos_font_draw(font, &v, x - clip.x, baseline - clip.y, size, text,
                   strlen(text), colour);
}

static int text_w(int size, const char *text) {
    if (font == NULL) {
        return opos_text_width(text, 2);
    }
    return opos_font_text_width(font, size, text, strlen(text));
}

static void draw_backspace_icon(struct box b, uint32_t colour) {
    int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    int w = 26, h = 18;
    int left = cx - w / 2, top = cy - h / 2;
    /* the arrow-shaped outline */
    opos_fill_rect(&canvas, left + 8, top, w - 8, 2, colour);
    opos_fill_rect(&canvas, left + 8, top + h - 2, w - 8, 2, colour);
    opos_fill_rect(&canvas, left + w - 2, top, 2, h, colour);
    for (int k = 0; k <= 8; k++) {
        opos_fill_rect(&canvas, left + 8 - k, top + k, 2, 1, colour);
        opos_fill_rect(&canvas, left + 8 - k, top + h - 1 - k, 2, 1, colour);
    }
    /* the x */
    for (int k = 0; k < 8; k++) {
        opos_fill_rect(&canvas, left + 12 + k, top + 5 + k, 2, 1, colour);
        opos_fill_rect(&canvas, left + 19 - k, top + 5 + k, 2, 1, colour);
    }
}

/* A radical sign over an x. */
static void draw_root_icon(struct box b, uint32_t colour) {
    int size = b.h / 2 > 24 ? 22 : b.h / 2 - 2;
    int h = 20, left = b.x + b.w / 2 - 15, top = b.y + (b.h - h) / 2;
    /* the short stroke up-left, the long stroke down, then up */
    for (int k = 0; k < 4; k++) {
        opos_fill_rect(&canvas, left + k, top + 11 - k / 2, 2, 2, colour);
    }
    for (int k = 0; k < 9; k++) {
        opos_fill_rect(&canvas, left + 4 + k / 2, top + 10 + k, 2, 1, colour);
    }
    for (int k = 0; k < 20; k++) {
        opos_fill_rect(&canvas, left + 9 + k / 3, top + 19 - k, 2, 1, colour);
    }
    opos_fill_rect(&canvas, left + 15, top, 18, 2, colour);
    draw_text_in(b, left + 17, top + h - 1, size, "x", colour);
}

static void draw_keys(void) {
    int count = 6 * 7;
    for (int index = 0; index < count; index++) {
        const struct key *k = key_at(index);
        if (k == NULL || index % 7 >= key_columns()) {
            continue;
        }
        struct box b = key_box(index);
        bool hover = inside(b, mouse_x, mouse_y);
        uint32_t back = k->kind == K_EQUALS
            ? (hover ? UI_ACTIVE_HOVER : UI_ACTIVE)
            : hover ? UI_HOVER
            : k->kind == K_DIGIT ? UI_DIGIT : UI_BUTTON;
        if (index == pressed && hover) {
            back = UI_ACTIVE;
        }
        fill(b, back);
        if (k->kind == K_BACK) {
            draw_backspace_icon(b, UI_TEXT);
            continue;
        }
        if (strcmp(k->label, S_ROOT) == 0) {
            draw_root_icon(b, UI_TEXT); /* Noto Sans has no √ */
            continue;
        }
        int size = b.h / 2;
        if (size > 28) {
            size = 28;
        }
        if (k->kind == K_FUNC || (scientific && index % 7 < 3)) {
            size = size * 4 / 5;
        }
        while (size > 10 && text_w(size, k->label) > b.w - 8) {
            size--;
        }
        int w = text_w(size, k->label);
        int ascent = font != NULL ? opos_font_ascent(font, size) : 14;
        draw_text_in(b, b.x + (b.w - w) / 2, b.y + (b.h + ascent) / 2 - 2, size,
                     k->label, UI_TEXT);
    }
}

static void draw_display(void) {
    struct box d = {0, BAR_H, main_w(), display_h()};
    fill(d, UI_PANEL);
    int pad = 14;
    struct box inner = {d.x + pad, d.y, d.w - 2 * pad, d.h};
    /* The calculation, right-aligned; its end stays in view. */
    int small = 22;
    int line_y = d.y + 12 + small;
    int caret_x;
    {
        char before[EXPR_MAX];
        snprintf(before, sizeof(before), "%.*s", caret, expr);
        int total = text_w(small, expr);
        int x = inner.x + inner.w - total;
        if (x < inner.x) {
            /* Too long: keep the caret in view. */
            int caret_w = text_w(small, before);
            x = inner.x + inner.w - caret_w - 4;
            if (x > inner.x) {
                x = inner.x;
            }
        }
        draw_text_in((struct box){inner.x, d.y, inner.w, d.h}, x, line_y, small,
                     expr, have_result ? UI_DIM : UI_TEXT);
        caret_x = x + text_w(small, before);
        if (!have_result) {
            opos_fill_rect(&canvas, caret_x, line_y - small, 2, small + 4,
                           UI_ACTIVE_HOVER);
        }
    }
    /* The answer: big when it is a result, dim while typing. */
    char shown[80] = "";
    uint32_t colour = UI_TEXT;
    if (result_problem[0] != '\0') {
        snprintf(shown, sizeof(shown), "%s", result_problem);
        colour = UI_WARN;
    } else if (have_result) {
        show_number(result, true, shown, sizeof(shown));
    } else {
        double v;
        if (evaluate(expr, ans, &v) == P_NONE) {
            show_number(v, true, shown, sizeof(shown));
            colour = UI_DIM;
        } else if (expr_len == 0) {
            snprintf(shown, sizeof(shown), "0");
            colour = UI_DIM;
        }
    }
    int big = d.h - small - 40;
    if (big > 60) {
        big = 60;
    }
    if (result_problem[0] != '\0') {
        big = big > 26 ? 26 : big;
    }
    while (big > 14 && text_w(big, shown) > inner.w) {
        big--;
    }
    int x = inner.x + inner.w - text_w(big, shown);
    draw_text_in(d, x, d.y + d.h - 16, big, shown, colour);
}

static void draw_bar(void) {
    fill((struct box){0, 0, view_width, BAR_H}, UI_BACK);
    struct box b = bar_box(BAR_STANDARD);
    fill(b, !scientific ? UI_ACTIVE : inside(b, mouse_x, mouse_y) ? UI_HOVER
                                                               : UI_BUTTON);
    label_bitmap(b, "STANDARD", UI_TEXT);
    b = bar_box(BAR_SCIENCE);
    fill(b, scientific ? UI_ACTIVE : inside(b, mouse_x, mouse_y) ? UI_HOVER
                                                              : UI_BUTTON);
    label_bitmap(b, "SCIENTIFIC", UI_TEXT);
    if (scientific) {
        b = bar_box(BAR_ANGLE);
        fill(b, inside(b, mouse_x, mouse_y) ? UI_HOVER : UI_BUTTON);
        label_bitmap(b, degrees ? "DEG" : "RAD", UI_TEXT);
    }
    if (view_width >= 560 && bar_box(BAR_HISTORY).x > 310) {
        b = bar_box(BAR_HISTORY);
        fill(b, show_history ? UI_ACTIVE : inside(b, mouse_x, mouse_y)
                                               ? UI_HOVER : UI_BUTTON);
        label_bitmap(b, "HISTORY", UI_TEXT);
    }
}

#define HISTORY_ROW 58

static int history_rows(void) {
    return (view_height - BAR_H - 52) / HISTORY_ROW;
}

/* The history entry at a row from the top (newest first), or -1. */
static int history_at_row(int row) {
    int index = history_count - 1 - history_scroll - row;
    return index >= 0 && index < history_count ? index : -1;
}

static void draw_history(void) {
    int w = history_w();
    if (w == 0) {
        return;
    }
    struct box panel = {view_width - w, BAR_H, w, view_height - BAR_H};
    fill(panel, UI_BACK);
    opos_fill_rect(&canvas, panel.x, panel.y, 1, panel.h, UI_PANEL);
    if (history_count == 0) {
        label_bitmap((struct box){panel.x, panel.y + 20, w, 30},
                     "NO HISTORY YET", UI_DIM);
        return;
    }
    for (int row = 0; row < history_rows(); row++) {
        int index = history_at_row(row);
        if (index < 0) {
            break;
        }
        struct box r = {panel.x + 6, panel.y + 6 + row * HISTORY_ROW, w - 12,
                        HISTORY_ROW - 4};
        if (inside(r, mouse_x, mouse_y)) {
            fill(r, UI_PANEL);
        }
        const struct entry *e = &history[index];
        int tw = text_w(15, e->expr);
        draw_text_in(r, r.x + r.w - 8 - tw, r.y + 20, 15, e->expr, UI_DIM);
        char shown[80], line[84];
        show_number(e->value, true, shown, sizeof(shown));
        snprintf(line, sizeof(line), "= %s", shown);
        int size = 22;
        while (size > 12 && text_w(size, line) > r.w - 16) {
            size--;
        }
        draw_text_in(r, r.x + r.w - 8 - text_w(size, line), r.y + 46, size,
                     line, UI_TEXT);
    }
    struct box c = bar_box(BAR_CLEAR);
    fill(c, inside(c, mouse_x, mouse_y) ? UI_HOVER : UI_BUTTON);
    label_bitmap(c, "CLEAR HISTORY", UI_TEXT);
}

static void draw(void) {
    if (canvas.pixels == NULL) {
        return;
    }
    fill((struct box){0, 0, view_width, view_height}, UI_BACK);
    draw_bar();
    draw_display();
    draw_keys();
    draw_history();
    if (message[0] != '\0') {
        int w = opos_text_width(message, 2) + 20;
        struct box m = {(main_w() - w) / 2, BAR_H + display_h() - 34, w, 26};
        fill(m, UI_BUTTON);
        label_bitmap(m, message, UI_TEXT);
    }
    opos_canvas_present_area(&canvas, view_width, view_height);
}

/* ================================================== input */

static void press_key(const struct key *k) {
    switch (k->kind) {
    case K_EQUALS: equals(); break;
    case K_CLEAR: clear_all(); break;
    case K_BACK: backspace(); break;
    case K_NEGATE: negate(); break;
    case K_INVERSE: wrap("1" S_DIVIDE "(", ")"); break;
    case K_ABS: wrap("abs(", ")"); break;
    case K_FUNC:
        if (have_result && (strcmp(k->text, "e^") == 0
                            || strcmp(k->text, "10^") == 0)) {
            /* e^x of the result */
            char number[48], text[64];
            number_text(result, number, sizeof(number));
            snprintf(text, sizeof(text), "%s(%s)", k->text, number);
            have_result = false;
            set_expr(text);
            break;
        }
        type_text(k->text);
        break;
    default: type_text(k->text); break;
    }
}

static void mouse_down(int x, int y) {
    message[0] = '\0';
    if (inside(bar_box(BAR_STANDARD), x, y)) {
        scientific = false;
        return;
    }
    if (inside(bar_box(BAR_SCIENCE), x, y)) {
        scientific = true;
        return;
    }
    if (scientific && inside(bar_box(BAR_ANGLE), x, y)) {
        degrees = !degrees;
        return;
    }
    if (view_width >= 560 && inside(bar_box(BAR_HISTORY), x, y)) {
        show_history = !show_history;
        return;
    }
    int w = history_w();
    if (w > 0 && x >= view_width - w) {
        if (history_count > 0 && inside(bar_box(BAR_CLEAR), x, y)) {
            history_count = 0;
            history_scroll = 0;
            return;
        }
        int row = (y - BAR_H - 6) / HISTORY_ROW;
        int index = y > BAR_H + 6 && row < history_rows() ? history_at_row(row) : -1;
        if (index >= 0) {
            /* Reuse the answer. */
            char number[48];
            number_text(history[index].value, number, sizeof(number));
            if (have_result || expr_len == 0) {
                have_result = false;
                set_expr(number);
            } else {
                insert_text(number);
            }
            result_problem[0] = '\0';
        }
        return;
    }
    for (int index = 0; index < 6 * 7; index++) {
        const struct key *k = key_at(index);
        if (k != NULL && index % 7 < key_columns()
            && inside(key_box(index), x, y)) {
            pressed = index;
            press_key(k);
            return;
        }
    }
}

static void key_typed(uint32_t key, bool ctrl) {
    if (ctrl) {
        if (key == 'c' || key == 'C') {
            copy_result();
        } else if (key == 'v' || key == 'V') {
            paste();
        } else if (key == 'h' || key == 'H') {
            show_history = !show_history;
        }
        return;
    }
    message[0] = '\0';
    switch (key) {
    case '\n':
    case '=':
        equals();
        return;
    case '\b':
        backspace();
        return;
    case OPOS_KEY_ESCAPE:
        clear_all();
        return;
    case OPOS_KEY_DELETE:
        if (have_result || caret == expr_len) {
            clear_all();
        } else {
            int end = caret + 1;
            while (end < expr_len && ((unsigned char)expr[end] & 0xc0) == 0x80) {
                end++;
            }
            memmove(expr + caret, expr + end, (size_t)(expr_len - end + 1));
            expr_len -= end - caret;
        }
        return;
    case OPOS_KEY_LEFT:
        have_result = false;
        if (caret > 0) {
            caret--;
            while (caret > 0 && ((unsigned char)expr[caret] & 0xc0) == 0x80) {
                caret--;
            }
        }
        return;
    case OPOS_KEY_RIGHT:
        have_result = false;
        if (caret < expr_len) {
            caret++;
            while (caret < expr_len
                   && ((unsigned char)expr[caret] & 0xc0) == 0x80) {
                caret++;
            }
        }
        return;
    case OPOS_KEY_HOME:
        have_result = false;
        caret = 0;
        return;
    case OPOS_KEY_END:
        have_result = false;
        caret = expr_len;
        return;
    case '*': type_text(S_TIMES); return;
    case '/': type_text(S_DIVIDE); return;
    case '-': type_text(S_MINUS); return;
    case ',': type_text("."); return;
    default:
        break;
    }
    if (key >= 0x20 && key < 0x7f) {
        char text[2] = {(char)key, '\0'};
        if (is_letter((char)key)) {
            text[0] = (char)(key | 0x20); /* functions in lower case */
        }
        type_text(text);
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
                opos_print("calculator: out of memory\n");
                opos_exit(1);
            }
        }
        break;
    case OPOS_EVENT_MOUSE_DOWN:
        mouse_x = event->x;
        mouse_y = event->y;
        if (event->button & OPOS_BUTTON_LEFT) {
            mouse_down(event->x, event->y);
        }
        break;
    case OPOS_EVENT_MOUSE_MOVE:
        mouse_x = event->x;
        mouse_y = event->y;
        break;
    case OPOS_EVENT_MOUSE_UP:
        pressed = -1;
        break;
    case OPOS_EVENT_WHEEL:
        if (history_w() > 0 && mouse_x >= view_width - history_w()) {
            history_scroll -= OPOS_WHEEL_NOTCHES(*event);
            int most = history_count - history_rows();
            if (history_scroll > most) {
                history_scroll = most;
            }
            if (history_scroll < 0) {
                history_scroll = 0;
            }
        }
        break;
    case OPOS_EVENT_KEY:
        key_typed(event->key, (event->buttons & OPOS_MOD_CTRL) != 0);
        break;
    case OPOS_EVENT_KEY_STATE:
        if (event->key == 0x43 && event->button != 0) { /* F9 */
            message[0] = '\0';
            negate();
        }
        break;
    case OPOS_EVENT_CLOSE:
        opos_exit(0);
        break;
    default:
        break;
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc >= 3 && strcmp(argv[1], "--eval") == 0) {
        /* For tests: --eval [--rad] EXPRESSION... */
        int first = 2;
        if (strcmp(argv[2], "--rad") == 0) {
            degrees = false;
            first = 3;
        }
        for (int k = first; k < argc; k++) {
            double v;
            enum problem problem = evaluate(argv[k], 0, &v);
            char shown[80];
            if (problem == P_NONE) {
                show_number(v, true, shown, sizeof(shown));
            } else {
                snprintf(shown, sizeof(shown), "ERROR %s", problem_text[problem]);
            }
            printf("%s\t%s\n", argv[k], shown);
        }
        return 0;
    }
    if (opos_window_create("Calculator", 640, 600) < 0) {
        opos_print("calculator: no window; run it from the desktop\n");
        return 1;
    }
    opos_window_options(OPOS_WINDOW_KEY_STATE); /* for F9 */
    font = opos_font_load("/share/fonts/NotoSans-Regular.ttf");
    struct opos_event event;
    for (;;) {
        if (opos_window_event(&event, -1) <= 0) {
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
