/*
 * HTTP/HTTPS client: see include/opos_http.h.
 *
 * One request per connection ("Connection: close"). The response body is
 * read into memory, chunked coding is removed while reading, and gzip or
 * deflate content coding is undone afterwards.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opos_http.h>
#include <opos_tls.h>

#define HEADER_MAX 16384
#define READ_BUFFER 16384
#define USER_AGENT "Mozilla/5.0 (compatible; OPOS)"

static opos_http_progress progress_callback;
static void *progress_context;

void opos_http_set_progress(opos_http_progress callback, void *context) {
    progress_callback = callback;
    progress_context = context;
}

/* ---------------------------------------------------------------- URLs */

struct url {
    bool secure;
    char host[256];
    unsigned port;
    char path[OPOS_HTTP_URL_MAX]; /* with the query, without the fragment */
};

static bool parse_url(const char *text, struct url *url) {
    url->secure = false;
    if (strncmp(text, "https://", 8) == 0) {
        url->secure = true;
        text += 8;
    } else if (strncmp(text, "http://", 7) == 0) {
        text += 7;
    } else {
        return false;
    }
    size_t host_length = 0;
    while (text[host_length] != '\0' && text[host_length] != '/'
           && text[host_length] != ':' && text[host_length] != '?'
           && text[host_length] != '#') {
        host_length++;
    }
    if (host_length == 0 || host_length >= sizeof(url->host)) {
        return false;
    }
    memcpy(url->host, text, host_length);
    url->host[host_length] = '\0';
    text += host_length;
    url->port = url->secure ? 443 : 80;
    if (*text == ':') {
        url->port = 0;
        text++;
        while (*text >= '0' && *text <= '9') {
            url->port = url->port * 10 + (unsigned)(*text++ - '0');
            if (url->port > 65535) {
                return false;
            }
        }
        if (url->port == 0) {
            return false;
        }
    }
    size_t length = strcspn(text, "#");
    if (length + 2 > sizeof(url->path)) {
        return false;
    }
    size_t used = 0;
    if (*text != '/') {
        url->path[used++] = '/';
    }
    memcpy(url->path + used, text, length);
    url->path[used + length] = '\0';
    return true;
}

/* Appends to out; false when it does not fit. */
static bool append(char *out, size_t capacity, size_t *used, const char *text,
                   size_t length) {
    if (*used + length + 1 > capacity) {
        return false;
    }
    memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

/* Removes "." and ".." segments from path (starting with '/'), in place.
   A query string after '?' is kept as it is. */
static void remove_dots(char *path) {
    size_t query = strcspn(path, "?");
    char tail[OPOS_HTTP_URL_MAX];
    strcpy(tail, path + query);
    path[query] = '\0';
    char *write = path;
    const char *read = path;
    while (*read != '\0') {
        /* read points at '/' here */
        const char *end = read + 1;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        size_t length = (size_t)(end - read - 1);
        const char *segment = read + 1;
        if (length == 1 && segment[0] == '.') {
            if (*end == '\0') {
                *write++ = '/';
            }
        } else if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            /* Drop the previous segment (never above the root). */
            if (write > path) {
                write--;
                while (write > path && *write != '/') {
                    write--;
                }
            }
            if (*end == '\0') {
                *write++ = '/';
            }
        } else {
            memmove(write, read, (size_t)(end - read));
            write += end - read;
        }
        read = end;
    }
    if (write == path) {
        *write++ = '/';
    }
    *write = '\0';
    strcat(path, tail);
}

