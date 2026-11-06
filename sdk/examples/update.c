/*
 * OPOS Updater: checks for and installs new OPOS versions.
 *
 *   update                    same as "update check"
 *   update check              shows whether a newer version is out
 *   update install            downloads and installs it (administrators)
 *   update ... --source URL   uses another release folder, for example
 *                             http://10.0.2.2:8000/ for local testing
 *                             (or put the URL in SOURCE_FILE)
 *
 * A release (tools/mkrelease.py) is a signed manifest plus one file per
 * changed system file. Only files that differ from the installed ones
 * are downloaded. The kernel checks the signature and every file before
 * it installs anything (kernel/update.c); this program only downloads.
 */
#include <ctype.h>
#include <opos_tls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SOURCE \
    "https://github.com/PorkifyNET/OPOS/releases/latest/download/"
#define MANIFEST_NAME "opos-update.manifest"
#define SIGNATURE_NAME "opos-update.sig"
/* Plain text shown before installing; optional, not signed. */
#define NOTES_NAME "opos-release-notes.txt"
#define NOTES_MAX 4096
/* Optional: a different release folder URL on its first line. */
#define SOURCE_FILE "/disk/OPOS/UPDATE-SOURCE.TXT"
#define MANIFEST_MAX 8192
#define SIGNATURE_MAX 128
#define URL_MAX 2048
#define HEADER_MAX 16384
#define READ_BUFFER 16384
#define MAX_REDIRECTS 5
#define FILE_BUFFER (256 * 1024)
#define DOWNLOAD_TRIES 3
/* http_get result when the sink refused data (EFBIG). */
#define SINK_STOPPED 27

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

/* Fills the buffer when it is empty: >0 bytes ready, 0 at the end, <0. */
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

/* One line without CR/LF into line. Returns its length, or <0. */
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
            return -OPOS_EINVAL;
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

/* ---------------------------------------------------------------- URLs */

struct url {
    bool secure;
    char host[256];
    unsigned port;
    char path[URL_MAX];
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
           && text[host_length] != ':' && text[host_length] != '?') {
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
    if (*text == '\0') {
        text = "/";
    }
    if (*text != '/' || strlen(text) >= sizeof(url->path)) {
        return false;
    }
    strcpy(url->path, text);
    return true;
}

/* ----------------------------------------------------------------- HTTP */

/* Receives the body in pieces; returns false to stop. */
typedef bool (*body_sink)(void *context, const uint8_t *data, size_t length);

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

/* Streams count bytes (or everything until the end when count < 0). */
static long stream_body(struct connection *c, long long count,
                        body_sink sink, void *context) {
    while (count != 0) {
        long ready = fill(c);
        if (ready < 0) {
            return ready;
        }
        if (ready == 0) {
            return count < 0 ? 0 : -OPOS_ECONNRESET;
        }
        size_t take = (size_t)ready;
        if (count > 0 && (long long)take > count) {
            take = (size_t)count;
        }
        if (!sink(context, (const uint8_t *)c->buffer + c->position, take)) {
            return -SINK_STOPPED;
        }
        c->position += take;
        if (count > 0) {
            count -= (long long)take;
        }
    }
    return 0;
}

static long stream_chunked(struct connection *c, body_sink sink,
                           void *context) {
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
            /* Trailer lines up to an empty one. */
            do {
                length = read_line(c, line, sizeof(line));
            } while (length > 0);
            return length < 0 ? length : 0;
        }
        long result = stream_body(c, (long long)size, sink, context);
        if (result < 0) {
            return result;
        }
        length = read_line(c, line, sizeof(line));
        if (length != 0) {
            return length < 0 ? length : -OPOS_EINVAL;
        }
    }
}

static const char *current_version(void);

/*
 * GET url, following redirects, with the body going to sink. Returns 0,
 * the HTTP status (> 0) when it is not 200, or a negative error.
 */
