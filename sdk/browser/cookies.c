/*
 * OPOS browser: cookies (RFC 6265).
 *
 * The jar is attached to the HTTP library only while the browser loads a
 * page, sends a form or downloads a file. Images and style sheets are
 * fetched without cookies, so other sites embedded in a page cannot track
 * the person with them (no third-party cookies).
 *
 * Cookies with an expiry date are kept in cookies.txt in the data folder,
 * one per line:
 *   domain TAB host-only TAB path TAB secure TAB expires TAB created TAB
 *   name TAB value
 * Session cookies (no expiry date) last only while the browser runs.
 *
 * There is no full Public Suffix List: a Domain attribute without a dot,
 * or one of a few well-known shared suffixes (co.uk, github.io, ...), is
 * refused so one site cannot set cookies for all of them.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos.h>

#include "browser.h"

#define COOKIES_MAX 800
#define COOKIES_PER_DOMAIN 80
#define COOKIE_TEXT_MAX 4096 /* name plus value */
#define COOKIE_FILE_MAX (4u << 20)

struct cookie {
    char *name;
    char *value;
    char domain[128]; /* lower case, no leading dot */
    char path[160];
    long long expires; /* Unix time; 0 for a session cookie */
    long long created;
    bool host_only;
    bool secure;
};

static struct cookie *jar;
static int cookie_count;
static char cookies_path[200];
static bool dirty; /* persistent cookies changed since the last save */
static long long last_created; /* keeps creation times unique and ordered */

static long long now(void) {
    struct opos_timespec time = {0, 0};
    if (opos_syscall2(228, 0 /* CLOCK_REALTIME */, (long)&time) != 0) {
        return 0;
    }
    return time.seconds;
}

/* ------------------------------------------------------------------ URLs */

struct cookie_url {
    bool secure;
    char host[128];
    char path[OPOS_HTTP_URL_MAX];
};

static bool url_parse(const char *text, struct cookie_url *url) {
    url->secure = strncmp(text, "https://", 8) == 0;
    if (url->secure) {
        text += 8;
    } else if (strncmp(text, "http://", 7) == 0) {
        text += 7;
    } else {
        return false;
    }
    size_t host = strcspn(text, "/:?#");
    if (host == 0 || host >= sizeof(url->host)) {
        return false;
    }
    for (size_t i = 0; i < host; i++) {
        url->host[i] = (char)tolower((unsigned char)text[i]);
    }
    url->host[host] = '\0';
    text += host;
    text += strcspn(text, "/?#"); /* skip the port */
    size_t path = *text == '/' ? strcspn(text, "?#") : 0;
    if (path == 0) {
        strcpy(url->path, "/");
    } else {
        if (path >= sizeof(url->path)) {
            path = sizeof(url->path) - 1;
        }
        memcpy(url->path, text, path);
        url->path[path] = '\0';
    }
    return true;
}

static bool is_ip_address(const char *host) {
    if (host[0] == '[') {
        return true;
    }
    for (; *host != '\0'; host++) {
        if (!isdigit((unsigned char)*host) && *host != '.') {
            return false;
        }
    }
    return true;
}

static bool domain_match(const char *host, const char *domain) {
    if (strcmp(host, domain) == 0) {
        return true;
    }
    size_t h = strlen(host), d = strlen(domain);
    return h > d && host[h - d - 1] == '.' && strcmp(host + h - d, domain) == 0
        && !is_ip_address(host);
}

static bool path_match(const char *request, const char *cookie) {
    size_t length = strlen(cookie);
    if (strncmp(request, cookie, length) != 0) {
        return false;
    }
    return request[length] == '\0' || cookie[length - 1] == '/'
        || request[length] == '/';
}

/* Shared suffixes under which sites belong to different owners. */
static const char *const shared_suffixes[] = {
    "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk", "ltd.uk", "plc.uk",
    "com.au", "net.au", "org.au", "edu.au", "gov.au", "co.nz", "org.nz",
    "co.jp", "ne.jp", "or.jp", "ac.jp", "co.kr", "or.kr", "com.cn",
    "net.cn", "org.cn", "com.tw", "com.hk", "com.sg", "co.in", "net.in",
    "org.in", "com.br", "net.br", "org.br", "com.mx", "com.ar", "co.za",
    "com.tr", "com.ua", "com.pl", "co.il", "co.id", "com.my", "com.ph",
    "com.vn", "co.th", "github.io", "gitlab.io", "blogspot.com",
    "herokuapp.com", "netlify.app", "vercel.app", "pages.dev",
    "workers.dev", "appspot.com", "azurewebsites.net", "cloudfront.net",
    "firebaseapp.com", "web.app", "s3.amazonaws.com",
};

