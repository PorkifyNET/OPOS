/*
 * Spreadsheet: formulas.
 *
 * A formula is tokenized, parsed into a small tree (struct formula) and
 * evaluated. References are compiled to absolute positions, so a cell's
 * compiled formula is thrown away whenever its text or the sheets change.
 * The same tokenizer rewrites formula text: when cells are pasted or
 * filled (relative references move), rows or columns are inserted or
 * deleted, and sheets are renamed or deleted.
 *
 * Needs, from spreadsheet.c: struct cell, struct sheet, sheets[],
 * nsheets, cell_find(), sheet_find(), sheet_extent(), error_names[] and
 * the V_ and E_ values.
 */
#ifndef SHEET_FORMULA_H
#define SHEET_FORMULA_H

/* ------------------------------------------------------------ tokens */

enum tok_type {
    T_END, T_NUM, T_STR, T_BOOL, T_ERR, T_REF, T_FUNC, T_NAME, T_OP,
    T_LP, T_RP, T_COMMA, T_BAD,
};

struct refpart {
    int row, col;
    bool abs_row, abs_col, has_row, has_col;
};

struct tok {
    int type;
    size_t start, end;      /* the whole token in the text */
    double num;
    char op;                /* + - * / ^ & = < > l(<=) g(>=) n(<>) % */
    int err;
    /* T_REF */
    bool has_sheet, range;
    size_t sheet_start, sheet_end; /* the name, without quotes */
    bool sheet_quoted;
    struct refpart p1, p2;
    /* T_FUNC and T_NAME: the name (without _xlfn.) */
    size_t name_start, name_end;
};

static bool ident_char(unsigned char c) {
    return isalnum(c) || c == '_' || c == '.' || c >= 0x80;
}

static int col_from_letters(const char *s, size_t n) {
    if (n == 0 || n > 3) {
        return -1;
    }
    int col = 0;
    for (size_t i = 0; i < n; i++) {
        col = col * 26 + (toupper((unsigned char)s[i]) - 'A' + 1);
    }
    return col - 1 < MAX_COLS ? col - 1 : -1;
}

static void col_name(int col, char *out) {
    char rev[4];
    int n = 0;
    col++;
    while (col > 0 && n < 3) {
        int rem = (col - 1) % 26;
        rev[n++] = (char)('A' + rem);
        col = (col - 1) / 26;
    }
    for (int i = 0; i < n; i++) {
        out[i] = rev[n - 1 - i];
    }
    out[n] = '\0';
}

/* One side of a reference: $A$1, A1, $A (with cols), 1 (with rows). */
static size_t scan_refpart(const char *s, size_t n, size_t i, int want,
                           struct refpart *p) {
    /* want: 0 = cell, 1 = column only, 2 = row only */
    memset(p, 0, sizeof(*p));
    size_t k = i;
    if (want != 2) {
        if (k < n && s[k] == '$') {
            p->abs_col = true;
            k++;
        }
        size_t letters = k;
        while (k < n && isalpha((unsigned char)s[k]) && k - letters < 4) {
            k++;
        }
        p->col = col_from_letters(s + letters, k - letters);
        if (p->col < 0) {
            return 0;
        }
        p->has_col = true;
        if (want == 1) {
            return k < n && isalnum((unsigned char)s[k]) ? 0 : k - i;
        }
    }
    if (k < n && s[k] == '$') {
        p->abs_row = true;
        k++;
    }
    size_t digits = k;
    long row = 0;
    while (k < n && isdigit((unsigned char)s[k]) && k - digits < 8) {
        row = row * 10 + (s[k] - '0');
        k++;
    }
    if (k == digits || row < 1 || row > MAX_ROWS) {
        return 0;
    }
    p->row = (int)row - 1;
    p->has_row = true;
    if (k < n && (ident_char((unsigned char)s[k]) || s[k] == '(')) {
        return 0;
    }
    return k - i;
}

/* A reference (with an optional range) at i; returns its length. */
static size_t scan_ref(const char *s, size_t n, size_t i, struct tok *t) {
    for (int want = 0; want < 3; want++) {
        struct refpart a, b;
        size_t la = scan_refpart(s, n, i, want, &a);
        if (la == 0) {
            continue;
        }
        size_t k = i + la;
        if (k < n && s[k] == ':') {
            size_t lb = scan_refpart(s, n, k + 1, want, &b);
            if (lb > 0) {
                t->p1 = a;
                t->p2 = b;
                t->range = true;
                return k + 1 + lb - i;
            }
        }
        if (want != 0) {
            continue; /* A or 1 alone is not a reference */
        }
        t->p1 = a;
        t->range = false;
        return la;
    }
    return 0;
}

static const char *const error_literals[] = {
    "#NULL!", "#DIV/0!", "#VALUE!", "#REF!", "#NAME?", "#NUM!", "#N/A",
};

/* The next token from i. */
static void next_tok(const char *s, size_t n, size_t *pos, struct tok *t) {
    memset(t, 0, sizeof(*t));
    size_t i = *pos;
    while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r'
                     || s[i] == '\t')) {
        i++;
    }
    t->start = i;
    if (i >= n) {
        t->type = T_END;
        t->end = *pos = i;
        return;
    }
    char c = s[i];
    /* Strings. */
    if (c == '"') {
        size_t k = i + 1;
        for (;;) {
            if (k >= n) {
                t->type = T_BAD;
                t->end = *pos = n;
                return;
            }
            if (s[k] == '"') {
                if (k + 1 < n && s[k + 1] == '"') {
                    k += 2;
                    continue;
                }
                break;
            }
            k++;
        }
        t->type = T_STR;
        t->end = *pos = k + 1;
        return;
    }
    /* Errors. */
    if (c == '#') {
        for (int e = 0; e < 7; e++) {
            size_t length = strlen(error_literals[e]);
            if (i + length <= n
                && strncmp(s + i, error_literals[e], length) == 0) {
                t->type = T_ERR;
                t->err = e + 1;
                t->end = *pos = i + length;
                return;
            }
        }
        t->type = T_BAD;
        t->end = *pos = i + 1;
        return;
    }
    /* Sheet prefixes: 'My sheet'!A1 or Sheet1!A1. */
    size_t ref_at = i;
    if (c == '\'') {
        size_t k = i + 1;
        for (;;) {
            if (k >= n) {
                t->type = T_BAD;
                t->end = *pos = n;
                return;
            }
            if (s[k] == '\'') {
                if (k + 1 < n && s[k + 1] == '\'') {
                    k += 2;
                    continue;
                }
                break;
            }
            k++;
        }
        if (k + 1 >= n || s[k + 1] != '!') {
            t->type = T_BAD;
            t->end = *pos = k + 1;
            return;
        }
        t->has_sheet = true;
        t->sheet_quoted = true;
        t->sheet_start = i + 1;
        t->sheet_end = k;
        ref_at = k + 2;
    } else if (ident_char((unsigned char)c) && c != '.') {
        size_t k = i;
        while (k < n && ident_char((unsigned char)s[k])) {
            k++;
        }
        if (k < n && s[k] == '!') {
            t->has_sheet = true;
            t->sheet_start = i;
            t->sheet_end = k;
            ref_at = k + 1;
        }
    }
    if (t->has_sheet) {
        size_t length = scan_ref(s, n, ref_at, t);
        if (length == 0) {
            /* Sheet1!#REF! keeps the error. */
            if (ref_at + 5 <= n && strncmp(s + ref_at, "#REF!", 5) == 0) {
                t->type = T_ERR;
                t->err = E_REF;
                t->end = *pos = ref_at + 5;
                return;
            }
            t->type = T_BAD;
            t->end = *pos = ref_at;
            return;
        }
        t->type = T_REF;
        t->end = *pos = ref_at + length;
        return;
    }
    /* Numbers (and row ranges like 1:3). */
    if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n
                                      && isdigit((unsigned char)s[i + 1]))) {
        size_t length = scan_ref(s, n, i, t);
        if (length > 0) {
            t->type = T_REF;
            t->end = *pos = i + length;
            return;
        }
        double value;
        size_t used = num_scan(s + i, n - i, &value);
        t->type = T_NUM;
        t->num = value;
        t->end = *pos = i + (used > 0 ? used : 1);
        return;
    }
    if (c == '$' || isalpha((unsigned char)c) || c == '_' || c == '\\'
        || (unsigned char)c >= 0x80) {
        size_t k = i;
        while (k < n && (ident_char((unsigned char)s[k]) || s[k] == '\\')) {
            k++;
        }
        size_t after = k;
        while (after < n && s[after] == ' ') {
            after++;
        }
        if (c != '$' && after < n && s[after] == '(') {
            t->type = T_FUNC;
            t->name_start = i;
            t->name_end = k;
            /* _xlfn.CONCAT and similar: the name is after the dot. */
            static const char *const prefixes[] = {"_xlfn._xlws.", "_xlfn.",
                                                   "_xlws."};
            for (size_t p = 0; p < 3; p++) {
                size_t length = strlen(prefixes[p]);
                if (k - i > length && strncmp(s + i, prefixes[p], length) == 0) {
                    t->name_start = i + length;
                    break;
                }
            }
            t->end = *pos = k;
            return;
        }
        size_t length = scan_ref(s, n, i, t);
        if (length > 0) {
            t->type = T_REF;
            t->end = *pos = i + length;
            return;
        }
        if (k - i == 4 && fmt_word(s, n, i, "true")) {
            t->type = T_BOOL;
            t->num = 1;
        } else if (k - i == 5 && fmt_word(s, n, i, "false")) {
            t->type = T_BOOL;
            t->num = 0;
        } else {
            t->type = k > i ? T_NAME : T_BAD;
        }
        t->name_start = i;
        t->name_end = k;
        t->end = *pos = k > i ? k : i + 1;
        return;
    }
    t->end = i + 1;
    switch (c) {
    case '(': t->type = T_LP; break;
    case ')': t->type = T_RP; break;
    case ',': case ';': t->type = T_COMMA; break;
    case '+': case '-': case '*': case '/': case '^': case '&': case '=':
    case '%':
        t->type = T_OP;
        t->op = c;
        break;
    case '<':
        t->type = T_OP;
        t->op = '<';
        if (i + 1 < n && s[i + 1] == '=') {
            t->op = 'l';
            t->end++;
        } else if (i + 1 < n && s[i + 1] == '>') {
            t->op = 'n';
            t->end++;
        }
        break;
    case '>':
        t->type = T_OP;
        t->op = '>';
        if (i + 1 < n && s[i + 1] == '=') {
            t->op = 'g';
            t->end++;
        }
        break;
    default:
        t->type = T_BAD;
        break;
    }
    *pos = t->end;
}

/* ----------------------------------------------------------- values */

enum { V_RANGE = 10 };

struct val {
    uint8_t kind, err;
    double num;
    char *str;                 /* V_STR, owned */
    int sheet, r1, c1, r2, c2; /* V_RANGE */
};

static struct val v_num(double x) {
    struct val v = {0};
    if (!num_finite(x)) {
        v.kind = V_ERR;
        v.err = E_NUM;
        return v;
    }
    v.kind = V_NUM;
    v.num = x;
    return v;
}

static struct val v_err(int e) {
    struct val v = {0};
    v.kind = V_ERR;
    v.err = (uint8_t)e;
    return v;
}

static struct val v_bool(bool b) {
    struct val v = {0};
    v.kind = V_BOOL;
    v.num = b ? 1 : 0;
    return v;
}

static struct val v_str_n(const char *s, size_t n) {
    struct val v = {0};
    v.kind = V_STR;
    v.str = malloc(n + 1);
    if (v.str == NULL) {
        return v_err(E_NUM);
    }
    memcpy(v.str, s, n);
    v.str[n] = '\0';
    return v;
}

static struct val v_str(const char *s) {
    return v_str_n(s, strlen(s));
}

/* Takes ownership of a malloc'd string. */
static struct val v_str_own(char *s) {
    struct val v = {0};
    if (s == NULL) {
        return v_err(E_NUM);
    }
    v.kind = V_STR;
    v.str = s;
    return v;
}

static void v_free(struct val *v) {
    if (v->kind == V_STR) {
        free(v->str);
    }
    v->str = NULL;
    v->kind = V_EMPTY;
}