static long http_get(const char *address, body_sink sink, void *context) {
    static char location[URL_MAX];
    static char line[HEADER_MAX];
    static struct connection connection;
    struct url url;
    if (strlen(address) >= sizeof(location)) {
        return -OPOS_EINVAL;
    }
    strcpy(location, address);
    for (int redirects = 0;; redirects++) {
        if (!parse_url(location, &url)) {
            return -OPOS_EINVAL;
        }
        struct connection *c = &connection;
        c->fd = -1;
        c->tls = NULL;
        c->position = 0;
        c->length = 0;
        long result = url.secure
            ? opos_tls_connect(url.host, (uint16_t)url.port, &c->tls)
            : connect_plain(url.host, (uint16_t)url.port, c);
        if (result < 0) {
            return result;
        }
        static char request[URL_MAX + 512];
        int length = snprintf(request, sizeof(request),
                              "GET %s HTTP/1.1\r\nHost: %s\r\n"
                              "User-Agent: OPOS-update/%s\r\n"
                              "Accept: */*\r\nConnection: close\r\n\r\n",
                              url.path, url.host, current_version());
        if (length <= 0 || (size_t)length >= sizeof(request)
            || raw_write(c, request, (size_t)length) != length) {
            connection_close(c);
            return -OPOS_EIO;
        }
        long status_length = read_line(c, line, sizeof(line));
        if (status_length < 0) {
            connection_close(c);
            return status_length;
        }
        long status = 0;
        const char *space = strchr(line, ' ');
        if (strncmp(line, "HTTP/1.", 7) != 0 || space == NULL) {
            connection_close(c);
            return -OPOS_EINVAL;
        }
        status = strtol(space + 1, NULL, 10);
        bool chunked = false;
        long long content_length = -1;
        bool have_location = false;
        for (;;) {
            long got = read_line(c, line, sizeof(line));
            if (got < 0) {
                connection_close(c);
                return got;
            }
            if (got == 0) {
                break;
            }
            const char *value;
            if (header_is(line, "transfer-encoding", &value)) {
                chunked = contains_folded(value, "chunked");
            } else if (header_is(line, "content-length", &value)) {
                content_length = (long long)strtoul(value, NULL, 10);
            } else if (header_is(line, "location", &value)) {
                if (value[0] == '/' && value[1] != '/') {
                    /* Same server, new path. */
                    int made = snprintf(location, sizeof(location),
                                        "%s://%s:%u%s",
                                        url.secure ? "https" : "http",
                                        url.host, url.port, value);
                    have_location = made > 0
                        && (size_t)made < sizeof(location);
                } else if (strlen(value) < sizeof(location)) {
                    strcpy(location, value);
                    have_location = true;
                }
            }
        }
        if (status == 301 || status == 302 || status == 303 || status == 307
            || status == 308) {
            connection_close(c);
            if (!have_location || redirects == MAX_REDIRECTS) {
                return status;
            }
            continue;
        }
        if (status != 200) {
            connection_close(c);
            return status;
        }
        result = chunked ? stream_chunked(c, sink, context)
                         : stream_body(c, content_length, sink, context);
        connection_close(c);
        return result;
    }
}

/* ------------------------------------------------------------- helpers */

static const char *current_version(void) {
    /* uname(2): release is the third 65-byte field. */
    static char name[390];
    static bool loaded;
    if (!loaded) {
        if (opos_syscall1(63, (long)name) < 0) {
            strcpy(name + 130, "unknown");
        }
        loaded = true;
    }
    return name + 130;
}

struct memory_sink {
    uint8_t *data;
    size_t length;
    size_t capacity;
};

static bool memory_write(void *context, const uint8_t *data, size_t length) {
    struct memory_sink *sink = context;
    if (length > sink->capacity - sink->length) {
        return false;
    }
    memcpy(sink->data + sink->length, data, length);
    sink->length += length;
    return true;
}

static void join(char *out, const char *base, const char *name) {
    snprintf(out, URL_MAX, "%s%s%s", base,
             base[0] != '\0' && base[strlen(base) - 1] == '/' ? "" : "/", name);
}

/*
 * --gui (Settings > UPDATES): besides the readable log, lines starting
 * with '@' tell the desktop the state:
 *   @current V  @latest V  @notes TEXT  @status none|uptodate|available|
 *   installed|error  @phase TEXT  @progress PERCENT  @error TEXT
 */
static bool gui;
static uint64_t download_total;
static uint64_t download_done;
static int last_percent = -1;

static void report_error(const char *text) {
    printf("update: %s\n", text);
    if (gui) {
        printf("@error %s\n@status error\n", text);
    }
}

static void report_progress(int percent) {
    if (gui && percent != last_percent) {
        printf("@progress %d\n", percent);
        last_percent = percent;
    }
}

static void describe_download_error(const char *what, long result) {
    char text[160];
    if (result > 0) {
        snprintf(text, sizeof(text), "downloading %s failed: HTTP status %ld",
                 what, result);
    } else if (result == -SINK_STOPPED) {
        snprintf(text, sizeof(text), "%s is larger than expected", what);
    } else {
        snprintf(text, sizeof(text), "downloading %s failed: %s (error %ld)",
                 what, opos_tls_error_text(result), -result);
    }
    report_error(text);
}