static bool shared_suffix(const char *domain) {
    if (strchr(domain, '.') == NULL) {
        return true;
    }
    for (size_t i = 0; i < sizeof(shared_suffixes) / sizeof(shared_suffixes[0]);
         i++) {
        if (strcmp(domain, shared_suffixes[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------- dates */

static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

/* The cookie date algorithm of RFC 6265 section 5.1.1; -1 when the text
   is not a date. */
static long long parse_date(const char *text, size_t length) {
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    int hour = -1, minute = 0, second = 0, day = -1, month = -1, year = -1;
    size_t i = 0;
    while (i < length) {
        while (i < length && !isalnum((unsigned char)text[i])
               && text[i] != ':') {
            i++;
        }
        size_t start = i;
        while (i < length && (isalnum((unsigned char)text[i])
                              || text[i] == ':')) {
            i++;
        }
        size_t n = i - start;
        if (n == 0) {
            continue;
        }
        char token[32];
        if (n >= sizeof(token)) {
            n = sizeof(token) - 1;
        }
        memcpy(token, text + start, n);
        token[n] = '\0';
        char *end1 = NULL, *end2 = NULL, *end3 = NULL;
        long a = strtol(token, &end1, 10);
        long b = *end1 == ':' ? strtol(end1 + 1, &end2, 10) : -1;
        long c = end2 != NULL && *end2 == ':' ? strtol(end2 + 1, &end3, 10)
                                              : -1;
        if (hour < 0 && isdigit((unsigned char)token[0]) && end3 != NULL
            && end3 != end2 + 1 && end2 != end1 + 1 && *end3 == '\0') {
            hour = (int)a;
            minute = (int)b;
            second = (int)c;
        } else if (day < 0 && isdigit((unsigned char)token[0]) && n <= 2) {
            day = atoi(token);
        } else if (month < 0 && n >= 3 && isalpha((unsigned char)token[0])) {
            for (int m = 0; m < 12; m++) {
                if (tolower((unsigned char)token[0]) == months[m * 3]
                    && tolower((unsigned char)token[1]) == months[m * 3 + 1]
                    && tolower((unsigned char)token[2]) == months[m * 3 + 2]) {
                    month = m + 1;
                    break;
                }
            }
        } else if (year < 0 && isdigit((unsigned char)token[0])
                   && (n == 2 || n == 4)) {
            year = atoi(token);
        }
    }
    if (year >= 70 && year <= 99) {
        year += 1900;
    } else if (year >= 0 && year <= 69) {
        year += 2000;
    }
    if (hour < 0 || day < 1 || day > 31 || month < 0 || year < 1601
        || hour > 23 || minute > 59 || second > 59) {
        return -1;
    }
    return days_from_civil(year, (unsigned)month, (unsigned)day) * 86400
         + hour * 3600 + minute * 60 + second;
}

/* ------------------------------------------------------------------ jar */

static void cookie_free(struct cookie *c) {
    free(c->name);
    free(c->value);
    c->name = c->value = NULL;
}

static void cookie_remove(int index) {
    if (jar[index].expires != 0) {
        dirty = true;
    }
    cookie_free(&jar[index]);
    memmove(jar + index, jar + index + 1,
            (size_t)(cookie_count - index - 1) * sizeof(jar[0]));
    cookie_count--;
}

static void remove_expired(void) {
    long long t = now();
    for (int i = cookie_count - 1; i >= 0; i--) {
        if (jar[i].expires != 0 && jar[i].expires <= t) {
            cookie_remove(i);
        }
    }
}

/* Makes room for one more cookie in domain: drops expired cookies, then
   the oldest of the domain when it has too many, then the oldest of all. */
static bool make_room(const char *domain) {
    remove_expired();
    int in_domain = 0, oldest_domain = -1, oldest = -1;
    for (int i = 0; i < cookie_count; i++) {
        if (strcmp(jar[i].domain, domain) == 0) {
            in_domain++;
            if (oldest_domain < 0
                || jar[i].created < jar[oldest_domain].created) {
                oldest_domain = i;
            }
        }
        if (oldest < 0 || jar[i].created < jar[oldest].created) {
            oldest = i;
        }
    }
    if (in_domain >= COOKIES_PER_DOMAIN) {
        cookie_remove(oldest_domain);
    } else if (cookie_count >= COOKIES_MAX) {
        cookie_remove(oldest);
    }
    return cookie_count < COOKIES_MAX;
}

static bool text_ok(const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f || c == ';') {
            return false;
        }
    }
    return true;
}

static void trim(const char **start, size_t *length) {
    while (*length > 0 && (**start == ' ' || **start == '\t')) {
        (*start)++;
        (*length)--;
    }
    while (*length > 0
           && ((*start)[*length - 1] == ' ' || (*start)[*length - 1] == '\t')) {
        (*length)--;
    }
}

static bool attribute_is(const char *name, size_t length, const char *word) {
    size_t n = strlen(word);
    if (length != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)name[i]) != word[i]) {
            return false;
        }
    }
    return true;
}