bool opos_url_resolve(const char *base, const char *reference, char *out,
                      size_t capacity) {
    while (*reference == ' ') {
        reference++;
    }
    size_t used = 0;
    out[0] = '\0';
    /* An absolute reference needs no base. */
    if (strncmp(reference, "http://", 7) == 0
        || strncmp(reference, "https://", 8) == 0) {
        struct url parsed;
        if (!parse_url(reference, &parsed)) {
            return false;
        }
        return append(out, capacity, &used, reference, strlen(reference));
    }
    struct url parsed;
    if (!parse_url(base, &parsed)) {
        return false;
    }
    char origin[300];
    int made = snprintf(origin, sizeof(origin), "%s://%s",
                        parsed.secure ? "https" : "http", parsed.host);
    if (made <= 0 || (size_t)made >= sizeof(origin)) {
        return false;
    }
    bool default_port = parsed.port == (parsed.secure ? 443u : 80u);
    if (!default_port) {
        made += snprintf(origin + made, sizeof(origin) - (size_t)made, ":%u",
                         parsed.port);
    }
    if (reference[0] == '/' && reference[1] == '/') {
        const char *scheme = parsed.secure ? "https:" : "http:";
        return append(out, capacity, &used, scheme, strlen(scheme))
            && append(out, capacity, &used, reference, strlen(reference));
    }
    if (reference[0] == '#') {
        size_t base_length = strcspn(base, "#");
        return append(out, capacity, &used, base, base_length)
            && append(out, capacity, &used, reference, strlen(reference));
    }
    char path[OPOS_HTTP_URL_MAX];
    size_t fragment_at = strcspn(reference, "#");
    char fragment[OPOS_HTTP_URL_MAX];
    strcpy(fragment, reference + fragment_at);
    char ref[OPOS_HTTP_URL_MAX];
    if (fragment_at >= sizeof(ref)) {
        return false;
    }
    memcpy(ref, reference, fragment_at);
    ref[fragment_at] = '\0';
    if (ref[0] == '\0') {
        strcpy(path, parsed.path);
    } else if (ref[0] == '/') {
        strcpy(path, ref);
    } else if (ref[0] == '?') {
        size_t query = strcspn(parsed.path, "?");
        if (query + strlen(ref) + 1 > sizeof(path)) {
            return false;
        }
        memcpy(path, parsed.path, query);
        strcpy(path + query, ref);
    } else {
        /* Relative to the folder of the base path. */
        size_t query = strcspn(parsed.path, "?");
        size_t folder = query;
        while (folder > 0 && parsed.path[folder - 1] != '/') {
            folder--;
        }
        if (folder + strlen(ref) + 1 > sizeof(path)) {
            return false;
        }
        memcpy(path, parsed.path, folder);
        strcpy(path + folder, ref);
    }
    remove_dots(path);
    return append(out, capacity, &used, origin, strlen(origin))
        && append(out, capacity, &used, path, strlen(path))
        && append(out, capacity, &used, fragment, strlen(fragment));
}

/* ---------------------------------------------------------- connection */

struct connection {
    long fd;
    struct opos_tls *tls;
    char buffer[READ_BUFFER];
    size_t position;
    size_t length;
};

static long raw_read(struct connection *c, void *buffer, size_t capacity) {
    return c->tls != NULL ? opos_tls_read(c->tls, buffer, capacity)
                          : opos_recv(c->fd, buffer, capacity);
}

static long raw_write(struct connection *c, const void *data, size_t length) {
    return c->tls != NULL ? opos_tls_write(c->tls, data, length)
                          : opos_send(c->fd, data, length);
}

static void connection_close(struct connection *c) {
    if (c->tls != NULL) {
        opos_tls_close(c->tls);
        c->tls = NULL;
    } else if (c->fd >= 0) {
        opos_close((int)c->fd);
    }
    c->fd = -1;
}

/* >0 bytes ready, 0 at the end of the stream, <0 on an error. */
static long fill(struct connection *c) {
    if (c->position < c->length) {
        return (long)(c->length - c->position);
    }
    long got = raw_read(c, c->buffer, sizeof(c->buffer));
    if (got <= 0) {
        return got;
    }
    c->position = 0;
    c->length = (size_t)got;
    return got;
}

static long read_line(struct connection *c, char *line, size_t capacity) {
    size_t used = 0;
    for (;;) {
        long ready = fill(c);
        if (ready <= 0) {
            return ready < 0 ? ready : -OPOS_ECONNRESET;
        }
        char character = c->buffer[c->position++];
        if (character == '\n') {
            if (used > 0 && line[used - 1] == '\r') {
                used--;
            }
            line[used] = '\0';
            return (long)used;
        }
        if (used + 1 >= capacity) {
            /* An over-long header line: keep what fits, skip the rest. */
            continue;
        }
        line[used++] = character;
    }
}

static long connect_plain(const char *host, uint16_t port,
                          struct connection *c) {
    uint8_t ip[4];
    long result = opos_net_resolve(host, ip);
    if (result < 0) {
        return result;
    }
    c->fd = opos_socket(OPOS_SOCK_STREAM);
    if (c->fd < 0) {
        return c->fd;
    }
    struct opos_sockaddr address = opos_net_address(ip, port);
    result = opos_connect(c->fd, &address);
    if (result < 0) {
        opos_close((int)c->fd);
        c->fd = -1;
    }
    return result;
}