/* --------------------------------------------------------- formulas */

enum node_type {
    N_NUM, N_STR, N_BOOL, N_ERR, N_REF, N_RANGE, N_NEG, N_PERCENT, N_BIN,
    N_FUNC, N_MISSING,
};

struct node {
    uint8_t type;
    char op;
    int16_t func;
    int sheet;          /* -1 = the formula's own sheet */
    int a, b;           /* N_NEG, N_PERCENT, N_BIN: children; N_FUNC:
                           first argument in kids[], argument count */
    int r1, c1, r2, c2; /* N_REF (r1, c1) and N_RANGE */
    double num;
    char *str;
};

struct formula {
    struct node *nodes;
    int count, capacity;
    int *kids;
    int nkids, kids_capacity;
    int root;
    unsigned compiled_for; /* sheets_version when compiled */
    bool unknown;          /* has functions or names Spreadsheet lacks */
};

static unsigned sheets_version = 1; /* changes when sheets change */

static void formula_free(struct formula *f) {
    if (f == NULL) {
        return;
    }
    for (int i = 0; i < f->count; i++) {
        free(f->nodes[i].str);
    }
    free(f->nodes);
    free(f->kids);
    free(f);
}

struct ctx; /* evaluation */
typedef struct val (*fn_t)(struct val *args, int n, struct ctx *x);

struct function {
    const char *name;
    int min, max;
    fn_t fn;
    bool modern; /* saved as _xlfn.NAME */
};

static const struct function functions[];
static const int function_count;

static int function_find(const char *s, size_t n) {
    for (int i = 0; i < function_count; i++) {
        if (strlen(functions[i].name) == n
            && fmt_word(s, n, 0, functions[i].name)) {
            return i;
        }
    }
    return -1;
}

struct parser {
    const char *s;
    size_t n, pos;
    struct tok t;  /* the current token */
    struct formula *f;
    int own_sheet, row, col;
    bool failed;
    int depth;
};

static int p_node(struct parser *p, int type) {
    struct formula *f = p->f;
    if (f->count == f->capacity) {
        int capacity = f->capacity < 16 ? 16 : f->capacity * 2;
        struct node *grown = realloc(f->nodes, (size_t)capacity * sizeof(*grown));
        if (grown == NULL) {
            p->failed = true;
            return -1;
        }
        f->nodes = grown;
        f->capacity = capacity;
    }
    struct node *node = &f->nodes[f->count];
    memset(node, 0, sizeof(*node));
    node->type = (uint8_t)type;
    node->sheet = -1;
    return f->count++;
}

static void p_advance(struct parser *p) {
    next_tok(p->s, p->n, &p->pos, &p->t);
}

static int p_expr(struct parser *p);

/* The sheet a reference token names: -1 for none, -2 if unknown. */
static int tok_sheet(const char *s, const struct tok *t) {
    if (!t->has_sheet) {
        return -1;
    }
    char name[96];
    size_t used = 0;
    for (size_t i = t->sheet_start; i < t->sheet_end && used + 1 < sizeof(name);
         i++) {
        name[used++] = s[i];
        if (t->sheet_quoted && s[i] == '\'' && i + 1 < t->sheet_end) {
            i++; /* '' is one quote */
        }
    }
    name[used] = '\0';
    int index = sheet_find(name);
    return index >= 0 ? index : -2;
}

static int p_primary(struct parser *p) {
    struct tok t = p->t;
    if (++p->depth > 200) {
        p->failed = true;
        return -1;
    }
    int node = -1;
    switch (t.type) {
    case T_NUM:
        node = p_node(p, N_NUM);
        if (node >= 0) {
            p->f->nodes[node].num = t.num;
        }
        p_advance(p);
        break;
    case T_BOOL:
        node = p_node(p, N_BOOL);
        if (node >= 0) {
            p->f->nodes[node].num = t.num;
        }
        p_advance(p);
        break;
    case T_ERR:
        node = p_node(p, N_ERR);
        if (node >= 0) {
            p->f->nodes[node].num = t.err;
        }
        p_advance(p);
        break;
    case T_STR: {
        node = p_node(p, N_STR);
        if (node < 0) {
            break;
        }
        size_t length = t.end - t.start - 2;
        char *text = malloc(length + 1);
        if (text == NULL) {
            p->failed = true;
            break;
        }
        size_t used = 0;
        for (size_t i = t.start + 1; i + 1 < t.end; i++) {
            text[used++] = p->s[i];
            if (p->s[i] == '"') {
                i++;
            }
        }
        text[used] = '\0';
        p->f->nodes[node].str = text;
        p_advance(p);
        break;
    }
    case T_REF: {
        int sheet = tok_sheet(p->s, &t);
        if (sheet == -2) {
            node = p_node(p, N_ERR);
            if (node >= 0) {
                p->f->nodes[node].num = E_REF;
            }
            p_advance(p);
            break;
        }
        node = p_node(p, t.range ? N_RANGE : N_REF);
        if (node < 0) {
            break;
        }
        struct node *x = &p->f->nodes[node];
        x->sheet = sheet;
        x->r1 = t.p1.has_row ? t.p1.row : 0;
        x->c1 = t.p1.has_col ? t.p1.col : 0;
        if (t.range) {
            x->r2 = t.p2.has_row ? t.p2.row : MAX_ROWS - 1;
            x->c2 = t.p2.has_col ? t.p2.col : MAX_COLS - 1;
            if (x->r1 > x->r2) {
                int swap = x->r1;
                x->r1 = x->r2;
                x->r2 = swap;
            }
            if (x->c1 > x->c2) {
                int swap = x->c1;
                x->c1 = x->c2;
                x->c2 = swap;
            }
        } else {
            x->r2 = x->r1;
            x->c2 = x->c1;
        }
        p_advance(p);
        break;
    }
    case T_FUNC: {
        int func = function_find(p->s + t.name_start, t.name_end - t.name_start);
        p_advance(p); /* the name */
        p_advance(p); /* ( */
        int args[64];
        int count = 0;
        if (p->t.type == T_RP) {
            p_advance(p);
        } else {
            for (;;) {
                int arg;
                if (p->t.type == T_COMMA || p->t.type == T_RP) {
                    arg = p_node(p, N_MISSING);
                } else {
                    arg = p_expr(p);
                }
                if (arg < 0 || count == 64) {
                    p->failed = true;
                    return -1;
                }
                args[count++] = arg;
                if (p->t.type == T_COMMA) {
                    p_advance(p);
                    continue;
                }
                if (p->t.type == T_RP) {
                    p_advance(p);
                    break;
                }
                p->failed = true;
                return -1;
            }
        }
        if (func < 0) {
            p->f->unknown = true;
            node = p_node(p, N_ERR);
            if (node >= 0) {
                p->f->nodes[node].num = E_NAME;
            }
            break;
        }
        if (count < functions[func].min || count > functions[func].max) {
            p->failed = true;
            return -1;
        }
        node = p_node(p, N_FUNC);
        if (node < 0) {
            break;
        }
        struct formula *f = p->f;
        if (f->nkids + count > f->kids_capacity) {
            int capacity = f->kids_capacity < 16 ? 16 : f->kids_capacity;
            while (capacity < f->nkids + count) {
                capacity *= 2;
            }
            int *grown = realloc(f->kids, (size_t)capacity * sizeof(int));
            if (grown == NULL) {
                p->failed = true;
                return -1;
            }
            f->kids = grown;
            f->kids_capacity = capacity;
        }
        memcpy(f->kids + f->nkids, args, (size_t)count * sizeof(int));
        f->nodes[node].func = (int16_t)func;
        f->nodes[node].a = f->nkids;
        f->nodes[node].b = count;
        f->nkids += count;
        break;
    }
    case T_NAME:
        p->f->unknown = true;
        node = p_node(p, N_ERR);
        if (node >= 0) {
            p->f->nodes[node].num = E_NAME;
        }
        p_advance(p);
        break;
    case T_LP:
        p_advance(p);
        node = p_expr(p);
        if (p->t.type != T_RP) {
            p->failed = true;
            return -1;
        }
        p_advance(p);
        break;
    default:
        p->failed = true;
        return -1;
    }
    p->depth--;
    return node;
}

static int p_unary(struct parser *p) {
    if (p->t.type == T_OP && (p->t.op == '-' || p->t.op == '+')) {
        char op = p->t.op;
        p_advance(p);
        int inner = p_unary(p);
        if (inner < 0 || op == '+') {
            return inner;
        }
        int node = p_node(p, N_NEG);
        if (node >= 0) {
            p->f->nodes[node].a = inner;
        }
        return node;
    }
    int node = p_primary(p);
    while (node >= 0 && p->t.type == T_OP && p->t.op == '%') {
        p_advance(p);
        int outer = p_node(p, N_PERCENT);
        if (outer < 0) {
            return -1;
        }
        p->f->nodes[outer].a = node;
        node = outer;
    }
    return node;
}

static int p_binary(struct parser *p, int level);

static int p_bin_node(struct parser *p, char op, int left, int right) {
    if (left < 0 || right < 0) {
        return -1;
    }
    int node = p_node(p, N_BIN);
    if (node >= 0) {
        p->f->nodes[node].op = op;
        p->f->nodes[node].a = left;
        p->f->nodes[node].b = right;
    }
    return node;
}

/* Levels: 0 comparison, 1 &, 2 + -, 3 * /, 4 ^. */
static bool op_at_level(char op, int level) {
    switch (level) {
    case 0: return op == '=' || op == '<' || op == '>' || op == 'l'
                   || op == 'g' || op == 'n';
    case 1: return op == '&';
    case 2: return op == '+' || op == '-';
    case 3: return op == '*' || op == '/';
    default: return op == '^';
    }
}

static int p_binary(struct parser *p, int level) {
    if (level > 4) {
        return p_unary(p);
    }
    int left = p_binary(p, level + 1);
    while (left >= 0 && p->t.type == T_OP && op_at_level(p->t.op, level)) {
        char op = p->t.op;
        p_advance(p);
        int right = p_binary(p, level + 1);
        left = p_bin_node(p, op, left, right);
    }
    return left;
}

static int p_expr(struct parser *p) {
    if (++p->depth > 200) {
        p->failed = true;
        return -1;
    }
    int node = p_binary(p, 0);
    p->depth--;
    return node;
}

/* Compiles a formula (the text after '='); NULL if it is not valid. */
static struct formula *formula_compile(const char *text, int own_sheet) {
    struct formula *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        return NULL;
    }
    struct parser p = {0};
    p.s = text;
    p.n = strlen(text);
    p.f = f;
    p.own_sheet = own_sheet;
    p_advance(&p);
    f->root = p_expr(&p);
    if (p.failed || f->root < 0 || p.t.type != T_END) {
        formula_free(f);
        return NULL;
    }
    f->compiled_for = sheets_version;
    return f;
}

/* Is this text a valid formula (after '=')? */
static bool formula_valid(const char *text) {
    struct formula *f = formula_compile(text, 0);
    formula_free(f);
    return f != NULL;
}

/* -------------------------------------------------------- evaluation */

struct ctx {
    int sheet, row, col; /* the cell being computed */
    struct formula *f;
};

static unsigned calc_gen = 1;  /* changes with every recalculation */
static int calc_depth;         /* nested cell computations */
static unsigned long deep_events;
#define CALC_DEPTH_MAX 160

static struct val eval(struct ctx *x, int index);
static void cell_compute(int sheet, struct cell *c);

/* A cell's value (computing it if needed), as a light view. */
struct cv {
    uint8_t kind, err;
    double num;
    const char *text;
};

static struct cv cell_view(int sheet, int row, int col) {
    struct cv v = {V_EMPTY, 0, 0, ""};
    if (sheet < 0 || sheet >= nsheets) {
        v.kind = V_ERR;
        v.err = E_REF;
        return v;
    }
    struct cell *c = cell_find(sheets[sheet], row, col);
    if (c == NULL) {
        return v;
    }
    if (c->formula && c->gen != calc_gen) {
        cell_compute(sheet, c);
        /* compute may not finish (too deep); the value is not ready */
        if (c->gen != calc_gen && !c->busy) {
            v.kind = V_ERR;
            v.err = E_DEEP;
            deep_events++;
            return v;
        }
    }
    if (c->busy) {
        v.kind = V_ERR;
        v.err = E_CYCLE;
        return v;
    }
    v.kind = c->kind;
    v.err = c->err;
    v.num = c->num;
    v.text = c->kind == V_STR && c->text != NULL ? c->text : "";
    return v;
}