/* Puts a parsed cookie into the jar, replacing one with the same name,
   domain and path. Takes name and value. */
static void jar_store(struct cookie *c) {
    long long t = now();
    for (int i = 0; i < cookie_count; i++) {
        struct cookie *old = &jar[i];
        if (strcmp(old->name, c->name) == 0
            && strcmp(old->domain, c->domain) == 0
            && strcmp(old->path, c->path) == 0
            && old->host_only == c->host_only) {
            c->created = old->created;
            cookie_remove(i);
            break;
        }
    }
    if (c->expires != 0 && c->expires <= t) {
        cookie_free(c); /* a deletion */
        return;
    }
    if (!make_room(c->domain)) {
        cookie_free(c);
        return;
    }
    jar[cookie_count++] = *c;
    if (c->expires != 0) {
        dirty = true;
    }
}

static void cookie_set(void *context, const char *url_text,
                       const char *header) {
    (void)context;
    struct cookie_url url;
    if (jar == NULL || !url_parse(url_text, &url)) {
        return;
    }
    /* name=value */
    size_t pair = strcspn(header, ";");
    const char *equals = memchr(header, '=', pair);
    const char *name = header, *value;
    size_t name_length, value_length;
    if (equals == NULL) {
        return; /* browsers differ here; skipping is the safe choice */
    }
    name_length = (size_t)(equals - header);
    value = equals + 1;
    value_length = pair - name_length - 1;
    trim(&name, &name_length);
    trim(&value, &value_length);
    if (value_length >= 2 && value[0] == '"' && value[value_length - 1] == '"') {
        value++;
        value_length -= 2;
    }
    if (name_length == 0 || name_length + value_length > COOKIE_TEXT_MAX
        || !text_ok(name, name_length) || !text_ok(value, value_length)
        || memchr(name, '=', name_length) != NULL) {
        return;
    }
    struct cookie c;
    memset(&c, 0, sizeof(c));
    c.host_only = true;
    snprintf(c.domain, sizeof(c.domain), "%s", url.host);
    bool have_max_age = false, have_domain = false, have_path = false;
    long long t = now();

    const char *at = header + pair;
    while (*at == ';') {
        at++;
        size_t length = strcspn(at, ";");
        const char *attr = at;
        at += length;
        const char *eq = memchr(attr, '=', length);
        size_t attr_length = eq != NULL ? (size_t)(eq - attr) : length;
        const char *val = eq != NULL ? eq + 1 : attr + length;
        size_t val_length = eq != NULL ? length - attr_length - 1 : 0;
        trim(&attr, &attr_length);
        trim(&val, &val_length);
        if (attribute_is(attr, attr_length, "expires")) {
            if (!have_max_age) {
                long long when = parse_date(val, val_length);
                if (when >= 0) {
                    c.expires = when > 0 ? when : 1;
                }
            }
        } else if (attribute_is(attr, attr_length, "max-age")) {
            char digits[24];
            if (val_length == 0 || val_length >= sizeof(digits)) {
                continue;
            }
            memcpy(digits, val, val_length);
            digits[val_length] = '\0';
            char *end;
            long long seconds = strtol(digits, &end, 10);
            if (*end != '\0') {
                continue;
            }
            have_max_age = true;
            c.expires = seconds <= 0 ? 1 : t + seconds;
        } else if (attribute_is(attr, attr_length, "domain")) {
            while (val_length > 0 && *val == '.') {
                val++;
                val_length--;
            }
            if (val_length == 0 || val_length >= sizeof(c.domain)) {
                continue;
            }
            char domain[128];
            for (size_t i = 0; i < val_length; i++) {
                domain[i] = (char)tolower((unsigned char)val[i]);
            }
            domain[val_length] = '\0';
            if (strcmp(domain, url.host) == 0) {
                have_domain = true; /* same as no attribute, but marked */
                continue;
            }
            if (!domain_match(url.host, domain) || shared_suffix(domain)) {
                return; /* a cookie for a site this response is not from */
            }
            strcpy(c.domain, domain);
            c.host_only = false;
            have_domain = true;
        } else if (attribute_is(attr, attr_length, "path")) {
            if (val_length > 0 && val[0] == '/'
                && val_length < sizeof(c.path)) {
                memcpy(c.path, val, val_length);
                c.path[val_length] = '\0';
                have_path = true;
            }
        } else if (attribute_is(attr, attr_length, "secure")) {
            c.secure = true;
        }
    }
    if (!have_path) {
        /* The folder of the request path. */
        const char *slash = strrchr(url.path, '/');
        size_t length = slash != NULL ? (size_t)(slash - url.path) : 0;
        if (length == 0 || length >= sizeof(c.path)) {
            strcpy(c.path, "/");
        } else {
            memcpy(c.path, url.path, length);
            c.path[length] = '\0';
        }
    }
    /* A plain-HTTP page may not set or overwrite secure cookies, and the
       __Secure- and __Host- name prefixes promise more about the cookie. */
    if (c.secure && !url.secure) {
        return;
    }
    if (strncmp(name, "__Secure-", 9) == 0 && !c.secure) {
        return;
    }
    if (strncmp(name, "__Host-", 7) == 0
        && (!c.secure || !c.host_only || have_domain
            || strcmp(c.path, "/") != 0)) {
        return;
    }
    for (int i = 0; i < cookie_count; i++) {
        /* An http page cannot replace a secure cookie of the same name. */
        if (!url.secure && jar[i].secure
            && strlen(jar[i].name) == name_length
            && strncmp(jar[i].name, name, name_length) == 0
            && (domain_match(jar[i].domain, c.domain)
                || domain_match(c.domain, jar[i].domain))) {
            return;
        }
    }
    c.name = malloc(name_length + 1);
    c.value = malloc(value_length + 1);
    if (c.name == NULL || c.value == NULL) {
        cookie_free(&c);
        return;
    }
    memcpy(c.name, name, name_length);
    c.name[name_length] = '\0';
    memcpy(c.value, value, value_length);
    c.value[value_length] = '\0';
    c.created = t > last_created ? t : last_created + 1;
    last_created = c.created;
    jar_store(&c);
}

