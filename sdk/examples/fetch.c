/*
 * Fetches a web page over plain HTTP and prints it.
 *
 *   run /disk/fetch example.com
 *   run /disk/fetch example.com/some/path
 */
#include <opos_net.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: fetch HOST[/PATH]\n");
        return 2;
    }
    char host[256];
    const char *slash = strchr(argv[1], '/');
    size_t host_length = slash != NULL ? (size_t)(slash - argv[1])
                                       : strlen(argv[1]);
    if (host_length == 0 || host_length >= sizeof(host)) {
        printf("fetch: bad host name\n");
        return 2;
    }
    memcpy(host, argv[1], host_length);
    host[host_length] = '\0';
    const char *path = slash != NULL ? slash : "/";

    uint8_t ip[4];
    long result = opos_net_resolve(host, ip);
    if (result < 0) {
        printf("fetch: cannot find %s (error %ld)\n", host, -result);
        return 1;
    }
    char text[16];
    opos_net_format_ip(ip, text);
    printf("Connecting to %s (%s)...\n", host, text);

    long fd = opos_socket(OPOS_SOCK_STREAM);
    struct opos_sockaddr address = opos_net_address(ip, 80);
    result = fd < 0 ? fd : opos_connect(fd, &address);
    if (result < 0) {
        printf("fetch: connection failed (error %ld)\n", -result);
        return 1;
    }
    char request[512];
    int length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.0\r\nHost: %s\r\n"
                          "User-Agent: OPOS-fetch\r\n\r\n", path, host);
    if (opos_send(fd, request, (size_t)length) != length) {
        printf("fetch: sending failed\n");
        return 1;
    }
    char buffer[1024];
    long got;
    while ((got = opos_recv(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[got] = '\0';
        printf("%s", buffer);
    }
    opos_close(fd);
    printf("\n");
    return got < 0 ? 1 : 0;
}
