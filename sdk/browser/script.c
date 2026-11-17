/*
 * JavaScript for web pages: runs the page's scripts in QuickJS
 * (sdk/third_party/quickjs, used unchanged) and gives them a DOM.
 *
 * Each page gets its own QuickJS runtime and context, made when the page
 * is shown and freed with it. The DOM API itself is written in JavaScript
 * (dom.js, built into the program); this file gives it a small set of
 * native calls ("N.*") over the document tree of opos_html.h. Every node
 * a script sees has one wrapper object, kept until the page goes away, so
 * the same node is always the same object.
 *
 * Scripts change the tree directly; the browser lays the page out again
 * after each script, timer or event that changed it (script_take_dirty).
 * Navigation asked for by a script (location.href = ...) waits until the
 * script has finished (script_take_navigation).
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>
#include <opos_http.h>

#include "browser.h"
#include "quickjs.h"

#define SCRIPT_MAX_BYTES (4u << 20)   /* one external script */
#define SCRIPT_TIME_MS 5000           /* one script, timer or event */
#define JS_MEMORY_LIMIT (384u << 20)
#define JS_STACK_LIMIT (1u << 20)   /* of the 4 MiB stack (opos_stack_size) */
#define MAX_PENDING 128
#define LOG_MAX 64
#define JS_FETCH_MAX_BYTES (16u << 20) /* one fetch or XMLHttpRequest */
#define STORAGE_MAX_BYTES (5u << 20)  /* localStorage of one site */

#ifndef DOM_JS_PATH
#define DOM_JS_PATH "browser/dom.js"
#endif

/* dom.js, built in with a terminating zero. */
__asm__(".section .rodata\n"
        ".global browser_dom_js\n"
        "browser_dom_js:\n"
        ".incbin \"" DOM_JS_PATH "\"\n"
        ".byte 0\n"
        ".global browser_dom_js_end\n"
        "browser_dom_js_end:\n"
        ".previous\n");
extern const char browser_dom_js[];
extern const char browser_dom_js_end[];

/* ------------------------------------------------------------ allocator */

/*
 * QuickJS makes many small blocks. The SDK malloc keeps one address-sorted
 * free list, which gets slow with hundreds of thousands of blocks, so
 * blocks up to 1 KiB come from per-size free lists instead. Each block has
 * a 16-byte header with its size class; class LARGE blocks come from
 * malloc and keep their size in the header.
 */
#define POOL_STEP 16
#define POOL_CLASSES 64              /* 16 .. 1024 bytes */
#define POOL_CHUNK (256u << 10)
#define LARGE 0xffffffffu

struct pool_header {
    uint32_t size_class;
    uint32_t unused;
    size_t large_size;
};

_Static_assert(sizeof(struct pool_header) == 16, "pool header size");

static void *pool_free_lists[POOL_CLASSES];
static char *pool_chunk;
static size_t pool_chunk_left;

static size_t pool_usable(const void *ptr) {
    const struct pool_header *h = (const struct pool_header *)ptr - 1;
    return h->size_class == LARGE ? h->large_size
                                  : (size_t)(h->size_class + 1) * POOL_STEP;
}

static void *pool_get(size_t size) {
    if (size == 0) {
        size = 1;
    }
    if (size > POOL_CLASSES * POOL_STEP) {
        struct pool_header *h = malloc(sizeof(*h) + size);
        if (h == NULL) {
            return NULL;
        }
        h->size_class = LARGE;
        h->large_size = size;
        return h + 1;
    }
    uint32_t c = (uint32_t)((size - 1) / POOL_STEP);
    void *block = pool_free_lists[c];
    if (block != NULL) {
        pool_free_lists[c] = *(void **)block;
        return block;
    }
    size_t need = sizeof(struct pool_header) + (size_t)(c + 1) * POOL_STEP;
    if (pool_chunk_left < need) {
        pool_chunk = opos_alloc(POOL_CHUNK);
        if (pool_chunk == NULL) {
            pool_chunk_left = 0;
            return NULL;
        }
        pool_chunk_left = POOL_CHUNK;
    }
    struct pool_header *h = (struct pool_header *)pool_chunk;
    pool_chunk += need;
    pool_chunk_left -= need;
    h->size_class = c;
    return h + 1;
}

static void pool_put(void *ptr) {
    struct pool_header *h = (struct pool_header *)ptr - 1;
    if (h->size_class == LARGE) {
        free(h);
        return;
    }
    *(void **)ptr = pool_free_lists[h->size_class];
    pool_free_lists[h->size_class] = ptr;
}

static void *js_pool_malloc(JSMallocState *s, size_t size) {
    if (s->malloc_size + size > s->malloc_limit) {
        return NULL;
    }
    void *ptr = pool_get(size);
    if (ptr != NULL) {
        s->malloc_count++;
        s->malloc_size += pool_usable(ptr) + sizeof(struct pool_header);
    }
    return ptr;
}

static void js_pool_free(JSMallocState *s, void *ptr) {
    if (ptr == NULL) {
        return;
    }
    s->malloc_count--;
    s->malloc_size -= pool_usable(ptr) + sizeof(struct pool_header);
    pool_put(ptr);
}

static void *js_pool_realloc(JSMallocState *s, void *ptr, size_t size) {
    if (ptr == NULL) {
        return size == 0 ? NULL : js_pool_malloc(s, size);
    }
    if (size == 0) {
        js_pool_free(s, ptr);
        return NULL;
    }
    size_t old = pool_usable(ptr);
    if (size <= old && (old <= POOL_CLASSES * POOL_STEP || size > old / 2)) {
        return ptr;
    }
    void *fresh = js_pool_malloc(s, size);
    if (fresh == NULL) {
        return NULL;
    }
    memcpy(fresh, ptr, old < size ? old : size);
    js_pool_free(s, ptr);
    return fresh;
}

static const JSMallocFunctions pool_functions = {
    js_pool_malloc, js_pool_free, js_pool_realloc, pool_usable
};

/* ---------------------------------------------------------------- state */

struct wrapper {
    const struct opos_html_node *node; /* NULL: empty slot */
    JSValue object;
    bool ran;                          /* <script>: already run */
};

struct script_state {
    JSRuntime *runtime;
    JSContext *context;
    struct page *page;
    struct wrapper *wrappers;
    size_t wrapper_size, wrapper_used; /* size is a power of two */
    JSValue element_proto, text_proto;
    JSValue hooks;                     /* what dom.js returned */
    bool dirty;                        /* the tree changed */
    bool stale;                        /* not laid out since it changed */
    struct sheet_text *sheet_texts;    /* CSS object model sheets */
    size_t sheet_text_count;
    bool title_changed;
    long long deadline;                /* uptime ms; 0: no limit */
    long long started;                 /* uptime ms at page start */
    char navigation[OPOS_HTTP_URL_MAX];
    bool navigation_replace;
    char pushed[OPOS_HTTP_URL_MAX];   /* history.pushState URL, or "" */
    bool push_replace;
    int history_step;                  /* history.go(n) asked for */
    struct opos_html_node *pending[MAX_PENDING]; /* inserted scripts */
    int pending_count;
    const struct opos_html_node *current_script;
    int errors;
    bool running;                      /* inside a script call */
};

static JSClassID node_class;

bool scripting_enabled = true;

static long long uptime_ms(void) {
    struct opos_timespec t = {0, 0};
    opos_uptime(&t);
    return (long long)t.seconds * 1000 + t.nanoseconds / 1000000;
}

static struct script_state *state_of(JSContext *ctx) {
    return JS_GetContextOpaque(ctx);
}

static size_t node_hash(const struct opos_html_node *node) {
    uintptr_t v = (uintptr_t)node;
    v ^= v >> 17;
    v *= 0x9e3779b97f4a7c15ull;
    return (size_t)(v >> 7);
}

static struct wrapper *wrapper_slot(struct script_state *s,
                                    const struct opos_html_node *node) {
    size_t mask = s->wrapper_size - 1;
    for (size_t at = node_hash(node) & mask;; at = (at + 1) & mask) {
        struct wrapper *w = &s->wrappers[at];
        if (w->node == node || w->node == NULL) {
            return w;
        }
    }
}

static bool wrappers_grow(struct script_state *s) {
    size_t size = s->wrapper_size == 0 ? 1024 : s->wrapper_size * 2;
    struct wrapper *old = s->wrappers;
    size_t old_size = s->wrapper_size;
    s->wrappers = calloc(size, sizeof(*s->wrappers));
    if (s->wrappers == NULL) {
        s->wrappers = old;
        return false;
    }
    s->wrapper_size = size;
    for (size_t i = 0; i < old_size; i++) {
        if (old[i].node != NULL) {
            *wrapper_slot(s, old[i].node) = old[i];
        }
    }
    free(old);
    return true;
}

static struct wrapper *wrapper_find(struct script_state *s,
                                    const struct opos_html_node *node) {
    if (s->wrapper_size == 0) {
        return NULL;
    }
    struct wrapper *w = wrapper_slot(s, node);
    return w->node == node ? w : NULL;
}

/* The node's wrapper (a new reference), or null for NULL. */
static JSValue wrap(JSContext *ctx, const struct opos_html_node *node) {
    struct script_state *s = state_of(ctx);
    if (node == NULL) {
        return JS_NULL;
    }
    struct wrapper *w = wrapper_find(s, node);
    if (w != NULL) {
        return JS_DupValue(ctx, w->object);
    }
    if ((s->wrapper_used + 1) * 2 > s->wrapper_size && !wrappers_grow(s)) {
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue proto = node->type == HTML_NODE_TEXT ? s->text_proto
                                                 : s->element_proto;
    JSValue object = JS_NewObjectProtoClass(ctx, proto, node_class);
    if (JS_IsException(object)) {
        return object;
    }
    JS_SetOpaque(object, (void *)node);
    w = wrapper_slot(s, node);
    w->node = node;
    w->object = JS_DupValue(ctx, object);
    w->ran = false;
    s->wrapper_used++;
    return object;
}

static struct opos_html_node *unwrap(JSValueConst value) {
    return JS_GetOpaque(value, node_class);
}

/* ------------------------------------------------------------- helpers */

static void log_line(struct script_state *s, const char *level,
                     const char *text) {
    (void)s;
    printf("[js %s] %s\n", level, text);
}

static void report_exception(JSContext *ctx) {
    struct script_state *s = state_of(ctx);
    JSValue error = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, error);
    JSValue stack = JS_IsObject(error)
        ? JS_GetPropertyStr(ctx, error, "stack") : JS_UNDEFINED;
    const char *trace = JS_IsUndefined(stack) ? NULL
                                              : JS_ToCString(ctx, stack);
    char line[600];
    snprintf(line, sizeof(line), "Uncaught %s%s%s",
             message != NULL ? message : "exception",
             trace != NULL ? "\n" : "", trace != NULL ? trace : "");
    log_line(s, "error", line);
    s->errors++;
    JS_FreeCString(ctx, message);
    JS_FreeCString(ctx, trace);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, error);
}

