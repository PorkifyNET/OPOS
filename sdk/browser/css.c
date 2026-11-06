/*
 * A small CSS engine: parses style sheets, matches selectors and applies
 * the declarations of matching rules in cascade order.
 *
 * Supported: type, class, id, universal and attribute selectors, the
 * descendant, child and sibling combinators, :first-child, :last-child,
 * :link, :root; selector lists; !important; @media (width conditions and
 * screen/print), @supports; var() and light-dark(). Rules using pseudo
 * elements or state pseudo-classes (:hover ...) never match.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "browser.h"

#define MAX_PARTS 12
#define MAX_CLASSES 4
#define MAX_ATTRS 3
#define MAX_VARIABLES 400
#define VIEWPORT_WIDTH 900

enum attr_op { OP_EXISTS, OP_EQUALS, OP_WORD, OP_PREFIX, OP_SUFFIX, OP_SUBSTR,
               OP_DASH };

struct attr_test {
    const char *name;
    size_t name_length;
    enum attr_op op;
    const char *value;
    size_t value_length;
};

struct part {
    char combinator;               /* between this part and the previous one */
    const char *tag;               /* NULL: any */
    size_t tag_length;
    const char *id;
    size_t id_length;
    const char *classes[MAX_CLASSES];
    size_t class_lengths[MAX_CLASSES];
    int class_count;
    struct attr_test attrs[MAX_ATTRS];
    int attr_count;
    bool first_child, last_child, link, root;
};

struct rule {
    struct part *parts;
    int part_count;
    uint32_t specificity;
    uint32_t order;
    const char *normal;            /* declarations, NUL-terminated */
    const char *important;
};

struct css_sheet {
    char *text;                    /* owns the selector/declaration text */
    struct rule *rules;
    size_t count, capacity;
    uint32_t next_order;
    struct css_sheet *next;
};

struct variable {
    char name[40];
    char *value;
};

static struct variable variables[MAX_VARIABLES];
static int variable_count;

/* -------------------------------------------------------------- tokenising */