static int compare_order(const void *one, const void *other) {
    const struct cookie *a = *(const struct cookie *const *)one;
    const struct cookie *b = *(const struct cookie *const *)other;
    size_t la = strlen(a->path), lb = strlen(b->path);
    if (la != lb) {
        return la > lb ? -1 : 1;
    }
    return a->created < b->created ? -1 : a->created > b->created;
}

static void cookie_get(void *context, const char *url_text, char *out,
                       size_t capacity) {
    (void)context;
    out[0] = '\0';
    struct cookie_url url;
    if (jar == NULL || cookie_count == 0 || !url_parse(url_text, &url)) {
        return;
    }
    remove_expired();
    struct cookie **matches = malloc((size_t)cookie_count * sizeof(*matches));
    if (matches == NULL) {
        return;
    }
    int count = 0;
    for (int i = 0; i < cookie_count; i++) {
        struct cookie *c = &jar[i];
        bool host = c->host_only ? strcmp(url.host, c->domain) == 0
                                 : domain_match(url.host, c->domain);
        if (host && path_match(url.path, c->path)
            && (!c->secure || url.secure)) {
            matches[count++] = c;
        }
    }
    qsort(matches, (size_t)count, sizeof(*matches), compare_order);
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        size_t need = strlen(matches[i]->name) + 1 + strlen(matches[i]->value)
                    + (used > 0 ? 2 : 0);
        if (used + need + 1 > capacity) {
            continue; /* leave out what does not fit */
        }
        used += (size_t)snprintf(out + used, capacity - used, "%s%s=%s",
                                 used > 0 ? "; " : "", matches[i]->name,
                                 matches[i]->value);
    }
    free(matches);
}

/* ---------------------------------------------------------------- file */

static bool field(char **at, char **out) {
    if (*at == NULL) {
        return false;
    }
    *out = *at;
    char *tab = strchr(*at, '\t');
    if (tab != NULL) {
        *tab = '\0';
        *at = tab + 1;
    } else {
        *at = NULL;
    }
    return true;
}