static int interrupt(JSRuntime *runtime, void *opaque) {
    (void)runtime;
    struct script_state *s = opaque;
    return s->deadline != 0 && uptime_ms() > s->deadline;
}

static void begin_call(struct script_state *s) {
    s->deadline = uptime_ms() + SCRIPT_TIME_MS;
    s->running = true;
    JS_UpdateStackTop(s->runtime);
}

static void run_jobs(struct script_state *s) {
    JSContext *ctx;
    for (int i = 0; i < 100000; i++) {
        int got = JS_ExecutePendingJob(s->runtime, &ctx);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            report_exception(ctx);
        }
    }
}

static void end_call(struct script_state *s) {
    run_jobs(s);
    s->deadline = 0;
    s->running = false;
}

static char *to_string(JSContext *ctx, JSValueConst value, size_t *length) {
    const char *text = JS_ToCStringLen(ctx, length, value);
    return (char *)text;
}

/* A hook from dom.js, called with this = undefined. Returns the result
   (a new reference) or JS_EXCEPTION after reporting it. */
static JSValue call_hook(struct script_state *s, const char *name, int argc,
                         JSValueConst *argv) {
    JSContext *ctx = s->context;
    JSValue function = JS_GetPropertyStr(ctx, s->hooks, name);
    JSValue result = JS_UNDEFINED;
    if (JS_IsFunction(ctx, function)) {
        result = JS_Call(ctx, function, JS_UNDEFINED, argc, argv);
        if (JS_IsException(result)) {
            report_exception(ctx);
        }
    }
    JS_FreeValue(ctx, function);
    return result;
}

static bool connected(const struct script_state *s,
                      const struct opos_html_node *node) {
    while (node->parent != NULL) {
        node = node->parent;
    }
    return node == s->page->doc->root;
}

/* Remembers <script> elements put into the document, to run them once the
   current script is done. */
static void note_inserted_scripts(struct script_state *s,
                                  struct opos_html_node *node) {
    if (node->type != HTML_NODE_ELEMENT) {
        return;
    }
    if (node->tag == HTML_TAG_SCRIPT) {
        struct wrapper *w = wrapper_find(s, node);
        bool ran = w != NULL && w->ran;
        bool queued = false;
        for (int i = 0; i < s->pending_count; i++) {
            queued |= s->pending[i] == node;
        }
        if (!ran && !queued && s->pending_count < MAX_PENDING) {
            s->pending[s->pending_count++] = node;
        }
        return;
    }
    for (struct opos_html_node *child = node->first_child; child != NULL;
         child = child->next_sibling) {
        note_inserted_scripts(s, child);
    }
}

/* -------------------------------------------------------- serialisation */

struct text_buffer {
    char *data;
    size_t length, capacity;
    bool failed;
};

static void buffer_add(struct text_buffer *b, const char *text,
                       size_t length) {
    if (b->failed) {
        return;
    }
    if (b->length + length + 1 > b->capacity) {
        size_t capacity = b->capacity == 0 ? 256 : b->capacity;
        while (capacity < b->length + length + 1) {
            capacity *= 2;
        }
        char *bigger = realloc(b->data, capacity);
        if (bigger == NULL) {
            b->failed = true;
            return;
        }
        b->data = bigger;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, text, length);
    b->length += length;
    b->data[b->length] = '\0';
}

static void buffer_escaped(struct text_buffer *b, const char *text,
                           size_t length, bool attribute) {
    size_t from = 0;
    for (size_t i = 0; i < length; i++) {
        const char *entity = NULL;
        switch (text[i]) {
        case '&':
            entity = "&amp;";
            break;
        case '<':
            entity = attribute ? NULL : "&lt;";
            break;
        case '>':
            entity = attribute ? NULL : "&gt;";
            break;
        case '"':
            entity = attribute ? "&quot;" : NULL;
            break;
        default:
            break;
        }
        if (entity != NULL) {
            buffer_add(b, text + from, i - from);
            buffer_add(b, entity, strlen(entity));
            from = i + 1;
        }
    }
    buffer_add(b, text + from, length - from);
}

static void serialise(struct text_buffer *b, const struct opos_html_node *node,
                      bool raw_parent, int depth) {
    if (node->type == HTML_NODE_TEXT) {
        if (raw_parent) {
            buffer_add(b, node->text, node->text_length);
        } else {
            buffer_escaped(b, node->text, node->text_length, false);
        }
        return;
    }
    bool fragment = node->name[0] == '#';
    if (!fragment) {
        buffer_add(b, "<", 1);
        buffer_add(b, node->name, strlen(node->name));
        for (const struct opos_html_attr *a = node->attributes; a != NULL;
             a = a->next) {
            buffer_add(b, " ", 1);
            buffer_add(b, a->name, strlen(a->name));
            buffer_add(b, "=\"", 2);
            buffer_escaped(b, a->value, strlen(a->value), true);
            buffer_add(b, "\"", 1);
        }
        buffer_add(b, ">", 1);
        if (opos_html_is_void(node)) {
            return;
        }
    }
    if (depth < 512) {
        bool raw = opos_html_is_raw(node) && node->tag != HTML_TAG_TEXTAREA
                && node->tag != HTML_TAG_TITLE;
        for (const struct opos_html_node *child = node->first_child;
             child != NULL; child = child->next_sibling) {
            serialise(b, child, raw, depth + 1);
        }
    }
    if (!fragment) {
        buffer_add(b, "</", 2);
        buffer_add(b, node->name, strlen(node->name));
        buffer_add(b, ">", 1);
    }
}

static void text_content(struct text_buffer *b,
                         const struct opos_html_node *node, int depth) {
    if (node->type == HTML_NODE_TEXT) {
        buffer_add(b, node->text, node->text_length);
        return;
    }
    if (depth > 512) {
        return;
    }
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        text_content(b, child, depth + 1);
    }
}

/* ------------------------------------------------------------- natives */

#define NODE_ARG(name, index)                                          \
    struct opos_html_node *name = argc > (index) ? unwrap(argv[index]) \
                                                 : NULL;               \
    if (name == NULL) {                                                \
        return JS_ThrowTypeError(ctx, "not a node");                   \
    }

static JSValue n_kind(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    return JS_NewInt32(ctx, node->type == HTML_NODE_TEXT ? 3 : 1);
}

static JSValue n_name(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    return JS_NewString(ctx, node->type == HTML_NODE_TEXT ? "#text"
                                                          : node->name);
}

/* parent, first child, last child, next and previous sibling (magic). */
static JSValue n_link(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv, int magic) {
    (void)this_val;
    NODE_ARG(node, 0);
    const struct opos_html_node *other =
        magic == 0 ? node->parent : magic == 1 ? node->first_child
        : magic == 2 ? node->last_child : magic == 3 ? node->next_sibling
        : node->previous_sibling;
    return wrap(ctx, other);
}

static JSValue n_data(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    if (node->type != HTML_NODE_TEXT) {
        return JS_NULL;
    }
    return JS_NewStringLen(ctx, node->text, node->text_length);
}

static JSValue n_set_data(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    NODE_ARG(node, 0);
    if (node->type != HTML_NODE_TEXT || argc < 2) {
        return JS_UNDEFINED;
    }
    size_t length;
    char *text = to_string(ctx, argv[1], &length);
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    bool ok = opos_html_set_text(s->page->doc, node, text, length);
    JS_FreeCString(ctx, text);
    s->dirty = s->stale = true;
    return ok ? JS_UNDEFINED : JS_ThrowOutOfMemory(ctx);
}