static struct val val_from_view(struct cv v) {
    switch (v.kind) {
    case V_NUM: return v_num(v.num);
    case V_BOOL: return v_bool(v.num != 0);
    case V_STR: return v_str(v.text);
    case V_ERR: return v_err(v.err);
    default: {
        struct val e = {0};
        return e;
    }
    }
}

/* A 1x1 range becomes its cell's value; larger ranges are #VALUE!. */
static struct val scalar(struct ctx *x, struct val v) {
    if (v.kind != V_RANGE) {
        return v;
    }
    if (v.r1 == v.r2 && v.c1 == v.c2) {
        return val_from_view(cell_view(v.sheet, v.r1, v.c1));
    }
    /* Implicit intersection with the formula's row or column. */
    if (v.c1 == v.c2 && x->row >= v.r1 && x->row <= v.r2
        && v.sheet == x->sheet) {
        return val_from_view(cell_view(v.sheet, x->row, v.c1));
    }
    if (v.r1 == v.r2 && x->col >= v.c1 && x->col <= v.c2
        && v.sheet == x->sheet) {
        return val_from_view(cell_view(v.sheet, v.r1, x->col));
    }
    return v_err(E_VALUE);
}

/* Number from a scalar: text that reads as a number counts. */
static bool to_number(const struct val *v, double *out, int *err) {
    switch (v->kind) {
    case V_NUM: case V_BOOL:
        *out = v->num;
        return true;
    case V_EMPTY:
        *out = 0;
        return true;
    case V_STR: {
        char format[32];
        if (parse_typed_number(v->str, out, format, sizeof(format))) {
            return true;
        }
        *err = E_VALUE;
        return false;
    }
    case V_ERR:
        *err = v->err;
        return false;
    default:
        *err = E_VALUE;
        return false;
    }
}

/* Text from a scalar (malloc'd). */
static char *to_text(const struct val *v) {
    char buffer[48];
    switch (v->kind) {
    case V_NUM:
        num_full(v->num, buffer, sizeof(buffer));
        return strdup(buffer);
    case V_BOOL:
        return strdup(v->num != 0 ? "TRUE" : "FALSE");
    case V_STR:
        return strdup(v->str);
    default:
        return strdup("");
    }
}

static bool to_bool(const struct val *v, bool *out, int *err) {
    switch (v->kind) {
    case V_NUM: case V_BOOL:
        *out = v->num != 0;
        return true;
    case V_EMPTY:
        *out = false;
        return true;
    case V_STR:
        if (fmt_word(v->str, strlen(v->str), 0, "true") && strlen(v->str) == 4) {
            *out = true;
            return true;
        }
        if (fmt_word(v->str, strlen(v->str), 0, "false") && strlen(v->str) == 5) {
            *out = false;
            return true;
        }
        *err = E_VALUE;
        return false;
    case V_ERR:
        *err = v->err;
        return false;
    default:
        *err = E_VALUE;
        return false;
    }
}

static int text_compare(const char *a, const char *b) {
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || ca == 0) {
            return ca - cb;
        }
    }
}

/* Orders two scalars: numbers < text < booleans; empty is 0 or "". */
static int compare_values(const struct val *a, const struct val *b) {
    int ka = a->kind, kb = b->kind;
    if (ka == V_EMPTY) {
        ka = kb == V_STR ? V_STR : kb == V_BOOL ? V_BOOL : V_NUM;
    }
    if (kb == V_EMPTY) {
        kb = ka == V_STR ? V_STR : ka == V_BOOL ? V_BOOL : V_NUM;
    }
    int ra = ka == V_NUM ? 0 : ka == V_STR ? 1 : 2;
    int rb = kb == V_NUM ? 0 : kb == V_STR ? 1 : 2;
    if (ra != rb) {
        return ra - rb;
    }
    if (ka == V_STR) {
        return text_compare(a->kind == V_STR ? a->str : "",
                            b->kind == V_STR ? b->str : "");
    }
    double x = a->kind == V_EMPTY ? 0 : a->num;
    double y = b->kind == V_EMPTY ? 0 : b->num;
    return x < y ? -1 : x > y ? 1 : 0;
}

static struct val eval_scalar(struct ctx *x, int index) {
    return scalar(x, eval(x, index));
}

static struct val eval_binary(struct ctx *x, const struct node *node) {
    struct val a = eval_scalar(x, node->a);
    struct val b = eval_scalar(x, node->b);
    struct val r;
    if (a.kind == V_ERR) {
        v_free(&b);
        return a;
    }
    if (b.kind == V_ERR) {
        v_free(&a);
        return b;
    }
    char op = node->op;
    if (op == '&') {
        char *ta = to_text(&a), *tb = to_text(&b);
        if (ta == NULL || tb == NULL) {
            free(ta);
            free(tb);
            r = v_err(E_NUM);
        } else {
            size_t la = strlen(ta), lb = strlen(tb);
            char *joined = malloc(la + lb + 1);
            if (joined != NULL) {
                memcpy(joined, ta, la);
                memcpy(joined + la, tb, lb + 1);
            }
            r = v_str_own(joined);
            free(ta);
            free(tb);
        }
    } else if (op == '=' || op == '<' || op == '>' || op == 'l' || op == 'g'
               || op == 'n') {
        int c = compare_values(&a, &b);
        bool result = op == '=' ? c == 0 : op == 'n' ? c != 0 : op == '<'
            ? c < 0 : op == '>' ? c > 0 : op == 'l' ? c <= 0 : c >= 0;
        r = v_bool(result);
    } else {
        double p, q;
        int err = E_VALUE;
        if (!to_number(&a, &p, &err) || !to_number(&b, &q, &err)) {
            r = v_err(err);
        } else if (op == '+') {
            r = v_num(p + q);
        } else if (op == '-') {
            r = v_num(p - q);
        } else if (op == '*') {
            r = v_num(p * q);
        } else if (op == '/') {
            r = q == 0 ? v_err(E_DIV0) : v_num(p / q);
        } else {
            if (p == 0 && q == 0) {
                r = v_err(E_NUM);
            } else if (p == 0 && q < 0) {
                r = v_err(E_DIV0);
            } else if (p < 0 && q != floor(q)) {
                r = v_err(E_NUM);
            } else {
                r = v_num(pow(p, q));
            }
        }
    }
    v_free(&a);
    v_free(&b);
    return r;
}

static int lazy_function(int func);

static struct val eval(struct ctx *x, int index) {
    const struct node *node = &x->f->nodes[index];
    switch (node->type) {
    case N_NUM: return v_num(node->num);
    case N_BOOL: return v_bool(node->num != 0);
    case N_ERR: return v_err((int)node->num);
    case N_STR: return v_str(node->str);
    case N_MISSING: {
        struct val v = {0};
        return v;
    }
    case N_REF: case N_RANGE: {
        struct val v = {0};
        v.kind = V_RANGE;
        v.sheet = node->sheet >= 0 ? node->sheet : x->sheet;
        v.r1 = node->r1;
        v.c1 = node->c1;
        v.r2 = node->r2;
        v.c2 = node->c2;
        return v;
    }
    case N_NEG: case N_PERCENT: {
        struct val a = eval_scalar(x, node->a);
        double p;
        int err = E_VALUE;
        if (!to_number(&a, &p, &err)) {
            v_free(&a);
            return v_err(err);
        }
        v_free(&a);
        return v_num(node->type == N_NEG ? -p : p / 100);
    }
    case N_BIN:
        return eval_binary(x, node);
    case N_FUNC: {
        int count = node->b;
        const int *kids = x->f->kids + node->a;
        int lazy = lazy_function(node->func);
        if (lazy == 1) { /* IF(test, then, else) */
            struct val test = eval_scalar(x, kids[0]);
            bool b;
            int err = E_VALUE;
            if (!to_bool(&test, &b, &err)) {
                v_free(&test);
                return v_err(err);
            }
            v_free(&test);
            if (b) {
                return eval(x, kids[1]);
            }
            if (count < 3) {
                return v_bool(false);
            }
            return eval(x, kids[2]);
        }
        if (lazy == 2 || lazy == 3) { /* IFERROR, IFNA */
            struct val first = eval_scalar(x, kids[0]);
            if (first.kind == V_ERR && (lazy == 2 || first.err == E_NA)
                && first.err != E_DEEP) {
                return eval_scalar(x, kids[1]);
            }
            return first;
        }
        struct val args[64];
        for (int i = 0; i < count; i++) {
            args[i] = eval(x, kids[i]);
        }
        struct val r = functions[node->func].fn(args, count, x);
        for (int i = 0; i < count; i++) {
            v_free(&args[i]);
        }
        return r;
    }
    default:
        return v_err(E_VALUE);
    }
}

static void cell_set_value(struct cell *c, struct val v) {
    free(c->text);
    c->text = NULL;
    c->kind = v.kind == V_RANGE ? V_ERR : v.kind;
    c->err = v.kind == V_RANGE ? E_VALUE : v.err;
    c->num = v.num;
    if (v.kind == V_STR) {
        c->text = v.str; /* takes it */
    }
}

static void cell_compute(int sheet, struct cell *c) {
    if (c->busy || !c->formula) {
        return;
    }
    if (calc_depth >= CALC_DEPTH_MAX) {
        deep_events++;
        return;
    }
    if (c->code == NULL || c->code->compiled_for != sheets_version) {
        formula_free(c->code);
        c->code = formula_compile(c->input + 1, sheet);
    }
    if (c->cached && (c->code == NULL || c->code->unknown)) {
        c->gen = calc_gen; /* keep the value saved in the file */
        return;
    }
    if (c->code == NULL) {
        struct val e = v_err(E_NAME);
        cell_set_value(c, e);
        c->gen = calc_gen;
        return;
    }
    unsigned long deep_before = deep_events;
    c->busy = 1;
    calc_depth++;
    struct ctx x = {sheet, (int)((c->key - 1) / MAX_COLS),
                    (int)((c->key - 1) % MAX_COLS), c->code};
    struct val v = scalar(&x, eval(&x, c->code->root));
    calc_depth--;
    c->busy = 0;
    cell_set_value(c, v);
    /* Not cached if something it needs was too deep to compute now. */
    c->gen = deep_events == deep_before ? calc_gen : calc_gen - 1;
}

/* --------------------------------------------------- range iteration */

/* Calls visit for each value: ranges give only their cells (empty cells
   are skipped unless want_empty); scalars come as they are. */
typedef bool (*visit_fn)(struct cv v, bool in_range, void *data);

static bool each_value(struct val *args, int n, bool want_empty,
                       visit_fn visit, void *data) {
    for (int i = 0; i < n; i++) {
        struct val *a = &args[i];
        if (a->kind != V_RANGE) {
            struct cv v = {a->kind, a->err, a->num,
                           a->kind == V_STR ? a->str : ""};
            if (!visit(v, false, data)) {
                return false;
            }
            continue;
        }
        if (a->sheet < 0 || a->sheet >= nsheets) {
            struct cv v = {V_ERR, E_REF, 0, ""};
            if (!visit(v, true, data)) {
                return false;
            }
            continue;
        }
        int rows, cols;
        sheet_extent(sheets[a->sheet], &rows, &cols);
        int r2 = a->r2 < rows - 1 ? a->r2 : rows - 1;
        int c2 = a->c2 < cols - 1 ? a->c2 : cols - 1;
        for (int r = a->r1; r <= r2; r++) {
            for (int c = a->c1; c <= c2; c++) {
                struct cv v = cell_view(a->sheet, r, c);
                if (v.kind == V_EMPTY && !want_empty) {
                    continue;
                }
                if (!visit(v, true, data)) {
                    return false;
                }
            }
        }
        if (want_empty) {
            /* Cells past the used area are empty too. */
            long total = (long)(a->r2 - a->r1 + 1) * (a->c2 - a->c1 + 1);
            long seen = (long)(r2 >= a->r1 ? r2 - a->r1 + 1 : 0)
                * (c2 >= a->c1 ? c2 - a->c1 + 1 : 0);
            struct cv v = {V_EMPTY, 0, 0, ""};
            for (long k = seen; k < total; k++) {
                if (!visit(v, true, data)) {
                    return false;
                }
                if (k - seen > 2000000) {
                    break; /* whole columns: the rest changes nothing */
                }
            }
        }
    }
    return true;
}