static void load(void) {
    long fd = opos_open(cookies_path, OPOS_O_RDONLY);
    if (fd < 0) {
        return;
    }
    size_t used = 0, capacity = 16384;
    char *data = malloc(capacity + 1);
    while (data != NULL) {
        if (used + 4096 > capacity) {
            if (capacity >= COOKIE_FILE_MAX) {
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
    if (data == NULL) {
        return;
    }
    data[used] = '\0';
    long long t = now();
    for (char *line = data; *line != '\0' && cookie_count < COOKIES_MAX;) {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        char *at = line, *domain, *host_only, *path, *secure, *expires,
             *created, *name, *value;
        if (field(&at, &domain) && field(&at, &host_only) && field(&at, &path)
            && field(&at, &secure) && field(&at, &expires)
            && field(&at, &created) && field(&at, &name) && field(&at, &value)
            && strlen(domain) < sizeof(jar[0].domain)
            && strlen(path) < sizeof(jar[0].path) && name[0] != '\0') {
            struct cookie c;
            memset(&c, 0, sizeof(c));
            strcpy(c.domain, domain);
            strcpy(c.path, path);
            c.host_only = host_only[0] == '1';
            c.secure = secure[0] == '1';
            c.expires = strtol(expires, NULL, 10);
            c.created = strtol(created, NULL, 10);
            c.name = strdup(name);
            c.value = strdup(value);
            if (c.expires > t && c.name != NULL && c.value != NULL) {
                jar[cookie_count++] = c;
                if (c.created > last_created) {
                    last_created = c.created;
                }
            } else {
                cookie_free(&c);
            }
        }
        line = end != NULL ? end + 1 : line + strlen(line);
    }
    free(data);
}

void cookies_save(void) {
    if (!dirty || cookies_path[0] == '\0' || jar == NULL) {
        return;
    }
    dirty = false;
    char temporary[210];
    snprintf(temporary, sizeof(temporary), "%s.new", cookies_path);
    opos_unlink(temporary);
    long fd = opos_open(temporary, OPOS_O_WRONLY | OPOS_O_CREAT
                                       | OPOS_O_TRUNC);
    if (fd < 0) {
        return;
    }
    bool ok = true;
    size_t capacity = COOKIE_TEXT_MAX + 512;
    char *line = malloc(capacity);
    ok = line != NULL;
    for (int i = 0; ok && i < cookie_count; i++) {
        const struct cookie *c = &jar[i];
        if (c->expires == 0) {
            continue;
        }
        int length = snprintf(line, capacity, "%s\t%d\t%s\t%d\t%lld\t%lld\t%s\t%s\n",
                              c->domain, c->host_only ? 1 : 0, c->path,
                              c->secure ? 1 : 0, c->expires, c->created,
                              c->name, c->value);
        ok = length > 0 && (size_t)length < capacity
          && opos_write((int)fd, line, (size_t)length) == length;
    }
    free(line);
    opos_close((int)fd);
    if (ok) {
        opos_unlink(cookies_path);
        ok = opos_rename(temporary, cookies_path) == 0;
    }
    if (!ok) {
        opos_unlink(temporary);
        dirty = true;
    }
}

void cookies_open(const char *data_dir) {
    jar = calloc(COOKIES_MAX, sizeof(*jar));
    if (jar == NULL || data_dir[0] == '\0') {
        return;
    }
    snprintf(cookies_path, sizeof(cookies_path), "%s/cookies.txt", data_dir);
    load();
}

void cookies_attach(bool store) {
    opos_http_set_cookies(cookie_get, store ? cookie_set : NULL, NULL);
}

void cookies_detach(void) {
    opos_http_set_cookies(NULL, NULL, NULL);
    cookies_save();
}

/* ------------------------------------------------------- about:cookies */

static int compare_domain(const void *one, const void *other) {
    return strcmp(((const struct cookie *)one)->domain,
                  ((const struct cookie *)other)->domain);
}

int cookies_sites(void (*each)(void *context, int index, const char *site,
                               int count),
                  void *context) {
    if (jar == NULL) {
        return 0;
    }
    remove_expired();
    qsort(jar, (size_t)cookie_count, sizeof(jar[0]), compare_domain);
    int sites = 0;
    for (int i = 0; i < cookie_count;) {
        int j = i;
        while (j < cookie_count && strcmp(jar[j].domain, jar[i].domain) == 0) {
            j++;
        }
        each(context, sites++, jar[i].domain, j - i);
        i = j;
    }
    return sites;
}

void cookies_forget(int site) {
    if (jar == NULL) {
        return;
    }
    qsort(jar, (size_t)cookie_count, sizeof(jar[0]), compare_domain);
    int index = 0;
    for (int i = 0; i < cookie_count;) {
        int j = i;
        while (j < cookie_count && strcmp(jar[j].domain, jar[i].domain) == 0) {
            j++;
        }
        if (index++ == site || site < 0) {
            for (int k = j - 1; k >= i; k--) {
                cookie_remove(k);
            }
            if (site >= 0) {
                break;
            }
            j = i;
        }
        i = j;
    }
    cookies_save();
}