static JSValue n_attr(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    if (node->type != HTML_NODE_ELEMENT || argc < 2) {
        return JS_NULL;
    }
    const char *name = JS_ToCString(ctx, argv[1]);
    if (name == NULL) {
        return JS_EXCEPTION;
    }
    char lower[128];
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < sizeof(lower); i++) {
        char c = name[i];
        lower[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    lower[i] = '\0';
    JS_FreeCString(ctx, name);
    const char *value = opos_html_attribute(node, lower);
    return value != NULL ? JS_NewString(ctx, value) : JS_NULL;
}

/* A change that can affect which <style>/<link> sheets apply. */
static void note_change(struct script_state *s,
                        const struct opos_html_node *node) {
    (void)node;
    s->dirty = s->stale = true;
}

static JSValue n_set_attr(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    NODE_ARG(node, 0);
    if (node->type != HTML_NODE_ELEMENT || argc < 3) {
        return JS_UNDEFINED;
    }
    const char *name = JS_ToCString(ctx, argv[1]);
    const char *value = JS_ToCString(ctx, argv[2]);
    bool ok = name != NULL && value != NULL
           && opos_html_set_attribute(s->page->doc, node, name, value);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    note_change(s, node);
    if (name == NULL || value == NULL) {
        return JS_EXCEPTION;
    }
    return ok ? JS_UNDEFINED : JS_ThrowOutOfMemory(ctx);
}

static JSValue n_remove_attr(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    NODE_ARG(node, 0);
    if (node->type != HTML_NODE_ELEMENT || argc < 2) {
        return JS_UNDEFINED;
    }
    const char *name = JS_ToCString(ctx, argv[1]);
    if (name == NULL) {
        return JS_EXCEPTION;
    }
    if (opos_html_remove_attribute(node, name)) {
        note_change(s, node);
    }
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue n_attr_names(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    JSValue array = JS_NewArray(ctx);
    uint32_t index = 0;
    for (const struct opos_html_attr *a = node->attributes; a != NULL;
         a = a->next) {
        JS_SetPropertyUint32(ctx, array, index++, JS_NewString(ctx, a->name));
    }
    return array;
}

static JSValue n_create(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (name == NULL) {
        return JS_EXCEPTION;
    }
    struct opos_html_node *node = opos_html_create_element(s->page->doc,
                                                           name);
    JS_FreeCString(ctx, name);
    return node != NULL ? wrap(ctx, node) : JS_ThrowOutOfMemory(ctx);
}

static JSValue n_create_text(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    size_t length = 0;
    char *text = argc > 0 ? to_string(ctx, argv[0], &length) : NULL;
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    struct opos_html_node *node = opos_html_create_text(s->page->doc, text,
                                                        length);
    JS_FreeCString(ctx, text);
    return node != NULL ? wrap(ctx, node) : JS_ThrowOutOfMemory(ctx);
}

static JSValue n_insert(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    NODE_ARG(parent, 0);
    NODE_ARG(node, 1);
    struct opos_html_node *before = argc > 2 ? unwrap(argv[2]) : NULL;
    if (parent->type != HTML_NODE_ELEMENT) {
        return JS_ThrowTypeError(ctx, "HierarchyRequestError");
    }
    if (!opos_html_insert(parent, node, before)) {
        return JS_ThrowTypeError(ctx, "HierarchyRequestError");
    }
    s->dirty = s->stale = true;
    if (connected(s, parent)) {
        note_inserted_scripts(s, node);
    }
    return JS_UNDEFINED;
}

static JSValue n_remove(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    NODE_ARG(node, 0);
    if (node == s->page->doc->root) {
        return JS_UNDEFINED;
    }
    if (node->parent != NULL) {
        opos_html_remove(node);
        s->dirty = s->stale = true;
    }
    return JS_UNDEFINED;
}

static JSValue n_fragment(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    size_t length = 0;
    char *markup = argc > 0 ? to_string(ctx, argv[0], &length) : NULL;
    if (markup == NULL) {
        return JS_EXCEPTION;
    }
    struct opos_html_node *holder =
        opos_html_parse_fragment(s->page->doc, markup, length);
    JS_FreeCString(ctx, markup);
    if (holder == NULL) {
        return JS_ThrowOutOfMemory(ctx);
    }
    /* Scripts from innerHTML never run (as in other browsers). */
    return wrap(ctx, holder);
}

static void mark_scripts_ran(struct script_state *s, JSContext *ctx,
                             const struct opos_html_node *node) {
    if (node->type != HTML_NODE_ELEMENT) {
        return;
    }
    if (node->tag == HTML_TAG_SCRIPT) {
        JS_FreeValue(ctx, wrap(ctx, node));
        struct wrapper *w = wrapper_find(s, node);
        if (w != NULL) {
            w->ran = true;
        }
    }
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        mark_scripts_ran(s, ctx, child);
    }
}

static JSValue n_inert(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    mark_scripts_ran(state_of(ctx), ctx, node);
    return JS_UNDEFINED;
}

static JSValue n_clone(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    NODE_ARG(node, 0);
    bool deep = argc > 1 && JS_ToBool(ctx, argv[1]);
    struct opos_html_node *copy = opos_html_clone(s->page->doc, node, deep);
    return copy != NULL ? wrap(ctx, copy) : JS_ThrowOutOfMemory(ctx);
}

struct query {
    JSContext *ctx;
    const struct css_sheet *selectors;
    const char *id;                     /* getElementById */
    bool all;
    JSValue array;
    uint32_t count;
    const struct opos_html_node *found;
};

static bool query_walk(struct query *q, const struct opos_html_node *node,
                       int depth) {
    for (const struct opos_html_node *child = node->first_child;
         child != NULL; child = child->next_sibling) {
        if (child->type != HTML_NODE_ELEMENT) {
            continue;
        }
        bool match;
        if (q->id != NULL) {
            const char *id = opos_html_attribute(child, "id");
            match = id != NULL && strcmp(id, q->id) == 0;
        } else {
            match = css_selector_matches(q->selectors, child);
        }
        if (match) {
            if (!q->all) {
                q->found = child;
                return true;
            }
            JS_SetPropertyUint32(q->ctx, q->array, q->count++,
                                 wrap(q->ctx, child));
        }
        if (depth < 512 && query_walk(q, child, depth + 1)) {
            return true;
        }
    }
    return false;
}

/* N.query(scope, selectors, all): descendants of scope in document
   order. */
static JSValue n_query(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(scope, 0);
    const char *text = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    struct css_sheet *selectors = css_selector_parse(text);
    if (selectors == NULL) {
        JSValue error = JS_ThrowSyntaxError(ctx, "'%s' is not a valid "
                                            "selector", text);
        JS_FreeCString(ctx, text);
        return error;
    }
    JS_FreeCString(ctx, text);
    struct query q = {.ctx = ctx, .selectors = selectors,
                      .all = argc > 2 && JS_ToBool(ctx, argv[2])};
    if (q.all) {
        q.array = JS_NewArray(ctx);
    }
    query_walk(&q, scope, 0);
    css_free(selectors);
    return q.all ? q.array : wrap(ctx, q.found);
}

static JSValue n_matches(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    const char *text = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    struct css_sheet *selectors = css_selector_parse(text);
    JS_FreeCString(ctx, text);
    if (selectors == NULL) {
        return JS_ThrowSyntaxError(ctx, "not a valid selector");
    }
    bool match = css_selector_matches(selectors, node);
    css_free(selectors);
    return JS_NewBool(ctx, match);
}

static JSValue n_by_id(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    const char *id = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (id == NULL) {
        return JS_EXCEPTION;
    }
    struct query q = {.ctx = ctx, .id = id};
    const struct opos_html_node *root = s->page->doc->root;
    const char *root_id = opos_html_attribute(root, "id");
    if (root_id != NULL && strcmp(root_id, id) == 0) {
        q.found = root;
    } else {
        query_walk(&q, root, 0);
    }
    JS_FreeCString(ctx, id);
    return wrap(ctx, q.found);
}

/* root, head, body (magic). */
static JSValue n_document_part(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv, int magic) {
    (void)this_val;
    (void)argc;
    (void)argv;
    struct opos_html_doc *doc = state_of(ctx)->page->doc;
    return wrap(ctx, magic == 0 ? doc->root : magic == 1 ? doc->head
                                                          : doc->body);
}

static JSValue n_html(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    bool outer = argc > 1 && JS_ToBool(ctx, argv[1]);
    struct text_buffer b = {0};
    if (outer) {
        serialise(&b, node, false, 0);
    } else if (node->type == HTML_NODE_ELEMENT) {
        bool raw = opos_html_is_raw(node) && node->tag != HTML_TAG_TEXTAREA
                && node->tag != HTML_TAG_TITLE;
        for (const struct opos_html_node *child = node->first_child;
             child != NULL; child = child->next_sibling) {
            serialise(&b, child, raw, 1);
        }
    }
    JSValue result = b.failed ? JS_ThrowOutOfMemory(ctx)
                   : JS_NewStringLen(ctx, b.data != NULL ? b.data : "",
                                     b.length);
    free(b.data);
    return result;
}

static JSValue n_text(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    struct text_buffer b = {0};
    text_content(&b, node, 0);
    JSValue result = b.failed ? JS_ThrowOutOfMemory(ctx)
                   : JS_NewStringLen(ctx, b.data != NULL ? b.data : "",
                                     b.length);
    free(b.data);
    return result;
}

static JSValue n_connected(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    return JS_NewBool(ctx, connected(state_of(ctx), node));
}

/* N.field(node, what[, value]): what 0 value, 1 checked, 2 selectedIndex.
   Returns undefined for elements that are not form controls. */
static JSValue n_field(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    NODE_ARG(node, 0);
    int what = 0;
    if (argc > 1) {
        JS_ToInt32(ctx, &what, argv[1]);
    }
    struct form_field *field = form_field(s->page, node);
    if (field == NULL) {
        return JS_UNDEFINED;
    }
    if (argc > 2) {
        if (what == 0) {
            size_t length;
            char *text = to_string(ctx, argv[2], &length);
            if (text == NULL) {
                return JS_EXCEPTION;
            }
            field_set(field, text, length);
            JS_FreeCString(ctx, text);
        } else if (what == 1) {
            field->checked = JS_ToBool(ctx, argv[2]);
            if (field->checked && field->kind == FIELD_RADIO) {
                form_check_radio(s->page, node);
            }
        } else {
            int index = 0;
            JS_ToInt32(ctx, &index, argv[2]);
            if (index >= -1 && index < form_option_count(node)) {
                field->option = index;
            }
        }
        s->dirty = s->stale = true;
        return JS_UNDEFINED;
    }
    if (what == 1) {
        return JS_NewBool(ctx, field->checked);
    }
    if (what == 2) {
        return JS_NewInt32(ctx, field->option);
    }
    if (field->kind == FIELD_SELECT) {
        const struct opos_html_node *option = form_option(node,
                                                          field->option);
        if (option == NULL) {
            return JS_NewString(ctx, "");
        }
        const char *value = opos_html_attribute(option, "value");
        if (value != NULL) {
            return JS_NewString(ctx, value);
        }
        struct text_buffer b = {0};
        text_content(&b, option, 0);
        JSValue result = JS_NewStringLen(ctx, b.data != NULL ? b.data : "",
                                         b.length);
        free(b.data);
        return result;
    }
    return JS_NewStringLen(ctx, field->value != NULL ? field->value : "",
                           field->length);
}

static JSValue n_log(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv) {
    (void)this_val;
    const char *level = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    const char *text = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    log_line(state_of(ctx), level != NULL ? level : "log",
             text != NULL ? text : "");
    JS_FreeCString(ctx, level);
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

static JSValue n_now(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, (double)(uptime_ms() - state_of(ctx)->started));
}

static JSValue n_url(JSContext *ctx, JSValueConst this_val, int argc,
                     JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewString(ctx, state_of(ctx)->page->url);
}

static JSValue n_resolve(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    const char *reference = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (reference == NULL) {
        return JS_EXCEPTION;
    }
    const char *base = argc > 1 && !JS_IsUndefined(argv[1])
                           && !JS_IsNull(argv[1])
                       ? JS_ToCString(ctx, argv[1]) : NULL;
    char out[OPOS_HTTP_URL_MAX];
    bool ok = opos_url_resolve(base != NULL ? base : s->page->url, reference,
                               out, sizeof(out));
    JS_FreeCString(ctx, reference);
    JS_FreeCString(ctx, base);
    return ok ? JS_NewString(ctx, out) : JS_NULL;
}

static JSValue n_navigate(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (url == NULL) {
        return JS_EXCEPTION;
    }
    char out[OPOS_HTTP_URL_MAX];
    if (strncmp(url, "javascript:", 11) != 0
        && opos_url_resolve(s->page->url, url, out, sizeof(out))) {
        snprintf(s->navigation, sizeof(s->navigation), "%s", out);
        s->navigation_replace = argc > 1 && JS_ToBool(ctx, argv[1]);
    }
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}

static JSValue n_set_title(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    size_t length = 0;
    char *text = argc > 0 ? to_string(ctx, argv[0], &length) : NULL;
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    struct opos_html_node *copy = opos_html_create_text(s->page->doc, text,
                                                        length);
    JS_FreeCString(ctx, text);
    if (copy != NULL) {
        s->page->doc->title = copy->text;
        s->title_changed = true;
    }
    return JS_UNDEFINED;
}

static JSValue n_title(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    const char *title = state_of(ctx)->page->doc->title;
    return JS_NewString(ctx, title != NULL ? title : "");
}

static JSValue n_viewport(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv, int magic) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, magic == 0 ? viewport_width_px
                                       : viewport_height_px);
}