struct agg {
    double sum, product, min, max;
    long count, counta, blank;
    int err;
    bool any;
    double *list; /* MEDIAN */
    long list_count, list_capacity;
};

static bool agg_visit(struct cv v, bool in_range, void *data) {
    struct agg *g = data;
    double x;
    if (v.kind == V_ERR) {
        g->err = v.err;
        return false;
    }
    if (v.kind == V_EMPTY) {
        g->blank++;
        if (in_range) {
            return true;
        }
        x = 0;
    } else if (v.kind == V_NUM) {
        x = v.num;
    } else if (v.kind == V_BOOL) {
        g->counta++;
        if (in_range) {
            return true;
        }
        x = v.num;
    } else { /* text */
        g->counta++;
        if (v.text[0] == '\0') {
            g->blank++;
        }
        if (in_range) {
            return true;
        }
        char format[32];
        if (!parse_typed_number(v.text, &x, format, sizeof(format))) {
            g->err = E_VALUE;
            return false;
        }
    }
    if (v.kind == V_NUM) {
        g->counta++;
    }
    g->count++;
    g->sum += x;
    g->product *= x;
    if (!g->any || x < g->min) {
        g->min = x;
    }
    if (!g->any || x > g->max) {
        g->max = x;
    }
    g->any = true;
    if (g->list_capacity >= 0 && g->list != NULL) {
        if (g->list_count == g->list_capacity) {
            long capacity = g->list_capacity * 2;
            double *grown = realloc(g->list, (size_t)capacity * sizeof(double));
            if (grown == NULL) {
                g->err = E_NUM;
                return false;
            }
            g->list = grown;
            g->list_capacity = capacity;
        }
        g->list[g->list_count++] = x;
    }
    return true;
}

static struct agg aggregate(struct val *args, int n, bool want_list) {
    struct agg g = {0};
    g.product = 1;
    if (want_list) {
        g.list_capacity = 64;
        g.list = malloc(64 * sizeof(double));
    }
    each_value(args, n, false, agg_visit, &g);
    return g;
}

static struct val fn_sum(struct val *a, int n, struct ctx *x) {
    (void)x;
    struct agg g = aggregate(a, n, false);
    return g.err ? v_err(g.err) : v_num(g.sum);
}

static struct val fn_average(struct val *a, int n, struct ctx *x) {
    (void)x;
    struct agg g = aggregate(a, n, false);
    if (g.err) {
        return v_err(g.err);
    }
    return g.count == 0 ? v_err(E_DIV0) : v_num(g.sum / (double)g.count);
}

static struct val fn_min(struct val *a, int n, struct ctx *x) {
    (void)x;
    struct agg g = aggregate(a, n, false);
    return g.err ? v_err(g.err) : v_num(g.any ? g.min : 0);
}

static struct val fn_max(struct val *a, int n, struct ctx *x) {
    (void)x;
    struct agg g = aggregate(a, n, false);
    return g.err ? v_err(g.err) : v_num(g.any ? g.max : 0);
}

static struct val fn_product(struct val *a, int n, struct ctx *x) {
    (void)x;
    struct agg g = aggregate(a, n, false);
    return g.err ? v_err(g.err) : v_num(g.any ? g.product : 0);
}

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static struct val fn_median(struct val *a, int n, struct ctx *x) {
    (void)x;
    struct agg g = aggregate(a, n, true);
    struct val r;
    if (g.err) {
        r = v_err(g.err);
    } else if (g.list == NULL || g.list_count == 0) {
        r = v_err(g.list == NULL ? E_NUM : E_NUM);
    } else {
        qsort(g.list, (size_t)g.list_count, sizeof(double), compare_doubles);
        long m = g.list_count / 2;
        r = v_num(g.list_count % 2 ? g.list[m]
                                    : (g.list[m - 1] + g.list[m]) / 2);
    }
    free(g.list);
    return r;
}

struct counter {
    long count;
    int mode; /* 0 numbers, 1 non-empty, 2 empty */
};

static bool count_visit(struct cv v, bool in_range, void *data) {
    struct counter *c = data;
    if (c->mode == 0) {
        if (v.kind == V_NUM) {
            c->count++;
        } else if (!in_range && (v.kind == V_BOOL)) {
            c->count++;
        } else if (!in_range && v.kind == V_STR) {
            double number;
            char format[32];
            if (parse_typed_number(v.text, &number, format, sizeof(format))) {
                c->count++;
            }
        }
    } else if (c->mode == 1) {
        if (v.kind != V_EMPTY) {
            c->count++;
        }
    } else if (v.kind == V_EMPTY || (v.kind == V_STR && v.text[0] == '\0')) {
        c->count++;
    }
    return true;
}

static struct val count_with(struct val *a, int n, int mode) {
    struct counter c = {0, mode};
    each_value(a, n, mode == 2, count_visit, &c);
    return v_num((double)c.count);
}

static struct val fn_count(struct val *a, int n, struct ctx *x) {
    (void)x;
    return count_with(a, n, 0);
}

static struct val fn_counta(struct val *a, int n, struct ctx *x) {
    (void)x;
    return count_with(a, n, 1);
}

static struct val fn_countblank(struct val *a, int n, struct ctx *x) {
    (void)x;
    if (a[0].kind != V_RANGE) {
        return v_err(E_VALUE);
    }
    return count_with(a, n, 2);
}

/* Numeric arguments. */
static bool arg_num(struct ctx *x, struct val *a, double *out, struct val *err) {
    struct val v = scalar(x, *a);
    bool owned = a->kind == V_RANGE;
    int e = E_VALUE;
    bool ok = to_number(&v, out, &e);
    if (owned) {
        v_free(&v);
    }
    if (!ok) {
        *err = v_err(e);
    }
    return ok;
}

static bool arg_bool(struct ctx *x, struct val *a, bool *out, struct val *err) {
    struct val v = scalar(x, *a);
    bool owned = a->kind == V_RANGE;
    int e = E_VALUE;
    bool ok = to_bool(&v, out, &e);
    if (owned) {
        v_free(&v);
    }
    if (!ok) {
        *err = v_err(e);
    }
    return ok;
}

/* Text argument (malloc'd), or NULL with *err set. */
static char *arg_text(struct ctx *x, struct val *a, struct val *err) {
    struct val v = scalar(x, *a);
    bool owned = a->kind == V_RANGE;
    if (v.kind == V_ERR) {
        *err = v;
        return NULL;
    }
    char *text = to_text(&v);
    if (owned) {
        v_free(&v);
    }
    if (text == NULL) {
        *err = v_err(E_NUM);
    }
    return text;
}

#define NUM_ARG(i, name) \
    double name; \
    { struct val e_; if (!arg_num(x, &a[i], &name, &e_)) return e_; }

static double round_to(double v, int digits, int mode) {
    /* mode 0 nearest (half away from zero), 1 up (away), 2 down
       (towards zero) */
    double factor = p10(digits < 0 ? -digits : digits);
    double scaled = digits >= 0 ? v * factor : v / factor;
    double sign = scaled < 0 ? -1 : 1;
    double magnitude = scaled * sign;
    /* Like the display: work on 15 significant digits. */
    if (magnitude != 0) {
        char d[16];
        int e;
        num_digits15(magnitude, d, &e);
        double clean;
        num_scan(d, 15, &clean);
        magnitude = clean * p10(e - 14);
    }
    double r;
    if (mode == 0) {
        r = floor(magnitude + 0.5);
    } else if (mode == 1) {
        r = ceil(magnitude - 1e-9 * (magnitude > 1 ? 0 : 0));
        if (r - magnitude > 1 - 1e-12) {
            r -= 1;
        }
    } else {
        r = floor(magnitude);
    }
    r *= sign;
    return digits >= 0 ? r / factor : r * factor;
}

static struct val fn_round_mode(struct val *a, int n, struct ctx *x, int mode) {
    NUM_ARG(0, v);
    double digits = 0;
    if (n > 1) {
        struct val e;
        if (!arg_num(x, &a[1], &digits, &e)) {
            return e;
        }
    }
    if (digits > 15) {
        digits = 15;
    }
    if (digits < -15) {
        digits = -15;
    }
    return v_num(round_to(v, (int)trunc(digits), mode));
}

static struct val fn_round(struct val *a, int n, struct ctx *x) {
    return fn_round_mode(a, n, x, 0);
}

static struct val fn_roundup(struct val *a, int n, struct ctx *x) {
    return fn_round_mode(a, n, x, 1);
}

static struct val fn_rounddown(struct val *a, int n, struct ctx *x) {
    return fn_round_mode(a, n, x, 2);
}

static struct val fn_trunc(struct val *a, int n, struct ctx *x) {
    return fn_round_mode(a, n, x, 2);
}

static struct val fn_int(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    return v_num(floor(v));
}

static struct val fn_abs(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    return v_num(v < 0 ? -v : v);
}

static struct val fn_sign(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    return v_num(v > 0 ? 1 : v < 0 ? -1 : 0);
}

static struct val fn_mod(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    NUM_ARG(1, d);
    if (d == 0) {
        return v_err(E_DIV0);
    }
    return v_num(v - d * floor(v / d));
}

static struct val fn_sqrt(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    return v < 0 ? v_err(E_NUM) : v_num(sqrt(v));
}

static struct val fn_power(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    NUM_ARG(1, e);
    if (v == 0 && e <= 0) {
        return v_err(e == 0 ? E_NUM : E_DIV0);
    }
    if (v < 0 && e != floor(e)) {
        return v_err(E_NUM);
    }
    return v_num(pow(v, e));
}

static struct val fn_exp(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    return v_num(exp(v));
}

static struct val fn_ln(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    return v <= 0 ? v_err(E_NUM) : v_num(log(v));
}

static struct val fn_log10(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    return v <= 0 ? v_err(E_NUM) : v_num(log10(v));
}

static struct val fn_log(struct val *a, int n, struct ctx *x) {
    NUM_ARG(0, v);
    double base = 10;
    if (n > 1) {
        struct val e;
        if (!arg_num(x, &a[1], &base, &e)) {
            return e;
        }
    }
    if (v <= 0 || base <= 0 || base == 1) {
        return v_err(base == 1 ? E_DIV0 : E_NUM);
    }
    return v_num(log(v) / log(base));
}

static struct val fn_pi(struct val *a, int n, struct ctx *x) {
    (void)a;
    (void)n;
    (void)x;
    return v_num(3.14159265358979323846);
}

static struct val fn_ceiling_floor(struct val *a, int n, struct ctx *x,
                                   bool up) {
    NUM_ARG(0, v);
    double step = v < 0 ? -1 : 1;
    if (n > 1) {
        struct val e;
        if (!arg_num(x, &a[1], &step, &e)) {
            return e;
        }
    }
    if (step == 0) {
        return v_num(0);
    }
    if (v > 0 && step < 0) {
        return v_err(E_NUM);
    }
    double q = v / step;
    double whole = up ? ceil(q - 1e-12) : floor(q + 1e-12);
    return v_num(whole * step);
}

static struct val fn_ceiling(struct val *a, int n, struct ctx *x) {
    return fn_ceiling_floor(a, n, x, true);
}

static struct val fn_floor(struct val *a, int n, struct ctx *x) {
    return fn_ceiling_floor(a, n, x, false);
}

static uint64_t random_state;

static struct val fn_rand(struct val *a, int n, struct ctx *x) {
    (void)a;
    (void)n;
    (void)x;
    if (random_state == 0) {
        struct opos_timespec t;
        opos_uptime(&t);
        random_state = (uint64_t)t.nanoseconds * 2654435761u + 88172645463325252ull;
    }
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return v_num((double)(random_state >> 11) / 9007199254740992.0);
}