static void strip_comments(char *text) {
    char *in = text, *out = text;
    while (*in != '\0') {
        if (in[0] == '/' && in[1] == '*') {
            in += 2;
            while (*in != '\0' && !(in[0] == '*' && in[1] == '/')) {
                in++;
            }
            if (*in != '\0') {
                in += 2;
            }
            *out++ = ' ';
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

static char *skip_blank(char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'
           || *p == '\f') {
        p++;
    }
    return p;
}

/* Finds the matching close of a block whose '{' is at p. */
static char *block_end(char *p) {
    int depth = 0;
    char quote = 0;
    for (; *p != '\0'; p++) {
        if (quote != 0) {
            if (*p == '\\' && p[1] != '\0') {
                p++;
            } else if (*p == quote) {
                quote = 0;
            }
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            if (--depth == 0) {
                return p;
            }
        }
    }
    return p;
}

static bool is_ident(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_'
        || (unsigned char)c >= 0x80 || c == '\\';
}

/* ------------------------------------------------------------ selectors */

static bool parse_compound(char **cursor, struct part *part) {
    char *p = *cursor;
    memset(part, 0, sizeof(*part));
    bool any = false;
    for (;;) {
        char c = *p;
        if (c == '*') {
            p++;
            any = true;
        } else if (is_ident(c) && !any && part->tag == NULL
                   && part->id == NULL && part->class_count == 0
                   && part->attr_count == 0) {
            char *start = p;
            while (is_ident(*p)) {
                p++;
            }
            part->tag = start;
            part->tag_length = (size_t)(p - start);
            any = true;
        } else if (c == '#' || c == '.') {
            char *start = ++p;
            while (is_ident(*p)) {
                p++;
            }
            if (p == start) {
                return false;
            }
            if (c == '#') {
                part->id = start;
                part->id_length = (size_t)(p - start);
            } else {
                if (part->class_count == MAX_CLASSES) {
                    return false;
                }
                part->classes[part->class_count] = start;
                part->class_lengths[part->class_count++] = (size_t)(p - start);
            }
            any = true;
        } else if (c == '[') {
            if (part->attr_count == MAX_ATTRS) {
                return false;
            }
            struct attr_test *test = &part->attrs[part->attr_count++];
            p = skip_blank(p + 1);
            char *start = p;
            while (is_ident(*p)) {
                p++;
            }
            test->name = start;
            test->name_length = (size_t)(p - start);
            p = skip_blank(p);
            test->op = OP_EXISTS;
            if (*p == '=') {
                test->op = OP_EQUALS;
                p++;
            } else if (p[1] == '=' && strchr("~^$*|", *p) != NULL) {
                test->op = *p == '~' ? OP_WORD : *p == '^' ? OP_PREFIX
                         : *p == '$' ? OP_SUFFIX : *p == '*' ? OP_SUBSTR
                         : OP_DASH;
                p += 2;
            }
            if (test->op != OP_EXISTS) {
                p = skip_blank(p);
                char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
                start = p;
                while (*p != '\0' && (quote != 0 ? *p != quote
                                      : (*p != ']' && *p != ' '))) {
                    p++;
                }
                test->value = start;
                test->value_length = (size_t)(p - start);
                if (quote != 0 && *p == quote) {
                    p++;
                }
                p = skip_blank(p);
                if ((*p == 'i' || *p == 'I') && p[1] != '\0') {
                    p = skip_blank(p + 1);
                }
            }
            if (*p != ']') {
                return false;
            }
            p++;
            any = true;
        } else if (c == ':') {
            if (p[1] == ':') {
                return false; /* pseudo-element */
            }
            char *start = ++p;
            while (is_ident(*p)) {
                p++;
            }
            size_t n = (size_t)(p - start);
            if (n == 11 && strncmp(start, "first-child", 11) == 0) {
                part->first_child = true;
            } else if (n == 10 && strncmp(start, "last-child", 10) == 0) {
                part->last_child = true;
            } else if ((n == 4 && strncmp(start, "link", 4) == 0)
                       || (n == 8 && strncmp(start, "any-link", 8) == 0)) {
                part->link = true;
            } else if (n == 4 && strncmp(start, "root", 4) == 0) {
                part->root = true;
            } else {
                return false;
            }
            any = true;
        } else {
            break;
        }
    }
    *cursor = p;
    return any;
}

/* Parses one selector (no commas) ending before `end`. */
static bool parse_selector(char *start, char *end, struct part *parts,
                           int *count, uint32_t *specificity) {
    char saved = *end;
    *end = '\0';
    char *p = skip_blank(start);
    int n = 0;
    uint32_t a = 0, b = 0, c = 0;
    char combinator = 0;
    bool ok = *p != '\0';
    while (ok && *p != '\0') {
        if (n == MAX_PARTS) {
            ok = false;
            break;
        }
        if (!parse_compound(&p, &parts[n])) {
            ok = false;
            break;
        }
        parts[n].combinator = combinator;
        a += parts[n].id != NULL;
        b += (uint32_t)parts[n].class_count + (uint32_t)parts[n].attr_count
           + parts[n].first_child + parts[n].last_child + parts[n].link
           + parts[n].root;
        c += parts[n].tag != NULL;
        n++;
        char *after = skip_blank(p);
        combinator = 0;
        if (*after == '>' || *after == '+' || *after == '~') {
            combinator = *after;
            after = skip_blank(after + 1);
        } else if (after != p && *after != '\0') {
            combinator = ' ';
        }
        if (combinator != 0 && *after == '\0') {
            ok = false;
        }
        p = after;
    }
    *end = saved;
    *count = n;
    *specificity = a << 16 | b << 8 | c;
    return ok && n > 0;
}

/* ----------------------------------------------------------- sheet storage */

static bool add_rule(struct css_sheet *sheet, struct rule *rule) {
    if (sheet->count == sheet->capacity) {
        size_t capacity = sheet->capacity == 0 ? 128 : sheet->capacity * 2;
        struct rule *grown = realloc(sheet->rules, capacity * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        sheet->rules = grown;
        sheet->capacity = capacity;
    }
    sheet->rules[sheet->count++] = *rule;
    return true;
}

static void store_variable(const char *name, size_t name_length,
                           const char *value) {
    if (name_length >= sizeof(variables[0].name)) {
        return;
    }
    for (int i = 0; i < variable_count; i++) {
        if (strlen(variables[i].name) == name_length
            && strncmp(variables[i].name, name, name_length) == 0) {
            return; /* the first definition wins (the light theme) */
        }
    }
    if (variable_count == MAX_VARIABLES) {
        return;
    }
    memcpy(variables[variable_count].name, name, name_length);
    variables[variable_count].name[name_length] = '\0';
    variables[variable_count].value = strdup(value);
    variable_count++;
}

/* Splits a declaration block (modified in place) into the normal and the
   !important parts; custom properties are recorded. The two outputs point
   into freshly allocated space inside `sheet->text`-independent heap, so
   they are copied. */
static void split_block(char *block, char **normal, char **important) {
    size_t length = strlen(block);
    char *n = malloc(length + 2);
    char *im = malloc(length + 2);
    size_t nu = 0, iu = 0;
    char *p = block;
    while (*p != '\0') {
        char *end = p;
        char quote = 0;
        int paren = 0;
        while (*end != '\0' && (quote != 0 || paren > 0 || *end != ';')) {
            if (quote != 0) {
                if (*end == quote) {
                    quote = 0;
                }
            } else if (*end == '"' || *end == '\'') {
                quote = *end;
            } else if (*end == '(') {
                paren++;
            } else if (*end == ')' && paren > 0) {
                paren--;
            }
            end++;
        }
        char saved = *end;
        *end = '\0';
        char *colon = strchr(p, ':');
        if (colon != NULL) {
            char *name = skip_blank(p);
            size_t name_length = (size_t)(colon - name);
            while (name_length > 0 && name[name_length - 1] == ' ') {
                name_length--;
            }
            char *value = skip_blank(colon + 1);
            if (name_length > 2 && name[0] == '-' && name[1] == '-') {
                store_variable(name, name_length, value);
            } else if (n != NULL && im != NULL) {
                char *bang = strstr(value, "!important");
                size_t span = strlen(p);
                if (bang != NULL) {
                    memcpy(im + iu, p, span);
                    iu += span;
                    im[iu++] = ';';
                } else {
                    memcpy(n + nu, p, span);
                    nu += span;
                    n[nu++] = ';';
                }
            }
        }
        *end = saved;
        p = saved == '\0' ? end : end + 1;
    }
    if (n != NULL) {
        n[nu] = '\0';
    }
    if (im != NULL) {
        im[iu] = '\0';
    }
    *normal = n;
    *important = im;
}

static bool media_matches(const char *query) {
    /* Comma-separated list: any alternative may match. */
    char copy[256];
    {
        size_t n = strlen(query);
        if (n >= sizeof(copy)) {
            n = sizeof(copy) - 1;
        }
        memcpy(copy, query, n);
        copy[n] = '\0';
    }
    for (char *c = copy; *c != '\0'; c++) {
        *c = (char)tolower((unsigned char)*c);
    }
    char *alternative = copy;
    for (;;) {
        char *comma = strchr(alternative, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        bool ok = strstr(alternative, "print") == NULL
               && strstr(alternative, "speech") == NULL
               && strstr(alternative, "dark") == NULL
               && strstr(alternative, "portrait") == NULL
               && strstr(alternative, "reduce") == NULL
               && strstr(alternative, "hover: none") == NULL
               && strstr(alternative, "hover:none") == NULL
               && strstr(alternative, "pointer: coarse") == NULL
               && strstr(alternative, "pointer:coarse") == NULL;
        const char *f;
        if (ok && (f = strstr(alternative, "min-width")) != NULL) {
            const char *colon = strchr(f, ':');
            if (colon != NULL) {
                int v = atoi(colon + 1);
                if (strstr(colon, "em") != NULL
                    && strstr(colon, "em") < strchr(colon, ')')) {
                    v *= 16;
                }
                ok = v <= VIEWPORT_WIDTH;
            }
        }
        if (ok && (f = strstr(alternative, "max-width")) != NULL) {
            const char *colon = strchr(f, ':');
            if (colon != NULL) {
                int v = atoi(colon + 1);
                if (strstr(colon, "em") != NULL
                    && strstr(colon, "em") < strchr(colon, ')')) {
                    v *= 16;
                }
                ok = v >= VIEWPORT_WIDTH;
            }
        }
        if (ok) {
            return true;
        }
        if (comma == NULL) {
            return false;
        }
        alternative = comma + 1;
    }
}

static void parse_rules(struct css_sheet *sheet, char *p, char *limit);

static void parse_rules(struct css_sheet *sheet, char *p, char *limit) {
    while (p < limit) {
        p = skip_blank(p);
        if (p >= limit || *p == '\0') {
            return;
        }
        if (*p == '@') {
            char *name = p + 1;
            char *q = name;
            while (isalpha((unsigned char)*q) || *q == '-') {
                q++;
            }
            size_t name_length = (size_t)(q - name);
            while (*q != '\0' && *q != '{' && *q != ';') {
                q++;
            }
            if (*q == ';' || *q == '\0') {
                p = *q == '\0' ? q : q + 1;
                continue;
            }
            char *close = block_end(q);
            bool descend = false;
            if (name_length == 5 && strncmp(name, "media", 5) == 0) {
                char saved = *q;
                *q = '\0';
                descend = media_matches(name + 5);
                *q = saved;
            } else if ((name_length == 8 && strncmp(name, "supports", 8) == 0)
                       || (name_length == 5 && strncmp(name, "layer", 5) == 0)
                       || (name_length == 9
                           && strncmp(name, "container", 9) == 0)
                       || (name_length == 5
                           && strncmp(name, "scope", 5) == 0)) {
                descend = true;
            }
            if (descend) {
                char saved = *close;
                *close = '\0';
                parse_rules(sheet, q + 1, close);
                *close = saved;
            }
            p = *close == '\0' ? close : close + 1;
            continue;
        }
        char *open = p;
        while (*open != '\0' && *open != '{') {
            open++;
        }
        if (*open == '\0') {
            return;
        }
        char *close = block_end(open);
        char *selectors = p;
        char *selectors_end = open;
        *open = '\0';
        char saved_close = *close;
        *close = '\0';
        char *normal, *important;
        split_block(open + 1, &normal, &important);
        bool used = false;
        /* Selector list. */
        char *s = selectors;
        for (;;) {
            char *comma = s;
            int paren = 0, bracket = 0;
            while (*comma != '\0' && (paren > 0 || bracket > 0
                                      || *comma != ',')) {
                paren += *comma == '(';
                paren -= *comma == ')' && paren > 0;
                bracket += *comma == '[';
                bracket -= *comma == ']' && bracket > 0;
                comma++;
            }
            struct part parts[MAX_PARTS];
            int count;
            uint32_t specificity;
            if (parse_selector(s, comma, parts, &count, &specificity)) {
                struct rule rule;
                rule.parts = malloc((size_t)count * sizeof(struct part));
                if (rule.parts != NULL) {
                    memcpy(rule.parts, parts, (size_t)count * sizeof(struct part));
                    rule.part_count = count;
                    rule.specificity = specificity;
                    rule.order = sheet->next_order++;
                    rule.normal = normal;
                    rule.important = important;
                    if (add_rule(sheet, &rule)) {
                        used = true;
                    } else {
                        free(rule.parts);
                    }
                }
            }
            if (*comma == '\0') {
                break;
            }
            s = comma + 1;
        }
        (void)selectors_end;
        if (!used) {
            free(normal);
            free(important);
        }
        /* normal/important strings are shared by the rules of the list and
           intentionally kept for the life of the sheet. */
        *close = saved_close;
        p = saved_close == '\0' ? close : close + 1;
    }
}

struct css_sheet *css_parse(const char *text, size_t length) {
    struct css_sheet *sheet = calloc(1, sizeof(*sheet));
    if (sheet == NULL) {
        return NULL;
    }
    sheet->text = malloc(length + 1);
    if (sheet->text == NULL) {
        free(sheet);
        return NULL;
    }
    memcpy(sheet->text, text, length);
    sheet->text[length] = '\0';
    strip_comments(sheet->text);
    {
        char *w = sheet->text;
        if ((unsigned char)w[0] == 0xef && (unsigned char)w[1] == 0xbb
            && (unsigned char)w[2] == 0xbf) {
            memmove(w, w + 3, strlen(w + 3) + 1);
        }
        for (char *p = w; *p != '\0'; p++) {
            if (strncmp(p, "<!--", 4) == 0) {
                memset(p, ' ', 4);
            } else if (strncmp(p, "-->", 3) == 0) {
                memset(p, ' ', 3);
            }
        }
    }
    parse_rules(sheet, sheet->text, sheet->text + strlen(sheet->text));
    return sheet;
}

void css_free(struct css_sheet *sheet) {
    while (sheet != NULL) {
        struct css_sheet *next = sheet->next;
        const char *last_normal = NULL;
        for (size_t i = 0; i < sheet->count; i++) {
            free(sheet->rules[i].parts);
            if (sheet->rules[i].normal != last_normal) {
                free((void *)sheet->rules[i].normal);
                free((void *)sheet->rules[i].important);
                last_normal = sheet->rules[i].normal;
            }
        }
        free(sheet->rules);
        free(sheet->text);
        free(sheet);
        sheet = next;
    }
}

/* Appends later (higher priority) to the end of the chain. */
struct css_sheet *css_append(struct css_sheet *chain, struct css_sheet *sheet) {
    if (sheet == NULL) {
        return chain;
    }
    uint32_t base = 0;
    struct css_sheet *tail = chain;
    while (tail != NULL) {
        base += tail->next_order;
        if (tail->next == NULL) {
            break;
        }
        tail = tail->next;
    }
    for (size_t i = 0; i < sheet->count; i++) {
        sheet->rules[i].order += base;
    }
    if (tail == NULL) {
        return sheet;
    }
    tail->next = sheet;
    return chain;
}

/* --------------------------------------------------------------- matching */

static bool name_equals(const char *a, size_t a_length, const char *b) {
    if (strlen(b) != a_length) {
        return false;
    }
    for (size_t i = 0; i < a_length; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

static bool has_class(const struct opos_html_node *node, const char *name,
                      size_t length) {
    const char *list = opos_html_attribute(node, "class");
    if (list == NULL) {
        return false;
    }
    while (*list != '\0') {
        while (*list == ' ' || *list == '\t' || *list == '\n') {
            list++;
        }
        const char *start = list;
        while (*list != '\0' && *list != ' ' && *list != '\t'
               && *list != '\n') {
            list++;
        }
        if ((size_t)(list - start) == length
            && strncmp(start, name, length) == 0) {
            return true;
        }
    }
    return false;
}

static bool attr_matches(const struct opos_html_node *node,
                         const struct attr_test *test) {
    char name[40];
    if (test->name_length >= sizeof(name)) {
        return false;
    }
    for (size_t i = 0; i < test->name_length; i++) {
        name[i] = (char)tolower((unsigned char)test->name[i]);
    }
    name[test->name_length] = '\0';
    const char *value = opos_html_attribute(node, name);
    if (value == NULL) {
        return false;
    }
    if (test->op == OP_EXISTS) {
        return true;
    }
    size_t n = strlen(value), t = test->value_length;
    switch (test->op) {
    case OP_EQUALS:
        return n == t && strncmp(value, test->value, t) == 0;
    case OP_PREFIX:
        return t > 0 && n >= t && strncmp(value, test->value, t) == 0;
    case OP_SUFFIX:
        return t > 0 && n >= t && strncmp(value + n - t, test->value, t) == 0;
    case OP_SUBSTR:
        if (t == 0) {
            return false;
        }
        for (size_t i = 0; i + t <= n; i++) {
            if (strncmp(value + i, test->value, t) == 0) {
                return true;
            }
        }
        return false;
    case OP_DASH:
        return n >= t && strncmp(value, test->value, t) == 0
            && (n == t || value[t] == '-');
    case OP_WORD: {
        const char *p = value;
        while (*p != '\0') {
            while (*p == ' ') {
                p++;
            }
            const char *start = p;
            while (*p != '\0' && *p != ' ') {
                p++;
            }
            if ((size_t)(p - start) == t && strncmp(start, test->value, t) == 0) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

static const struct opos_html_node *previous_element(
    const struct opos_html_node *node) {
    for (node = node->previous_sibling; node != NULL;
         node = node->previous_sibling) {
        if (node->type == HTML_NODE_ELEMENT) {
            return node;
        }
    }
    return NULL;
}

static bool next_element_exists(const struct opos_html_node *node) {
    for (node = node->next_sibling; node != NULL; node = node->next_sibling) {
        if (node->type == HTML_NODE_ELEMENT) {
            return true;
        }
    }
    return false;
}

static bool part_matches(const struct opos_html_node *node,
                         const struct part *part) {
    if (node->type != HTML_NODE_ELEMENT) {
        return false;
    }
    if (part->tag != NULL && !name_equals(part->tag, part->tag_length,
                                          node->name)) {
        return false;
    }
    if (part->id != NULL) {
        const char *id = opos_html_attribute(node, "id");
        if (id == NULL || strlen(id) != part->id_length
            || strncmp(id, part->id, part->id_length) != 0) {
            return false;
        }
    }
    for (int i = 0; i < part->class_count; i++) {
        if (!has_class(node, part->classes[i], part->class_lengths[i])) {
            return false;
        }
    }
    for (int i = 0; i < part->attr_count; i++) {
        if (!attr_matches(node, &part->attrs[i])) {
            return false;
        }
    }
    if (part->first_child && previous_element(node) != NULL) {
        return false;
    }
    if (part->last_child && next_element_exists(node)) {
        return false;
    }
    if (part->link && (node->tag != HTML_TAG_A
                       || opos_html_attribute(node, "href") == NULL)) {
        return false;
    }
    if (part->root && node->tag != HTML_TAG_HTML) {
        return false;
    }
    return true;
}

static bool match_from(const struct opos_html_node *node,
                       const struct part *parts, int index) {
    if (!part_matches(node, &parts[index])) {
        return false;
    }
    if (index == 0) {
        return true;
    }
    char combinator = parts[index].combinator;
    const struct opos_html_node *other;
    switch (combinator) {
    case '>':
        other = node->parent;
        return other != NULL && match_from(other, parts, index - 1);
    case '+':
        other = previous_element(node);
        return other != NULL && match_from(other, parts, index - 1);
    case '~':
        for (other = previous_element(node); other != NULL;
             other = previous_element(other)) {
            if (match_from(other, parts, index - 1)) {
                return true;
            }
        }
        return false;
    default:
        for (other = node->parent; other != NULL; other = other->parent) {
            if (match_from(other, parts, index - 1)) {
                return true;
            }
        }
        return false;
    }
}

/* ---------------------------------------------------------------- applying */

static const char *variable_value(const char *name, size_t length) {
    for (int i = variable_count - 1; i >= 0; i--) {
        if (strlen(variables[i].name) == length
            && strncmp(variables[i].name, name, length) == 0) {
            return variables[i].value;
        }
    }
    return NULL;
}

/* Replaces var(--x, fallback) and light-dark(a, b) in a value. */
static void substitute(const char *value, char *out, size_t capacity,
                       int depth) {
    size_t used = 0;
    while (*value != '\0' && used + 1 < capacity) {
        bool is_var = strncmp(value, "var(", 4) == 0;
        bool is_ld = strncmp(value, "light-dark(", 11) == 0;
        if ((is_var || is_ld) && depth < 4) {
            const char *p = value + (is_var ? 4 : 11);
            const char *start = p;
            int paren = 1;
            const char *comma = NULL;
            while (*p != '\0' && paren > 0) {
                if (*p == '(') {
                    paren++;
                } else if (*p == ')') {
                    paren--;
                } else if (*p == ',' && paren == 1 && comma == NULL) {
                    comma = p;
                }
                p++;
            }
            const char *argument_end = paren == 0 ? p - 1 : p;
            const char *first_end = comma != NULL ? comma : argument_end;
            char piece[160];
            size_t n = (size_t)(first_end - start);
            if (n >= sizeof(piece)) {
                n = sizeof(piece) - 1;
            }
            memcpy(piece, start, n);
            piece[n] = '\0';
            const char *replacement = NULL;
            char fallback[160];
            if (is_var) {
                char *trim = piece;
                while (*trim == ' ') {
                    trim++;
                }
                size_t tl = strlen(trim);
                while (tl > 0 && trim[tl - 1] == ' ') {
                    tl--;
                }
                replacement = variable_value(trim, tl);
                if (replacement == NULL && comma != NULL) {
                    size_t f = (size_t)(argument_end - comma - 1);
                    if (f >= sizeof(fallback)) {
                        f = sizeof(fallback) - 1;
                    }
                    memcpy(fallback, comma + 1, f);
                    fallback[f] = '\0';
                    replacement = fallback;
                }
            } else {
                replacement = piece;
            }
            if (replacement != NULL) {
                char inner[200];
                substitute(replacement, inner, sizeof(inner), depth + 1);
                for (size_t i = 0; inner[i] != '\0' && used + 1 < capacity; i++) {
                    out[used++] = inner[i];
                }
            }
            value = p;
            continue;
        }
        out[used++] = *value++;
    }
    out[used] = '\0';
}

void css_apply_block(struct style *style, const char *text,
                     const struct style *parent) {
    while (*text != '\0') {
        while (*text == ' ' || *text == '\n' || *text == '\t'
               || *text == '\r' || *text == ';') {
            text++;
        }
        const char *colon = text;
        while (*colon != '\0' && *colon != ':' && *colon != ';') {
            colon++;
        }
        if (*colon != ':') {
            if (*colon == '\0') {
                return;
            }
            text = colon + 1;
            continue;
        }
        const char *end = colon + 1;
        char quote = 0;
        int paren = 0;
        while (*end != '\0' && (quote != 0 || paren > 0 || *end != ';')) {
            if (quote != 0) {
                if (*end == quote) {
                    quote = 0;
                }
            } else if (*end == '"' || *end == '\'') {
                quote = *end;
            } else if (*end == '(') {
                paren++;
            } else if (*end == ')' && paren > 0) {
                paren--;
            }
            end++;
        }
        size_t name_length = (size_t)(colon - text);
        while (name_length > 0 && text[name_length - 1] == ' ') {
            name_length--;
        }
        char raw[300];
        size_t n = (size_t)(end - colon - 1);
        if (n >= sizeof(raw)) {
            n = sizeof(raw) - 1;
        }
        memcpy(raw, colon + 1, n);
        raw[n] = '\0';
        if (name_length > 0 && !(name_length > 1 && text[0] == '-'
                                 && text[1] == '-')) {
            if (strstr(raw, "var(") != NULL || strstr(raw, "light-dark(")
                                                    != NULL) {
                char value[300];
                substitute(raw, value, sizeof(value), 0);
                apply_declaration(style, text, name_length, value, parent);
            } else {
                apply_declaration(style, text, name_length, raw, parent);
            }
        }
        if (*end == '\0') {
            return;
        }
        text = end + 1;
    }
}

struct match {
    const struct rule *rule;
    uint32_t key;
};

static int compare_match(const void *a, const void *b) {
    const struct match *x = a, *y = b;
    if (x->rule->specificity != y->rule->specificity) {
        return x->rule->specificity < y->rule->specificity ? -1 : 1;
    }
    if (x->rule->order != y->rule->order) {
        return x->rule->order < y->rule->order ? -1 : 1;
    }
    return 0;
}

void css_apply(const struct css_sheet *chain,
               const struct opos_html_node *element, struct style *style,
               const struct style *parent) {
    if (chain == NULL) {
        return;
    }
    struct match matches[512];
    size_t count = 0;
    for (const struct css_sheet *sheet = chain; sheet != NULL;
         sheet = sheet->next) {
        for (size_t i = 0; i < sheet->count && count < 512; i++) {
            const struct rule *rule = &sheet->rules[i];
            if (match_from(element, rule->parts, rule->part_count - 1)) {
                matches[count].rule = rule;
                matches[count].key = rule->order;
                count++;
            }
        }
    }
    if (count == 0) {
        return;
    }
    for (size_t i = 1; i < count; i++) {
        struct match held = matches[i];
        size_t j = i;
        while (j > 0 && compare_match(&matches[j - 1], &held) > 0) {
            matches[j] = matches[j - 1];
            j--;
        }
        matches[j] = held;
    }
    for (size_t i = 0; i < count; i++) {
        if (matches[i].rule->normal != NULL) {
            css_apply_block(style, matches[i].rule->normal, parent);
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (matches[i].rule->important != NULL
            && matches[i].rule->important[0] != '\0') {
            css_apply_block(style, matches[i].rule->important, parent);
        }
    }
}

void css_reset_variables(void) {
    for (int i = 0; i < variable_count; i++) {
        free(variables[i].value);
    }
    variable_count = 0;
}