static JSValue n_current_script(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return wrap(ctx, state_of(ctx)->current_script);
}

static JSValue n_media(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv) {
    (void)this_val;
    const char *query = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (query == NULL) {
        return JS_EXCEPTION;
    }
    bool match = css_media_matches(query);
    JS_FreeCString(ctx, query);
    return JS_NewBool(ctx, match);
}

/* ------------------------------------------------------------ network */

/* "https://Example.com:443/a" -> "https://example.com" (the default port
   left out). False for URLs that are not http or https. */
static bool url_origin(const char *url, char *out, size_t capacity) {
    bool secure;
    if (strncasecmp(url, "https://", 8) == 0) {
        secure = true;
        url += 8;
    } else if (strncasecmp(url, "http://", 7) == 0) {
        secure = false;
        url += 7;
    } else {
        return false;
    }
    size_t length = strcspn(url, "/?#");
    const char *at = memchr(url, '@', length);
    if (at != NULL) {
        length -= (size_t)(at + 1 - url);
        url = at + 1;
    }
    char host[300];
    if (length == 0 || length >= sizeof(host)) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        host[i] = (char)tolower((unsigned char)url[i]);
    }
    host[length] = '\0';
    char *colon = strrchr(host, ':');
    if (colon != NULL && strchr(colon, ']') == NULL
        && strcmp(colon + 1, secure ? "443" : "80") == 0) {
        *colon = '\0';
    }
    snprintf(out, capacity, "%s://%s", secure ? "https" : "http", host);
    return true;
}

/* Header names a page may not set: the browser sets them itself. */
static bool forbidden_header(const char *name, size_t length) {
    static const char *const names[] = {
        "accept-charset", "accept-encoding", "access-control-request-headers",
        "access-control-request-method", "connection", "content-length",
        "cookie", "cookie2", "date", "dnt", "expect", "host", "keep-alive",
        "origin", "referer", "te", "trailer", "transfer-encoding", "upgrade",
        "user-agent", "via",
    };
    if ((length >= 6 && strncasecmp(name, "proxy-", 6) == 0)
        || (length >= 4 && strncasecmp(name, "sec-", 4) == 0)) {
        return true;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == length && strncasecmp(name, names[i], length)
                                              == 0) {
            return true;
        }
    }
    return false;
}

/* A header a cross-origin request may send without asking the server
   first (CORS "safelisted"). */
static bool simple_header(const char *name, size_t length, const char *value) {
    if ((length == 6 && strncasecmp(name, "accept", 6) == 0)
        || (length == 15 && strncasecmp(name, "accept-language", 15) == 0)
        || (length == 16 && strncasecmp(name, "content-language", 16) == 0)) {
        return true;
    }
    if (length == 12 && strncasecmp(name, "content-type", 12) == 0) {
        return strncasecmp(value, "text/plain", 10) == 0
            || strncasecmp(value, "multipart/form-data", 19) == 0
            || strncasecmp(value, "application/x-www-form-urlencoded", 33)
                   == 0;
    }
    return false;
}

/* The value of header name (lower case) in "Name: value\n" lines, copied
   to out; false when it is missing. */
static bool find_header(const char *headers, const char *name, char *out,
                        size_t capacity) {
    size_t length = strlen(name);
    for (const char *line = headers; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t line_length = end != NULL ? (size_t)(end - line)
                                         : strlen(line);
        if (line_length > length && line[length] == ':'
            && strncasecmp(line, name, length) == 0) {
            const char *value = line + length + 1;
            while (*value == ' ' || *value == '\t') {
                value++;
            }
            size_t value_length = (size_t)(line + line_length - value);
            while (value_length > 0 && (value[value_length - 1] == ' '
                                        || value[value_length - 1] == '\r')) {
                value_length--;
            }
            snprintf(out, capacity, "%.*s", (int)value_length, value);
            return true;
        }
        line = end != NULL ? end + 1 : NULL;
    }
    return false;
}

/* Whether a cross-origin response may be shown to the page from origin
   (credentials: cookies were sent). */
static bool cors_allows(const char *headers, const char *origin,
                        bool credentials) {
    char allow[300];
    if (!find_header(headers, "access-control-allow-origin", allow,
                     sizeof(allow))) {
        return false;
    }
    if (credentials) {
        char yes[16];
        return strcmp(allow, origin) == 0
            && find_header(headers, "access-control-allow-credentials", yes,
                           sizeof(yes))
            && strcmp(yes, "true") == 0;
    }
    return strcmp(allow, "*") == 0 || strcmp(allow, origin) == 0;
}

static bool list_has(const char *list, const char *word, size_t length) {
    for (const char *p = list; *p != '\0';) {
        while (*p == ' ' || *p == ',') {
            p++;
        }
        size_t n = strcspn(p, ", ");
        if (n == 1 && *p == '*') {
            return true;
        }
        if (n == length && strncasecmp(p, word, length) == 0) {
            return true;
        }
        p += n;
    }
    return false;
}

/* N.http(method, url, headers, contentType, body, credentials): one
   request, run to its end (the page waits). headers are "Name: value\n"
   lines; body is a string, an ArrayBuffer or undefined; credentials is
   "omit", "same-origin" or "include". Returns {status, url, headers,
   body (ArrayBuffer)}, or throws a TypeError like browsers do for network
   and CORS errors.

   Cookies go only to the page's own site (cookies of other sites are
   never sent: SameSite is not tracked). Requests to other sites carry an
   Origin header, are asked first with OPTIONS when they are not simple,
   and their answers reach the page only when the server allows it with
   Access-Control-Allow-Origin. */
static JSValue n_http(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    if (argc < 6) {
        return JS_ThrowTypeError(ctx, "Failed to fetch");
    }
    const char *method = JS_ToCString(ctx, argv[0]);
    const char *address = JS_ToCString(ctx, argv[1]);
    const char *given = JS_ToCString(ctx, argv[2]);
    const char *type = JS_IsString(argv[3]) ? JS_ToCString(ctx, argv[3])
                                            : NULL;
    const char *mode = JS_ToCString(ctx, argv[5]);
    char *headers = NULL, *ask = NULL;
    const char *body = NULL, *body_text = NULL;
    size_t body_length = 0;
    struct opos_http_response response;
    memset(&response, 0, sizeof(response));
    JSValue result = JS_UNDEFINED;
    const char *failure = "Failed to fetch";
    char url[OPOS_HTTP_URL_MAX], page_origin[300], target_origin[300];
    if (method == NULL || address == NULL || given == NULL || mode == NULL
        || !opos_url_resolve(s->page->url, address, url, sizeof(url))
        || !url_origin(url, target_origin, sizeof(target_origin))) {
        goto out;
    }
    url[strcspn(url, "#")] = '\0';
    bool same = url_origin(s->page->url, page_origin, sizeof(page_origin))
        && strcmp(page_origin, target_origin) == 0;
    bool credentials = same && strcmp(mode, "omit") != 0;
    if (!JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])) {
        if (JS_IsString(argv[4])) {
            body_text = JS_ToCStringLen(ctx, &body_length, argv[4]);
            body = body_text;
        } else {
            body = (const char *)JS_GetArrayBuffer(ctx, &body_length,
                                                   argv[4]);
        }
        if (body == NULL) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            goto out;
        }
    }
    /* The page's headers as "Name: value\r\n" lines, without forbidden
       ones; simple says whether CORS needs a preflight. */
    size_t given_length = strlen(given);
    headers = malloc(given_length * 2 + 400);
    ask = malloc(given_length + 64);
    if (headers == NULL || ask == NULL) {
        goto out;
    }
    size_t used = 0, ask_used = 0;
    bool simple = strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0
               || strcmp(method, "POST") == 0;
    ask[0] = '\0';
    for (const char *line = given; *line != '\0';) {
        size_t length = strcspn(line, "\n");
        const char *colon = memchr(line, ':', length);
        if (colon != NULL && colon > line) {
            size_t name_length = (size_t)(colon - line);
            const char *value = colon + 1;
            while (*value == ' ') {
                value++;
            }
            size_t value_length = (size_t)(line + length - value);
            bool ok = memchr(line, '\r', length) == NULL;
            for (size_t i = 0; ok && i < name_length; i++) {
                ok = line[i] > 32 && line[i] < 127;
            }
            if (ok && !forbidden_header(line, name_length)) {
                used += (size_t)sprintf(headers + used, "%.*s: %.*s\r\n",
                                        (int)name_length, line,
                                        (int)value_length, value);
                if (!simple_header(line, name_length, value)) {
                    simple = false;
                    ask_used += (size_t)sprintf(ask + ask_used, "%s%.*s",
                                                ask_used > 0 ? "," : "",
                                                (int)name_length, line);
                }
            }
        }
        line += length + (line[length] == '\n');
    }
    if (type != NULL && !simple_header("content-type", 12, type)) {
        simple = false;
        ask_used += (size_t)sprintf(ask + ask_used, "%scontent-type",
                                    ask_used > 0 ? "," : "");
    }
    if (!same) {
        used += (size_t)sprintf(headers + used, "Origin: %s\r\n",
                                page_origin[0] != '\0' ? page_origin
                                                       : "null");
    }
    headers[used] = '\0';

    char status[OPOS_HTTP_URL_MAX + 16];
    snprintf(status, sizeof(status), "Loading %s", url);
    browser_status(status);
    if (!same && !simple) {
        /* Preflight: may the page send this? */
        char *pre = malloc(ask_used + 600);
        if (pre == NULL) {
            goto out;
        }
        sprintf(pre, "Origin: %s\r\nAccess-Control-Request-Method: %s\r\n",
                page_origin[0] != '\0' ? page_origin : "null", method);
        if (ask_used > 0) {
            sprintf(pre + strlen(pre),
                    "Access-Control-Request-Headers: %s\r\n", ask);
        }
        struct opos_http_response check;
        long error = opos_http_request("OPTIONS", url, pre, NULL, NULL, 0,
                                       64 << 10, &check);
        free(pre);
        char allowed[512] = "";
        bool ok = error == 0 && check.status >= 200 && check.status < 300
            && check.headers != NULL
            && cors_allows(check.headers, page_origin, false);
        if (ok && strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0
            && strcmp(method, "POST") != 0) {
            ok = find_header(check.headers, "access-control-allow-methods",
                             allowed, sizeof(allowed))
              && list_has(allowed, method, strlen(method));
        }
        for (const char *p = ask; ok && *p != '\0';) {
            size_t n = strcspn(p, ",");
            ok = find_header(check.headers, "access-control-allow-headers",
                             allowed, sizeof(allowed))
              && list_has(allowed, p, n);
            p += n + (p[n] == ',');
        }
        opos_http_free(&check);
        if (!ok) {
            failure = "Failed to fetch (blocked by CORS)";
            goto out;
        }
    }
    if (credentials) {
        cookies_attach(true);
    }
    long error = opos_http_request(method, url, headers, type, body,
                                   body_length, JS_FETCH_MAX_BYTES,
                                   &response);
    if (credentials) {
        cookies_detach();
    }
    if (error != 0) {
        goto out;
    }
    /* The answer may come from another site after redirects. */
    char final_origin[300];
    if (!url_origin(response.url, final_origin, sizeof(final_origin))) {
        goto out;
    }
    if (strcmp(final_origin, page_origin) != 0
        && !cors_allows(response.headers != NULL ? response.headers : "",
                        page_origin, credentials)) {
        failure = "Failed to fetch (blocked by CORS)";
        goto out;
    }
    /* Set-Cookie never reaches scripts. */
    if (response.headers != NULL) {
        char *write = response.headers;
        for (char *line = response.headers; *line != '\0';) {
            size_t length = strcspn(line, "\n");
            size_t next = length + (line[length] == '\n');
            if (!(length > 11 && strncasecmp(line, "set-cookie", 10) == 0
                  && (line[10] == ':' || line[10] == '2'))) {
                memmove(write, line, next);
                write += next;
            }
            line += next;
        }
        *write = '\0';
    }
    result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "status",
                      JS_NewInt32(ctx, (int32_t)response.status));
    JS_SetPropertyStr(ctx, result, "url", JS_NewString(ctx, response.url));
    JS_SetPropertyStr(ctx, result, "headers",
                      JS_NewString(ctx, response.headers != NULL
                                            ? response.headers : ""));
    JS_SetPropertyStr(ctx, result, "body",
                      JS_NewArrayBufferCopy(ctx, response.body,
                                            response.length));