static struct val fn_randbetween(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, low);
    NUM_ARG(1, high);
    low = ceil(low);
    high = floor(high);
    if (low > high) {
        return v_err(E_NUM);
    }
    struct val r = fn_rand(a, 0, x);
    return v_num(low + floor(r.num * (high - low + 1)));
}

/* ------------------------------------------------------ logic, info */

static struct val fn_if(struct val *a, int n, struct ctx *x) {
    (void)a; (void)n; (void)x;
    return v_err(E_VALUE); /* evaluated lazily in eval() */
}

struct logic {
    int mode; /* 0 and, 1 or */
    bool result, any;
    int err;
};

static bool logic_visit(struct cv v, bool in_range, void *data) {
    struct logic *g = data;
    bool b;
    if (v.kind == V_ERR) {
        g->err = v.err;
        return false;
    }
    if (v.kind == V_EMPTY || (v.kind == V_STR && in_range)) {
        return true;
    }
    if (v.kind == V_STR) {
        struct val s = {0};
        s.kind = V_STR;
        s.str = (char *)v.text;
        int e = E_VALUE;
        if (!to_bool(&s, &b, &e)) {
            g->err = e;
            return false;
        }
    } else {
        b = v.num != 0;
    }
    g->any = true;
    if (g->mode == 0) {
        g->result = g->result && b;
    } else {
        g->result = g->result || b;
    }
    return true;
}

static struct val fn_logic(struct val *a, int n, int mode) {
    struct logic g = {mode, mode == 0, false, 0};
    each_value(a, n, false, logic_visit, &g);
    if (g.err) {
        return v_err(g.err);
    }
    return g.any ? v_bool(g.result) : v_err(E_VALUE);
}

static struct val fn_and(struct val *a, int n, struct ctx *x) {
    (void)x;
    return fn_logic(a, n, 0);
}

static struct val fn_or(struct val *a, int n, struct ctx *x) {
    (void)x;
    return fn_logic(a, n, 1);
}

static struct val fn_not(struct val *a, int n, struct ctx *x) {
    (void)n;
    bool b;
    struct val e;
    if (!arg_bool(x, &a[0], &b, &e)) {
        return e;
    }
    return v_bool(!b);
}

static struct val fn_true(struct val *a, int n, struct ctx *x) {
    (void)a; (void)n; (void)x;
    return v_bool(true);
}

static struct val fn_false(struct val *a, int n, struct ctx *x) {
    (void)a; (void)n; (void)x;
    return v_bool(false);
}

static struct val fn_na(struct val *a, int n, struct ctx *x) {
    (void)a; (void)n; (void)x;
    return v_err(E_NA);
}

static struct val is_kind(struct val *a, struct ctx *x, int test) {
    struct val v = scalar(x, a[0]);
    bool result;
    switch (test) {
    case 0: result = v.kind == V_EMPTY; break;
    case 1: result = v.kind == V_NUM; break;
    case 2: result = v.kind == V_STR; break;
    case 3: result = v.kind == V_ERR; break;
    case 4: result = v.kind == V_ERR && v.err == E_NA; break;
    default: result = v.kind == V_BOOL; break;
    }
    if (a[0].kind == V_RANGE) {
        v_free(&v);
    }
    return v_bool(result);
}

static struct val fn_isblank(struct val *a, int n, struct ctx *x) {
    (void)n;
    return is_kind(a, x, 0);
}

static struct val fn_isnumber(struct val *a, int n, struct ctx *x) {
    (void)n;
    return is_kind(a, x, 1);
}

static struct val fn_istext(struct val *a, int n, struct ctx *x) {
    (void)n;
    return is_kind(a, x, 2);
}

static struct val fn_iserror(struct val *a, int n, struct ctx *x) {
    (void)n;
    return is_kind(a, x, 3);
}

static struct val fn_isna(struct val *a, int n, struct ctx *x) {
    (void)n;
    return is_kind(a, x, 4);
}

static struct val fn_islogical(struct val *a, int n, struct ctx *x) {
    (void)n;
    return is_kind(a, x, 5);
}

/* -------------------------------------------------------------- text */

static size_t utf8_count(const char *s) {
    size_t count = 0;
    for (; *s != '\0'; s++) {
        if (((unsigned char)*s & 0xc0) != 0x80) {
            count++;
        }
    }
    return count;
}

/* Byte offset of character index k (clamped to the end). */
static size_t utf8_offset(const char *s, size_t k) {
    size_t i = 0;
    while (s[i] != '\0' && k > 0) {
        i++;
        while (((unsigned char)s[i] & 0xc0) == 0x80) {
            i++;
        }
        k--;
    }
    return i;
}

#define TEXT_ARG(i, name) \
    char *name; \
    { struct val e_; name = arg_text(x, &a[i], &e_); if (name == NULL) return e_; }

static struct val fn_len(struct val *a, int n, struct ctx *x) {
    (void)n;
    TEXT_ARG(0, s);
    size_t count = utf8_count(s);
    free(s);
    return v_num((double)count);
}

static struct val fn_case(struct val *a, struct ctx *x, int mode) {
    TEXT_ARG(0, s);
    bool start = true;
    for (char *c = s; *c != '\0'; c++) {
        if (mode == 0) {
            *c = (char)toupper((unsigned char)*c);
        } else if (mode == 1) {
            *c = (char)tolower((unsigned char)*c);
        } else {
            *c = (char)(start ? toupper((unsigned char)*c)
                              : tolower((unsigned char)*c));
            start = !isalpha((unsigned char)*c)
                && ((unsigned char)*c < 0x80);
        }
    }
    return v_str_own(s);
}

static struct val fn_upper(struct val *a, int n, struct ctx *x) {
    (void)n;
    return fn_case(a, x, 0);
}

static struct val fn_lower(struct val *a, int n, struct ctx *x) {
    (void)n;
    return fn_case(a, x, 1);
}

static struct val fn_proper(struct val *a, int n, struct ctx *x) {
    (void)n;
    return fn_case(a, x, 2);
}

static struct val fn_trim(struct val *a, int n, struct ctx *x) {
    (void)n;
    TEXT_ARG(0, s);
    size_t used = 0;
    bool space = false;
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] == ' ') {
            space = used > 0;
            continue;
        }
        if (space) {
            s[used++] = ' ';
            space = false;
        }
        s[used++] = s[i];
    }
    s[used] = '\0';
    return v_str_own(s);
}

static struct val fn_left_right(struct val *a, int n, struct ctx *x,
                                bool left) {
    TEXT_ARG(0, s);
    double count = 1;
    if (n > 1) {
        struct val e;
        if (!arg_num(x, &a[1], &count, &e)) {
            free(s);
            return e;
        }
    }
    if (count < 0) {
        free(s);
        return v_err(E_VALUE);
    }
    size_t total = utf8_count(s);
    size_t k = count > (double)total ? total : (size_t)count;
    struct val r;
    if (left) {
        r = v_str_n(s, utf8_offset(s, k));
    } else {
        r = v_str(s + utf8_offset(s, total - k));
    }
    free(s);
    return r;
}

static struct val fn_left(struct val *a, int n, struct ctx *x) {
    return fn_left_right(a, n, x, true);
}

static struct val fn_right(struct val *a, int n, struct ctx *x) {
    return fn_left_right(a, n, x, false);
}

static struct val fn_mid(struct val *a, int n, struct ctx *x) {
    (void)n;
    TEXT_ARG(0, s);
    double start, count;
    struct val e;
    if (!arg_num(x, &a[1], &start, &e) || !arg_num(x, &a[2], &count, &e)) {
        free(s);
        return e;
    }
    if (start < 1 || count < 0) {
        free(s);
        return v_err(E_VALUE);
    }
    size_t total = utf8_count(s);
    size_t from = (size_t)start - 1 > total ? total : (size_t)start - 1;
    size_t to = from + (count > (double)total ? total : (size_t)count);
    if (to > total) {
        to = total;
    }
    size_t a_off = utf8_offset(s, from), b_off = utf8_offset(s, to);
    struct val r = v_str_n(s + a_off, b_off - a_off);
    free(s);
    return r;
}

struct joiner {
    struct buf out;
    const char *sep;
    bool skip_empty, first;
    int err;
};

static bool join_visit(struct cv v, bool in_range, void *data) {
    (void)in_range;
    struct joiner *j = data;
    if (v.kind == V_ERR) {
        j->err = v.err;
        return false;
    }
    struct val s = {0};
    s.kind = v.kind;
    s.num = v.num;
    s.str = (char *)v.text;
    char *text = to_text(&s);
    if (text == NULL) {
        j->err = E_NUM;
        return false;
    }
    if (!(j->skip_empty && text[0] == '\0')) {
        if (!j->first && j->sep != NULL) {
            buf_puts(&j->out, j->sep);
        }
        buf_puts(&j->out, text);
        j->first = false;
    }
    free(text);
    return true;
}

static struct val join_values(struct val *a, int n, const char *sep,
                              bool skip_empty) {
    struct joiner j = {{0}, sep, skip_empty, true, 0};
    each_value(a, n, false, join_visit, &j);
    if (j.err || j.out.failed) {
        free(j.out.data);
        return v_err(j.err ? j.err : E_NUM);
    }
    if (j.out.data == NULL) {
        return v_str("");
    }
    return v_str_own(j.out.data);
}

static struct val fn_concat(struct val *a, int n, struct ctx *x) {
    (void)x;
    return join_values(a, n, NULL, false);
}

static struct val fn_concatenate(struct val *a, int n, struct ctx *x) {
    for (int i = 0; i < n; i++) {
        if (a[i].kind == V_RANGE) {
            struct val v = scalar(x, a[i]);
            a[i] = v;
        }
    }
    return join_values(a, n, NULL, false);
}

static struct val fn_textjoin(struct val *a, int n, struct ctx *x) {
    TEXT_ARG(0, sep);
    bool skip;
    struct val e;
    if (!arg_bool(x, &a[1], &skip, &e)) {
        free(sep);
        return e;
    }
    struct val r = join_values(a + 2, n - 2, sep, skip);
    free(sep);
    return r;
}

static struct val fn_rept(struct val *a, int n, struct ctx *x) {
    (void)n;
    TEXT_ARG(0, s);
    double times;
    struct val e;
    if (!arg_num(x, &a[1], &times, &e)) {
        free(s);
        return e;
    }
    size_t length = strlen(s);
    if (times < 0 || (double)length * times > 32767) {
        free(s);
        return v_err(E_VALUE);
    }
    size_t count = (size_t)times;
    char *out = malloc(length * count + 1);
    if (out != NULL) {
        for (size_t i = 0; i < count; i++) {
            memcpy(out + i * length, s, length);
        }
        out[length * count] = '\0';
    }
    free(s);
    return v_str_own(out);
}

/* Wildcard match (* and ?, ~ escapes), case-insensitive. */
static bool wild_match(const char *p, const char *s) {
    for (;;) {
        if (*p == '\0') {
            return *s == '\0';
        }
        if (*p == '*') {
            while (*p == '*') {
                p++;
            }
            if (*p == '\0') {
                return true;
            }
            for (; *s != '\0'; s++) {
                if (wild_match(p, s)) {
                    return true;
                }
            }
            return false;
        }
        if (*s == '\0') {
            return false;
        }
        if (*p == '~' && (p[1] == '*' || p[1] == '?' || p[1] == '~')) {
            p++;
            if (*p != *s) {
                return false;
        	}
        } else if (*p != '?'
                   && tolower((unsigned char)*p) != tolower((unsigned char)*s)) {
            return false;
        }
        p++;
        s++;
    }
}