static const char *update_error_text(long error) {
    switch (-error) {
    case OPOS_UPDATE_ENOT_INSTALLED:
        return "OPOS is not installed on this computer (live media)";
    case OPOS_UPDATE_ENOT_ADMIN:
        return "only an administrator can install updates";
    case OPOS_UPDATE_EBAD_SIGNATURE:
        return "the update is not signed with the OPOS release key";
    case OPOS_UPDATE_EBAD_MANIFEST:
        return "the update's file list is damaged";
    case OPOS_UPDATE_EUNSUPPORTED:
        return "this update needs a newer updater; install it from the ISO";
    case OPOS_UPDATE_ENOT_NEWER:
        return "OPOS is up to date";
    case OPOS_UPDATE_ENO_SESSION:
        return "no update in progress";
    case OPOS_UPDATE_EBAD_FILE:
        return "a downloaded file is damaged (wrong size or checksum)";
    case OPOS_UPDATE_EINCOMPLETE:
        return "not every file was downloaded";
    case OPOS_UPDATE_EDISK:
        return "writing to the disk failed";
    case OPOS_UPDATE_EINVAL:
        return "invalid request";
    case OPOS_UPDATE_EBUSY:
        return "another program is installing an update";
    case OPOS_ENOSYS:
        return "this kernel cannot install updates";
    default:
        return "unknown error";
    }
}

/* Reads "version X" from the manifest text. */
static bool manifest_version(const char *text, size_t length, char *out,
                             size_t capacity) {
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && text[end] != '\n') {
            end++;
        }
        if (end - start > 8 && strncmp(text + start, "version ", 8) == 0
            && end - start - 8 < capacity) {
            memcpy(out, text + start + 8, end - start - 8);
            out[end - start - 8] = '\0';
            return true;
        }
        start = end + 1;
    }
    return false;
}

/* --------------------------------------------------------- file upload */

struct file_sink {
    size_t index;
    uint64_t offset;
    uint64_t size;
    uint8_t *buffer;
    size_t used;
    long error;
    uint64_t next_report;
};

static bool file_flush(struct file_sink *sink) {
    if (sink->used == 0) {
        return true;
    }
    long result = opos_update_write(sink->index, sink->offset, sink->buffer,
                                    sink->used);
    if (result < 0) {
        sink->error = result;
        return false;
    }
    sink->offset += sink->used;
    sink->used = 0;
    if (download_total != 0) {
        /* Downloading is the first 90%; installing the rest. */
        report_progress((int)((download_done + sink->offset) * 90
                              / download_total));
    }
    if (sink->offset >= sink->next_report || sink->offset == sink->size) {
        printf("  %lu of %lu KB\n", (unsigned long)(sink->offset / 1024),
               (unsigned long)(sink->size / 1024));
        sink->next_report = sink->offset + 1024 * 1024;
    }
    return true;
}

static bool file_write(void *context, const uint8_t *data, size_t length) {
    struct file_sink *sink = context;
    while (length > 0) {
        if (sink->offset + sink->used + length > sink->size) {
            sink->error = -OPOS_UPDATE_EBAD_FILE;
            return false;
        }
        size_t take = FILE_BUFFER - sink->used;
        if (take > length) {
            take = length;
        }
        memcpy(sink->buffer + sink->used, data, take);
        sink->used += take;
        data += take;
        length -= take;
        if (sink->used == FILE_BUFFER && !file_flush(sink)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- main */

static void usage(void) {
    printf("Usage: update [check|install] [--source URL|FOLDER]\n");
}

static void report_status(const char *status) {
    if (gui) {
        printf("@status %s\n", status);
    }
}

static void report_phase(const char *phase) {
    if (gui) {
        printf("@phase %s\n", phase);
    }
}

/* Prints the release notes: as "@notes" lines with --gui. */
static void show_notes(const char *text, size_t length) {
    if (length == 0) {
        return;
    }
    if (!gui) {
        printf("Release notes:\n");
    }
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && text[end] != '\n') {
            end++;
        }
        size_t line_end = end;
        if (line_end > start && text[line_end - 1] == '\r') {
            line_end--;
        }
        printf(gui ? "@notes %.*s\n" : "  %.*s\n", (int)(line_end - start),
               text + start);
        start = end + 1;
    }
}

/* Reads a local file (a source starting with '/') into sink, like
   http_get: 0, 404 when it does not exist, or a negative error. */
static long file_get(const char *path, body_sink sink, void *context) {
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        return fd == -OPOS_ENOENT || fd == -OPOS_ENOTDIR ? 404 : fd;
    }
    /* 4 KB: older OPOS kernels refuse larger file reads. */
    static uint8_t buffer[4096];
    long got;
    while ((got = opos_read((int)fd, buffer, sizeof(buffer))) > 0) {
        if (!sink(context, buffer, (size_t)got)) {
            opos_close((int)fd);
            return -SINK_STOPPED;
        }
    }
    opos_close((int)fd);
    return got;
}