out:
    /* The wait for the network does not count against the script. */
    if (s->deadline != 0) {
        s->deadline = uptime_ms() + SCRIPT_TIME_MS;
    }
    opos_http_free(&response);
    free(headers);
    free(ask);
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, address);
    JS_FreeCString(ctx, given);
    JS_FreeCString(ctx, type);
    JS_FreeCString(ctx, mode);
    JS_FreeCString(ctx, body_text);
    if (JS_IsUndefined(result)) {
        return JS_ThrowTypeError(ctx, "%s", failure);
    }
    return result;
}

/* N.decode(arrayBuffer): the bytes as UTF-8 text. */
static JSValue n_decode(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    size_t length = 0;
    uint8_t *data = argc > 0 ? JS_GetArrayBuffer(ctx, &length, argv[0])
                             : NULL;
    if (data == NULL) {
        return JS_EXCEPTION;
    }
    /* Skip a byte order mark. */
    if (length >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        data += 3;
        length -= 3;
    }
    return JS_NewStringLen(ctx, (const char *)data, length);
}

/* N.random(n): an ArrayBuffer of n random bytes (at most 65536). */
static JSValue n_random(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    int32_t n = 0;
    if (argc < 1 || JS_ToInt32(ctx, &n, argv[0]) != 0) {
        return JS_EXCEPTION;
    }
    if (n < 0 || n > 65536) {
        return JS_ThrowRangeError(ctx, "too many random bytes");
    }
    uint8_t bytes[256];
    uint8_t *buffer = n <= (int32_t)sizeof(bytes) ? bytes : malloc((size_t)n);
    if (buffer == NULL) {
        return JS_ThrowOutOfMemory(ctx);
    }
    long got = n == 0 ? 0 : opos_syscall3(318 /* getrandom */, (long)buffer,
                                          n, 0);
    JSValue result = got == n ? JS_NewArrayBufferCopy(ctx, buffer, (size_t)n)
                              : JS_ThrowInternalError(ctx, "no randomness");
    if (buffer != bytes) {
        free(buffer);
    }
    return result;
}

/* N.encode(string): its UTF-8 bytes as an ArrayBuffer. */
static JSValue n_encode(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    size_t length = 0;
    const char *text = argc > 0 ? JS_ToCStringLen(ctx, &length, argv[0])
                                : NULL;
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    JSValue buffer = JS_NewArrayBufferCopy(ctx, (const uint8_t *)text,
                                           length);
    JS_FreeCString(ctx, text);
    return buffer;
}

/* N.cookie(): document.cookie. N.setCookie(text): document.cookie = text.
   Only for http and https pages. */
static JSValue n_cookie(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    struct script_state *s = state_of(ctx);
    char origin[300];
    if (!url_origin(s->page->url, origin, sizeof(origin))) {
        return JS_NewString(ctx, "");
    }
    char *out = malloc(OPOS_HTTP_COOKIE_MAX);
    if (out == NULL) {
        return JS_NewString(ctx, "");
    }
    cookies_for_script(s->page->url, out, OPOS_HTTP_COOKIE_MAX);
    JSValue text = JS_NewString(ctx, out);
    free(out);
    return text;
}

static JSValue n_set_cookie(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    const char *text = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    char origin[300];
    if (url_origin(s->page->url, origin, sizeof(origin))) {
        cookies_set_from_script(s->page->url, text);
    }
    JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

/* localStorage lives in data_dir/storage/<origin>, one file per site:
   the JSON text dom.js writes. */
static char storage_dir[200];

void script_set_data_dir(const char *data_dir) {
    if (data_dir[0] == '\0') {
        storage_dir[0] = '\0';
        return;
    }
    snprintf(storage_dir, sizeof(storage_dir), "%s/storage", data_dir);
    opos_mkdir(storage_dir);
}

static bool storage_path(struct script_state *s, char *out, size_t capacity) {
    char origin[300];
    if (storage_dir[0] == '\0'
        || !url_origin(s->page->url, origin, sizeof(origin))) {
        return false;
    }
    /* "https://example.com:8080" -> "https_example.com_8080" */
    char name[300];
    size_t n = 0;
    for (const char *p = origin; *p != '\0' && n + 1 < sizeof(name); p++) {
        if (*p == ':' && p[1] == '/' && p[2] == '/') {
            name[n++] = '_';
            p += 2;
        } else if (isalnum((unsigned char)*p) || *p == '.' || *p == '-') {
            name[n++] = *p;
        } else {
            name[n++] = '_';
        }
    }
    name[n] = '\0';
    snprintf(out, capacity, "%s/%s", storage_dir, name);
    return true;
}

static JSValue n_storage_load(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    char path[400];
    if (!storage_path(state_of(ctx), path, sizeof(path))) {
        return JS_NULL;
    }
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        return JS_NULL;
    }
    size_t used = 0, capacity = 16384;
    char *data = malloc(capacity);
    while (data != NULL) {
        if (used + 4096 > capacity) {
            if (capacity >= STORAGE_MAX_BYTES * 2) {
                break;
            }
            char *bigger = realloc(data, capacity * 2);
            if (bigger == NULL) {
                break;
            }
            data = bigger;
            capacity *= 2;
        }
        long got = opos_read((int)fd, data + used, 4096);
        if (got <= 0) {
            break;
        }
        used += (size_t)got;
    }
    opos_close((int)fd);
    if (data == NULL) {
        return JS_NULL;
    }
    JSValue text = JS_NewStringLen(ctx, data, used);
    free(data);
    return text;
}