static struct val fn_find_search(struct val *a, int n, struct ctx *x,
                                 bool search) {
    TEXT_ARG(0, needle);
    TEXT_ARG(1, hay);
    double start = 1;
    if (n > 2) {
        struct val e;
        if (!arg_num(x, &a[2], &start, &e)) {
            free(needle);
            free(hay);
            return e;
        }
    }
    size_t total = utf8_count(hay);
    struct val r = v_err(E_VALUE);
    if (start >= 1 && start <= (double)total + 1) {
        size_t from = utf8_offset(hay, (size_t)start - 1);
        size_t nl = strlen(needle);
        for (size_t i = from; hay[i] != '\0' || nl == 0; i++) {
            bool hit;
            if (search) {
                /* the needle may have wildcards: try each prefix length */
                hit = false;
                char *pattern = malloc(nl + 2);
                if (pattern != NULL) {
                    memcpy(pattern, needle, nl);
                    pattern[nl] = '*';
                    pattern[nl + 1] = '\0';
                    hit = wild_match(pattern, hay + i);
                    free(pattern);
                }
            } else {
                hit = strncmp(hay + i, needle, nl) == 0;
            }
            if (hit) {
                /* character position */
                size_t chars = 0;
                for (size_t k = 0; k < i; k++) {
                    if (((unsigned char)hay[k] & 0xc0) != 0x80) {
                        chars++;
                    }
                }
                r = v_num((double)chars + 1);
                break;
            }
            if (hay[i] == '\0') {
                break;
            }
        }
    }
    free(needle);
    free(hay);
    return r;
}

static struct val fn_find(struct val *a, int n, struct ctx *x) {
    return fn_find_search(a, n, x, false);
}

static struct val fn_search(struct val *a, int n, struct ctx *x) {
    return fn_find_search(a, n, x, true);
}

static struct val fn_substitute(struct val *a, int n, struct ctx *x) {
    TEXT_ARG(0, s);
    TEXT_ARG(1, old);
    TEXT_ARG(2, with);
    double which = 0;
    if (n > 3) {
        struct val e;
        if (!arg_num(x, &a[3], &which, &e) || which < 1) {
            free(s);
            free(old);
            free(with);
            return which < 1 && e.kind != V_ERR ? v_err(E_VALUE) : e;
        }
    }
    size_t ol = strlen(old);
    struct buf out = {0};
    int seen = 0;
    for (size_t i = 0; s[i] != '\0';) {
        if (ol > 0 && strncmp(s + i, old, ol) == 0) {
            seen++;
            if (which == 0 || seen == (int)which) {
                buf_puts(&out, with);
                i += ol;
                continue;
            }
        }
        buf_byte(&out, (uint8_t)s[i]);
        i++;
    }
    free(s);
    free(old);
    free(with);
    if (out.failed) {
        free(out.data);
        return v_err(E_NUM);
    }
    return out.data != NULL ? v_str_own(out.data) : v_str("");
}

static struct val fn_exact(struct val *a, int n, struct ctx *x) {
    (void)n;
    TEXT_ARG(0, p);
    TEXT_ARG(1, q);
    bool same = strcmp(p, q) == 0;
    free(p);
    free(q);
    return v_bool(same);
}

static struct val fn_value(struct val *a, int n, struct ctx *x) {
    (void)n;
    struct val v = scalar(x, a[0]);
    double number;
    int e = E_VALUE;
    bool ok = v.kind == V_STR ? true : v.kind != V_ERR;
    if (v.kind == V_ERR) {
        return v;
    }
    ok = to_number(&v, &number, &e);
    if (a[0].kind == V_RANGE) {
        v_free(&v);
    }
    return ok ? v_num(number) : v_err(e);
}

static struct val fn_text(struct val *a, int n, struct ctx *x) {
    (void)n;
    struct val v = scalar(x, a[0]);
    TEXT_ARG(1, format);
    struct shown shown;
    double number;
    int e = E_VALUE;
    if (v.kind == V_ERR) {
        free(format);
        return v;
    }
    if (v.kind == V_STR && !to_number(&v, &number, &e)) {
        format_text(v.str, format, &shown);
    } else if (to_number(&v, &number, &e)) {
        format_number(number, format, 11, &shown);
    } else {
        shown.text[0] = '\0';
    }
    if (a[0].kind == V_RANGE) {
        v_free(&v);
    }
    free(format);
    return v_str(shown.text);
}

static struct val fn_char(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, code);
    if (code < 1 || code > 255) {
        return v_err(E_VALUE);
    }
    char out[4];
    int length = utf8_put(out, (uint32_t)code);
    return v_str_n(out, (size_t)length);
}

/* ------------------------------------------------------------ dates */

static double now_serial(void) {
    struct {
        int64_t seconds, nanoseconds;
    } t = {0, 0};
    if (opos_syscall2(228, 0 /* CLOCK_REALTIME */, (long)&t) != 0) {
        return 0;
    }
    return (double)t.seconds / 86400.0 + 25569.0 /* 1970-01-01 */;
}

static struct val fn_today(struct val *a, int n, struct ctx *x) {
    (void)a; (void)n; (void)x;
    return v_num(floor(now_serial()));
}

static struct val fn_now(struct val *a, int n, struct ctx *x) {
    (void)a; (void)n; (void)x;
    return v_num(now_serial());
}

static struct val fn_date(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, y);
    NUM_ARG(1, m);
    NUM_ARG(2, d);
    y = trunc(y);
    m = trunc(m);
    d = trunc(d);
    if (y < 1900) {
        y += 1900;
    }
    double months = y * 12 + (m - 1);
    long year = (long)floor(months / 12);
    int month = (int)(months - (double)year * 12) + 1;
    if (year < 1900 || year > 9999) {
        return v_err(E_NUM);
    }
    double serial = serial_from_date(year, month, 1) + d - 1;
    if (serial < 0) {
        return v_err(E_NUM);
    }
    return v_num(serial);
}

static struct val fn_time(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, h);
    NUM_ARG(1, m);
    NUM_ARG(2, s);
    double total = trunc(h) * 3600 + trunc(m) * 60 + trunc(s);
    if (total < 0) {
        return v_err(E_NUM);
    }
    return v_num(fmod(total, 86400) / 86400);
}

static struct val date_part(struct val *a, struct ctx *x, int part) {
    NUM_ARG(0, v);
    struct when w;
    if (!date_from_serial(v, &w)) {
        return v_err(E_NUM);
    }
    switch (part) {
    case 0: return v_num((double)w.year);
    case 1: return v_num(w.month);
    case 2: return v_num(w.day);
    case 3: return v_num(w.hour);
    case 4: return v_num(w.minute);
    default: return v_num(w.second);
    }
}

static struct val fn_year(struct val *a, int n, struct ctx *x) {
    (void)n;
    return date_part(a, x, 0);
}

static struct val fn_month(struct val *a, int n, struct ctx *x) {
    (void)n;
    return date_part(a, x, 1);
}

static struct val fn_day(struct val *a, int n, struct ctx *x) {
    (void)n;
    return date_part(a, x, 2);
}

static struct val fn_hour(struct val *a, int n, struct ctx *x) {
    (void)n;
    return date_part(a, x, 3);
}

static struct val fn_minute(struct val *a, int n, struct ctx *x) {
    (void)n;
    return date_part(a, x, 4);
}

static struct val fn_second(struct val *a, int n, struct ctx *x) {
    (void)n;
    return date_part(a, x, 5);
}

static struct val fn_weekday(struct val *a, int n, struct ctx *x) {
    NUM_ARG(0, v);
    double type = 1;
    if (n > 1) {
        struct val e;
        if (!arg_num(x, &a[1], &type, &e)) {
            return e;
        }
    }
    struct when w;
    if (!date_from_serial(v, &w)) {
        return v_err(E_NUM);
    }
    int day = w.weekday; /* 0 = Sunday */
    if (type == 1) {
        return v_num(day + 1);
    }
    if (type == 2) {
        return v_num(day == 0 ? 7 : day);
    }
    if (type == 3) {
        return v_num(day == 0 ? 6 : day - 1);
    }
    return v_err(E_NUM);
}

static struct val fn_days(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, end);
    NUM_ARG(1, start);
    return v_num(floor(end) - floor(start));
}

static struct val fn_edate(struct val *a, int n, struct ctx *x) {
    (void)n;
    NUM_ARG(0, v);
    NUM_ARG(1, months);
    struct when w;
    if (!date_from_serial(v, &w)) {
        return v_err(E_NUM);
    }
    long total = w.year * 12 + (w.month - 1) + (long)trunc(months);
    long year = total / 12;
    int month = (int)(total % 12) + 1;
    static const int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30,
                                       31, 30, 31};
    int last = month_days[month - 1];
    if (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) {
        last = 29;
    }
    int day = w.day > last ? last : w.day;
    if (year < 1900) {
        return v_err(E_NUM);
    }
    return v_num(serial_from_date(year, month, day));
}

/* ----------------------------------------------------- lookup, refs */

static struct val range_cell(const struct val *range, int r, int c) {
    return val_from_view(cell_view(range->sheet, range->r1 + r, range->c1 + c));
}

/* Is v found by an exact lookup of key? (text may have wildcards) */
static bool lookup_equal(const struct val *key, const struct val *v) {
    if (key->kind == V_STR && v->kind == V_STR) {
        return wild_match(key->str, v->str);
    }
    if (key->kind != v->kind) {
        return false;
    }
    return compare_values(key, v) == 0;
}

/* Position (0-based) of key along a row or column; -1 if not found.
   approx: 1 = largest <= key, -1 = smallest >= key, 0 = exact. */
static long lookup_line(struct val key, const struct val *range, bool down,
                        int approx) {
    long length = down ? range->r2 - range->r1 + 1 : range->c2 - range->c1 + 1;
    int rows, cols;
    sheet_extent(sheets[range->sheet], &rows, &cols);
    long used = down ? rows - range->r1 : cols - range->c1;
    if (used < length) {
        length = used < 0 ? 0 : used;
    }
    long best = -1;
    for (long k = 0; k < length; k++) {
        struct val v = down ? range_cell(range, (int)k, 0)
                            : range_cell(range, 0, (int)k);
        if (approx == 0) {
            bool hit = lookup_equal(&key, &v);
            v_free(&v);
            if (hit) {
                return k;
            }
            continue;
        }
        if (v.kind == V_EMPTY || v.kind == V_ERR
            || (v.kind == V_STR) != (key.kind == V_STR)) {
            v_free(&v);
            continue;
        }
        int c = compare_values(&v, &key);
        v_free(&v);
        if (c == 0) {
            return k;
        }
        if (approx == 1) {
            if (c < 0) {
                best = k;
            } else {
                break; /* sorted: the rest are larger */
            }
        } else if (c > 0) {
            best = k;
        } else {
            break;
        }
    }
    return best;
}

static struct val fn_lookup2(struct val *a, int n, struct ctx *x, bool down) {
    struct val key = scalar(x, a[0]);
    if (key.kind == V_ERR) {
        return key;
    }
    if (a[1].kind != V_RANGE) {
        if (a[0].kind == V_RANGE) {
            v_free(&key);
        }
        return v_err(E_VALUE);
    }
    double index;
    struct val e;
    if (!arg_num(x, &a[2], &index, &e)) {
        if (a[0].kind == V_RANGE) {
            v_free(&key);
        }
        return e;
    }
    bool approx = true;
    if (n > 3 && a[3].kind != V_EMPTY) {
        if (!arg_bool(x, &a[3], &approx, &e)) {
            if (a[0].kind == V_RANGE) {
                v_free(&key);
            }
            return e;
        }
    }
    struct val *range = &a[1];
    long width = down ? range->c2 - range->c1 + 1 : range->r2 - range->r1 + 1;
    struct val r;
    if (index < 1 || index > (double)width) {
        r = v_err(index < 1 ? E_VALUE : E_REF);
    } else {
        long at = lookup_line(key, range, down, approx ? 1 : 0);
        if (at < 0) {
            r = v_err(E_NA);
        } else if (down) {
            r = range_cell(range, (int)at, (int)index - 1);
        } else {
            r = range_cell(range, (int)index - 1, (int)at);
        }
    }
    if (a[0].kind == V_RANGE) {
        v_free(&key);
    }
    if (r.kind == V_EMPTY) {
        r = v_num(0);
    }
    return r;
}

static struct val fn_vlookup(struct val *a, int n, struct ctx *x) {
    return fn_lookup2(a, n, x, true);
}

static struct val fn_hlookup(struct val *a, int n, struct ctx *x) {
    return fn_lookup2(a, n, x, false);
}

