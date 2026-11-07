/*
 * OPOS browser: form fields (their current values) and form submission
 * data (application/x-www-form-urlencoded).
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "browser.h"

static bool word_equal(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == *b;
}

enum field_kind form_kind(const struct opos_html_node *node) {
    if (node == NULL || node->type != HTML_NODE_ELEMENT) {
        return FIELD_NONE;
    }
    if (node->tag == HTML_TAG_TEXTAREA) {
        return FIELD_TEXTAREA;
    }
    if (node->tag == HTML_TAG_SELECT) {
        return FIELD_SELECT;
    }
    if (node->tag == HTML_TAG_BUTTON) {
        const char *type = opos_html_attribute(node, "type");
        if (type == NULL || word_equal(type, "submit")) {
            return FIELD_SUBMIT;
        }
        return word_equal(type, "reset") ? FIELD_RESET : FIELD_BUTTON;
    }
    if (node->tag != HTML_TAG_INPUT) {
        return FIELD_NONE;
    }
    const char *type = opos_html_attribute(node, "type");
    if (type == NULL) {
        return FIELD_TEXT;
    }
    static const struct {
        const char *name;
        enum field_kind kind;
    } kinds[] = {
        {"password", FIELD_PASSWORD}, {"checkbox", FIELD_CHECKBOX},
        {"radio", FIELD_RADIO},       {"submit", FIELD_SUBMIT},
        {"image", FIELD_SUBMIT},      {"button", FIELD_BUTTON},
        {"reset", FIELD_RESET},       {"hidden", FIELD_HIDDEN},
        {"file", FIELD_BUTTON},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (word_equal(type, kinds[i].name)) {
            return kinds[i].kind;
        }
    }
    /* text, search, email, url, tel, number, date, ... and unknown types */
    return FIELD_TEXT;
}

/* Calls visit for each <option> of a select in document order (also
   inside <optgroup>). Stops when visit returns true and returns that
   option. */
static const struct opos_html_node *walk_options(
    const struct opos_html_node *node, int *index, int wanted) {
    for (const struct opos_html_node *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        if (child->type != HTML_NODE_ELEMENT) {
            continue;
        }
        if (child->tag == HTML_TAG_OPTION) {
            if (*index == wanted) {
                return child;
            }
            (*index)++;
        } else if (child->tag == HTML_TAG_OPTGROUP) {
            const struct opos_html_node *found = walk_options(child, index,
                                                              wanted);
            if (found != NULL) {
                return found;
            }
        }
    }
    return NULL;
}

const struct opos_html_node *form_option(const struct opos_html_node *select,
                                         int index) {
    int at = 0;
    return index < 0 ? NULL : walk_options(select, &at, index);
}

int form_option_count(const struct opos_html_node *select) {
    int count = 0;
    walk_options(select, &count, -1);
    return count;
}

/* Text of an element's descendants, white space as it is. */
static void collect_text(const struct opos_html_node *node, char **out,
                         size_t *length, size_t *capacity) {
    for (const struct opos_html_node *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        if (child->type == HTML_NODE_TEXT) {
            if (*out == NULL) {
                continue;
            }
            if (*length + child->text_length + 1 > *capacity) {
                size_t grown = (*length + child->text_length + 1) * 2;
                char *bigger = realloc(*out, grown);
                if (bigger == NULL) {
                    free(*out);
                    *out = NULL;
                    continue;
                }
                *out = bigger;
                *capacity = grown;
            }
            memcpy(*out + *length, child->text, child->text_length);
            *length += child->text_length;
            (*out)[*length] = '\0';
        } else {
            collect_text(child, out, length, capacity);
        }
    }
}

bool field_set(struct form_field *field, const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    free(field->value);
    field->value = copy;
    field->length = length;
    if (field->selection_anchor > length) {
        field->selection_anchor = length;
    }
    if (field->selection_cursor > length) {
        field->selection_cursor = length;
    }
    return true;
}