static JSValue n_storage_save(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    char path[400], temporary[410];
    size_t length = 0;
    const char *text = argc > 0 ? JS_ToCStringLen(ctx, &length, argv[0])
                                : NULL;
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    bool ok = length <= STORAGE_MAX_BYTES * 2
        && storage_path(state_of(ctx), path, sizeof(path));
    if (ok) {
        snprintf(temporary, sizeof(temporary), "%s.new", path);
        opos_unlink(temporary);
        long fd = opos_open(temporary, OPOS_O_WRONLY | OPOS_O_CREAT
                                           | OPOS_O_TRUNC);
        ok = fd >= 0;
        if (ok) {
            size_t done = 0;
            while (ok && done < length) {
                long wrote = opos_write((int)fd, text + done, length - done);
                ok = wrote > 0;
                done += ok ? (size_t)wrote : 0;
            }
            opos_close((int)fd);
        }
        if (ok) {
            opos_unlink(path);
            ok = opos_rename(temporary, path) == 0;
        }
        if (!ok) {
            opos_unlink(temporary);
        }
    }
    JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

/* N.pushUrl(url, replace): history.pushState/replaceState. Only a URL of
   the page's own site; the page is not loaded again. */
static JSValue n_push_url(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    const char *given = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (given == NULL) {
        return JS_EXCEPTION;
    }
    char url[OPOS_HTTP_URL_MAX], one[300], two[300];
    bool ok = opos_url_resolve(s->page->url, given, url, sizeof(url))
        && url_origin(url, one, sizeof(one))
        && url_origin(s->page->url, two, sizeof(two))
        && strcmp(one, two) == 0;
    JS_FreeCString(ctx, given);
    if (!ok) {
        return JS_ThrowTypeError(ctx, "SecurityError: not the page's origin");
    }
    bool replace = argc > 1 && JS_ToBool(ctx, argv[1]);
    /* A push not yet taken by the browser stays a push. */
    s->push_replace = s->pushed[0] == '\0' ? replace
                                           : s->push_replace && replace;
    snprintf(s->page->url, sizeof(s->page->url), "%s", url);
    snprintf(s->pushed, sizeof(s->pushed), "%s", url);
    return JS_UNDEFINED;
}

/* N.navigateHistory(step): history.back(), forward() and go(n). */
static JSValue n_navigate_history(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv) {
    (void)this_val;
    int32_t step = 0;
    if (argc > 0 && JS_ToInt32(ctx, &step, argv[0]) == 0 && step != 0) {
        state_of(ctx)->history_step = step < -50 ? -50 : step > 50 ? 50
                                                                  : step;
    }
    return JS_UNDEFINED;
}

/* ----------------------------------------------------- styles and layout */

/* Sheet text set through the CSS object model (insertRule ...) for a
   <style> element, or for node NULL the document's adopted sheets. It
   holds while the element's own text stays what it was (hash). */
struct sheet_text {
    const struct opos_html_node *node;
    char *text;
    size_t length;
    uint32_t hash;
};

static uint32_t child_text_hash(const struct opos_html_node *node) {
    uint32_t hash = 2166136261u;
    if (node == NULL) {
        return hash;
    }
    for (const struct opos_html_node *t = node->first_child; t != NULL;
         t = t->next_sibling) {
        if (t->type == HTML_NODE_TEXT) {
            for (size_t i = 0; i < t->text_length; i++) {
                hash = (hash ^ (uint8_t)t->text[i]) * 16777619u;
            }
        }
    }
    return hash;
}

static struct sheet_text *sheet_text_find(struct script_state *s,
                                          const struct opos_html_node *node) {
    for (size_t i = 0; i < s->sheet_text_count; i++) {
        if (s->sheet_texts[i].node == node) {
            return &s->sheet_texts[i];
        }
    }
    return NULL;
}

/* N.setSheet(node or null, text or null) */
static JSValue n_set_sheet(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    const struct opos_html_node *node = argc > 0 && !JS_IsNull(argv[0])
                                      ? unwrap(argv[0]) : NULL;
    if (argc > 0 && !JS_IsNull(argv[0]) && node == NULL) {
        return JS_UNDEFINED;
    }
    struct sheet_text *entry = sheet_text_find(s, node);
    if (argc < 2 || JS_IsNull(argv[1]) || JS_IsUndefined(argv[1])) {
        if (entry != NULL) {
            free(entry->text);
            *entry = s->sheet_texts[--s->sheet_text_count];
            s->dirty = s->stale = true;
        }
        return JS_UNDEFINED;
    }
    size_t length;
    const char *text = JS_ToCStringLen(ctx, &length, argv[1]);
    if (text == NULL) {
        return JS_EXCEPTION;
    }
    char *copy = malloc(length + 1);
    if (copy == NULL) {
        JS_FreeCString(ctx, text);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(copy, text, length + 1);
    JS_FreeCString(ctx, text);
    if (entry == NULL) {
        struct sheet_text *grown = realloc(
            s->sheet_texts, (s->sheet_text_count + 1) * sizeof(*grown));
        if (grown == NULL) {
            free(copy);
            return JS_ThrowOutOfMemory(ctx);
        }
        s->sheet_texts = grown;
        entry = &grown[s->sheet_text_count++];
        entry->node = node;
    } else {
        free(entry->text);
    }
    entry->text = copy;
    entry->length = length;
    entry->hash = child_text_hash(node);
    s->dirty = s->stale = true;
    return JS_UNDEFINED;
}

const char *script_sheet_text(struct page *page,
                              const struct opos_html_node *node,
                              size_t *length) {
    struct script_state *s = page->script;
    if (s == NULL) {
        return NULL;
    }
    struct sheet_text *entry = sheet_text_find(s, node);
    if (entry == NULL || (node != NULL && entry->hash != child_text_hash(node))) {
        return NULL;
    }
    *length = entry->length;
    return entry->text;
}

void script_layout_done(struct page *page) {
    if (page->script != NULL) {
        page->script->stale = false;
    }
}

/* Scripts that ask for sizes get the layout of the document as it is
   now. */
static void layout_fresh(struct script_state *s) {
    if (s->stale) {
        s->stale = false;
        browser_layout_now(s->page);
    }
}

/* N.rect(node): [x, y, width, height] in the view, or null. */
static JSValue n_rect(JSContext *ctx, JSValueConst this_val, int argc,
                      JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    struct script_state *s = state_of(ctx);
    layout_fresh(s);
    int rect[4];
    if (!browser_element_rect(s->page, node, rect)) {
        return JS_NULL;
    }
    JSValue list = JS_NewArray(ctx);
    for (int i = 0; i < 4; i++) {
        JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_NewInt32(ctx, rect[i]));
    }
    return list;
}

/* N.metrics(): [scrollX, scrollY, page width, page height]. */
static JSValue n_metrics(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    struct script_state *s = state_of(ctx);
    layout_fresh(s);
    int v[4];
    browser_page_metrics(s->page, &v[0], &v[1], &v[2], &v[3]);
    JSValue list = JS_NewArray(ctx);
    for (int i = 0; i < 4; i++) {
        JS_SetPropertyUint32(ctx, list, (uint32_t)i, JS_NewInt32(ctx, v[i]));
    }
    return list;
}

/* N.scrollTo(y) */
static JSValue n_scroll_to(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    struct script_state *s = state_of(ctx);
    double y = 0;
    if (argc < 1 || JS_ToFloat64(ctx, &y, argv[0]) != 0) {
        return JS_EXCEPTION;
    }
    if (y != y) {
        return JS_UNDEFINED;
    }
    layout_fresh(s);
    browser_scroll_to(s->page, y < 0 ? 0 : y > 1e8 ? 100000000 : (int)y);
    return JS_UNDEFINED;
}

static void put(JSContext *ctx, JSValue object, const char *name,
                const char *format, ...) {
    char text[96];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    JS_SetPropertyStr(ctx, object, name, JS_NewString(ctx, text));
}

static void put_colour(JSContext *ctx, JSValue object, const char *name,
                       uint32_t colour, int alpha) {
    int r = (int)(colour >> 16 & 255), g = (int)(colour >> 8 & 255);
    int b = (int)(colour & 255);
    if (alpha >= 255) {
        put(ctx, object, name, "rgb(%d, %d, %d)", r, g, b);
    } else if (alpha <= 0) {
        put(ctx, object, name, "rgba(0, 0, 0, 0)");
    } else {
        put(ctx, object, name, "rgba(%d, %d, %d, 0.%02d)", r, g, b,
            alpha * 100 / 255);
    }
}

static const char *display_name(const struct style *st,
                                const struct opos_html_node *node) {
    enum display d = st->out_of_flow ? st->flow_display : st->display;
    switch (d) {
    case DISPLAY_NONE: return "none";
    case DISPLAY_INLINE: return "inline";
    case DISPLAY_BLOCK:
        return st->flex ? "flex" : st->grid ? "grid" : "block";
    case DISPLAY_LIST_ITEM: return "list-item";
    case DISPLAY_TABLE: return "table";
    case DISPLAY_TABLE_PART:
        return node->tag == HTML_TAG_TR ? "table-row"
             : node->tag == HTML_TAG_THEAD ? "table-header-group"
             : node->tag == HTML_TAG_TFOOT ? "table-footer-group"
             : node->tag == HTML_TAG_CAPTION ? "table-caption"
             : "table-row-group";
    case DISPLAY_TABLE_CELL: return "table-cell";
    case DISPLAY_INLINE_BLOCK:
        return st->flex ? "inline-flex" : st->grid ? "inline-grid"
                                                   : "inline-block";
    case DISPLAY_CONTENTS: return "contents";
    }
    return "block";
}

static void put_offset(JSContext *ctx, JSValue object, const char *name,
                       const struct style *st, int side) {
    if (!st->offset_set[side]) {
        put(ctx, object, name, "auto");
    } else {
        put(ctx, object, name, st->offset_pct[side] ? "%d%%" : "%dpx",
            st->offset[side]);
    }
}

/* N.computed(node): the element's computed style as
   {"property-name": "value"} for the properties the browser knows. */
static JSValue n_computed(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
    (void)this_val;
    NODE_ARG(node, 0);
    if (node->type != HTML_NODE_ELEMENT) {
        return JS_NULL;
    }
    struct script_state *s = state_of(ctx);
    layout_fresh(s);
    struct style st;
    page_element_style(s->page, node, &st);
    JSValue o = JS_NewObject(ctx);
    static const char *const sides[4] = {"top", "right", "bottom", "left"};
    static const char *const positions[] = {"static", "relative", "absolute",
                                            "fixed", "sticky"};
    static const char *const aligns[] = {"left", "center", "right"};
    static const char *const flex_align[] = {"normal", "flex-start", "center",
                                             "flex-end"};
    static const char *const justify[] = {"flex-start", "center", "flex-end",
                                          "space-between"};
    char name[40];
    put(ctx, o, "display", "%s", display_name(&st, node));
    put(ctx, o, "position", "%s", positions[st.position <= POS_STICKY
                                            ? st.position : 0]);
    put(ctx, o, "visibility", "%s", st.invisible ? "hidden" : "visible");
    put(ctx, o, "opacity", "%s", st.opacity_zero ? "0" : "1");
    put_colour(ctx, o, "color", st.colour, 255);
    put_colour(ctx, o, "background-color", st.background,
               st.has_background ? st.background_alpha : 0);
    put(ctx, o, "background-image", "none");
    put(ctx, o, "font-size", "%dpx", st.font_size);
    put(ctx, o, "font-weight", "%s", st.bold ? "700" : "400");
    put(ctx, o, "font-style", "%s", st.italic ? "italic" : "normal");
    put(ctx, o, "font-family", "%s", st.mono ? "monospace" : "sans-serif");
    put(ctx, o, "line-height", "normal");
    const char *decoration = st.underline ? "underline"
                           : st.strike ? "line-through" : "none";
    put(ctx, o, "text-decoration-line", "%s", decoration);
    put(ctx, o, "text-decoration", "%s", decoration);
    put(ctx, o, "text-align", "%s", aligns[st.align <= ALIGN_RIGHT
                                           ? st.align : 0]);
    put(ctx, o, "white-space", "%s", st.pre ? "pre" : "normal");
    for (int i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "margin-%s", sides[i]);
        put(ctx, o, name, "%dpx", st.margin[i]);
        snprintf(name, sizeof(name), "padding-%s", sides[i]);
        put(ctx, o, name, "%dpx", st.padding[i]);
        snprintf(name, sizeof(name), "border-%s-width", sides[i]);
        put(ctx, o, name, "%dpx", st.border);
        snprintf(name, sizeof(name), "border-%s-style", sides[i]);
        put(ctx, o, name, "%s", st.border > 0 ? "solid" : "none");
        snprintf(name, sizeof(name), "border-%s-color", sides[i]);
        put_colour(ctx, o, name, st.border_colour, 255);
        put_offset(ctx, o, sides[i], &st, i);
    }
    put(ctx, o, "box-sizing", "%s", st.border_box ? "border-box"
                                                  : "content-box");
    int rect[4] = {0, 0, 0, 0};
    if (browser_element_rect(s->page, node, rect)) {
        int frame_x = st.border_box ? 0 : st.padding[LEFT] + st.padding[RIGHT]
                                          + 2 * st.border;
        int frame_y = st.border_box ? 0 : st.padding[TOP] + st.padding[BOTTOM]
                                          + 2 * st.border;
        put(ctx, o, "width", "%dpx", rect[2] > frame_x ? rect[2] - frame_x : 0);
        put(ctx, o, "height", "%dpx",
            rect[3] > frame_y ? rect[3] - frame_y : 0);
    } else {
        put(ctx, o, "width", "auto");
        put(ctx, o, "height", "auto");
    }
    if (st.max_width >= 0) {
        put(ctx, o, "max-width", "%dpx", st.max_width);
    } else if (st.max_width_pct >= 0) {
        put(ctx, o, "max-width", "%d%%", st.max_width_pct);
    } else {
        put(ctx, o, "max-width", "none");
    }
    put(ctx, o, "float", "%s", st.float_side == 1 ? "left"
                             : st.float_side == 2 ? "right" : "none");
    put(ctx, o, "clear", "%s", st.clear_both ? "both" : "none");
    put(ctx, o, "flex-direction", "%s", st.flex_column ? "column" : "row");
    put(ctx, o, "flex-wrap", "%s", st.flex_wrap ? "wrap" : "nowrap");
    put(ctx, o, "flex-grow", "%d", st.flex_grow);
    put(ctx, o, "flex-shrink", "%s", st.no_shrink ? "0" : "1");
    put(ctx, o, "justify-content", "%s", justify[st.justify >= 0
                                                 && st.justify <= 3
                                                 ? st.justify : 0]);
    put(ctx, o, "align-items", "%s", flex_align[st.align_items <= 3
                                                ? st.align_items : 0]);
    put(ctx, o, "gap", "%dpx", st.gap);
    put(ctx, o, "list-style-type", "%s", st.no_marker ? "none"
                                       : st.ordered ? "decimal" : "disc");
    if (st.translate[0] != 0 || st.translate[1] != 0) {
        int dx = st.translate_pct[0] && rect[2] > 0
               ? rect[2] * st.translate[0] / 100 : st.translate[0];
        int dy = st.translate_pct[1] && rect[3] > 0
               ? rect[3] * st.translate[1] / 100 : st.translate[1];
        put(ctx, o, "transform", "matrix(1, 0, 0, 1, %d, %d)", dx, dy);
    } else {
        put(ctx, o, "transform", "none");
    }
    put(ctx, o, "z-index", "auto");
    put(ctx, o, "overflow", "visible");
    put(ctx, o, "overflow-x", "visible");
    put(ctx, o, "overflow-y", "visible");
    put(ctx, o, "cursor", "auto");
    put(ctx, o, "pointer-events", "auto");
    put(ctx, o, "content", "none");
    put(ctx, o, "direction", "ltr");
    return o;
}