/* ---------------------------------------------------------------- body */

struct body {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t max;
    size_t total; /* announced size or 0 */
};

static long body_add(struct body *b, const uint8_t *data, size_t length) {
    if (length > b->max - b->length) {
        return -OPOS_EFBIG;
    }
    if (b->length + length > b->capacity) {
        size_t capacity = b->capacity == 0 ? 16384 : b->capacity;
        while (capacity < b->length + length) {
            capacity *= 2;
        }
        if (capacity > b->max) {
            capacity = b->max;
        }
        uint8_t *grown = realloc(b->data, capacity + 1);
        if (grown == NULL) {
            return -OPOS_ENOMEM;
        }
        b->data = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, data, length);
    b->length += length;
    if (progress_callback != NULL) {
        progress_callback(progress_context, b->length, b->total);
    }
    return 0;
}

/* count < 0: until the end of the stream. */
static long read_body(struct connection *c, long long count, struct body *b) {
    while (count != 0) {
        long ready = fill(c);
        if (ready < 0) {
            /* A server that closes a delimited-by-close TLS stream without
               close_notify is accepted once some body has arrived. */
            return count < 0 && b->length > 0 ? 0 : ready;
        }
        if (ready == 0) {
            return count < 0 ? 0 : -OPOS_ECONNRESET;
        }
        size_t take = (size_t)ready;
        if (count > 0 && (long long)take > count) {
            take = (size_t)count;
        }
        long result = body_add(b, (const uint8_t *)c->buffer + c->position,
                               take);
        if (result < 0) {
            return result;
        }
        c->position += take;
        if (count > 0) {
            count -= (long long)take;
        }
    }
    return 0;
}

static long read_chunked(struct connection *c, struct body *b) {
    char line[256];
    for (;;) {
        long length = read_line(c, line, sizeof(line));
        if (length < 0) {
            return length;
        }
        char *end;
        unsigned long size = strtoul(line, &end, 16);
        if (end == line || (*end != '\0' && *end != ';' && *end != ' ')) {
            return -OPOS_EINVAL;
        }
        if (size == 0) {
            do {
                length = read_line(c, line, sizeof(line));
            } while (length > 0);
            return length < 0 ? length : 0;
        }
        long result = read_body(c, (long long)size, b);
        if (result < 0) {
            return result;
        }
        length = read_line(c, line, sizeof(line));
        if (length != 0) {
            return length < 0 ? length : -OPOS_EINVAL;
        }
    }
}

/* ------------------------------------------------------------- request */

static bool header_is(const char *line, const char *name, const char **value) {
    size_t length = strlen(name);
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)line[i]) != name[i]) {
            return false;
        }
    }
    if (line[length] != ':') {
        return false;
    }
    line += length + 1;
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    *value = line;
    return true;
}

static bool contains_folded(const char *text, const char *word) {
    size_t length = strlen(word);
    for (; *text != '\0'; text++) {
        size_t i = 0;
        while (i < length && tolower((unsigned char)text[i]) == word[i]) {
            i++;
        }
        if (i == length) {
            return true;
        }
    }
    return false;
}

void opos_http_free(struct opos_http_response *response) {
    free(response->body);
    response->body = NULL;
    response->length = 0;
}

