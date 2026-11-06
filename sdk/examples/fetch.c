/*
 * Fetches a web page over HTTP or HTTPS and prints it.
 *
 *   run /bin/fetch example.com
 *   run /bin/fetch https://example.com/some/path
 *   run /bin/fetch -o /disk/page.html https://example.com
 *
 * With -o the body (without headers) goes to a file instead.
 */
#include <opos_tls.h>
#include <stdio.h>
#include <string.h>

/* One connection, plain or TLS. */
struct connection {
    long fd;
    struct opos_tls *tls;
};

static long connection_write(struct connection *c, const void *data,
                             size_t length) {
    return c->tls != NULL ? opos_tls_write(c->tls, data, length)
                          : opos_send(c->fd, data, length);
}

static long connection_read(struct connection *c, void *buffer,
                            size_t capacity) {
    return c->tls != NULL ? opos_tls_read(c->tls, buffer, capacity)
                          : opos_recv(c->fd, buffer, capacity);
}

static void connection_close(struct connection *c) {
    if (c->tls != NULL) {
        opos_tls_close(c->tls);
    } else {
        opos_close((int)c->fd);
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
    }
    return result;
}

static void usage(void) {
    printf("Usage: fetch [-o FILE] [http://|https://]HOST[:PORT][/PATH]\n");
}

int main(int argc, char **argv) {
    const char *output = NULL;
    int argument = 1;
    if (argc > 2 && strcmp(argv[1], "-o") == 0) {
        output = argv[2];
        argument = 3;
    }
    if (argument >= argc) {
        usage();
        return 2;
    }
    const char *url = argv[argument];
    bool secure = false;
    if (strncmp(url, "https://", 8) == 0) {
        secure = true;
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    }
    char host[256];
    size_t host_length = 0;
    while (url[host_length] != '\0' && url[host_length] != '/'
           && url[host_length] != ':') {
        host_length++;
    }
    if (host_length == 0 || host_length >= sizeof(host)) {
        usage();
        return 2;
    }
    memcpy(host, url, host_length);
    host[host_length] = '\0';
    const char *rest = url + host_length;
    unsigned port = secure ? 443 : 80;
    if (*rest == ':') {
        port = 0;
        rest++;
        while (*rest >= '0' && *rest <= '9') {
            port = port * 10 + (unsigned)(*rest++ - '0');
        }
        if (port == 0 || port > 65535) {
            usage();
            return 2;
        }
    }
    const char *path = *rest == '/' ? rest : "/";

    struct connection connection = {-1, NULL};
    printf("Connecting to %s:%u%s...\n", host, port, secure ? " (TLS)" : "");
    long result = secure
        ? opos_tls_connect(host, (uint16_t)port, &connection.tls)
        : connect_plain(host, (uint16_t)port, &connection);
    if (result < 0) {
        printf("fetch: %s (error %ld)\n", opos_tls_error_text(result),
               -result);
        return 1;
    }
    char request[640];
    int length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.0\r\nHost: %s\r\n"
                          "User-Agent: OPOS-fetch\r\n"
                          "Connection: close\r\n\r\n", path, host);
    if (connection_write(&connection, request, (size_t)length) != length) {
        printf("fetch: sending failed\n");
        connection_close(&connection);
        return 1;
    }

    long file = -1;
    if (output != NULL) {
        file = opos_open(output, OPOS_O_WRONLY | OPOS_O_CREAT
                                     | OPOS_O_TRUNC);
        if (file < 0) {
            printf("fetch: cannot write %s\n", output);
            connection_close(&connection);
            return 1;
        }
    }
    char buffer[2048];
    long got;
    size_t body = 0;
    /* With -o the headers are printed and the body saved; the end of the
       headers is the first empty line. */
    bool in_headers = output != NULL;
    int line_length = 0;
    while ((got = connection_read(&connection, buffer, sizeof(buffer) - 1))
           > 0) {
        size_t start = 0;
        if (in_headers) {
            while (start < (size_t)got && in_headers) {
                char character = buffer[start++];
                if (character == '\n') {
                    if (line_length == 0) {
                        in_headers = false;
                    }
                    line_length = 0;
                } else if (character != '\r') {
                    line_length++;
                }
            }
            buffer[start] = '\0';
            printf("%.*s", (int)start, buffer);
        }
        if (start == (size_t)got) {
            continue;
        }
        if (file >= 0) {
            if (opos_write((int)file, buffer + start, (size_t)got - start)
                != got - (long)start) {
                printf("fetch: writing %s failed\n", output);
                got = -1;
                break;
            }
        } else {
            buffer[got] = '\0';
            printf("%s", buffer + start);
        }
        body += (size_t)got - start;
    }
    connection_close(&connection);
    if (file >= 0) {
        opos_close((int)file);
        printf("Saved %u bytes to %s\n", (unsigned)body, output);
    } else {
        printf("\n");
    }
    if (got < 0) {
        printf("fetch: %s (error %ld)\n", opos_tls_error_text(got), -got);
        return 1;
    }
    return 0;
}