static const JSCFunctionListEntry native_functions[] = {
    JS_CFUNC_DEF("http", 6, n_http),
    JS_CFUNC_DEF("setSheet", 2, n_set_sheet),
    JS_CFUNC_DEF("rect", 1, n_rect),
    JS_CFUNC_DEF("metrics", 0, n_metrics),
    JS_CFUNC_DEF("scrollTo", 1, n_scroll_to),
    JS_CFUNC_DEF("computed", 1, n_computed),
    JS_CFUNC_DEF("navigateHistory", 1, n_navigate_history),
    JS_CFUNC_DEF("decode", 1, n_decode),
    JS_CFUNC_DEF("encode", 1, n_encode),
    JS_CFUNC_DEF("random", 1, n_random),
    JS_CFUNC_DEF("cookie", 0, n_cookie),
    JS_CFUNC_DEF("setCookie", 1, n_set_cookie),
    JS_CFUNC_DEF("storageLoad", 0, n_storage_load),
    JS_CFUNC_DEF("storageSave", 1, n_storage_save),
    JS_CFUNC_DEF("pushUrl", 2, n_push_url),
    JS_CFUNC_DEF("media", 1, n_media),
    JS_CFUNC_DEF("kind", 1, n_kind),
    JS_CFUNC_DEF("name", 1, n_name),
    JS_CFUNC_MAGIC_DEF("parent", 1, n_link, 0),
    JS_CFUNC_MAGIC_DEF("first", 1, n_link, 1),
    JS_CFUNC_MAGIC_DEF("last", 1, n_link, 2),
    JS_CFUNC_MAGIC_DEF("next", 1, n_link, 3),
    JS_CFUNC_MAGIC_DEF("prev", 1, n_link, 4),
    JS_CFUNC_DEF("data", 1, n_data),
    JS_CFUNC_DEF("setData", 2, n_set_data),
    JS_CFUNC_DEF("attr", 2, n_attr),
    JS_CFUNC_DEF("setAttr", 3, n_set_attr),
    JS_CFUNC_DEF("removeAttr", 2, n_remove_attr),
    JS_CFUNC_DEF("attrNames", 1, n_attr_names),
    JS_CFUNC_DEF("create", 1, n_create),
    JS_CFUNC_DEF("createText", 1, n_create_text),
    JS_CFUNC_DEF("insert", 3, n_insert),
    JS_CFUNC_DEF("remove", 1, n_remove),
    JS_CFUNC_DEF("fragment", 1, n_fragment),
    JS_CFUNC_DEF("inert", 1, n_inert),
    JS_CFUNC_DEF("clone", 2, n_clone),
    JS_CFUNC_DEF("query", 3, n_query),
    JS_CFUNC_DEF("matches", 2, n_matches),
    JS_CFUNC_DEF("byId", 1, n_by_id),
    JS_CFUNC_MAGIC_DEF("root", 0, n_document_part, 0),
    JS_CFUNC_MAGIC_DEF("head", 0, n_document_part, 1),
    JS_CFUNC_MAGIC_DEF("body", 0, n_document_part, 2),
    JS_CFUNC_DEF("html", 2, n_html),
    JS_CFUNC_DEF("text", 1, n_text),
    JS_CFUNC_DEF("connected", 1, n_connected),
    JS_CFUNC_DEF("field", 3, n_field),
    JS_CFUNC_DEF("log", 2, n_log),
    JS_CFUNC_DEF("now", 0, n_now),
    JS_CFUNC_DEF("url", 0, n_url),
    JS_CFUNC_DEF("resolve", 2, n_resolve),
    JS_CFUNC_DEF("navigate", 2, n_navigate),
    JS_CFUNC_DEF("title", 0, n_title),
    JS_CFUNC_DEF("setTitle", 1, n_set_title),
    JS_CFUNC_MAGIC_DEF("viewportWidth", 0, n_viewport, 0),
    JS_CFUNC_MAGIC_DEF("viewportHeight", 0, n_viewport, 1),
    JS_CFUNC_DEF("currentScript", 0, n_current_script),
};

/* -------------------------------------------------------------- running */

