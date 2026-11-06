/*
 * Networking for OPOS applications: IPv4 TCP and UDP sockets.
 *
 * These are the Linux socket calls (socket, connect, bind, listen, accept,
 * send, recv, poll, ...), so code written against BSD sockets ports with
 * few changes. Results follow the OPOS convention: a value >= 0 on
 * success, or a negative error number (-OPOS_ECONNREFUSED and so on).
 *
 * Limits: 8 TCP connections and 4 listening ports for the whole system,
 * 8 UDP sockets. There is no IPv6 and no loopback, except that UDP sent to
 * 127.0.0.1 port 53 reaches the DNS server the network gave us, which is
 * what opos_net_resolve uses.
 */
#ifndef OPOS_NET_H
#define OPOS_NET_H

#include <opos.h>

#define OPOS_EAGAIN 11
#define OPOS_EINTR 4
#define OPOS_EPIPE 32
#define OPOS_ENOTSOCK 88
#define OPOS_EADDRINUSE 98
#define OPOS_ENETUNREACH 101
#define OPOS_ECONNRESET 104
#define OPOS_ENOTCONN 107
#define OPOS_ETIMEDOUT 110
#define OPOS_ECONNREFUSED 111
#define OPOS_EINPROGRESS 115

#define OPOS_AF_INET 2
#define OPOS_SOCK_STREAM 1   /* TCP */
#define OPOS_SOCK_DGRAM 2    /* UDP */
#define OPOS_SOCK_NONBLOCK 0x800
#define OPOS_SOCK_CLOEXEC 0x80000

#define OPOS_SHUT_RD 0
#define OPOS_SHUT_WR 1
#define OPOS_SHUT_RDWR 2

#define OPOS_POLLIN 0x001
#define OPOS_POLLOUT 0x004
#define OPOS_POLLERR 0x008
#define OPOS_POLLHUP 0x010
#define OPOS_POLLNVAL 0x020

/* An IPv4 address and port (struct sockaddr_in): both in network byte
   order, as opos_net_address fills them. */
struct opos_sockaddr {
    uint16_t family;
    uint16_t port;
    uint8_t ip[4];
    uint8_t zero[8];
};

struct opos_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

static inline struct opos_sockaddr opos_net_address(const uint8_t ip[4],
                                                    uint16_t port) {
    struct opos_sockaddr address = {
        OPOS_AF_INET, (uint16_t)(port >> 8 | port << 8),
        {ip[0], ip[1], ip[2], ip[3]}, {0}};
    return address;
}

static inline uint16_t opos_net_port(const struct opos_sockaddr *address) {
    return (uint16_t)(address->port >> 8 | address->port << 8);
}

static inline long opos_socket(int type) {
    return opos_syscall3(41, OPOS_AF_INET, type, 0);
}

/* Blocks until the connection is up (unless the socket is non-blocking:
   then -OPOS_EINPROGRESS, and poll for OPOS_POLLOUT). */
static inline long opos_connect(long fd, const struct opos_sockaddr *to) {
    return opos_syscall3(42, fd, (long)to, sizeof(*to));
}

static inline long opos_bind(long fd, const struct opos_sockaddr *address) {
    return opos_syscall3(49, fd, (long)address, sizeof(*address));
}

static inline long opos_listen(long fd, int backlog) {
    return opos_syscall2(50, fd, backlog);
}

/* from may be NULL. */
static inline long opos_accept(long fd, struct opos_sockaddr *from) {
    uint32_t length = sizeof(*from);
    return opos_syscall3(43, fd, (long)from, from != NULL ? (long)&length : 0);
}

/* Sends all of data unless the connection fails (TCP). */
static inline long opos_send(long fd, const void *data, size_t length) {
    return opos_syscall6(44, fd, (long)data, (long)length, 0, 0, 0);
}

/* Returns what has arrived (at most capacity), 0 at the end of the
   stream; blocks while nothing is waiting. */
static inline long opos_recv(long fd, void *buffer, size_t capacity) {
    return opos_syscall6(45, fd, (long)buffer, (long)capacity, 0, 0, 0);
}

static inline long opos_sendto(long fd, const void *data, size_t length,
                               const struct opos_sockaddr *to) {
    return opos_syscall6(44, fd, (long)data, (long)length, 0, (long)to,
                         sizeof(*to));
}

/* from may be NULL. */
static inline long opos_recvfrom(long fd, void *buffer, size_t capacity,
                                 struct opos_sockaddr *from) {
    uint32_t length = sizeof(*from);
    return opos_syscall6(45, fd, (long)buffer, (long)capacity, 0, (long)from,
                         from != NULL ? (long)&length : 0);
}

static inline long opos_shutdown(long fd, int how) {
    return opos_syscall2(48, fd, how);
}

/* Close sockets with opos_close (opos.h). */

/* timeout_ms < 0 waits forever. Returns how many entries are ready. */
static inline long opos_poll(struct opos_pollfd *fds, size_t count,
                             int timeout_ms) {
    return opos_syscall3(7, (long)fds, (long)count, timeout_ms);
}

/* Looks up a host name's IPv4 address (a dotted address is parsed
   directly). Returns 0, or a negative error: -OPOS_ENOENT when the name
   does not exist, -OPOS_ETIMEDOUT when no DNS server answered. */
long opos_net_resolve(const char *name, uint8_t ip[4]);

/* "a.b.c.d" to 4 bytes; returns 0 or -OPOS_EINVAL. */
long opos_net_parse_ip(const char *text, uint8_t ip[4]);

/* Writes "a.b.c.d" (at most 16 bytes with the terminating zero). */
void opos_net_format_ip(const uint8_t ip[4], char text[16]);

#endif
