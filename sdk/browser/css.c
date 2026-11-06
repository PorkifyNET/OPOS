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
#define MAX_MATCHES 1024
#define MAX_LISTS 96

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

struct css_index;

struct css_sheet {
    char *text;                    /* owns the selector/declaration text */
    struct rule *rules;
    size_t count, capacity;
    uint32_t next_order;
    struct css_sheet *next;
    /* Only on the first sheet of a chain: rules of the whole chain by the
       id, class or tag their last compound needs (see build_index). */
    struct css_index *index;
};

/* Custom properties: each element that declares any gets a scope (a hash
   table of its own declarations) linked to the scope it inherits. */
struct var_entry {
    const char *name;              /* NULL: empty slot */
    size_t length;
    uint32_t hash;
    char *value;                   /* malloc'd */
};

struct var_scope {
    const struct var_scope *parent;
    struct var_entry *entries;
    size_t size, used;             /* size is a power of two */
    struct var_scope *next_owned;  /* every scope of an index, for freeing */
};

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
            /* Custom properties stay in the block: they cascade per element
               (see element_scope). */
            if (n != NULL && im != NULL) {
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
                ok = v <= viewport_width_px;
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
                ok = v >= viewport_width_px;
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

bool css_media_matches(const char *query) {
    return media_matches(query);
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

static void index_free(struct css_index *index);

void css_free(struct css_sheet *sheet) {
    if (sheet != NULL) {
        index_free(sheet->index);
        sheet->index = NULL;
    }
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

/* ----------------------------------------------------------------- index */

/*
 * Every rule is filed under one key taken from the last compound of its
 * selector: its id, else its first class, else its tag name; rules with
 * none of these go to a universal list. An element then only has to test
 * the rules filed under its own id, classes and tag, plus the universal
 * ones, instead of every rule of every sheet.
 */
struct bucket {
    char type;                     /* '#' id, '.' class, 't' tag,
                                      '@' attribute name; 0 empty */
    const char *name;
    size_t length;
    uint32_t hash;
    const struct rule **rules;
    size_t count, capacity;
};

/* Matched rules per element, kept with the index: matching depends only on
   the document and the sheets, not on the layout. */
struct cached {
    const struct opos_html_node *node;
    const struct rule **rules;     /* in cascade order */
    size_t count;
    bool scope_ready;
    const struct var_scope *scope; /* custom properties seen by the node */
};

struct css_index {
    struct bucket *buckets;
    size_t size;                   /* a power of two */
    size_t used;
    const struct rule **universal;
    size_t universal_count, universal_capacity;
    size_t rule_total;             /* rules of the chain when built */
    struct cached *cache;          /* matched rules per element */
    size_t cache_size, cache_used;
    struct var_scope *scopes;      /* every scope built for this index */
};

static uint32_t key_hash(char type, const char *name, size_t length) {
    uint32_t hash = 2166136261u ^ (uint8_t)type;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (type == 't' || type == '@') {
            c = (unsigned char)tolower(c);
        }
        hash = (hash ^ c) * 16777619u;
    }
    return hash;
}

static bool key_equal(const struct bucket *bucket, char type, const char *name,
                      size_t length, uint32_t hash) {
    if (bucket->type != type || bucket->hash != hash
        || bucket->length != length) {
        return false;
    }
    if (type == 't' || type == '@') {
        for (size_t i = 0; i < length; i++) {
            if (tolower((unsigned char)bucket->name[i])
                != tolower((unsigned char)name[i])) {
                return false;
            }
        }
        return true;
    }
    return strncmp(bucket->name, name, length) == 0;
}

static bool push_rule(const struct rule ***list, size_t *count,
                      size_t *capacity, const struct rule *rule) {
    if (*count == *capacity) {
        size_t grown = *capacity == 0 ? 4 : *capacity * 2;
        const struct rule **bigger = realloc(*list, grown * sizeof(**list));
        if (bigger == NULL) {
            return false;
        }
        *list = bigger;
        *capacity = grown;
    }
    (*list)[(*count)++] = rule;
    return true;
}

static struct bucket *index_find(const struct css_index *index, char type,
                                 const char *name, size_t length) {
    if (index->size == 0) {
        return NULL;
    }
    uint32_t hash = key_hash(type, name, length);
    size_t mask = index->size - 1;
    for (size_t at = hash & mask;; at = (at + 1) & mask) {
        struct bucket *bucket = &index->buckets[at];
        if (bucket->type == 0) {
            return NULL;
        }
        if (key_equal(bucket, type, name, length, hash)) {
            return bucket;
        }
    }
}

static bool index_grow(struct css_index *index) {
    size_t size = index->size == 0 ? 256 : index->size * 2;
    struct bucket *buckets = calloc(size, sizeof(*buckets));
    if (buckets == NULL) {
        return false;
    }
    for (size_t i = 0; i < index->size; i++) {
        struct bucket *old = &index->buckets[i];
        if (old->type == 0) {
            continue;
        }
        size_t at = old->hash & (size - 1);
        while (buckets[at].type != 0) {
            at = (at + 1) & (size - 1);
        }
        buckets[at] = *old;
    }
    free(index->buckets);
    index->buckets = buckets;
    index->size = size;
    return true;
}

static bool index_add(struct css_index *index, char type, const char *name,
                      size_t length, const struct rule *rule) {
    struct bucket *bucket = index_find(index, type, name, length);
    if (bucket == NULL) {
        if ((index->used + 1) * 2 > index->size && !index_grow(index)) {
            return false;
        }
        uint32_t hash = key_hash(type, name, length);
        size_t mask = index->size - 1;
        size_t at = hash & mask;
        while (index->buckets[at].type != 0) {
            at = (at + 1) & mask;
        }
        bucket = &index->buckets[at];
        bucket->type = type;
        bucket->name = name;
        bucket->length = length;
        bucket->hash = hash;
        index->used++;
    }
    return push_rule(&bucket->rules, &bucket->count, &bucket->capacity, rule);
}

static void index_free(struct css_index *index) {
    if (index == NULL) {
        return;
    }
    for (size_t i = 0; i < index->size; i++) {
        free(index->buckets[i].rules);
    }
    free(index->buckets);
    free(index->universal);
    for (size_t i = 0; i < index->cache_size; i++) {
        free(index->cache[i].rules);
    }
    free(index->cache);
    while (index->scopes != NULL) {
        struct var_scope *next = index->scopes->next_owned;
        for (size_t i = 0; i < index->scopes->size; i++) {
            free(index->scopes->entries[i].value);
        }
        free(index->scopes->entries);
        free(index->scopes);
        index->scopes = next;
    }
    free(index);
}

static size_t chain_rules(const struct css_sheet *chain) {
    size_t total = 0;
    for (; chain != NULL; chain = chain->next) {
        total += chain->count;
    }
    return total;
}

/* The chain's index, (re)built when sheets were added since. */
static struct css_index *chain_index(struct css_sheet *chain) {
    size_t total = chain_rules(chain);
    if (chain->index != NULL && chain->index->rule_total == total) {
        return chain->index;
    }
    index_free(chain->index);
    chain->index = NULL;
    struct css_index *index = calloc(1, sizeof(*index));
    if (index == NULL) {
        return NULL;
    }
    for (const struct css_sheet *sheet = chain; sheet != NULL;
         sheet = sheet->next) {
        for (size_t i = 0; i < sheet->count; i++) {
            const struct rule *rule = &sheet->rules[i];
            const struct part *last = &rule->parts[rule->part_count - 1];
            bool ok;
            if (last->id != NULL) {
                ok = index_add(index, '#', last->id, last->id_length, rule);
            } else if (last->class_count > 0) {
                ok = index_add(index, '.', last->classes[0],
                               last->class_lengths[0], rule);
            } else if (last->tag != NULL) {
                ok = index_add(index, 't', last->tag, last->tag_length, rule);
            } else if (last->root) {
                ok = index_add(index, 't', "html", 4, rule);
            } else if (last->attr_count > 0) {
                ok = index_add(index, '@', last->attrs[0].name,
                               last->attrs[0].name_length, rule);
            } else {
                ok = push_rule(&index->universal, &index->universal_count,
                               &index->universal_capacity, rule);
            }
            if (!ok) {
                index_free(index);
                return NULL;
            }
        }
    }
    index->rule_total = total;
    chain->index = index;
    return index;
}

/* ---------------------------------------------------------------- applying */

static uint32_t name_hash(const char *name, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++) {
        hash = (hash ^ (unsigned char)name[i]) * 16777619u;
    }
    return hash;
}

static const char *variable_value(const struct var_scope *scope,
                                  const char *name, size_t length) {
    uint32_t hash = name_hash(name, length);
    for (; scope != NULL; scope = scope->parent) {
        size_t mask = scope->size - 1;
        for (size_t at = hash & mask;; at = (at + 1) & mask) {
            const struct var_entry *e = &scope->entries[at];
            if (e->name == NULL) {
                break;
            }
            if (e->hash == hash && e->length == length
                && strncmp(e->name, name, length) == 0) {
                return e->value;
            }
        }
    }
    return NULL;
}

/* Replaces var(--x, fallback) and light-dark(a, b) in a value. */
static void substitute(const struct var_scope *scope, const char *value,
                       char *out, size_t capacity, int depth) {
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
            char piece[400];
            size_t n = (size_t)(first_end - start);
            if (n >= sizeof(piece)) {
                n = sizeof(piece) - 1;
            }
            memcpy(piece, start, n);
            piece[n] = '\0';
            const char *replacement = NULL;
            char fallback[400];
            if (is_var) {
                char *trim = piece;
                while (*trim == ' ') {
                    trim++;
                }
                size_t tl = strlen(trim);
                while (tl > 0 && trim[tl - 1] == ' ') {
                    tl--;
                }
                replacement = variable_value(scope, trim, tl);
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
                char inner[400];
                substitute(scope, replacement, inner, sizeof(inner),
                           depth + 1);
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
        char raw[400];
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
                char value[400];
                substitute(style->vars, raw, value, sizeof(value), 0);
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

static int compare_rule(const void *a, const void *b) {
    const struct rule *x = *(const struct rule *const *)a;
    const struct rule *y = *(const struct rule *const *)b;
    if (x->specificity != y->specificity) {
        return x->specificity < y->specificity ? -1 : 1;
    }
    if (x->order != y->order) {
        return x->order < y->order ? -1 : 1;
    }
    return 0;
}

static struct cached *cache_slot(struct css_index *index,
                                 const struct opos_html_node *node,
                                 bool *found) {
    if ((index->cache_used + 1) * 2 > index->cache_size) {
        size_t size = index->cache_size == 0 ? 1024 : index->cache_size * 2;
        struct cached *table = calloc(size, sizeof(*table));
        if (table == NULL) {
            return NULL;
        }
        for (size_t i = 0; i < index->cache_size; i++) {
            struct cached *old = &index->cache[i];
            if (old->node == NULL) {
                continue;
            }
            size_t at = ((uintptr_t)old->node >> 4) & (size - 1);
            while (table[at].node != NULL) {
                at = (at + 1) & (size - 1);
            }
            table[at] = *old;
        }
        free(index->cache);
        index->cache = table;
        index->cache_size = size;
    }
    size_t mask = index->cache_size - 1;
    for (size_t at = ((uintptr_t)node >> 4) & mask;; at = (at + 1) & mask) {
        struct cached *slot = &index->cache[at];
        if (slot->node == node) {
            *found = true;
            return slot;
        }
        if (slot->node == NULL) {
            *found = false;
            return slot;
        }
    }
}

static void add_list(const struct rule *const **lists, size_t *lengths,
                     size_t *list_count, const struct bucket *bucket) {
    if (bucket != NULL && *list_count < MAX_LISTS) {
        lists[*list_count] = bucket->rules;
        lengths[(*list_count)++] = bucket->count;
    }
}

/* The rules that match element, sorted by specificity and order. */
static struct cached *matched_rules(struct css_index *index,
                                          const struct opos_html_node *element) {
    bool found;
    struct cached *slot = cache_slot(index, element, &found);
    if (slot == NULL) {
        return NULL;
    }
    if (found) {
        return slot;
    }
    /* Candidate lists: universal, tag, id, each class, each attribute. */
    const struct rule *const *lists[MAX_LISTS];
    size_t lengths[MAX_LISTS];
    size_t list_count = 0;
    lists[list_count] = index->universal;
    lengths[list_count++] = index->universal_count;
    add_list(lists, lengths, &list_count,
             index_find(index, 't', element->name, strlen(element->name)));
    const char *id = opos_html_attribute(element, "id");
    if (id != NULL && id[0] != '\0') {
        add_list(lists, lengths, &list_count,
                 index_find(index, '#', id, strlen(id)));
    }
    for (const struct opos_html_attr *attr = element->attributes;
         attr != NULL; attr = attr->next) {
        add_list(lists, lengths, &list_count,
                 index_find(index, '@', attr->name, strlen(attr->name)));
    }
    const char *classes = opos_html_attribute(element, "class");
    const char *seen[64];
    size_t seen_length[64];
    size_t seen_count = 0;
    while (classes != NULL && *classes != '\0') {
        while (*classes == ' ' || *classes == '\t' || *classes == '\n'
               || *classes == '\r' || *classes == '\f') {
            classes++;
        }
        const char *start = classes;
        while (*classes != '\0' && *classes != ' ' && *classes != '\t'
               && *classes != '\n' && *classes != '\r' && *classes != '\f') {
            classes++;
        }
        size_t length = (size_t)(classes - start);
        if (length == 0) {
            break;
        }
        bool duplicate = false;
        for (size_t i = 0; i < seen_count; i++) {
            if (seen_length[i] == length
                && strncmp(seen[i], start, length) == 0) {
                duplicate = true;
            }
        }
        if (duplicate || seen_count == 64) {
            continue;
        }
        seen[seen_count] = start;
        seen_length[seen_count++] = length;
        add_list(lists, lengths, &list_count,
                 index_find(index, '.', start, length));
    }
    static const struct rule *found_rules[MAX_MATCHES];
    size_t count = 0;
    for (size_t l = 0; l < list_count; l++) {
        for (size_t i = 0; i < lengths[l] && count < MAX_MATCHES; i++) {
            const struct rule *rule = lists[l][i];
            if (match_from(element, rule->parts, rule->part_count - 1)) {
                found_rules[count++] = rule;
            }
        }
    }
    qsort(found_rules, count, sizeof(found_rules[0]), compare_rule);
    slot->rules = NULL;
    if (count > 0) {
        slot->rules = malloc(count * sizeof(found_rules[0]));
        if (slot->rules == NULL) {
            return NULL;
        }
        memcpy(slot->rules, found_rules, count * sizeof(found_rules[0]));
    }
    slot->count = count;
    slot->scope_ready = false;
    slot->scope = NULL;
    slot->node = element;
    index->cache_used++;
    return slot;
}

static bool scope_put(struct var_scope *scope, const char *name,
                      size_t length, const char *value, size_t value_length) {
    if ((scope->used + 1) * 2 > scope->size) {
        size_t size = scope->size == 0 ? 16 : scope->size * 2;
        struct var_entry *entries = calloc(size, sizeof(*entries));
        if (entries == NULL) {
            return false;
        }
        for (size_t i = 0; i < scope->size; i++) {
            struct var_entry *old = &scope->entries[i];
            if (old->name == NULL) {
                continue;
            }
            size_t at = old->hash & (size - 1);
            while (entries[at].name != NULL) {
                at = (at + 1) & (size - 1);
            }
            entries[at] = *old;
        }
        free(scope->entries);
        scope->entries = entries;
        scope->size = size;
    }
    uint32_t hash = name_hash(name, length);
    size_t mask = scope->size - 1;
    size_t at = hash & mask;
    while (scope->entries[at].name != NULL
           && !(scope->entries[at].hash == hash
                && scope->entries[at].length == length
                && strncmp(scope->entries[at].name, name, length) == 0)) {
        at = (at + 1) & mask;
    }
    char *copy = malloc(value_length + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, value, value_length);
    copy[value_length] = '\0';
    struct var_entry *e = &scope->entries[at];
    if (e->name == NULL) {
        scope->used++;
    }
    free(e->value);
    e->name = name;
    e->length = length;
    e->hash = hash;
    e->value = copy;
    return true;
}

/* Adds the custom properties declared in a block to *scope (created on
   first use). Later declarations replace earlier ones. */
static void collect_vars(struct css_index *index, struct var_scope **scope,
                         const struct var_scope *parent, const char *text) {
    while (text != NULL && *text != '\0') {
        while (*text == ' ' || *text == '\n' || *text == '\t' || *text == '\r'
               || *text == ';') {
            text++;
        }
        const char *end = text;
        char quote = 0;
        int paren = 0;
        while (*end != '\0' && (quote != 0 || paren > 0 || *end != ';')) {
            if (quote != 0) {
                if (*end == quote) {
                    quote = 0;
                }
            } else if (*end == '"' || *end == '\'') {
                quote = *end;
            } else if (*end == '(' || *end == '{' || *end == '[') {
                paren++;
            } else if ((*end == ')' || *end == '}' || *end == ']')
                       && paren > 0) {
                paren--;
            }
            end++;
        }
        if (text[0] == '-' && text[1] == '-') {
            const char *colon = text;
            while (colon < end && *colon != ':') {
                colon++;
            }
            if (colon < end) {
                size_t name_length = (size_t)(colon - text);
                while (name_length > 0 && text[name_length - 1] == ' ') {
                    name_length--;
                }
                const char *value = colon + 1;
                while (value < end && (*value == ' ' || *value == '\t'
                                       || *value == '\n')) {
                    value++;
                }
                size_t value_length = (size_t)(end - value);
                while (value_length > 0
                       && (value[value_length - 1] == ' '
                           || value[value_length - 1] == '\n')) {
                    value_length--;
                }
                /* "!important" is not part of the value. */
                if (value_length >= 10
                    && strncmp(value + value_length - 10, "!important", 10)
                           == 0) {
                    value_length -= 10;
                    while (value_length > 0 && value[value_length - 1] == ' ') {
                        value_length--;
                    }
                }
                if (*scope == NULL) {
                    *scope = calloc(1, sizeof(**scope));
                    if (*scope == NULL) {
                        return;
                    }
                    (*scope)->parent = parent;
                    (*scope)->next_owned = index->scopes;
                    index->scopes = *scope;
                }
                scope_put(*scope, text, name_length, value, value_length);
            }
        }
        text = *end == '\0' ? end : end + 1;
    }
}

/* The custom properties the element sees: its own declarations (rules in
   cascade order, then its style attribute, then !important ones) over the
   parent's. Cached with the element's rules. */
static const struct var_scope *element_scope(
    struct css_index *index, struct cached *matched,
    const struct opos_html_node *element, const struct var_scope *parent) {
    if (matched->scope_ready) {
        return matched->scope;
    }
    struct var_scope *scope = NULL;
    for (size_t i = 0; i < matched->count; i++) {
        collect_vars(index, &scope, parent, matched->rules[i]->normal);
    }
    collect_vars(index, &scope, parent, opos_html_attribute(element, "style"));
    for (size_t i = 0; i < matched->count; i++) {
        collect_vars(index, &scope, parent, matched->rules[i]->important);
    }
    matched->scope = scope != NULL ? scope : parent;
    matched->scope_ready = true;
    return matched->scope;
}

void css_apply(const struct css_sheet *chain,
               const struct opos_html_node *element, struct style *style,
               const struct style *parent) {
    if (chain == NULL) {
        return;
    }
    struct css_index *index = chain_index((struct css_sheet *)chain);
    if (index == NULL) {
        return;
    }
    struct cached *matched = matched_rules(index, element);
    if (matched == NULL) {
        return;
    }
    style->vars = element_scope(index, matched, element, parent->vars);
    for (size_t i = 0; i < matched->count; i++) {
        if (matched->rules[i]->normal != NULL) {
            css_apply_block(style, matched->rules[i]->normal, parent);
        }
    }
    for (size_t i = 0; i < matched->count; i++) {
        const char *important = matched->rules[i]->important;
        if (important != NULL && important[0] != '\0') {
            css_apply_block(style, important, parent);
        }
    }
}

void css_reset_variables(void) {
    /* Custom properties live in per-element scopes (freed with the
       sheets); nothing global is left to reset. */
}