static bool is_classic_script(const struct opos_html_node *node) {
    const char *type = opos_html_attribute(node, "type");
    if (type == NULL || type[0] == '\0') {
        return true;
    }
    static const char *const classic[] = {
        "text/javascript", "application/javascript", "text/ecmascript",
        "application/ecmascript", "application/x-javascript",
        "text/x-javascript", "text/jscript", NULL
    };
    char lower[48];
    size_t i = 0;
    for (; type[i] != '\0' && type[i] != ';' && i + 1 < sizeof(lower); i++) {
        char c = type[i];
        lower[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    while (i > 0 && lower[i - 1] == ' ') {
        i--;
    }
    lower[i] = '\0';
    for (int k = 0; classic[k] != NULL; k++) {
        if (strcmp(lower, classic[k]) == 0) {
            return true;
        }
    }
    return false;
}

static void evaluate(struct script_state *s, const char *code, size_t length,
                     const char *name) {
    JSContext *ctx = s->context;
    begin_call(s);
    JSValue result = JS_Eval(ctx, code, length, name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        report_exception(ctx);
    }
    JS_FreeValue(ctx, result);
    end_call(s);
}

static void run_script(struct script_state *s,
                       const struct opos_html_node *node) {
    JSContext *ctx = s->context;
    JS_FreeValue(ctx, wrap(ctx, node));
    struct wrapper *w = wrapper_find(s, node);
    if (w == NULL || w->ran) {
        return;
    }
    w->ran = true;
    if (!is_classic_script(node) || opos_html_attribute(node, "nomodule")) {
        return;
    }
    const char *src = opos_html_attribute(node, "src");
    s->current_script = node;
    if (src != NULL) {
        char url[OPOS_HTTP_URL_MAX];
        if (src[0] != '\0'
            && opos_url_resolve(s->page->url, src, url, sizeof(url))) {
            char status[OPOS_HTTP_URL_MAX + 16];
            snprintf(status, sizeof(status), "Script: %s", url);
            browser_status(status);
            struct opos_http_response response;
            cookies_attach(true);
            long error = opos_http_get(url, SCRIPT_MAX_BYTES, &response);
            cookies_detach();
            if (error == 0) {
                if (response.status == 200) {
                    evaluate(s, (const char *)response.body, response.length,
                             url);
                } else {
                    char line[OPOS_HTTP_URL_MAX + 40];
                    snprintf(line, sizeof(line), "HTTP %ld for %s",
                             response.status, url);
                    log_line(s, "error", line);
                }
                opos_http_free(&response);
            } else {
                char line[OPOS_HTTP_URL_MAX + 40];
                snprintf(line, sizeof(line), "could not load %s (%ld)", url,
                         error);
                log_line(s, "error", line);
            }
        }
    } else {
        struct text_buffer b = {0};
        text_content(&b, node, 0);
        if (b.data != NULL && !b.failed) {
            evaluate(s, b.data, b.length, s->page->url);
        }
        free(b.data);
    }
    s->current_script = NULL;
    /* "load" for scripts with src. */
    if (src != NULL) {
        JSValue args[2] = {wrap(ctx, node), JS_NewString(ctx, "load")};
        begin_call(s);
        JS_FreeValue(ctx, call_hook(s, "fire", 2, args));
        end_call(s);
        JS_FreeValue(ctx, args[0]);
        JS_FreeValue(ctx, args[1]);
    }
}

static void run_pending(struct script_state *s) {
    for (int round = 0; s->pending_count > 0 && round < 1000; round++) {
        struct opos_html_node *node = s->pending[0];
        memmove(s->pending, s->pending + 1,
                (size_t)(s->pending_count - 1) * sizeof(s->pending[0]));
        s->pending_count--;
        if (connected(s, node)) {
            run_script(s, node);
        }
    }
}

static void collect_scripts(const struct opos_html_node *node,
                            const struct opos_html_node **list, size_t *count,
                            size_t capacity) {
    for (; node != NULL; node = node->next_sibling) {
        if (node->type != HTML_NODE_ELEMENT) {
            continue;
        }
        if (node->tag == HTML_TAG_SCRIPT) {
            if (*count < capacity) {
                list[(*count)++] = node;
            }
            continue;
        }
        collect_scripts(node->first_child, list, count, capacity);
    }
}

static void fire(struct script_state *s, const char *target,
                 const char *type) {
    JSContext *ctx = s->context;
    JSValue args[2] = {JS_NewString(ctx, target), JS_NewString(ctx, type)};
    begin_call(s);
    JS_FreeValue(ctx, call_hook(s, "fireGlobal", 2, args));
    end_call(s);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    run_pending(s);
}

void script_page_start(struct page *page) {
    if (page->doc == NULL || page->script != NULL) {
        return;
    }
    struct script_state *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return;
    }
    s->page = page;
    s->started = uptime_ms();
    s->stale = true; /* not laid out yet */
    s->runtime = JS_NewRuntime2(&pool_functions, NULL);
    if (s->runtime == NULL) {
        free(s);
        return;
    }
    JS_SetMemoryLimit(s->runtime, JS_MEMORY_LIMIT);
    JS_SetMaxStackSize(s->runtime, JS_STACK_LIMIT);
    JS_SetCanBlock(s->runtime, 0);
    JS_SetInterruptHandler(s->runtime, interrupt, s);
    if (node_class == 0) {
        JS_NewClassID(&node_class);
    }
    JSClassDef def = {.class_name = "Node"};
    JS_NewClass(s->runtime, node_class, &def);
    s->context = JS_NewContext(s->runtime);
    if (s->context == NULL) {
        JS_FreeRuntime(s->runtime);
        free(s);
        return;
    }
    JSContext *ctx = s->context;
    JS_SetContextOpaque(ctx, s);
    s->element_proto = JS_NULL;
    s->text_proto = JS_NULL;
    s->hooks = JS_UNDEFINED;
    page->script = s;

    /* dom.js is "(function (N, setProtos) { ... return hooks; })". */
    begin_call(s);
    s->deadline = 0;
    JSValue setup = JS_Eval(ctx, browser_dom_js,
                            (size_t)(browser_dom_js_end - browser_dom_js - 1),
                            "dom.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(setup)) {
        report_exception(ctx);
        end_call(s);
        return;
    }
    JSValue native = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, native, native_functions,
                               sizeof(native_functions)
                               / sizeof(native_functions[0]));
    JSValue result = JS_Call(ctx, setup, JS_UNDEFINED, 1, &native);
    JS_FreeValue(ctx, native);
    JS_FreeValue(ctx, setup);
    if (JS_IsException(result)) {
        report_exception(ctx);
        end_call(s);
        return;
    }
    s->hooks = result;
    s->element_proto = JS_GetPropertyStr(ctx, result, "elementProto");
    s->text_proto = JS_GetPropertyStr(ctx, result, "textProto");
    end_call(s);

    /* Scripts in document order (all as if parsing had finished; async
       and defer make no difference here). */
    size_t capacity = 1024, count = 0;
    const struct opos_html_node **list = malloc(capacity * sizeof(*list));
    if (list != NULL) {
        collect_scripts(page->doc->root, list, &count, capacity);
        for (size_t i = 0; i < count && page->script == s; i++) {
            if (connected(s, list[i])) {
                run_script(s, list[i]);
                run_pending(s);
            }
        }
        free(list);
    }
    fire(s, "document", "DOMContentLoaded");
    fire(s, "window", "load");
    browser_status("Done");
}

void script_page_end(struct page *page) {
    struct script_state *s = page->script;
    if (s == NULL) {
        return;
    }
    JSContext *ctx = s->context;
    if (!s->running && JS_IsObject(s->hooks)) {
        begin_call(s);
        JS_FreeValue(ctx, call_hook(s, "flush", 0, NULL));
        end_call(s);
    }
    for (size_t i = 0; i < s->wrapper_size; i++) {
        if (s->wrappers[i].node != NULL) {
            JS_SetOpaque(s->wrappers[i].object, NULL);
            JS_FreeValue(ctx, s->wrappers[i].object);
        }
    }
    free(s->wrappers);
    for (size_t i = 0; i < s->sheet_text_count; i++) {
        free(s->sheet_texts[i].text);
    }
    free(s->sheet_texts);
    JS_FreeValue(ctx, s->element_proto);
    JS_FreeValue(ctx, s->text_proto);
    JS_FreeValue(ctx, s->hooks);
    JS_FreeContext(ctx);
    JS_FreeRuntime(s->runtime);
    free(s);
    page->script = NULL;
}

long script_run_timers(struct page *page) {
    struct script_state *s = page->script;
    if (s == NULL || s->running) {
        return -1;
    }
    JSContext *ctx = s->context;
    JSValue now = JS_NewFloat64(ctx, (double)(uptime_ms() - s->started));
    begin_call(s);
    JSValue next = call_hook(s, "runTimers", 1, &now);
    end_call(s);
    run_pending(s);
    double wait = -1;
    if (JS_IsNumber(next)) {
        JS_ToFloat64(ctx, &wait, next);
    }
    JS_FreeValue(ctx, next);
    /* Promise callbacks that ran after the timers may have started new
       requests. */
    JSValue waiting = call_hook(s, "waiting", 0, NULL);
    if (JS_ToBool(ctx, waiting) > 0) {
        wait = 0;
    }
    JS_FreeValue(ctx, waiting);
    return wait < 0 ? -1 : wait > 1e9 ? 1000000000 : (long)wait;
}

bool script_event(struct page *page, const struct opos_html_node *target,
                  const char *type, int x, int y, int button) {
    struct script_state *s = page->script;
    if (s == NULL || s->running || target == NULL) {
        return false;
    }
    JSContext *ctx = s->context;
    JSValue args[5] = {wrap(ctx, target), JS_NewString(ctx, type),
                       JS_NewInt32(ctx, x), JS_NewInt32(ctx, y),
                       JS_NewInt32(ctx, button)};
    begin_call(s);
    JSValue result = call_hook(s, "dispatch", 5, args);
    end_call(s);
    for (int i = 0; i < 5; i++) {
        JS_FreeValue(ctx, args[i]);
    }
    bool prevented = JS_ToBool(ctx, result) > 0;
    JS_FreeValue(ctx, result);
    run_pending(s);
    return prevented;
}

bool script_key(struct page *page, const struct opos_html_node *target,
                const char *type, const char *key, int code, int modifiers) {
    struct script_state *s = page->script;
    if (s == NULL || s->running) {
        return false;
    }
    JSContext *ctx = s->context;
    JSValue args[5] = {wrap(ctx, target != NULL ? target : page->doc->body),
                       JS_NewString(ctx, type), JS_NewString(ctx, key),
                       JS_NewInt32(ctx, code), JS_NewInt32(ctx, modifiers)};
    begin_call(s);
    JSValue result = call_hook(s, "dispatchKey", 5, args);
    end_call(s);
    for (int i = 0; i < 5; i++) {
        JS_FreeValue(ctx, args[i]);
    }
    bool prevented = JS_ToBool(ctx, result) > 0;
    JS_FreeValue(ctx, result);
    run_pending(s);
    return prevented;
}

bool script_take_dirty(struct page *page) {
    struct script_state *s = page->script;
    if (s == NULL || !s->dirty) {
        return false;
    }
    s->dirty = false;
    css_forget_matches(page->sheets);
    return true;
}

bool script_take_title(struct page *page) {
    struct script_state *s = page->script;
    if (s == NULL || !s->title_changed) {
        return false;
    }
    s->title_changed = false;
    return true;
}

bool script_take_navigation(struct page *page, char *url, size_t capacity,
                            bool *replace) {
    struct script_state *s = page->script;
    if (s == NULL || s->navigation[0] == '\0') {
        return false;
    }
    snprintf(url, capacity, "%s", s->navigation);
    *replace = s->navigation_replace;
    s->navigation[0] = '\0';
    return true;
}

int script_take_history_step(struct page *page) {
    struct script_state *s = page->script;
    if (s == NULL) {
        return 0;
    }
    int step = s->history_step;
    s->history_step = 0;
    return step;
}

bool script_take_url(struct page *page, char *url, size_t capacity,
                     bool *replace) {
    struct script_state *s = page->script;
    if (s == NULL || s->pushed[0] == '\0') {
        return false;
    }
    snprintf(url, capacity, "%s", s->pushed);
    *replace = s->push_replace;
    s->pushed[0] = '\0';
    return true;
}

bool script_runs_javascript_url(struct page *page, const char *url) {
    struct script_state *s = page->script;
    if (s == NULL || s->running || strncmp(url, "javascript:", 11) != 0) {
        return false;
    }
    /* Percent-escapes in javascript: URLs are decoded first. */
    size_t length = strlen(url + 11);
    char *code = malloc(length + 1);
    if (code == NULL) {
        return false;
    }
    size_t used = 0;
    for (size_t i = 11; url[i] != '\0'; i++) {
        char c = url[i];
        if (c == '%' && url[i + 1] != '\0' && url[i + 2] != '\0') {
            char hex[3] = {url[i + 1], url[i + 2], 0};
            char *end;
            long value = strtol(hex, &end, 16);
            if (*end == '\0') {
                c = (char)value;
                i += 2;
            }
        }
        code[used++] = c;
    }
    code[used] = '\0';
    evaluate(s, code, used, "javascript:");
    free(code);
    run_pending(s);
    return true;
}

int script_errors(const struct page *page) {
    return page->script != NULL ? page->script->errors : 0;
}

void script_window_event(struct page *page, const char *type) {
    struct script_state *s = page->script;
    if (s == NULL || s->running) {
        return;
    }
    fire(s, "window", type);
}
