/*
 * OPOS browser: bookmarks, history and downloads, and the about: pages
 * that show them (and about:cookies).
 *
 * Bookmarks and history are kept as text files in the data folder (one
 * "URL<TAB>title" per line), so they last between runs. Without a data
 * folder (no account), they last only while the browser runs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>

#include "browser.h"

#define BOOKMARKS_MAX 500
#define HISTORY_KEEP 1000
#define DOWNLOADS_MAX 64

struct entry {
    char *url;
    char *title;
};

static char bookmarks_path[200];
static char history_path[200];
static struct entry bookmarks[BOOKMARKS_MAX];
static int bookmark_count;
/* Oldest first; the newest is history[history_count - 1]. */
static struct entry history[HISTORY_KEEP];
static int history_count;
static int history_lines; /* lines in the history file */

struct download {
    int id;
    char name[96];
    char path[256];
    unsigned long long size;     /* bytes so far while running */
    unsigned long long total;    /* announced size while running, or 0 */
    bool running;
    bool failed;
    bool cancelled;
};
static struct download downloads[DOWNLOADS_MAX];
static int download_count;
static int download_next_id = 1;

/* -------------------------------------------------------------- files */

static bool write_all(int fd, const char *data, size_t length) {
    while (length > 0) {
        size_t chunk = length < 4096 ? length : 4096;
        long written = opos_write(fd, data, chunk);
        if (written <= 0) {
            return false;
        }
        data += written;
        length -= (size_t)written;
    }
    return true;
}