static struct val fn_match(struct val *a, int n, struct ctx *x) {
    struct val key = scalar(x, a[0]);
    if (key.kind == V_ERR) {
        return key;
    }
    double type = 1;
    struct val e;
    if (n > 2 && !arg_num(x, &a[2], &type, &e)) {
        if (a[0].kind == V_RANGE) {
            v_free(&key);
        }
        return e;
    }
    struct val r;
    if (a[1].kind != V_RANGE
        || (a[1].r1 != a[1].r2 && a[1].c1 != a[1].c2)) {
        r = v_err(E_NA);
    } else {
        bool down = a[1].c1 == a[1].c2 && a[1].r1 != a[1].r2;
        long at = lookup_line(key, &a[1], down,
                              type > 0 ? 1 : type < 0 ? -1 : 0);
        r = at < 0 ? v_err(E_NA) : v_num((double)at + 1);
    }
    if (a[0].kind == V_RANGE) {
        v_free(&key);
    }
    return r;
}

static struct val fn_index(struct val *a, int n, struct ctx *x) {
    if (a[0].kind != V_RANGE) {
        return v_err(E_VALUE);
    }
    NUM_ARG(1, row);
    double col = 0;
    if (n > 2) {
        struct val e;
        if (!arg_num(x, &a[2], &col, &e)) {
            return e;
        }
    }
    struct val *range = &a[0];
    long rows = range->r2 - range->r1 + 1, cols = range->c2 - range->c1 + 1;
    if (n < 3 && rows == 1) {
        col = row;
        row = 1;
    }
    if (row < 0 || col < 0 || row > (double)rows || col > (double)cols) {
        return v_err(E_REF);
    }
    if (row == 0 || col == 0) {
        /* a whole row or column: only as a range */
        struct val v = *range;
        if (row > 0) {
            v.r1 = v.r2 = range->r1 + (int)row - 1;
        }
        if (col > 0) {
            v.c1 = v.c2 = range->c1 + (int)col - 1;
        }
        return v;
    }
    struct val v = {0};
    v.kind = V_RANGE;
    v.sheet = range->sheet;
    v.r1 = v.r2 = range->r1 + (int)row - 1;
    v.c1 = v.c2 = range->c1 + (int)col - 1;
    return v;
}

static struct val fn_choose(struct val *a, int n, struct ctx *x) {
    NUM_ARG(0, which);
    if (which < 1 || which >= n) {
        return v_err(E_VALUE);
    }
    struct val chosen = a[(int)which];
    if (chosen.kind == V_STR) {
        return v_str(chosen.str);
    }
    return chosen;
}

static struct val fn_row_col(struct val *a, int n, struct ctx *x, bool row,
                             bool count) {
    if (n == 0) {
        return v_num((row ? x->row : x->col) + 1);
    }
    if (a[0].kind != V_RANGE) {
        return v_err(E_VALUE);
    }
    if (count) {
        return v_num(row ? a[0].r2 - a[0].r1 + 1 : a[0].c2 - a[0].c1 + 1);
    }
    return v_num((row ? a[0].r1 : a[0].c1) + 1);
}

static struct val fn_row(struct val *a, int n, struct ctx *x) {
    return fn_row_col(a, n, x, true, false);
}

static struct val fn_column(struct val *a, int n, struct ctx *x) {
    return fn_row_col(a, n, x, false, false);
}

static struct val fn_rows(struct val *a, int n, struct ctx *x) {
    return fn_row_col(a, n, x, true, true);
}

static struct val fn_columns(struct val *a, int n, struct ctx *x) {
    return fn_row_col(a, n, x, false, true);
}

/* ------------------------------------------------- criteria (..IF) */

struct criterion {
    char op; /* = n < > l g */
    struct val value;
    bool wildcard;
};

static void criterion_make(struct ctx *x, struct val *arg,
                           struct criterion *c) {
    struct val v = scalar(x, *arg);
    if (arg->kind != V_RANGE && v.kind == V_STR) {
        v = v_str(v.str); /* our own copy */
    }
    c->op = '=';
    c->wildcard = false;
    if (v.kind != V_STR) {
        c->value = v;
        return;
    }
    const char *s = v.str;
    if (strncmp(s, "<=", 2) == 0) {
        c->op = 'l';
        s += 2;
    } else if (strncmp(s, ">=", 2) == 0) {
        c->op = 'g';
        s += 2;
    } else if (strncmp(s, "<>", 2) == 0) {
        c->op = 'n';
        s += 2;
    } else if (*s == '<' || *s == '>' || *s == '=') {
        c->op = *s;
        s++;
    }
    double number;
    char format[32];
    if (*s != '\0' && parse_typed_number(s, &number, format, sizeof(format))) {
        c->value = v_num(number);
    } else if (fmt_word(s, strlen(s), 0, "true") && strlen(s) == 4) {
        c->value = v_bool(true);
    } else if (fmt_word(s, strlen(s), 0, "false") && strlen(s) == 5) {
        c->value = v_bool(false);
    } else {
        c->value = v_str(s);
        c->wildcard = strchr(s, '*') != NULL || strchr(s, '?') != NULL;
    }
    v_free(&v);
}

static bool criterion_test(const struct criterion *c, struct cv cell) {
    struct val v = {0};
    v.kind = cell.kind;
    v.num = cell.num;
    v.str = (char *)cell.text;
    v.err = cell.err;
    const struct val *want = &c->value;
    if (want->kind == V_STR && want->str[0] == '\0') {
        /* "" or "=" matches empty cells; "<>" non-empty ones */
        bool empty = v.kind == V_EMPTY || (v.kind == V_STR && v.str[0] == '\0');
        return c->op == 'n' ? !empty : c->op == '=' ? empty : false;
    }
    if (v.kind == V_ERR) {
        return false;
    }
    bool same_kind = (v.kind == V_STR) == (want->kind == V_STR)
        && (v.kind == V_BOOL) == (want->kind == V_BOOL);
    if (c->op == '=' || c->op == 'n') {
        bool equal;
        if (!same_kind || v.kind == V_EMPTY) {
            equal = false;
        } else if (c->wildcard) {
            equal = wild_match(want->str, v.str);
        } else {
            equal = compare_values(&v, want) == 0;
        }
        return c->op == '=' ? equal : !equal;
    }
    if (!same_kind || v.kind == V_EMPTY) {
        return false;
    }
    int k = compare_values(&v, want);
    switch (c->op) {
    case '<': return k < 0;
    case '>': return k > 0;
    case 'l': return k <= 0;
    default: return k >= 0;
    }
}

/* SUMIF, COUNTIF, AVERAGEIF and the ..IFS forms. pairs: (range,
   criterion) pairs; sum: the range to add up (or NULL to count). */
static struct val conditional(struct ctx *x, struct val *sum,
                              struct val *pairs, int npairs, int mode) {
    /* mode 0 sum, 1 count, 2 average */
    struct criterion crit[32];
    if (npairs > 32) {
        return v_err(E_VALUE);
    }
    for (int i = 0; i < npairs; i++) {
        if (pairs[2 * i].kind != V_RANGE) {
            for (int k = 0; k < i; k++) {
                v_free(&crit[k].value);
            }
            return v_err(E_VALUE);
        }
        criterion_make(x, &pairs[2 * i + 1], &crit[i]);
    }
    struct val *first = &pairs[0];
    int rows = first->r2 - first->r1 + 1, cols = first->c2 - first->c1 + 1;
    int ur, uc;
    sheet_extent(sheets[first->sheet], &ur, &uc);
    if (first->r1 + rows > ur) {
        rows = ur - first->r1;
    }
    if (first->c1 + cols > uc) {
        cols = uc - first->c1;
    }
    double total = 0;
    long count = 0;
    int err = 0;
    for (int r = 0; r < rows && !err; r++) {
        for (int c = 0; c < cols && !err; c++) {
            bool all = true;
            for (int i = 0; i < npairs && all; i++) {
                struct val *range = &pairs[2 * i];
                struct cv v = cell_view(range->sheet, range->r1 + r,
                                        range->c1 + c);
                all = criterion_test(&crit[i], v);
            }
            if (!all) {
                continue;
            }
            if (mode == 1) {
                count++;
                continue;
            }
            struct cv v = cell_view(sum->sheet, sum->r1 + r, sum->c1 + c);
            if (v.kind == V_ERR) {
                err = v.err;
            } else if (v.kind == V_NUM) {
                total += v.num;
                count++;
            }
        }
    }
    for (int i = 0; i < npairs; i++) {
        v_free(&crit[i].value);
    }
    if (err) {
        return v_err(err);
    }
    if (mode == 1) {
        return v_num((double)count);
    }
    if (mode == 2) {
        return count == 0 ? v_err(E_DIV0) : v_num(total / (double)count);
    }
    return v_num(total);
}

static struct val fn_sumif(struct val *a, int n, struct ctx *x) {
    struct val *sum = n > 2 ? &a[2] : &a[0];
    if (sum->kind != V_RANGE) {
        return v_err(E_VALUE);
    }
    return conditional(x, sum, a, 1, 0);
}

static struct val fn_averageif(struct val *a, int n, struct ctx *x) {
    struct val *sum = n > 2 ? &a[2] : &a[0];
    if (sum->kind != V_RANGE) {
        return v_err(E_VALUE);
    }
    return conditional(x, sum, a, 1, 2);
}

static struct val fn_countif(struct val *a, int n, struct ctx *x) {
    (void)n;
    return conditional(x, NULL, a, 1, 1);
}

static struct val fn_sumifs(struct val *a, int n, struct ctx *x) {
    if (n % 2 == 0 || a[0].kind != V_RANGE) {
        return v_err(E_VALUE);
    }
    return conditional(x, &a[0], a + 1, (n - 1) / 2, 0);
}

static struct val fn_averageifs(struct val *a, int n, struct ctx *x) {
    if (n % 2 == 0 || a[0].kind != V_RANGE) {
        return v_err(E_VALUE);
    }
    return conditional(x, &a[0], a + 1, (n - 1) / 2, 2);
}

static struct val fn_countifs(struct val *a, int n, struct ctx *x) {
    if (n % 2 != 0) {
        return v_err(E_VALUE);
    }
    return conditional(x, NULL, a, n / 2, 1);
}

static struct val fn_sumproduct(struct val *a, int n, struct ctx *x) {
    (void)x;
    for (int i = 0; i < n; i++) {
        if (a[i].kind != V_RANGE || a[i].r2 - a[i].r1 != a[0].r2 - a[0].r1
            || a[i].c2 - a[i].c1 != a[0].c2 - a[0].c1) {
            return v_err(E_VALUE);
        }
    }
    int rows = a[0].r2 - a[0].r1 + 1, cols = a[0].c2 - a[0].c1 + 1;
    int ur, uc;
    sheet_extent(sheets[a[0].sheet], &ur, &uc);
    if (rows > ur) {
        rows = ur;
    }
    if (cols > uc) {
        cols = uc;
    }
    double total = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double product = 1;
            for (int i = 0; i < n; i++) {
                struct cv v = cell_view(a[i].sheet, a[i].r1 + r, a[i].c1 + c);
                if (v.kind == V_ERR) {
                    return v_err(v.err);
                }
                product *= v.kind == V_NUM ? v.num : 0;
            }
            total += product;
        }
    }
    return v_num(total);
}

/* --------------------------------------------------------- the list */