struct form_field *form_field(struct page *page,
                              const struct opos_html_node *node) {
    enum field_kind kind = form_kind(node);
    if (kind == FIELD_NONE) {
        return NULL;
    }
    for (size_t i = 0; i < page->field_count; i++) {
        if (page->fields[i].node == node) {
            return &page->fields[i];
        }
    }
    if (page->field_count == page->field_capacity) {
        size_t capacity = page->field_capacity == 0 ? 16
                        : page->field_capacity * 2;
        struct form_field *grown = realloc(page->fields,
                                           capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        page->fields = grown;
        page->field_capacity = capacity;
    }
    struct form_field *field = &page->fields[page->field_count];
    memset(field, 0, sizeof(*field));
    field->node = node;
    field->kind = kind;
    field->checked = opos_html_attribute(node, "checked") != NULL;
    const char *initial = "";
    char *owned = NULL;
    if (kind == FIELD_TEXTAREA) {
        size_t length = 0, capacity = 64;
        owned = malloc(capacity);
        if (owned != NULL) {
            owned[0] = '\0';
            collect_text(node, &owned, &length, &capacity);
        }
        /* A newline right after <textarea> is not part of the value. */
        if (owned != NULL) {
            initial = owned[0] == '\n' ? owned + 1 : owned;
        }
    } else if (kind == FIELD_SELECT) {
        int count = form_option_count(node);
        for (int i = 0; i < count; i++) {
            if (opos_html_attribute(form_option(node, i), "selected")
                != NULL) {
                field->option = i;
            }
        }
    } else {
        const char *value = opos_html_attribute(node, "value");
        if (value != NULL) {
            initial = value;
        }
    }
    if (!field_set(field, initial, strlen(initial))) {
        free(owned);
        return NULL;
    }
    field->selection_anchor = field->selection_cursor = field->length;
    free(owned);
    page->field_count++;
    return field;
}

void form_fields_free(struct page *page) {
    for (size_t i = 0; i < page->field_count; i++) {
        free(page->fields[i].value);
    }
    free(page->fields);
    page->fields = NULL;
    page->field_count = page->field_capacity = 0;
    page->focus = NULL;
}

const struct opos_html_node *form_of(const struct opos_html_node *node) {
    for (const struct opos_html_node *up = node; up != NULL; up = up->parent) {
        if (up->type == HTML_NODE_ELEMENT && up->tag == HTML_TAG_FORM) {
            return up;
        }
    }
    return NULL;
}

/* Every radio button in the same form with the same name goes off. */
void form_check_radio(struct page *page, const struct opos_html_node *radio) {
    const char *name = opos_html_attribute(radio, "name");
    const struct opos_html_node *form = form_of(radio);
    for (size_t i = 0; i < page->field_count; i++) {
        struct form_field *other = &page->fields[i];
        if (other->kind != FIELD_RADIO) {
            continue;
        }
        if (other->node == radio) {
            other->checked = true;
            continue;
        }
        const char *other_name = opos_html_attribute(other->node, "name");
        if (name != NULL && other_name != NULL && strcmp(name, other_name) == 0
            && form_of(other->node) == form) {
            other->checked = false;
        }
    }
}

/* ------------------------------------------------------------- encoding */

struct buffer {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
};

static void put(struct buffer *out, const char *text, size_t length) {
    if (out->failed) {
        return;
    }
    if (out->length + length + 1 > out->capacity) {
        size_t grown = (out->length + length + 1) * 2;
        char *bigger = realloc(out->data, grown);
        if (bigger == NULL) {
            out->failed = true;
            return;
        }
        out->data = bigger;
        out->capacity = grown;
    }
    memcpy(out->data + out->length, text, length);
    out->length += length;
    out->data[out->length] = '\0';
}

/* application/x-www-form-urlencoded: letters, digits and "*-._" stay,
   space becomes "+", line breaks become CRLF, the rest is %XX. */
static void put_encoded(struct buffer *out, const char *text, size_t length) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (isalnum(ch) || ch == '*' || ch == '-' || ch == '.' || ch == '_') {
            put(out, (const char *)&text[i], 1);
        } else if (ch == ' ') {
            put(out, "+", 1);
        } else if (ch == '\n' && (i == 0 || text[i - 1] != '\r')) {
            put(out, "%0D%0A", 6);
        } else {
            char escaped[3] = {'%', hex[ch >> 4], hex[ch & 15]};
            put(out, escaped, 3);
        }
    }
}

