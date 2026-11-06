#include <opos_net.h>
#include <stdio.h>
#include <string.h>

long opos_net_parse_ip(const char *text, uint8_t ip[4]) {
    for (int part = 0; part < 4; part++) {
        if (*text < '0' || *text > '9') {
            return -OPOS_EINVAL;
        }
        unsigned value = 0;
        int digits = 0;
        while (*text >= '0' && *text <= '9') {
            value = value * 10 + (unsigned)(*text++ - '0');
            if (++digits > 3 || value > 255) {
                return -OPOS_EINVAL;
            }
        }
        ip[part] = (uint8_t)value;
        if (part < 3 && *text++ != '.') {
            return -OPOS_EINVAL;
        }
    }
    return *text == '\0' ? 0 : -OPOS_EINVAL;
}

void opos_net_format_ip(const uint8_t ip[4], char text[16]) {
    snprintf(text, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/* Skips a (possibly compressed) name in a DNS message. */
static size_t dns_skip_name(const uint8_t *message, size_t length,
                            size_t at) {
    while (at < length) {
        uint8_t label = message[at];
        if (label == 0) {
            return at + 1;
        }
        if ((label & 0xc0) == 0xc0) {
            return at + 2;
        }
        at += 1 + label;
    }
    return length + 1;
}

long opos_net_resolve(const char *name, uint8_t ip[4]) {
    if (opos_net_parse_ip(name, ip) == 0) {
        return 0;
    }
    size_t name_length = strlen(name);
    if (name_length == 0 || name_length > 253) {
        return -OPOS_EINVAL;
    }
    uint8_t query[512];
    static uint16_t next_id = 0x4f50;
    uint16_t id = next_id++;
    size_t at = 12;
    memset(query, 0, 12);
    query[0] = (uint8_t)(id >> 8);
    query[1] = (uint8_t)id;
    query[2] = 0x01; /* recursion desired */
    query[5] = 1;    /* one question */
    /* "www.example.com" as 3www7example3com0. */
    const char *label = name;
    while (*label != '\0') {
        const char *end = label;
        while (*end != '\0' && *end != '.') {
            end++;
        }
        size_t size = (size_t)(end - label);
        if (size == 0 || size > 63) {
            return -OPOS_EINVAL;
        }
        query[at++] = (uint8_t)size;
        memcpy(query + at, label, size);
        at += size;
        label = *end == '.' ? end + 1 : end;
    }
    query[at++] = 0;
    query[at++] = 0;
    query[at++] = 1; /* type A */
    query[at++] = 0;
    query[at++] = 1; /* class IN */

    long fd = opos_socket(OPOS_SOCK_DGRAM);
    if (fd < 0) {
        return fd;
    }
    static const uint8_t server_ip[4] = {127, 0, 0, 1};
    struct opos_sockaddr server = opos_net_address(server_ip, 53);
    long result = -OPOS_ETIMEDOUT;
    for (int attempt = 0; attempt < 3 && result == -OPOS_ETIMEDOUT;
         attempt++) {
        long sent = opos_sendto(fd, query, at, &server);
        if (sent < 0) {
            result = sent;
            break;
        }
        struct opos_pollfd wait = {(int32_t)fd, OPOS_POLLIN, 0};
        while (opos_poll(&wait, 1, 2000) == 1) {
            uint8_t answer[512];
            long length = opos_recvfrom(fd, answer, sizeof(answer), NULL);
            if (length < 12 || answer[0] != query[0]
                || answer[1] != query[1] || (answer[2] & 0x80) == 0) {
                continue; /* not our answer */
            }
            if ((answer[3] & 0x0f) == 3) {
                result = -OPOS_ENOENT;
                break;
            }
            size_t count = (size_t)answer[6] << 8 | answer[7];
            size_t position = dns_skip_name(answer, (size_t)length, 12) + 4;
            result = -OPOS_ENOENT;
            for (size_t i = 0; i < count && position < (size_t)length; i++) {
                position = dns_skip_name(answer, (size_t)length, position);
                if (position + 10 > (size_t)length) {
                    break;
                }
                uint16_t type = (uint16_t)(answer[position] << 8
                                           | answer[position + 1]);
                size_t data = (size_t)answer[position + 8] << 8
                            | answer[position + 9];
                position += 10;
                if (type == 1 && data == 4 && position + 4 <= (size_t)length) {
                    memcpy(ip, answer + position, 4);
                    result = 0;
                    break;
                }
                position += data;
            }
            break;
        }
    }
    opos_close(fd);
    return result;
}
