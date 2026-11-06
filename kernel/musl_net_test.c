/*
 * Network test for the Linux socket calls, built against real musl.
 *
 *   musl-net-test [host [port]]   resolves host (default example.com) with
 *                                 getaddrinfo (UDP to 127.0.0.1:53, which
 *                                 the kernel forwards), fetches / over HTTP
 *                                 and prints the status line
 *   musl-net-test serve [port]    answers one HTTP request on port
 *                                 (default 8080) and exits
 */
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

static int fetch(const char *host, const char *port) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = NULL;
    int error = getaddrinfo(host, port, &hints, &result);
    if (error != 0) {
        printf("net-test: getaddrinfo %s: %s\n", host, gai_strerror(error));
        return 1;
    }
    struct sockaddr_in *address = (struct sockaddr_in *)result->ai_addr;
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
    printf("net-test: %s is %s\n", host, text);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
        printf("net-test: connect: %s\n", strerror(errno));
        return 1;
    }
    freeaddrinfo(result);
    struct sockaddr_in local;
    socklen_t length = sizeof(local);
    getsockname(fd, (struct sockaddr *)&local, &length);
    printf("net-test: connected from port %u\n", ntohs(local.sin_port));

    char request[256];
    int size = snprintf(request, sizeof(request),
                        "GET / HTTP/1.0\r\nHost: %s\r\n"
                        "User-Agent: OPOS-net-test\r\n\r\n", host);
    if (send(fd, request, (size_t)size, 0) != size) {
        printf("net-test: send: %s\n", strerror(errno));
        return 1;
    }
    char buffer[1024];
    size_t total = 0;
    char status[128] = "";
    for (;;) {
        ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got < 0) {
            printf("net-test: recv: %s\n", strerror(errno));
            return 1;
        }
        if (got == 0) {
            break;
        }
        if (total == 0) {
            size_t end = 0;
            while (end < (size_t)got && end < sizeof(status) - 1
                   && buffer[end] != '\r' && buffer[end] != '\n') {
                end++;
            }
            memcpy(status, buffer, end);
            status[end] = '\0';
        }
        total += (size_t)got;
    }
    close(fd);
    printf("net-test: \"%s\", %zu bytes\n", status, total);
    return strncmp(status, "HTTP/", 5) == 0 ? 0 : 1;
}

static int serve(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0
        || listen(fd, 4) != 0) {
        printf("net-test: listen: %s\n", strerror(errno));
        return 1;
    }
    printf("net-test: listening on port %d\n", port);
    struct pollfd wait = {.fd = fd, .events = POLLIN};
    int ready = poll(&wait, 1, 60000);
    if (ready != 1) {
        printf("net-test: nobody connected (%d)\n", ready);
        return 1;
    }
    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    int client = accept(fd, (struct sockaddr *)&peer, &length);
    if (client < 0) {
        printf("net-test: accept: %s\n", strerror(errno));
        return 1;
    }
    char text[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, text, sizeof(text));
    char request[512];
    ssize_t got = recv(client, request, sizeof(request) - 1, 0);
    if (got < 0) {
        got = 0;
    }
    request[got] = '\0';
    char *end = strpbrk(request, "\r\n");
    if (end != NULL) {
        *end = '\0';
    }
    printf("net-test: %s:%u asked \"%s\"\n", text, ntohs(peer.sin_port),
           request);
    static const char reply[] =
        "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n"
        "Hello from OPOS\n";
    write(client, reply, sizeof(reply) - 1);
    shutdown(client, SHUT_WR);
    /* Wait for the peer to close too. */
    while (recv(client, request, sizeof(request), 0) > 0) {
    }
    close(client);
    close(fd);
    printf("net-test: served one request\n");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "serve") == 0) {
        return serve(argc > 2 ? atoi(argv[2]) : 8080);
    }
    return fetch(argc > 1 ? argv[1] : "example.com",
                 argc > 2 ? argv[2] : "80");
}