long opos_http_get(const char *address, size_t max_body,
                   struct opos_http_response *response) {
    memset(response, 0, sizeof(*response));
    if (strlen(address) >= OPOS_HTTP_URL_MAX) {
        return -OPOS_ENAMETOOLONG;
    }
    char location[OPOS_HTTP_URL_MAX];
    strcpy(location, address);
    struct connection *c = malloc(sizeof(*c));
    char *line = malloc(HEADER_MAX);
    char *request = malloc(OPOS_HTTP_URL_MAX + 512);
    struct url *url = malloc(sizeof(*url));
    long result = -OPOS_ENOMEM;
    struct body body = {NULL, 0, 0, max_body, 0};
    if (c == NULL || line == NULL || request == NULL || url == NULL) {
        goto done;
    }
    for (int redirects = 0;; redirects++) {
        if (!parse_url(location, url)) {
            result = -OPOS_EINVAL;
            goto done;
        }
        c->fd = -1;
        c->tls = NULL;
        c->position = 0;
        c->length = 0;
        result = url->secure
            ? opos_tls_connect(url->host, (uint16_t)url->port, &c->tls)
            : connect_plain(url->host, (uint16_t)url->port, c);
        if (result < 0) {
            goto done;
        }
        bool default_port = url->port == (url->secure ? 443u : 80u);
        char host_header[300];
        if (default_port) {
            snprintf(host_header, sizeof(host_header), "%s", url->host);
        } else {
            snprintf(host_header, sizeof(host_header), "%s:%u", url->host,
                     url->port);
        }
        int length = snprintf(request, OPOS_HTTP_URL_MAX + 512,
                              "GET %s HTTP/1.1\r\nHost: %s\r\n"
                              "User-Agent: " USER_AGENT "\r\n"
                              "Accept: text/html,*/*;q=0.8\r\n"
                              "Accept-Encoding: gzip\r\n"
                              "Connection: close\r\n\r\n",
                              url->path, host_header);
        if (length <= 0 || length >= OPOS_HTTP_URL_MAX + 512
            || raw_write(c, request, (size_t)length) != length) {
            connection_close(c);
            result = -OPOS_EIO;
            goto done;
        }
        long got = read_line(c, line, HEADER_MAX);
        if (got < 0) {
            connection_close(c);
            result = got;
            goto done;
        }
        const char *space = strchr(line, ' ');
        if (strncmp(line, "HTTP/1.", 7) != 0 || space == NULL) {
            connection_close(c);
            result = -OPOS_EINVAL;
            goto done;
        }
        long status = strtol(space + 1, NULL, 10);
        bool chunked = false;
        bool gzip = false;
        bool deflate = false;
        long long content_length = -1;
        bool have_location = false;
        char next[OPOS_HTTP_URL_MAX];
        response->content_type[0] = '\0';
        for (;;) {
            got = read_line(c, line, HEADER_MAX);
            if (got < 0) {
                connection_close(c);
                result = got;
                goto done;
            }
            if (got == 0) {
                break;
            }
            const char *value;
            if (header_is(line, "transfer-encoding", &value)) {
                chunked = contains_folded(value, "chunked");
            } else if (header_is(line, "content-length", &value)) {
                content_length = (long long)strtoul(value, NULL, 10);
            } else if (header_is(line, "content-encoding", &value)) {
                gzip = contains_folded(value, "gzip");
                deflate = contains_folded(value, "deflate");
            } else if (header_is(line, "content-type", &value)) {
                snprintf(response->content_type,
                         sizeof(response->content_type), "%s", value);
            } else if (header_is(line, "location", &value)) {
                have_location = opos_url_resolve(location, value, next,
                                                 sizeof(next));
            }
        }
        if ((status == 301 || status == 302 || status == 303 || status == 307
             || status == 308) && have_location
            && redirects < OPOS_HTTP_MAX_REDIRECTS) {
            connection_close(c);
            strcpy(location, next);
            continue;
        }
        if (content_length > 0 && (unsigned long long)content_length > max_body
            && !gzip && !deflate) {
            connection_close(c);
            result = -OPOS_EFBIG;
            goto done;
        }
        body.total = content_length > 0 ? (size_t)content_length : 0;
        result = chunked ? read_chunked(c, &body)
                         : read_body(c, content_length, &body);
        connection_close(c);
        if (result < 0) {
            goto done;
        }
        if (gzip || deflate) {
            uint8_t *plain;
            size_t plain_length;
            result = opos_inflate(body.data, body.length, 0, max_body,
                                  &plain, &plain_length);
            if (result < 0) {
                goto done;
            }
            free(body.data);
            body.data = plain;
            body.length = plain_length;
        } else if (body.data == NULL) {
            body.data = malloc(1);
            if (body.data == NULL) {
                result = -OPOS_ENOMEM;
                goto done;
            }
        }
        body.data[body.length] = '\0';
        response->status = status;
        strcpy(response->url, location);
        response->body = body.data;
        response->length = body.length;
        body.data = NULL;
        result = 0;
        goto done;
    }
done:
    free(body.data);
    free(c);
    free(line);
    free(request);
    free(url);
    return result;
}