/* A release folder: a URL, or a folder on a disk, USB stick or CD. */
static long source_get(const char *location, body_sink sink, void *context) {
    return location[0] == '/' ? file_get(location, sink, context)
                              : http_get(location, sink, context);
}

/*
 * Update media: a USB stick, disk or CD/DVD (or the new OPOS ISO, which
 * carries the signed manifest in /boot) with the release files at its
 * top, in opos-update/ or in boot/. The first one found is used.
 */
static const char *media_source(void) {
    static const char *const folders[] = {"/", "/opos-update/", "/boot/"};
    static char found[64];
    for (int kind = 0; kind < 2; kind++) {
        for (int number = 1; number <= 8; number++) {
            char root[16];
            if (kind == 0 && number == 1) {
                continue; /* /disk is OPOS's own volume */
            }
            if (number == 1) {
                snprintf(root, sizeof(root), "/cdrom");
            } else {
                snprintf(root, sizeof(root), "%s%d",
                         kind == 0 ? "/disk" : "/cdrom", number);
            }
            for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]);
                 i++) {
                char path[96];
                snprintf(path, sizeof(path), "%s%s%s", root, folders[i],
                         MANIFEST_NAME);
                long fd = opos_open(path, OPOS_O_RDONLY);
                if (fd >= 0) {
                    opos_close((int)fd);
                    snprintf(found, sizeof(found), "%s%s", root, folders[i]);
                    return found;
                }
            }
        }
    }
    return NULL;
}

/* SOURCE_FILE's first line, if it holds a URL or folder: another release
   folder (a mirror, or a test server). Updates are checked the same way. */