static void put_pair(struct buffer *out, const char *name, const char *value,
                     size_t value_length) {
    if (out->length > 0) {
        put(out, "&", 1);
    }
    put_encoded(out, name, strlen(name));
    put(out, "=", 1);
    put_encoded(out, value, value_length);
}

static void encode_node(struct page *page, const struct opos_html_node *node,
                        const struct opos_html_node *submitter,
                        struct buffer *out) {
    for (const struct opos_html_node *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        if (child->type != HTML_NODE_ELEMENT) {
            continue;
        }
        enum field_kind kind = form_kind(child);
        const char *name = opos_html_attribute(child, "name");
        if (kind == FIELD_NONE || name == NULL || name[0] == '\0'
            || opos_html_attribute(child, "disabled") != NULL) {
            if (kind == FIELD_NONE) {
                encode_node(page, child, submitter, out);
            }
            continue;
        }
        struct form_field *field = form_field(page, child);
        if (field == NULL) {
            out->failed = true;
            return;
        }
        switch (kind) {
        case FIELD_TEXT: case FIELD_PASSWORD: case FIELD_HIDDEN:
        case FIELD_TEXTAREA:
            put_pair(out, name, field->value, field->length);
            break;
        case FIELD_CHECKBOX: case FIELD_RADIO:
            if (field->checked) {
                const char *value = opos_html_attribute(child, "value");
                if (value == NULL) {
                    value = "on";
                }
                put_pair(out, name, value, strlen(value));
            }
            break;
        case FIELD_SELECT: {
            const struct opos_html_node *option = form_option(child,
                                                              field->option);
            if (option == NULL) {
                break;
            }
            const char *value = opos_html_attribute(option, "value");
            if (value != NULL) {
                put_pair(out, name, value, strlen(value));
            } else {
                char *text = malloc(64);
                size_t length = 0, capacity = 64;
                if (text == NULL) {
                    out->failed = true;
                    return;
                }
                text[0] = '\0';
                collect_text(option, &text, &length, &capacity);
                if (text == NULL) {
                    out->failed = true;
                    return;
                }
                /* Trim and collapse white space like the label. */
                size_t used = 0;
                bool space = true;
                for (size_t i = 0; i < length; i++) {
                    char ch = text[i];
                    if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') {
                        if (!space) {
                            text[used++] = ' ';
                        }
                        space = true;
                    } else {
                        text[used++] = ch;
                        space = false;
                    }
                }
                while (used > 0 && text[used - 1] == ' ') {
                    used--;
                }
                put_pair(out, name, text, used);
                free(text);
            }
            break;
        }
        case FIELD_SUBMIT:
            /* Only the button that submitted the form is sent. */
            if (child == submitter) {
                const char *value = opos_html_attribute(child, "value");
                if (value == NULL) {
                    value = "";
                }
                put_pair(out, name, value, strlen(value));
            }
            break;
        default:
            break;
        }
        if (kind == FIELD_SELECT || kind == FIELD_TEXTAREA
            || kind == FIELD_SUBMIT) {
            continue; /* their children are not fields */
        }
        encode_node(page, child, submitter, out);
    }
}

char *form_encode(struct page *page, const struct opos_html_node *form,
                  const struct opos_html_node *submitter, size_t *length) {
    struct buffer out = {NULL, 0, 0, false};
    put(&out, "", 0);
    encode_node(page, form, submitter, &out);
    if (out.failed) {
        free(out.data);
        return NULL;
    }
    *length = out.length;
    return out.data;
}
