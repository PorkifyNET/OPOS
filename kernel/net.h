#ifndef OPOS_NET_H
#define OPOS_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * IPv4 networking over the e1000 driver: Ethernet, ARP, IPv4 (no
 * fragments), ICMP echo, UDP, a DHCP client and a DNS resolver.
 * Everything runs from net_poll(), which the desktop and the kernel shell
 * call on every timer tick.
 */

struct net_status {
    bool card;       /* a supported network card was found */
    bool link;       /* cable/virtual link up */
    bool configured; /* DHCP gave us an address */
    uint32_t pci_id; /* vendor << 16 | device */
    uint8_t mac[6];
    uint8_t ip[4];
    uint8_t mask[4];
    uint8_t gateway[4];
    uint8_t dns[4];
    uint32_t lease_seconds;
    const char *state; /* DHCP state, for display */
    uint64_t sent;
    uint64_t received;
};

void net_initialize(void);
void net_poll(void);
void net_get_status(struct net_status *status);
/* Prints a few lines about the card and the address. */
void net_describe(void (*print)(const char *line));
/* Drops the address and asks the DHCP server again. */
void net_dhcp_restart(void);

/* "a.b.c.d" (exactly length characters) to bytes. */
bool net_parse_ipv4(const char *text, size_t length, uint8_t ip[4]);
/* Appends "a.b.c.d" to text at position length; returns the new length. */
size_t net_format_ipv4(char *text, size_t capacity, size_t length,
                       const uint8_t ip[4]);
size_t net_format_mac(char *text, size_t capacity, size_t length,
                      const uint8_t mac[6]);

/*
 * One interactive session at a time, shared by the kernel shell and the
 * desktop Terminal: ping (4 echo requests, one a second) or a DNS lookup.
 * The host is a name or a dotted address. Results come out as text lines.
 */
bool net_ping_start(const char *host, size_t length);
bool net_lookup_start(const char *host, size_t length);
/* Fetches http://HOST/PATH (text is "[http://]host[/path]") and prints the
   headers and the start of the body. */
bool net_http_start(const char *text, size_t length);
bool net_session_active(void);
void net_session_stop(void);
/* Takes the next output line (without newline); false when none. */
bool net_take_line(char *line, size_t capacity);

/*
 * TCP client connections (up to 4). Handles are small integers. Send and
 * receive never block: they copy what fits / what has arrived.
 */
enum net_tcp_status {
    NET_TCP_CONNECTING,
    NET_TCP_OPEN,
    NET_TCP_FINISHED, /* the peer closed and every byte has been read */
    NET_TCP_FAILED,   /* refused, reset or timed out */
};
int net_tcp_connect(const uint8_t ip[4], uint16_t port);
enum net_tcp_status net_tcp_status(int handle);
size_t net_tcp_send(int handle, const void *data, size_t length);
size_t net_tcp_receive(int handle, void *buffer, size_t capacity);
/* Closes our side; the handle is invalid afterwards. */
void net_tcp_close(int handle);

#endif