static const char *configured_source(void) {
    static char text[URL_MAX];
    long fd = opos_open(SOURCE_FILE, OPOS_O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    long got = opos_read((int)fd, text, sizeof(text) - 1);
    opos_close((int)fd);
    if (got <= 0) {
        return NULL;
    }
    text[got] = '\0';
    size_t end = 0;
    while (text[end] != '\0' && text[end] != '\n' && text[end] != '\r'
           && text[end] != ' ') {
        end++;
    }
    text[end] = '\0';
    return strncmp(text, "http://", 7) == 0 || strncmp(text, "https://", 8) == 0
        || text[0] == '/' ? text : NULL;
}

int main(int argc, char **argv) {
    bool install = false;
    /* --source, else update media, else SOURCE_FILE, else GitHub. */
    const char *source = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "check") == 0) {
            install = false;
        } else if (strcmp(argv[i], "install") == 0) {
            install = true;
        } else if (strcmp(argv[i], "--gui") == 0) {
            gui = true;
        } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            source = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (source == NULL) {
        source = media_source();
        if (source != NULL) {
            printf("Found update files on %s\n", source);
        }
    }
    if (source == NULL) {
        source = configured_source();
    }
    if (source == NULL) {
        source = DEFAULT_SOURCE;
    }

    if (gui) {
        printf("@current %s\n", current_version());
    }
    report_phase("CHECKING FOR UPDATES");
    printf("Installed version: OPOS %s\n", current_version());
    printf("Checking %s ...\n", source);
    static uint8_t manifest[MANIFEST_MAX];
    static uint8_t signature[SIGNATURE_MAX];
    static char notes[NOTES_MAX];
    static char url[URL_MAX];
    struct memory_sink manifest_sink = {manifest, 0, sizeof(manifest)};
    struct memory_sink signature_sink = {signature, 0, sizeof(signature)};
    struct memory_sink notes_sink = {(uint8_t *)notes, 0, sizeof(notes)};
    join(url, source, MANIFEST_NAME);
    long result = source_get(url, memory_write, &manifest_sink);
    if (result == 404) {
        printf("No OPOS release has been published there yet.\n");
        report_status("none");
        return 0;
    }
    if (result != 0) {
        describe_download_error(MANIFEST_NAME, result);
        return 1;
    }
    join(url, source, SIGNATURE_NAME);
    result = source_get(url, memory_write, &signature_sink);
    if (result != 0) {
        describe_download_error(SIGNATURE_NAME, result);
        return 1;
    }
    char version[32];
    if (!manifest_version((const char *)manifest, manifest_sink.length,
                          version, sizeof(version))) {
        report_error(update_error_text(-OPOS_UPDATE_EBAD_MANIFEST));
        return 1;
    }
    printf("Latest version:    OPOS %s\n", version);
    if (gui) {
        printf("@latest %s\n", version);
    }

    struct opos_user_info user;
    bool admin = opos_user_info(&user) == 0
        && (user.flags & OPOS_USER_ADMIN) != 0;
    long count = -OPOS_UPDATE_ENOT_ADMIN;
    if (admin || install) {
        count = opos_update_begin(manifest, manifest_sink.length, signature,
                                  signature_sink.length);
    }
    if (count == -OPOS_UPDATE_ENOT_NEWER) {
        printf("OPOS is up to date.\n");
        report_status("uptodate");
        return 0;
    }
    /* Release notes are only shown, so they need no signature, and a
       release without them is fine. */
    join(url, source, NOTES_NAME);
    if (source_get(url, memory_write, &notes_sink) != 0) {
        notes_sink.length = 0;
    }
    show_notes(notes, notes_sink.length);
    if (count == -OPOS_UPDATE_ENOT_ADMIN && !install) {
        printf("Run \"update install\" as an administrator to install it.\n");
        report_status("available");
        return 0;
    }
    if (count < 0) {
        report_error(update_error_text(count));
        return 1;
    }
    long needed = 0;
    struct opos_update_file files[32];
    if (count > 32) {
        count = 32;
    }
    for (long i = 0; i < count; i++) {
        result = opos_update_file((size_t)i, &files[i]);
        if (result < 0) {
            report_error(update_error_text(result));
            opos_update_cancel();
            return 1;
        }
        if (files[i].needed) {
            download_total += files[i].size;
            needed++;
        }
    }
    printf("The signature is valid. %ld of %ld files changed (%lu KB to "
           "download).\n", needed, count,
           (unsigned long)(download_total / 1024));
    if (!install) {
        opos_update_cancel();
        printf("Run \"update install\" to install it.\n");
        report_status("available");
        return 0;
    }

    uint8_t *buffer = malloc(FILE_BUFFER);
    if (buffer == NULL) {
        report_error("out of memory");
        opos_update_cancel();
        return 1;
    }
    report_phase("DOWNLOADING");
    report_progress(0);
    for (long i = 0; i < count; i++) {
        if (!files[i].needed) {
            continue;
        }
        printf("Downloading %s (%lu KB) ...\n", files[i].asset,
               (unsigned long)(files[i].size / 1024));
        join(url, source, files[i].asset);
        struct file_sink sink;
        for (int attempt = 1;; attempt++) {
            sink = (struct file_sink){(size_t)i, 0, files[i].size, buffer, 0,
                                      0, 1024 * 1024};
            if (files[i].size == 0) {
                /* An empty file still has to be created. */
                sink.error = opos_update_write((size_t)i, 0, NULL, 0);
                result = 0;
            } else {
                result = source_get(url, file_write, &sink);
            }
            if (result == 0 && sink.error == 0 && file_flush(&sink)
                && sink.offset != sink.size) {
                sink.error = -OPOS_UPDATE_EINCOMPLETE;
            }
            if (result == 0 && sink.error == 0) {
                break;
            }
            if (sink.error != 0) {
                printf("update: %s\n", update_error_text(sink.error));
            } else {
                describe_download_error(files[i].asset, result);
            }
            if (attempt == DOWNLOAD_TRIES
                || sink.error == -OPOS_UPDATE_EDISK) {
                if (sink.error != 0 && gui) {
                    printf("@error %s\n@status error\n",
                           update_error_text(sink.error));
                }
                opos_update_cancel();
                return 1;
            }
            printf("Trying again ...\n");
            report_status("retrying");
        }
        download_done += files[i].size;
    }
    report_progress(90);
    report_phase("INSTALLING");
    printf("Installing OPOS %s (this can take a minute) ...\n", version);
    result = opos_update_commit();
    if (result < 0) {
        report_error(update_error_text(result));
        printf("If OPOS does not start, choose \"OPOS (previous version)\" "
               "in the boot menu.\n");
        return 1;
    }
    report_progress(100);
    printf("OPOS %s is installed. Restart to use it; the boot menu keeps "
           "\"OPOS (previous version)\".\n", version);
    report_status("installed");
    return 0;
}