static const struct function functions[] = {
    {"ABS", 1, 1, fn_abs, false},
    {"AND", 1, 64, fn_and, false},
    {"AVERAGE", 1, 64, fn_average, false},
    {"AVERAGEIF", 2, 3, fn_averageif, false},
    {"AVERAGEIFS", 3, 63, fn_averageifs, false},
    {"CEILING", 1, 2, fn_ceiling, false},
    {"CHAR", 1, 1, fn_char, false},
    {"CHOOSE", 2, 64, fn_choose, false},
    {"COLUMN", 0, 1, fn_column, false},
    {"COLUMNS", 1, 1, fn_columns, false},
    {"CONCAT", 1, 64, fn_concat, true},
    {"CONCATENATE", 1, 64, fn_concatenate, false},
    {"COUNT", 1, 64, fn_count, false},
    {"COUNTA", 1, 64, fn_counta, false},
    {"COUNTBLANK", 1, 1, fn_countblank, false},
    {"COUNTIF", 2, 2, fn_countif, false},
    {"COUNTIFS", 2, 64, fn_countifs, false},
    {"DATE", 3, 3, fn_date, false},
    {"DAY", 1, 1, fn_day, false},
    {"DAYS", 2, 2, fn_days, true},
    {"EDATE", 2, 2, fn_edate, false},
    {"EXACT", 2, 2, fn_exact, false},
    {"EXP", 1, 1, fn_exp, false},
    {"FALSE", 0, 0, fn_false, false},
    {"FIND", 2, 3, fn_find, false},
    {"FLOOR", 1, 2, fn_floor, false},
    {"HLOOKUP", 3, 4, fn_hlookup, false},
    {"HOUR", 1, 1, fn_hour, false},
    {"IF", 2, 3, fn_if, false},
    {"IFERROR", 2, 2, fn_if, false},
    {"IFNA", 2, 2, fn_if, true},
    {"INDEX", 2, 3, fn_index, false},
    {"INT", 1, 1, fn_int, false},
    {"ISBLANK", 1, 1, fn_isblank, false},
    {"ISERROR", 1, 1, fn_iserror, false},
    {"ISLOGICAL", 1, 1, fn_islogical, false},
    {"ISNA", 1, 1, fn_isna, false},
    {"ISNUMBER", 1, 1, fn_isnumber, false},
    {"ISTEXT", 1, 1, fn_istext, false},
    {"LEFT", 1, 2, fn_left, false},
    {"LEN", 1, 1, fn_len, false},
    {"LN", 1, 1, fn_ln, false},
    {"LOG", 1, 2, fn_log, false},
    {"LOG10", 1, 1, fn_log10, false},
    {"LOWER", 1, 1, fn_lower, false},
    {"MATCH", 2, 3, fn_match, false},
    {"MAX", 1, 64, fn_max, false},
    {"MEDIAN", 1, 64, fn_median, false},
    {"MID", 3, 3, fn_mid, false},
    {"MIN", 1, 64, fn_min, false},
    {"MINUTE", 1, 1, fn_minute, false},
    {"MOD", 2, 2, fn_mod, false},
    {"MONTH", 1, 1, fn_month, false},
    {"NA", 0, 0, fn_na, false},
    {"NOT", 1, 1, fn_not, false},
    {"NOW", 0, 0, fn_now, false},
    {"OR", 1, 64, fn_or, false},
    {"PI", 0, 0, fn_pi, false},
    {"POWER", 2, 2, fn_power, false},
    {"PRODUCT", 1, 64, fn_product, false},
    {"PROPER", 1, 1, fn_proper, false},
    {"RAND", 0, 0, fn_rand, false},
    {"RANDBETWEEN", 2, 2, fn_randbetween, false},
    {"REPT", 2, 2, fn_rept, false},
    {"RIGHT", 1, 2, fn_right, false},
    {"ROUND", 1, 2, fn_round, false},
    {"ROUNDDOWN", 1, 2, fn_rounddown, false},
    {"ROUNDUP", 1, 2, fn_roundup, false},
    {"ROW", 0, 1, fn_row, false},
    {"ROWS", 1, 1, fn_rows, false},
    {"SEARCH", 2, 3, fn_search, false},
    {"SECOND", 1, 1, fn_second, false},
    {"SIGN", 1, 1, fn_sign, false},
    {"SQRT", 1, 1, fn_sqrt, false},
    {"SUBSTITUTE", 3, 4, fn_substitute, false},
    {"SUM", 1, 64, fn_sum, false},
    {"SUMIF", 2, 3, fn_sumif, false},
    {"SUMIFS", 3, 63, fn_sumifs, false},
    {"SUMPRODUCT", 1, 64, fn_sumproduct, false},
    {"TEXT", 2, 2, fn_text, false},
    {"TEXTJOIN", 3, 64, fn_textjoin, true},
    {"TIME", 3, 3, fn_time, false},
    {"TODAY", 0, 0, fn_today, false},
    {"TRIM", 1, 1, fn_trim, false},
    {"TRUE", 0, 0, fn_true, false},
    {"TRUNC", 1, 2, fn_trunc, false},
    {"UPPER", 1, 1, fn_upper, false},
    {"VALUE", 1, 1, fn_value, false},
    {"VLOOKUP", 3, 4, fn_vlookup, false},
    {"WEEKDAY", 1, 2, fn_weekday, false},
    {"YEAR", 1, 1, fn_year, false},
};
static const int function_count =
    (int)(sizeof(functions) / sizeof(functions[0]));

/* 1 IF, 2 IFERROR, 3 IFNA, 0 others. */
static int lazy_function(int func) {
    fn_t fn = functions[func].fn;
    if (fn != fn_if) {
        return 0;
    }
    const char *name = functions[func].name;
    return strcmp(name, "IF") == 0 ? 1 : strcmp(name, "IFERROR") == 0 ? 2 : 3;
}


/* --------------------------------------------------------- rewriting */

enum rewrite_mode {
    RW_NORMAL,      /* upper-case names, nothing else */
    RW_SHIFT,       /* relative references move by dr, dc */
    RW_ROWS,        /* rows at..at+count-1 inserted (count > 0) or
                       deleted (count < 0) in sheet */
    RW_COLS,
    RW_RENAME,      /* sheet gets new_name */
    RW_DROP_SHEET,  /* references to sheet become #REF! */
    RW_SAVE,        /* modern functions get _xlfn. */
};

struct rewrite {
    int mode;
    int dr, dc;
    int sheet;        /* the sheet changed (RW_ROWS ...) */
    int at, count;
    const char *new_name;
    int own_sheet;    /* the sheet of the formula being rewritten */
};

static bool sheet_name_needs_quotes(const char *name) {
    if (name[0] == '\0' || isdigit((unsigned char)name[0])) {
        return true;
    }
    for (const char *c = name; *c != '\0'; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '_'
              || (unsigned char)*c >= 0x80)) {
            return true;
        }
    }
    /* A name that reads as a cell (AB12) or TRUE needs quotes too. */
    struct tok t;
    memset(&t, 0, sizeof(t));
    size_t n = strlen(name);
    if (scan_ref(name, n, 0, &t) == n) {
        return true;
    }
    return false;
}

static void put_sheet_name(struct buf *b, const char *name) {
    if (!sheet_name_needs_quotes(name)) {
        buf_puts(b, name);
        return;
    }
    buf_byte(b, '\'');
    for (const char *c = name; *c != '\0'; c++) {
        if (*c == '\'') {
            buf_byte(b, '\'');
        }
        buf_byte(b, (uint8_t)*c);
    }
    buf_byte(b, '\'');
}

static void put_refpart(struct buf *b, const struct refpart *p) {
    if (p->has_col) {
        if (p->abs_col) {
            buf_byte(b, '$');
        }
        char name[4];
        col_name(p->col, name);
        buf_puts(b, name);
    }
    if (p->has_row) {
        if (p->abs_row) {
            buf_byte(b, '$');
        }
        buf_int(b, p->row + 1);
    }
}

/* Moves a row or column number for an insertion or deletion; false if
   it was deleted. */
static bool shift_line(int *value, int at, int count, int limit) {
    if (count > 0) {
        if (*value >= at) {
            *value += count;
            if (*value >= limit) {
                return false;
            }
        }
        return true;
    }
    int removed = -count;
    if (*value >= at + removed) {
        *value -= removed;
        return true;
    }
    return *value < at;
}

/* Rewrites one reference token; false makes it #REF!. */
static bool rewrite_ref(struct tok *t, const struct rewrite *rw, int sheet) {
    struct refpart *parts[2] = {&t->p1, t->range ? &t->p2 : NULL};
    if (rw->mode == RW_SHIFT) {
        for (int i = 0; i < 2; i++) {
            struct refpart *p = parts[i];
            if (p == NULL) {
                continue;
            }
            if (p->has_row && !p->abs_row) {
                p->row += rw->dr;
                if (p->row < 0 || p->row >= MAX_ROWS) {
                    return false;
                }
            }
            if (p->has_col && !p->abs_col) {
                p->col += rw->dc;
                if (p->col < 0 || p->col >= MAX_COLS) {
                    return false;
                }
            }
        }
        return true;
    }
    if (rw->mode == RW_DROP_SHEET) {
        return sheet != rw->sheet;
    }
    if ((rw->mode != RW_ROWS && rw->mode != RW_COLS) || sheet != rw->sheet) {
        return true;
    }
    bool rows = rw->mode == RW_ROWS;
    int limit = rows ? MAX_ROWS : MAX_COLS;
    if (!t->range) {
        int *v = rows ? &t->p1.row : &t->p1.col;
        if (!(rows ? t->p1.has_row : t->p1.has_col)) {
            return true;
        }
        return shift_line(v, rw->at, rw->count, limit);
    }
    if (!(rows ? t->p1.has_row : t->p1.has_col)) {
        return true; /* A:B is not touched by rows */
    }
    int *a = rows ? &t->p1.row : &t->p1.col;
    int *b = rows ? &t->p2.row : &t->p2.col;
    if (*a > *b) {
        int *swap = a;
        a = b;
        b = swap;
    }
    if (rw->count > 0) {
        /* A range grows when rows are inserted inside it. */
        if (*a >= rw->at) {
            *a += rw->count;
        }
        if (*b >= rw->at) {
            *b += rw->count;
        }
        if (*b >= limit) {
            *b = limit - 1;
        }
        return *a < limit;
    }
    int removed = -rw->count, end = rw->at + removed; /* [at, end) */
    if (*a >= rw->at && *b < end) {
        return false; /* all of it deleted */
    }
    int na = *a < rw->at ? *a : *a >= end ? *a - removed : rw->at;
    int nb = *b < rw->at ? *b : *b >= end ? *b - removed : rw->at - 1;
    *a = na;
    *b = nb;
    return true;
}

/* Rewrites formula text (after '='); returns malloc'd text, or NULL
   when out of memory. */
static char *formula_rewrite(const char *text, const struct rewrite *rw) {
    size_t n = strlen(text);
    struct buf out = {0};
    size_t pos = 0, copied = 0;
    bool last_was_ref = false;
    (void)last_was_ref;
    for (;;) {
        struct tok t;
        next_tok(text, n, &pos, &t);
        if (t.type == T_END) {
            break;
        }
        if (t.type == T_REF) {
            int sheet = t.has_sheet ? tok_sheet(text, &t) : rw->own_sheet;
            buf_put(&out, text + copied, t.start - copied);
            copied = t.end;
            bool keep = sheet == -2 ? true : rewrite_ref(&t, rw, sheet);
            if (t.has_sheet) {
                if (rw->mode == RW_RENAME && sheet == rw->sheet) {
                    put_sheet_name(&out, rw->new_name);
                } else if (sheet >= 0) {
                    put_sheet_name(&out, sheets[sheet]->name);
                } else {
                    buf_put(&out, text + t.start,
                            (t.sheet_quoted ? t.sheet_end + 1 : t.sheet_end)
                            - t.start);
                }
                buf_byte(&out, '!');
            }
            if (!keep) {
                buf_puts(&out, "#REF!");
                continue;
            }
            put_refpart(&out, &t.p1);
            if (t.range) {
                buf_byte(&out, ':');
                put_refpart(&out, &t.p2);
            }
            continue;
        }
        if (t.type == T_FUNC) {
            int func = function_find(text + t.name_start,
                                     t.name_end - t.name_start);
            buf_put(&out, text + copied, t.start - copied);
            copied = t.end;
            if (func < 0) {
                buf_put(&out, text + t.start, t.end - t.start);
                continue;
            }
            if (rw->mode == RW_SAVE && functions[func].modern) {
                buf_puts(&out, "_xlfn.");
            }
            buf_puts(&out, functions[func].name);
            continue;
        }
        if (t.type == T_BOOL) {
            buf_put(&out, text + copied, t.start - copied);
            copied = t.end;
            buf_puts(&out, t.num != 0 ? "TRUE" : "FALSE");
        }
    }
    buf_put(&out, text + copied, n - copied);
    if (out.failed) {
        free(out.data);
        return NULL;
    }
    if (out.data == NULL) {
        return strdup("");
    }
    return out.data;
}

#endif