/* The whole file, NUL-terminated (NULL if missing or unreadable). */
static char *read_file(const char *path, size_t limit) {
    if (path[0] == '\0') {
        return NULL;
    }
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    size_t used = 0, capacity = 4096;
    char *data = malloc(capacity + 1);
    while (data != NULL) {
        if (used + 4096 > capacity) {
            if (capacity >= limit) {
                break;
            }
            char *bigger = realloc(data, capacity * 2 + 1);
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
    if (data != NULL) {
        data[used] = '\0';
    }
    return data;
}

static char *copy_text(const char *text, size_t length) {
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

static void entry_free(struct entry *entry) {
    free(entry->url);
    free(entry->title);
    entry->url = entry->title = NULL;
}

/* Fills entry from "URL<TAB>title"; tabs and line breaks in either part
   were never written. */
static bool entry_parse(struct entry *entry, const char *line, size_t length) {
    const char *tab = memchr(line, '\t', length);
    size_t url_length = tab != NULL ? (size_t)(tab - line) : length;
    if (url_length == 0) {
        return false;
    }
    entry->url = copy_text(line, url_length);
    entry->title = tab != NULL ? copy_text(tab + 1, length - url_length - 1)
                               : copy_text("", 0);
    if (entry->url == NULL || entry->title == NULL) {
        entry_free(entry);
        return false;
    }
    return true;
}

/* Appends text; with clean, tabs and line breaks become spaces (they
   separate the fields and lines of the files). */
static void line_put(char **out, size_t *used, size_t *capacity,
                     const char *text, bool clean) {
    size_t length = strlen(text);
    if (*used + length + 2 > *capacity) {
        size_t grown = (*capacity + length + 2) * 2;
        char *bigger = realloc(*out, grown);
        if (bigger == NULL) {
            return;
        }
        *out = bigger;
        *capacity = grown;
    }
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        (*out)[(*used)++] = clean && (c == '\t' || c == '\n' || c == '\r')
                          ? ' ' : c;
    }
    (*out)[*used] = '\0';
}

static void entries_save(const char *path, const struct entry *entries,
                         int count) {
    if (path[0] == '\0') {
        return;
    }
    char *text = NULL;
    size_t used = 0, capacity = 0;
    line_put(&text, &used, &capacity, "", false);
    for (int i = 0; i < count; i++) {
        line_put(&text, &used, &capacity, entries[i].url, true);
        line_put(&text, &used, &capacity, "\t", false);
        line_put(&text, &used, &capacity, entries[i].title, true);
        line_put(&text, &used, &capacity, "\n", false);
    }
    if (text == NULL) {
        return;
    }
    /* Write a new file, then put it in place, so a crash keeps the old. */
    char temporary[220];
    snprintf(temporary, sizeof(temporary), "%s.new", path);
    long fd = opos_open(temporary, OPOS_O_WRONLY | OPOS_O_CREAT
                                       | OPOS_O_TRUNC);
    if (fd >= 0) {
        bool ok = write_all((int)fd, text, used);
        opos_close((int)fd);
        if (ok) {
            opos_unlink(path);
            opos_rename(temporary, path);
        } else {
            opos_unlink(temporary);
        }
    }
    free(text);
}

/* Reads the "URL<TAB>title" lines of the file into entries: the first max,
   or with keep_last the last max. */
static int entries_load(const char *path, struct entry *entries, int max,
                        bool keep_last) {
    char *data = read_file(path, 4u << 20);
    if (data == NULL) {
        return 0;
    }
    int count = 0, lines = 0;
    for (char *line = data; *line != '\0';) {
        char *end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        if (length > 0 && line[length - 1] == '\r') {
            length--;
        }
        struct entry entry;
        if (length > 0 && entry_parse(&entry, line, length)) {
            lines++;
            if (count == max) {
                if (!keep_last) {
                    entry_free(&entry);
                    break;
                }
                entry_free(&entries[0]);
                memmove(entries, entries + 1, (size_t)(max - 1)
                                                  * sizeof(entries[0]));
                count--;
            }
            entries[count++] = entry;
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    free(data);
    history_lines = lines;
    return count;
}

void library_open(const char *data_dir) {
    if (data_dir[0] == '\0') {
        return;
    }
    snprintf(bookmarks_path, sizeof(bookmarks_path), "%s/bookmarks.txt",
             data_dir);
    snprintf(history_path, sizeof(history_path), "%s/history.txt",
             data_dir);
    bookmark_count = entries_load(bookmarks_path, bookmarks, BOOKMARKS_MAX,
                                  false);
    history_lines = 0;
    history_count = entries_load(history_path, history, HISTORY_KEEP, true);
    /* Keep the file from growing without end. */
    if (history_lines > 2 * HISTORY_KEEP) {
        entries_save(history_path, history, history_count);
        history_lines = history_count;
    }
}

/* ---------------------------------------------------------- bookmarks */

static int bookmark_find(const char *url) {
    for (int i = 0; i < bookmark_count; i++) {
        if (strcmp(bookmarks[i].url, url) == 0) {
            return i;
        }
    }
    return -1;
}

bool bookmark_has(const char *url) { return bookmark_find(url) >= 0; }

static void bookmark_remove(int index) {
    if (index < 0 || index >= bookmark_count) {
        return;
    }
    entry_free(&bookmarks[index]);
    memmove(bookmarks + index, bookmarks + index + 1,
            (size_t)(bookmark_count - index - 1) * sizeof(bookmarks[0]));
    bookmark_count--;
    entries_save(bookmarks_path, bookmarks, bookmark_count);
}

bool bookmark_toggle(const char *url, const char *title) {
    int index = bookmark_find(url);
    if (index >= 0) {
        bookmark_remove(index);
        return false;
    }
    if (bookmark_count == BOOKMARKS_MAX) {
        return false;
    }
    struct entry entry = {copy_text(url, strlen(url)),
                          copy_text(title, strlen(title))};
    if (entry.url == NULL || entry.title == NULL) {
        entry_free(&entry);
        return false;
    }
    bookmarks[bookmark_count++] = entry;
    entries_save(bookmarks_path, bookmarks, bookmark_count);
    return true;
}

/* ------------------------------------------------------------ history */

void history_add(const char *url, const char *title) {
    /* Reloading or going back to the newest page adds nothing new. */
    if (history_count > 0
        && strcmp(history[history_count - 1].url, url) == 0) {
        if (title[0] != '\0'
            && strcmp(history[history_count - 1].title, title) != 0) {
            char *copy = copy_text(title, strlen(title));
            if (copy != NULL) {
                free(history[history_count - 1].title);
                history[history_count - 1].title = copy;
            }
        }
        return;
    }
    struct entry entry = {copy_text(url, strlen(url)),
                          copy_text(title, strlen(title))};
    if (entry.url == NULL || entry.title == NULL) {
        entry_free(&entry);
        return;
    }
    if (history_count == HISTORY_KEEP) {
        entry_free(&history[0]);
        memmove(history, history + 1,
                (HISTORY_KEEP - 1) * sizeof(history[0]));
        history_count--;
    }
    history[history_count++] = entry;
    if (history_path[0] == '\0') {
        return;
    }
    long fd = opos_open(history_path, OPOS_O_WRONLY | OPOS_O_CREAT
                                          | OPOS_O_APPEND);
    if (fd >= 0) {
        char *line = NULL;
        size_t used = 0, capacity = 0;
        line_put(&line, &used, &capacity, url, true);
        line_put(&line, &used, &capacity, "\t", false);
        line_put(&line, &used, &capacity, title, true);
        line_put(&line, &used, &capacity, "\n", false);
        if (line != NULL) {
            write_all((int)fd, line, used);
        }
        free(line);
        opos_close((int)fd);
        history_lines++;
    }
}

static void history_clear(void) {
    for (int i = 0; i < history_count; i++) {
        entry_free(&history[i]);
    }
    history_count = 0;
    history_lines = 0;
    if (history_path[0] != '\0') {
        opos_unlink(history_path);
    }
}

/* ---------------------------------------------------------- downloads */

int download_start(const char *name, const char *path) {
    if (download_count == DOWNLOADS_MAX) {
        /* Forget the oldest finished one; running ones stay listed. */
        int oldest = 0;
        while (oldest < download_count && downloads[oldest].running) {
            oldest++;
        }
        if (oldest == download_count) {
            oldest = 0;
        }
        memmove(downloads + oldest, downloads + oldest + 1,
                (size_t)(DOWNLOADS_MAX - oldest - 1) * sizeof(downloads[0]));
        download_count--;
    }
    struct download *d = &downloads[download_count++];
    memset(d, 0, sizeof(*d));
    d->id = download_next_id++;
    snprintf(d->name, sizeof(d->name), "%s", name);
    snprintf(d->path, sizeof(d->path), "%s", path);
    d->running = true;
    return d->id;
}

static struct download *download_find(int id) {
    for (int i = 0; i < download_count; i++) {
        if (downloads[i].id == id) {
            return &downloads[i];
        }
    }
    return NULL;
}

void download_update(int id, unsigned long long received,
                     unsigned long long total) {
    struct download *d = download_find(id);
    if (d != NULL) {
        d->size = received;
        d->total = total;
    }
}

void download_end(int id, unsigned long long size, bool failed,
                  bool cancelled) {
    struct download *d = download_find(id);
    if (d != NULL) {
        d->size = size;
        d->running = false;
        d->failed = failed;
        d->cancelled = cancelled;
    }
}

void download_record(const char *name, const char *path,
                     unsigned long long size, bool failed) {
    download_end(download_start(name, path), size, failed, false);
}

/* --------------------------------------------------------- about pages */

struct html {
    char *text;
    size_t used, capacity;
};

static void put(struct html *h, const char *text, size_t length) {
    if (h->used + length + 1 > h->capacity) {
        size_t grown = (h->capacity + length + 1) * 2;
        char *bigger = realloc(h->text, grown);
        if (bigger == NULL) {
            return;
        }
        h->text = bigger;
        h->capacity = grown;
    }
    memcpy(h->text + h->used, text, length);
    h->used += length;
    h->text[h->used] = '\0';
}

static void puts_raw(struct html *h, const char *text) {
    put(h, text, strlen(text));
}

static void puts_escaped(struct html *h, const char *text) {
    for (; *text != '\0'; text++) {
        switch (*text) {
        case '<': puts_raw(h, "&lt;"); break;
        case '>': puts_raw(h, "&gt;"); break;
        case '&': puts_raw(h, "&amp;"); break;
        case '"': puts_raw(h, "&quot;"); break;
        default: put(h, text, 1); break;
        }
    }
}

static void put_number(struct html *h, unsigned long long n) {
    char digits[24];
    snprintf(digits, sizeof(digits), "%llu", n);
    puts_raw(h, digits);
}

static void put_size(struct html *h, unsigned long long bytes) {
    char text[32];
    if (bytes >= 10ull << 20) {
        snprintf(text, sizeof(text), "%llu MB", bytes >> 20);
    } else if (bytes >= 10ull << 10) {
        snprintf(text, sizeof(text), "%llu KB", bytes >> 10);
    } else {
        snprintf(text, sizeof(text), "%llu bytes", bytes);
    }
    puts_raw(h, text);
}

/* A link to url showing title (or the URL when there is none). */
static void put_link(struct html *h, const struct entry *entry) {
    puts_raw(h, "<a href=\"");
    puts_escaped(h, entry->url);
    puts_raw(h, "\">");
    puts_escaped(h, entry->title[0] != '\0' ? entry->title : entry->url);
    puts_raw(h, "</a>");
    if (entry->title[0] != '\0') {
        puts_raw(h, " <span class=u>");
        puts_escaped(h, entry->url);
        puts_raw(h, "</span>");
    }
}

static const char page_style[] =
    "<style>body{font-family:sans-serif;margin:24px 40px;color:#222}"
    "h1{font-size:26px;font-weight:normal;margin:0 0 16px}"
    "h2{font-size:18px;margin:24px 0 8px}"
    "ul{list-style:none;padding:0;margin:0}li{margin:0 0 8px}"
    "a{color:#1a5fb4;text-decoration:none}"
    ".u{color:#777;font-size:12px}.x{font-size:12px;color:#a33}"
    ".n{color:#777}.nav a{display:inline-block;margin-right:20px}"
    ".bad{color:#a33}</style>";

static void page_start(struct html *h, const char *title) {
    puts_raw(h, "<html><head><title>");
    puts_escaped(h, title);
    puts_raw(h, "</title>");
    puts_raw(h, page_style);
    puts_raw(h, "</head><body><p class=nav><a href=\"about:home\">New tab"
                "</a><a href=\"about:bookmarks\">Bookmarks</a>"
                "<a href=\"about:history\">History</a>"
                "<a href=\"about:downloads\">Downloads</a>"
                "<a href=\"about:cookies\">Cookies</a></p><h1>");
    puts_escaped(h, title);
    puts_raw(h, "</h1>");
}

static void put_bookmarks(struct html *h, bool removable) {
    if (bookmark_count == 0) {
        puts_raw(h, "<p class=n>No bookmarks yet. Press Ctrl+D or the star"
                    " button to bookmark the page you are on.</p>");
        return;
    }
    puts_raw(h, "<ul>");
    for (int i = 0; i < bookmark_count; i++) {
        puts_raw(h, "<li>");
        put_link(h, &bookmarks[i]);
        if (removable) {
            puts_raw(h, " <a class=x href=\"about:bookmarks?remove=");
            put_number(h, (unsigned long long)i);
            puts_raw(h, "\">Remove</a>");
        }
        puts_raw(h, "</li>");
    }
    puts_raw(h, "</ul>");
}

static void put_history(struct html *h, int limit) {
    if (history_count == 0) {
        puts_raw(h, "<p class=n>No pages visited yet.</p>");
        return;
    }
    puts_raw(h, "<ul>");
    for (int i = history_count - 1, shown = 0; i >= 0 && shown < limit;
         i--, shown++) {
        puts_raw(h, "<li>");
        put_link(h, &history[i]);
        puts_raw(h, "</li>");
    }
    puts_raw(h, "</ul>");
}

static long number_after(const char *text, const char *key) {
    const char *at = strstr(text, key);
    if (at == NULL) {
        return -1;
    }
    at += strlen(key);
    long n = 0;
    if (*at < '0' || *at > '9') {
        return -1;
    }
    while (*at >= '0' && *at <= '9' && n < 1000000) {
        n = n * 10 + (*at++ - '0');
    }
    return n;
}

static void put_site(void *context, int index, const char *site,
                     int count) {
    struct html *h = context;
    puts_raw(h, "<li><b>");
    puts_escaped(h, site);
    puts_raw(h, "</b> <span class=u>");
    put_number(h, (unsigned long long)count);
    puts_raw(h, count == 1 ? " cookie" : " cookies");
    puts_raw(h, "</span> <a class=x href=\"about:cookies?remove=");
    put_number(h, (unsigned long long)index);
    puts_raw(h, "\">Delete</a></li>");
}

char *about_page(const char *url, size_t *length) {
    struct html h = {NULL, 0, 0};
    const char *name = url + 6; /* after "about:" */
    if (strncmp(name, "bookmarks", 9) == 0) {
        long remove = number_after(name, "?remove=");
        if (remove >= 0) {
            bookmark_remove((int)remove);
        }
        page_start(&h, "Bookmarks");
        put_bookmarks(&h, true);
    } else if (strncmp(name, "history", 7) == 0) {
        if (strstr(name, "?clear") != NULL) {
            history_clear();
        }
        page_start(&h, "History");
        if (history_count > 0) {
            puts_raw(&h, "<p><a class=x href=\"about:history?clear\">Clear"
                         " history</a></p>");
        }
        put_history(&h, HISTORY_KEEP);
    } else if (strncmp(name, "cookies", 7) == 0) {
        long remove = number_after(name, "?remove=");
        if (remove >= 0) {
            cookies_forget((int)remove);
        } else if (strstr(name, "?clear") != NULL) {
            cookies_forget(-1);
        }
        page_start(&h, "Cookies");
        puts_raw(&h, "<p class=n>Sites keep sign-ins and settings in "
                     "cookies. Only the site you visit can set them; images"
                     " and styles from other sites get none.</p>");
        struct html list = {NULL, 0, 0};
        int sites = cookies_sites(put_site, &list);
        if (sites == 0) {
            puts_raw(&h, "<p class=n>No site has cookies.</p>");
        } else {
            puts_raw(&h, "<p><a class=x href=\"about:cookies?clear\">Delete "
                         "all cookies</a></p><ul>");
            if (list.text != NULL) {
                puts_raw(&h, list.text);
            }
            puts_raw(&h, "</ul>");
        }
        free(list.text);
    } else if (strncmp(name, "downloads", 9) == 0) {
        long cancel = number_after(name, "?cancel=");
        if (cancel > 0) {
            download_cancel((int)cancel);
        }
        page_start(&h, "Downloads");
        if (download_count == 0) {
            puts_raw(&h, "<p class=n>Nothing downloaded since the browser "
                         "started. Files go to the Downloads folder in your"
                         " home folder; open them from Files.</p>");
        }
        puts_raw(&h, "<ul>");
        for (int i = download_count - 1; i >= 0; i--) {
            const struct download *d = &downloads[i];
            puts_raw(&h, "<li><b>");
            puts_escaped(&h, d->name);
            puts_raw(&h, "</b> ");
            if (d->running) {
                put_size(&h, d->size);
                if (d->total > 0) {
                    puts_raw(&h, " of ");
                    put_size(&h, d->total);
                }
                puts_raw(&h, " <a class=x href=\"about:downloads?cancel=");
                put_number(&h, (unsigned long long)d->id);
                puts_raw(&h, "\">Cancel</a>");
            } else if (d->cancelled) {
                puts_raw(&h, "<span class=n>cancelled</span>");
            } else if (d->failed) {
                puts_raw(&h, "<span class=bad>failed</span>");
            } else {
                put_size(&h, d->size);
            }
            puts_raw(&h, "<br><span class=u>");
            puts_escaped(&h, d->path);
            puts_raw(&h, "</span></li>");
        }
        puts_raw(&h, "</ul>");
    } else if (strncmp(name, "blank", 5) == 0) {
        puts_raw(&h, "<html><head><title></title></head><body></body>"
                     "</html>");
    } else {
        page_start(&h, "New tab");
        puts_raw(&h, "<p class=n>Type an address or search words in the "
                     "address bar (Ctrl+L).</p><h2>Bookmarks</h2>");
        put_bookmarks(&h, false);
        puts_raw(&h, "<h2>Recently visited</h2>");
        put_history(&h, 12);
    }
    puts_raw(&h, "</body></html>");
    *length = h.used;
    return h.text;
}
