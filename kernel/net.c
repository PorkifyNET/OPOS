#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nic.h"
#include "net.h"
#include "timer.h"

/* ------------------------------------------------------------- helpers */

#define TICKS_PER_SECOND 100

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP 0x0806
#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_UDP 17
#define ETHERNET_HEADER 14
#define IP_HEADER 20
#define UDP_HEADER 8
#define FRAME_CAPACITY 1514

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DNS_PORT 53
#define DNS_CLIENT_PORT 49153

static const uint8_t broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t zero_ip[4] = {0, 0, 0, 0};
static const uint8_t broadcast_ip[4] = {255, 255, 255, 255};

static uint8_t our_mac[6];
static uint8_t our_ip[4];
static uint8_t our_mask[4];
static uint8_t our_gateway[4];
static uint8_t our_dns[4];
static bool configured;
static uint8_t frame_out[FRAME_CAPACITY];
static uint8_t frame_in[FRAME_CAPACITY];
static uint16_t ip_identification = 1;

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8
         | p[3];
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void put32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void copy(void *to, const void *from, size_t length) {
    uint8_t *t = to;
    const uint8_t *f = from;
    for (size_t i = 0; i < length; i++) {
        t[i] = f[i];
    }
}

static void zero(void *to, size_t length) {
    uint8_t *t = to;
    for (size_t i = 0; i < length; i++) {
        t[i] = 0;
    }
}

static bool same(const void *a, const void *b, size_t length) {
    const uint8_t *x = a;
    const uint8_t *y = b;
    for (size_t i = 0; i < length; i++) {
        if (x[i] != y[i]) {
            return false;
        }
    }
    return true;
}

static uint32_t random32(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    uint32_t value = low ^ (high << 7) ^ (low >> 13);
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
}

/* Internet checksum over data, added to a running sum. */
static uint32_t checksum_add(uint32_t sum, const uint8_t *data,
                             size_t length) {
    for (size_t i = 0; i + 1 < length; i += 2) {
        sum += (uint32_t)(data[i] << 8 | data[i + 1]);
    }
    if ((length & 1) != 0) {
        sum += (uint32_t)(data[length - 1] << 8);
    }
    return sum;
}

static uint16_t checksum_finish(uint32_t sum) {
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint64_t now_microseconds(void) {
    uint64_t seconds;
    uint64_t nanoseconds;
    timer_monotonic_time(&seconds, &nanoseconds);
    return seconds * 1000000 + nanoseconds / 1000;
}

size_t net_format_ipv4(char *text, size_t capacity, size_t length,
                       const uint8_t ip[4]) {
    for (int part = 0; part < 4; part++) {
        char digits[3];
        size_t count = 0;
        unsigned value = ip[part];
        do {
            digits[count++] = (char)('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count != 0 && length + 1 < capacity) {
            text[length++] = digits[--count];
        }
        if (part != 3 && length + 1 < capacity) {
            text[length++] = '.';
        }
    }
    if (length < capacity) {
        text[length] = '\0';
    }
    return length;
}

size_t net_format_mac(char *text, size_t capacity, size_t length,
                      const uint8_t mac[6]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        if (length + 3 >= capacity) {
            break;
        }
        text[length++] = hex[mac[i] >> 4];
        text[length++] = hex[mac[i] & 15];
        if (i != 5) {
            text[length++] = ':';
        }
    }
    if (length < capacity) {
        text[length] = '\0';
    }
    return length;
}

bool net_parse_ipv4(const char *text, size_t length, uint8_t ip[4]) {
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        unsigned value = 0;
        size_t digits = 0;
        while (i < length && text[i] >= '0' && text[i] <= '9') {
            value = value * 10 + (unsigned)(text[i] - '0');
            i++;
            if (++digits > 3) {
                return false;
            }
        }
        if (digits == 0 || value > 255) {
            return false;
        }
        ip[part] = (uint8_t)value;
        if (part != 3) {
            if (i >= length || text[i] != '.') {
                return false;
            }
            i++;
        }
    }
    return i == length;
}

static size_t append(char *text, size_t capacity, size_t length,
                     const char *add) {
    while (*add != '\0' && length + 1 < capacity) {
        text[length++] = *add++;
    }
    if (length < capacity) {
        text[length] = '\0';
    }
    return length;
}

static size_t append_number(char *text, size_t capacity, size_t length,
                            uint64_t value) {
    char digits[20];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0 && length + 1 < capacity) {
        text[length++] = digits[--count];
    }
    if (length < capacity) {
        text[length] = '\0';
    }
    return length;
}

/* --------------------------------------------------------- output lines */

#define LINE_COUNT 16
#define LINE_CAPACITY 96

static char lines[LINE_COUNT][LINE_CAPACITY];
static size_t line_head;
static size_t line_count;

static void output(const char *line) {
    if (line_count == LINE_COUNT) {
        line_head = (line_head + 1) % LINE_COUNT;
        line_count--;
    }
    size_t slot = (line_head + line_count) % LINE_COUNT;
    lines[slot][0] = '\0';
    append(lines[slot], LINE_CAPACITY, 0, line);
    line_count++;
}

bool net_take_line(char *line, size_t capacity) {
    if (line_count == 0) {
        return false;
    }
    line[0] = '\0';
    append(line, capacity, 0, lines[line_head]);
    line_head = (line_head + 1) % LINE_COUNT;
    line_count--;
    return true;
}

/* ----------------------------------------------------------- ethernet */

static bool ethernet_send(const uint8_t destination[6], uint16_t type,
                          size_t payload_length) {
    copy(frame_out, destination, 6);
    copy(frame_out + 6, our_mac, 6);
    put16(frame_out + 12, type);
    return nic_send(frame_out, ETHERNET_HEADER + payload_length);
}

/* ---------------------------------------------------------------- ARP */

#define ARP_ENTRIES 16
#define ARP_RETRY_TICKS TICKS_PER_SECOND
#define ARP_TRIES 3

struct arp_entry {
    bool valid;
    uint8_t ip[4];
    uint8_t mac[6];
    uint64_t used;
};

static struct arp_entry arp_table[ARP_ENTRIES];

/* One IPv4 packet may wait for its next hop's address. */
static uint8_t pending_packet[FRAME_CAPACITY - ETHERNET_HEADER];
static size_t pending_length;
static uint8_t pending_hop[4];
static uint64_t pending_asked;
static int pending_tries;

static struct arp_entry *arp_find(const uint8_t ip[4]) {
    for (size_t i = 0; i < ARP_ENTRIES; i++) {
        if (arp_table[i].valid && same(arp_table[i].ip, ip, 4)) {
            arp_table[i].used = timer_ticks();
            return &arp_table[i];
        }
    }
    return NULL;
}

static void arp_learn(const uint8_t ip[4], const uint8_t mac[6]) {
    if (same(ip, zero_ip, 4)) {
        return;
    }
    struct arp_entry *entry = arp_find(ip);
    if (entry == NULL) {
        entry = &arp_table[0];
        for (size_t i = 0; i < ARP_ENTRIES; i++) {
            if (!arp_table[i].valid) {
                entry = &arp_table[i];
                break;
            }
            if (arp_table[i].used < entry->used) {
                entry = &arp_table[i];
            }
        }
    }
    entry->valid = true;
    copy(entry->ip, ip, 4);
    copy(entry->mac, mac, 6);
    entry->used = timer_ticks();
}

static void arp_send(uint16_t operation, const uint8_t target_mac[6],
                     const uint8_t target_ip[4]) {
    uint8_t *arp = frame_out + ETHERNET_HEADER;
    put16(arp, 1);       /* Ethernet */
    put16(arp + 2, ETHERTYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    put16(arp + 6, operation);
    copy(arp + 8, our_mac, 6);
    copy(arp + 14, our_ip, 4);
    copy(arp + 18, operation == 1 ? (const uint8_t *)"\0\0\0\0\0\0"
                                  : target_mac, 6);
    copy(arp + 24, target_ip, 4);
    (void)ethernet_send(operation == 1 ? broadcast_mac : target_mac,
                        ETHERTYPE_ARP, 28);
}

static void pending_flush(void) {
    if (pending_length == 0) {
        return;
    }
    struct arp_entry *entry = arp_find(pending_hop);
    if (entry == NULL) {
        return;
    }
    copy(frame_out + ETHERNET_HEADER, pending_packet, pending_length);
    (void)ethernet_send(entry->mac, ETHERTYPE_IPV4, pending_length);
    pending_length = 0;
}

static void arp_receive(const uint8_t *arp, size_t length) {
    if (length < 28 || get16(arp) != 1 || get16(arp + 2) != ETHERTYPE_IPV4
        || arp[4] != 6 || arp[5] != 4) {
        return;
    }
    uint16_t operation = get16(arp + 6);
    const uint8_t *sender_mac = arp + 8;
    const uint8_t *sender_ip = arp + 14;
    const uint8_t *target_ip = arp + 24;
    bool for_us = configured && same(target_ip, our_ip, 4);
    if (for_us || arp_find(sender_ip) != NULL) {
        arp_learn(sender_ip, sender_mac);
    }
    if (operation == 1 && for_us) {
        uint8_t mac[6];
        uint8_t ip[4];
        copy(mac, sender_mac, 6);
        copy(ip, sender_ip, 4);
        arp_send(2, mac, ip);
    }
    if (operation == 2 && for_us) {
        arp_learn(sender_ip, sender_mac);
    }
    pending_flush();
}

static void arp_poll(void) {
    if (pending_length == 0
        || timer_ticks() - pending_asked < ARP_RETRY_TICKS) {
        return;
    }
    if (++pending_tries > ARP_TRIES) {
        pending_length = 0;
        return;
    }
    pending_asked = timer_ticks();
    arp_send(1, broadcast_mac, pending_hop);
}

/* --------------------------------------------------------------- IPv4 */

static bool on_our_subnet(const uint8_t ip[4]) {
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & our_mask[i]) != (our_ip[i] & our_mask[i])) {
            return false;
        }
    }
    return true;
}

/* The IPv4 payload goes at frame_out + 14 + 20 before the call. */
static bool ip_send(const uint8_t destination[4], uint8_t protocol,
                    size_t payload_length) {
    uint8_t *ip = frame_out + ETHERNET_HEADER;
    size_t total = IP_HEADER + payload_length;
    if (total > FRAME_CAPACITY - ETHERNET_HEADER) {
        return false;
    }
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)total);
    put16(ip + 4, ip_identification++);
    put16(ip + 6, 0x4000); /* don't fragment */
    ip[8] = 64;
    ip[9] = protocol;
    put16(ip + 10, 0);
    copy(ip + 12, configured ? our_ip : zero_ip, 4);
    copy(ip + 16, destination, 4);
    put16(ip + 10, checksum_finish(checksum_add(0, ip, IP_HEADER)));

    if (same(destination, broadcast_ip, 4)) {
        return ethernet_send(broadcast_mac, ETHERTYPE_IPV4, total);
    }
    if (!configured) {
        return false;
    }
    const uint8_t *hop = on_our_subnet(destination) ? destination
                                                    : our_gateway;
    struct arp_entry *entry = arp_find(hop);
    if (entry != NULL) {
        return ethernet_send(entry->mac, ETHERTYPE_IPV4, total);
    }
    /* Park the packet and ask who has the next hop. */
    copy(pending_packet, ip, total);
    pending_length = total;
    copy(pending_hop, hop, 4);
    pending_tries = 1;
    pending_asked = timer_ticks();
    uint8_t target[4];
    copy(target, hop, 4);
    arp_send(1, broadcast_mac, target);
    return true;
}

static void icmp_receive(const uint8_t *ip, const uint8_t *icmp,
                         size_t length);
static void udp_receive(const uint8_t *ip, const uint8_t *udp,
                        size_t length);
static void tcp_receive(const uint8_t *ip, const uint8_t *tcp,
                        size_t length);

static void ip_receive(const uint8_t *ip, size_t length) {
    if (length < IP_HEADER || (ip[0] >> 4) != 4) {
        return;
    }
    size_t header = (size_t)(ip[0] & 15) * 4;
    size_t total = get16(ip + 2);
    if (header < IP_HEADER || total < header || total > length
        || checksum_finish(checksum_add(0, ip, header)) != 0) {
        return;
    }
    /* No reassembly: fragments are dropped. */
    if ((get16(ip + 6) & 0x3fff) != 0) {
        return;
    }
    const uint8_t *destination = ip + 16;
    bool ours = (configured && same(destination, our_ip, 4))
             || same(destination, broadcast_ip, 4);
    if (!ours && configured) {
        /* Subnet broadcast. */
        ours = true;
        for (int i = 0; i < 4; i++) {
            if ((destination[i] | our_mask[i]) != 0xff
                || (destination[i] & our_mask[i])
                       != (our_ip[i] & our_mask[i])) {
                ours = false;
            }
        }
    }
    if (!ours && configured) {
        return;
    }
    if (ip[9] == IP_PROTOCOL_ICMP) {
        icmp_receive(ip, ip + header, total - header);
    } else if (ip[9] == IP_PROTOCOL_UDP) {
        udp_receive(ip, ip + header, total - header);
    } else if (ip[9] == 6) {
        tcp_receive(ip, ip + header, total - header);
    }
}

/* ---------------------------------------------------------------- UDP */

static bool udp_send(const uint8_t destination[4], uint16_t source_port,
                     uint16_t destination_port, size_t payload_length) {
    uint8_t *udp = frame_out + ETHERNET_HEADER + IP_HEADER;
    size_t length = UDP_HEADER + payload_length;
    put16(udp, source_port);
    put16(udp + 2, destination_port);
    put16(udp + 4, (uint16_t)length);
    put16(udp + 6, 0);
    uint8_t pseudo[12];
    copy(pseudo, configured ? our_ip : zero_ip, 4);
    copy(pseudo + 4, destination, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTOCOL_UDP;
    put16(pseudo + 10, (uint16_t)length);
    uint16_t sum = checksum_finish(
        checksum_add(checksum_add(0, pseudo, 12), udp, length));
    put16(udp + 6, sum == 0 ? 0xffff : sum);
    return ip_send(destination, IP_PROTOCOL_UDP, length);
}

static void dhcp_receive(const uint8_t *data, size_t length);
static void dns_receive(const uint8_t *data, size_t length);

static void udp_receive(const uint8_t *ip, const uint8_t *udp,
                        size_t length) {
    (void)ip;
    if (length < UDP_HEADER) {
        return;
    }
    size_t udp_length = get16(udp + 4);
    if (udp_length < UDP_HEADER || udp_length > length) {
        return;
    }
    uint16_t port = get16(udp + 2);
    if (port == DHCP_CLIENT_PORT) {
        dhcp_receive(udp + UDP_HEADER, udp_length - UDP_HEADER);
    } else if (port == DNS_CLIENT_PORT && get16(udp) == DNS_PORT) {
        dns_receive(udp + UDP_HEADER, udp_length - UDP_HEADER);
    }
}

/* --------------------------------------------------------------- DHCP */

enum dhcp_state {
    DHCP_OFF,
    DHCP_SELECTING,
    DHCP_REQUESTING,
    DHCP_BOUND,
};

static enum dhcp_state dhcp_state;
static uint32_t dhcp_xid;
static uint64_t dhcp_sent_at;
static uint64_t dhcp_wait;
static uint8_t dhcp_offered[4];
static uint8_t dhcp_server[4];
static uint32_t dhcp_lease;
static uint64_t dhcp_bound_at;

#define DHCP_MAGIC 0x63825363u

static void dhcp_send(uint8_t type) {
    uint8_t *dhcp = frame_out + ETHERNET_HEADER + IP_HEADER + UDP_HEADER;
    zero(dhcp, 300);
    dhcp[0] = 1; /* BOOTREQUEST */
    dhcp[1] = 1; /* Ethernet */
    dhcp[2] = 6;
    put32(dhcp + 4, dhcp_xid);
    put16(dhcp + 10, 0x8000); /* reply by broadcast: we have no address */
    copy(dhcp + 28, our_mac, 6);
    put32(dhcp + 236, DHCP_MAGIC);
    uint8_t *option = dhcp + 240;
    *option++ = 53;
    *option++ = 1;
    *option++ = type;
    *option++ = 61; /* client identifier */
    *option++ = 7;
    *option++ = 1;
    copy(option, our_mac, 6);
    option += 6;
    if (type == 3) {
        *option++ = 50; /* requested address */
        *option++ = 4;
        copy(option, dhcp_offered, 4);
        option += 4;
        *option++ = 54; /* server */
        *option++ = 4;
        copy(option, dhcp_server, 4);
        option += 4;
    }
    *option++ = 12; /* host name */
    *option++ = 4;
    copy(option, "opos", 4);
    option += 4;
    *option++ = 55; /* parameters: mask, router, DNS, lease */
    *option++ = 4;
    *option++ = 1;
    *option++ = 3;
    *option++ = 6;
    *option++ = 51;
    *option++ = 255;
    (void)udp_send(broadcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, 300);
    dhcp_sent_at = timer_ticks();
}

static void dhcp_begin(void) {
    dhcp_xid = random32();
    dhcp_state = DHCP_SELECTING;
    dhcp_wait = 2 * TICKS_PER_SECOND;
    dhcp_send(1); /* DISCOVER */
}

void net_dhcp_restart(void) {
    if (!nic_ready()) {
        return;
    }
    configured = false;
    dhcp_begin();
}

static void dhcp_receive(const uint8_t *dhcp, size_t length) {
    if (length < 240 || dhcp[0] != 2 || get32(dhcp + 4) != dhcp_xid
        || get32(dhcp + 236) != DHCP_MAGIC || !same(dhcp + 28, our_mac, 6)) {
        return;
    }
    uint8_t type = 0;
    uint8_t mask[4] = {255, 255, 255, 0};
    uint8_t router[4] = {0, 0, 0, 0};
    uint8_t dns[4] = {0, 0, 0, 0};
    uint8_t server[4] = {0, 0, 0, 0};
    uint32_t lease = 3600;
    size_t i = 240;
    while (i < length && dhcp[i] != 255) {
        uint8_t code = dhcp[i];
        if (code == 0) {
            i++;
            continue;
        }
        if (i + 1 >= length || i + 2 + dhcp[i + 1] > length) {
            break;
        }
        uint8_t size = dhcp[i + 1];
        const uint8_t *value = dhcp + i + 2;
        if (code == 53 && size >= 1) {
            type = value[0];
        } else if (code == 1 && size >= 4) {
            copy(mask, value, 4);
        } else if (code == 3 && size >= 4) {
            copy(router, value, 4);
        } else if (code == 6 && size >= 4) {
            copy(dns, value, 4);
        } else if (code == 54 && size >= 4) {
            copy(server, value, 4);
        } else if (code == 51 && size >= 4) {
            lease = get32(value);
        }
        i += 2 + (size_t)size;
    }
    if (type == 2 && dhcp_state == DHCP_SELECTING) { /* OFFER */
        copy(dhcp_offered, dhcp + 16, 4);
        copy(dhcp_server, server, 4);
        dhcp_state = DHCP_REQUESTING;
        dhcp_wait = 2 * TICKS_PER_SECOND;
        dhcp_send(3);
    } else if (type == 5 && dhcp_state == DHCP_REQUESTING) { /* ACK */
        copy(our_ip, dhcp + 16, 4);
        copy(our_mask, mask, 4);
        copy(our_gateway, router, 4);
        copy(our_dns, dns, 4);
        dhcp_lease = lease < 60 ? 60 : lease;
        dhcp_bound_at = timer_ticks();
        dhcp_state = DHCP_BOUND;
        configured = true;
        /* Announce ourselves (gratuitous ARP) and learn the router. */
        arp_send(1, broadcast_mac, our_ip);
        if (!same(our_gateway, zero_ip, 4)) {
            arp_send(1, broadcast_mac, our_gateway);
        }
    } else if (type == 6 && dhcp_state == DHCP_REQUESTING) { /* NAK */
        dhcp_begin();
    }
}

static void dhcp_poll(void) {
    uint64_t now = timer_ticks();
    if (dhcp_state == DHCP_SELECTING || dhcp_state == DHCP_REQUESTING) {
        if (now - dhcp_sent_at >= dhcp_wait) {
            /* Back off up to 16 s; start over from DISCOVER. */
            if (dhcp_wait < 16 * TICKS_PER_SECOND) {
                dhcp_wait *= 2;
            }
            dhcp_state = DHCP_SELECTING;
            dhcp_send(1);
        }
    } else if (dhcp_state == DHCP_BOUND) {
        /* Renew halfway through the lease, keeping the address meanwhile. */
        if (now - dhcp_bound_at
            >= (uint64_t)dhcp_lease * TICKS_PER_SECOND / 2) {
            dhcp_xid = random32();
            dhcp_state = DHCP_REQUESTING;
            dhcp_wait = 2 * TICKS_PER_SECOND;
            copy(dhcp_offered, our_ip, 4);
            dhcp_send(3);
        }
    }
}

/* ---------------------------------------------------------------- DNS */

#define DNS_NAME_CAPACITY 128
#define DNS_CACHE 8
#define DNS_TRIES 3

struct dns_cache_entry {
    bool valid;
    char name[DNS_NAME_CAPACITY];
    uint8_t ip[4];
    uint64_t expires;
};

static struct dns_cache_entry dns_cache[DNS_CACHE];
static size_t dns_cache_next;

static bool dns_active;
static bool dns_done;
static bool dns_failed;
static char dns_name[DNS_NAME_CAPACITY];
static uint8_t dns_result[4];
static uint16_t dns_id;
static uint64_t dns_sent_at;
static int dns_tries;

static bool name_equal(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) {
            return false;
        }
        if (x == '\0') {
            return true;
        }
    }
}

static bool dns_cached(const char *name, uint8_t ip[4]) {
    for (size_t i = 0; i < DNS_CACHE; i++) {
        if (dns_cache[i].valid && dns_cache[i].expires > timer_ticks()
            && name_equal(dns_cache[i].name, name)) {
            copy(ip, dns_cache[i].ip, 4);
            return true;
        }
    }
    return false;
}

static void dns_send(void) {
    uint8_t *dns = frame_out + ETHERNET_HEADER + IP_HEADER + UDP_HEADER;
    put16(dns, dns_id);
    put16(dns + 2, 0x0100); /* recursion desired */
    put16(dns + 4, 1);
    put16(dns + 6, 0);
    put16(dns + 8, 0);
    put16(dns + 10, 0);
    size_t at = 12;
    const char *label = dns_name;
    while (*label != '\0') {
        size_t size = 0;
        while (label[size] != '\0' && label[size] != '.') {
            size++;
        }
        if (size == 0 || size > 63) {
            dns_failed = true;
            dns_done = true;
            return;
        }
        dns[at++] = (uint8_t)size;
        copy(dns + at, label, size);
        at += size;
        label += size;
        if (*label == '.') {
            label++;
        }
    }
    dns[at++] = 0;
    put16(dns + at, 1); /* A */
    put16(dns + at + 2, 1); /* IN */
    at += 4;
    (void)udp_send(our_dns, DNS_CLIENT_PORT, DNS_PORT, at);
    dns_sent_at = timer_ticks();
}

/* Skips a possibly compressed name; returns the offset after it or 0. */
static size_t dns_skip_name(const uint8_t *data, size_t length, size_t at) {
    while (at < length) {
        uint8_t size = data[at];
        if (size == 0) {
            return at + 1;
        }
        if ((size & 0xc0) == 0xc0) {
            return at + 2 <= length ? at + 2 : 0;
        }
        at += 1 + (size_t)size;
    }
    return 0;
}

static void dns_receive(const uint8_t *dns, size_t length) {
    if (!dns_active || dns_done || length < 12 || get16(dns) != dns_id
        || (dns[2] & 0x80) == 0) {
        return;
    }
    uint16_t questions = get16(dns + 4);
    uint16_t answers = get16(dns + 6);
    if ((dns[3] & 0x0f) != 0) {
        dns_failed = true;
        dns_done = true;
        return;
    }
    size_t at = 12;
    for (uint16_t q = 0; q < questions; q++) {
        at = dns_skip_name(dns, length, at);
        if (at == 0 || at + 4 > length) {
            return;
        }
        at += 4;
    }
    for (uint16_t a = 0; a < answers; a++) {
        at = dns_skip_name(dns, length, at);
        if (at == 0 || at + 10 > length) {
            break;
        }
        uint16_t type = get16(dns + at);
        uint16_t class = get16(dns + at + 2);
        uint32_t ttl = get32(dns + at + 4);
        uint16_t size = get16(dns + at + 8);
        at += 10;
        if (at + size > length) {
            break;
        }
        if (type == 1 && class == 1 && size == 4) {
            copy(dns_result, dns + at, 4);
            struct dns_cache_entry *entry = &dns_cache[dns_cache_next];
            dns_cache_next = (dns_cache_next + 1) % DNS_CACHE;
            entry->valid = true;
            entry->name[0] = '\0';
            append(entry->name, DNS_NAME_CAPACITY, 0, dns_name);
            copy(entry->ip, dns_result, 4);
            if (ttl > 86400) {
                ttl = 86400;
            }
            entry->expires = timer_ticks() + (uint64_t)ttl * TICKS_PER_SECOND;
            dns_done = true;
            return;
        }
        at += size;
    }
    dns_failed = true;
    dns_done = true;
}

/* Starts resolving name; false when it cannot even begin. */
static bool dns_start(const char *name, size_t length) {
    if (length == 0 || length >= DNS_NAME_CAPACITY) {
        return false;
    }
    copy(dns_name, name, length);
    dns_name[length] = '\0';
    dns_done = false;
    dns_failed = false;
    if (dns_cached(dns_name, dns_result)) {
        dns_active = true;
        dns_done = true;
        return true;
    }
    if (!configured || same(our_dns, zero_ip, 4)) {
        return false;
    }
    dns_active = true;
    dns_id = (uint16_t)random32();
    dns_tries = 1;
    dns_send();
    return true;
}

static void dns_poll(void) {
    if (!dns_active || dns_done
        || timer_ticks() - dns_sent_at < 2 * TICKS_PER_SECOND) {
        return;
    }
    if (++dns_tries > DNS_TRIES) {
        dns_failed = true;
        dns_done = true;
        return;
    }
    dns_send();
}

/* ---------------------------------------------------------------- TCP */

/*
 * TCP client connections. Sending is go-back-N: unacknowledged bytes stay
 * in the send buffer and a timeout resends from the oldest one. Received
 * segments are taken only in order; anything else gets a duplicate ACK.
 * No listening sockets yet.
 */
#define IP_PROTOCOL_TCP 6
#define TCP_CONNECTIONS 4
#define TCP_SEND_CAPACITY 16384
#define TCP_RECEIVE_CAPACITY 32768
#define TCP_MSS 1460
#define TCP_HEADER 20
#define TCP_RTO_START TICKS_PER_SECOND
#define TCP_RTO_MAX (16 * TICKS_PER_SECOND)
#define TCP_RETRIES 7
#define TCP_TIME_WAIT_TICKS (2 * TICKS_PER_SECOND)

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_connection {
    bool used;
    bool detached; /* the owner closed it; free the slot when done */
    bool failed;   /* reset by the peer or timed out */
    enum tcp_state state;
    uint8_t remote_ip[4];
    uint16_t remote_port;
    uint16_t local_port;
    uint32_t iss;
    uint32_t snd_una;
    uint32_t snd_nxt;
    uint32_t snd_wnd;
    uint32_t mss;
    uint32_t rcv_nxt;
    bool fin_queued;
    bool fin_sent;
    bool remote_closed;
    uint32_t advertised; /* window in the last segment we sent */
    uint64_t retransmit_at; /* 0: no timer */
    uint64_t rto;
    int retries;
    uint64_t time_wait_until;
    size_t send_length; /* bytes from snd_una on */
    size_t receive_start;
    size_t receive_length;
    uint8_t send_buffer[TCP_SEND_CAPACITY];
    uint8_t receive_buffer[TCP_RECEIVE_CAPACITY];
};

static struct tcp_connection tcp_table[TCP_CONNECTIONS];
static uint16_t tcp_next_port;

/* Sequence comparisons that survive wrap-around. */
static bool seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static bool seq_le(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) <= 0;
}

static uint32_t tcp_window(const struct tcp_connection *c) {
    size_t free = TCP_RECEIVE_CAPACITY - c->receive_length;
    return free > 65535 ? 65535 : (uint32_t)free;
}

/* Sends one segment; data may be NULL when length is 0. */
static void tcp_segment(struct tcp_connection *c, uint32_t seq, uint8_t flags,
                        const uint8_t *data, size_t length) {
    uint8_t *tcp = frame_out + ETHERNET_HEADER + IP_HEADER;
    size_t header = TCP_HEADER + ((flags & TCP_SYN) != 0 ? 4 : 0);
    put16(tcp, c->local_port);
    put16(tcp + 2, c->remote_port);
    put32(tcp + 4, seq);
    put32(tcp + 8, (flags & TCP_ACK) != 0 ? c->rcv_nxt : 0);
    tcp[12] = (uint8_t)(header / 4 << 4);
    tcp[13] = flags;
    c->advertised = tcp_window(c);
    put16(tcp + 14, (uint16_t)c->advertised);
    put16(tcp + 16, 0);
    put16(tcp + 18, 0);
    if ((flags & TCP_SYN) != 0) {
        tcp[20] = 2; /* maximum segment size */
        tcp[21] = 4;
        put16(tcp + 22, TCP_MSS);
    }
    if (length != 0) {
        copy(tcp + header, data, length);
    }
    size_t total = header + length;
    uint8_t pseudo[12];
    copy(pseudo, our_ip, 4);
    copy(pseudo + 4, c->remote_ip, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTOCOL_TCP;
    put16(pseudo + 10, (uint16_t)total);
    put16(tcp + 16, checksum_finish(
        checksum_add(checksum_add(0, pseudo, 12), tcp, total)));
    (void)ip_send(c->remote_ip, IP_PROTOCOL_TCP, total);
}

static void tcp_arm(struct tcp_connection *c) {
    if (c->retransmit_at == 0) {
        c->retransmit_at = timer_ticks() + c->rto;
    }
}

static void tcp_free(struct tcp_connection *c) {
    c->used = false;
    c->state = TCP_CLOSED;
}

/* Sends whatever the window allows, then the FIN once all data is out.
   force sends at least one byte into a closed window (a probe). */
static void tcp_push(struct tcp_connection *c, bool force) {
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT
        && c->state != TCP_FIN_WAIT_1 && c->state != TCP_LAST_ACK) {
        return;
    }
    while (!c->fin_sent) {
        size_t in_flight = c->snd_nxt - c->snd_una;
        if (in_flight >= c->send_length) {
            break;
        }
        uint32_t window = c->snd_wnd;
        if (force && window <= in_flight) {
            window = (uint32_t)in_flight + 1;
        }
        if (in_flight >= window) {
            if (c->snd_wnd == 0) {
                tcp_arm(c); /* probe the closed window later */
            }
            break;
        }
        size_t length = c->send_length - in_flight;
        if (length > window - in_flight) {
            length = window - in_flight;
        }
        if (length > c->mss) {
            length = c->mss;
        }
        tcp_segment(c, c->snd_nxt, TCP_ACK | TCP_PSH,
                    c->send_buffer + in_flight, length);
        c->snd_nxt += (uint32_t)length;
        tcp_arm(c);
        force = false;
    }
    if (c->fin_queued && !c->fin_sent
        && c->snd_nxt - c->snd_una == c->send_length) {
        tcp_segment(c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
        c->snd_nxt++;
        c->fin_sent = true;
        if (c->state == TCP_ESTABLISHED) {
            c->state = TCP_FIN_WAIT_1;
        } else if (c->state == TCP_CLOSE_WAIT) {
            c->state = TCP_LAST_ACK;
        }
        tcp_arm(c);
    }
}

static void tcp_send_reset(const uint8_t *ip, const uint8_t *tcp,
                           size_t payload, uint8_t flags) {
    /* Static: the buffers make the structure far too big for the stack. */
    static struct tcp_connection temporary;
    zero(&temporary, sizeof(temporary) - TCP_SEND_CAPACITY
                         - TCP_RECEIVE_CAPACITY);
    copy(temporary.remote_ip, ip + 12, 4);
    temporary.remote_port = get16(tcp);
    temporary.local_port = get16(tcp + 2);
    temporary.receive_length = TCP_RECEIVE_CAPACITY;
    if ((flags & TCP_ACK) != 0) {
        tcp_segment(&temporary, get32(tcp + 8), TCP_RST, NULL, 0);
    } else {
        uint32_t length = (uint32_t)payload + ((flags & TCP_SYN) ? 1 : 0)
                        + ((flags & TCP_FIN) ? 1 : 0);
        temporary.rcv_nxt = get32(tcp + 4) + length;
        tcp_segment(&temporary, 0, TCP_RST | TCP_ACK, NULL, 0);
    }
}

static void tcp_receive(const uint8_t *ip, const uint8_t *tcp,
                        size_t length) {
    if (length < TCP_HEADER || !configured) {
        return;
    }
    size_t header = (size_t)(tcp[12] >> 4) * 4;
    if (header < TCP_HEADER || header > length) {
        return;
    }
    uint8_t pseudo[12];
    copy(pseudo, ip + 12, 4);
    copy(pseudo + 4, ip + 16, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTOCOL_TCP;
    put16(pseudo + 10, (uint16_t)length);
    if (checksum_finish(checksum_add(checksum_add(0, pseudo, 12), tcp,
                                     length)) != 0) {
        return;
    }
    uint16_t remote_port = get16(tcp);
    uint16_t local_port = get16(tcp + 2);
    uint32_t seq = get32(tcp + 4);
    uint32_t ack = get32(tcp + 8);
    uint8_t flags = tcp[13];
    uint32_t window = get16(tcp + 14);
    const uint8_t *data = tcp + header;
    size_t data_length = length - header;

    struct tcp_connection *c = NULL;
    for (size_t i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *candidate = &tcp_table[i];
        if (candidate->used && candidate->state != TCP_CLOSED
            && candidate->local_port == local_port
            && candidate->remote_port == remote_port
            && same(candidate->remote_ip, ip + 12, 4)) {
            c = candidate;
            break;
        }
    }
    if (c == NULL) {
        if ((flags & TCP_RST) == 0) {
            tcp_send_reset(ip, tcp, data_length, flags);
        }
        return;
    }

    if (c->state == TCP_SYN_SENT) {
        if ((flags & TCP_ACK) != 0 && ack != c->iss + 1) {
            if ((flags & TCP_RST) == 0) {
                tcp_send_reset(ip, tcp, data_length, flags);
            }
            return;
        }
        if ((flags & TCP_RST) != 0) {
            if ((flags & TCP_ACK) != 0) {
                c->failed = true; /* connection refused */
                c->state = TCP_CLOSED;
            }
            return;
        }
        if ((flags & (TCP_SYN | TCP_ACK)) != (TCP_SYN | TCP_ACK)) {
            return;
        }
        c->rcv_nxt = seq + 1;
        c->snd_una = ack;
        c->snd_wnd = window;
        c->mss = 536;
        for (size_t i = TCP_HEADER; i + 1 < header;) {
            uint8_t kind = tcp[i];
            if (kind == 0) {
                break;
            }
            if (kind == 1) {
                i++;
                continue;
            }
            uint8_t size = tcp[i + 1];
            if (size < 2 || i + size > header) {
                break;
            }
            if (kind == 2 && size == 4) {
                c->mss = get16(tcp + i + 2);
            }
            i += size;
        }
        if (c->mss > TCP_MSS) {
            c->mss = TCP_MSS;
        }
        if (c->mss < 64) {
            c->mss = 64;
        }
        c->state = TCP_ESTABLISHED;
        c->retransmit_at = 0;
        c->retries = 0;
        c->rto = TCP_RTO_START;
        tcp_segment(c, c->snd_nxt, TCP_ACK, NULL, 0);
        tcp_push(c, false);
        return;
    }

    if ((flags & TCP_RST) != 0) {
        /* Accept a reset only inside the window we offered. */
        if (seq_le(c->rcv_nxt, seq)
            && seq_lt(seq, c->rcv_nxt + (c->advertised ? c->advertised : 1))) {
            c->failed = c->state != TCP_TIME_WAIT && !c->remote_closed;
            c->state = TCP_CLOSED;
            if (c->detached) {
                tcp_free(c);
            }
        }
        return;
    }
    if ((flags & TCP_SYN) != 0) {
        /* A retransmitted SYN+ACK: our ACK was lost. */
        tcp_segment(c, c->snd_nxt, TCP_ACK, NULL, 0);
        return;
    }
    if ((flags & TCP_ACK) == 0) {
        return;
    }

    /* Acknowledgement. */
    if (seq_lt(c->snd_una, ack) && seq_le(ack, c->snd_nxt)) {
        uint32_t acked = ack - c->snd_una;
        bool fin_acked = c->fin_sent && ack == c->snd_nxt;
        size_t data_acked = acked - (fin_acked ? 1 : 0);
        if (data_acked > c->send_length) {
            data_acked = c->send_length;
        }
        for (size_t i = data_acked; i < c->send_length; i++) {
            c->send_buffer[i - data_acked] = c->send_buffer[i];
        }
        c->send_length -= data_acked;
        c->snd_una = ack;
        c->retries = 0;
        c->rto = TCP_RTO_START;
        c->retransmit_at = 0;
        if (c->snd_una != c->snd_nxt) {
            tcp_arm(c);
        }
        if (fin_acked) {
            if (c->state == TCP_FIN_WAIT_1) {
                c->state = TCP_FIN_WAIT_2;
                /* Give up on a peer that never closes its side. */
                c->time_wait_until = timer_ticks() + 30 * TICKS_PER_SECOND;
            } else if (c->state == TCP_LAST_ACK) {
                c->state = TCP_CLOSED;
                if (c->detached) {
                    tcp_free(c);
                }
                return;
            }
        }
    }
    if (seq_le(c->snd_una, ack)) {
        c->snd_wnd = window;
    }

    /* Data, in order only. */
    bool send_ack = false;
    bool in_order = seq == c->rcv_nxt;
    if (data_length != 0) {
        send_ack = true;
        if (in_order && !c->remote_closed && c->detached) {
            /* Nobody reads any more: accept and drop. */
            c->rcv_nxt += (uint32_t)data_length;
        } else if (in_order && !c->remote_closed) {
            size_t room = TCP_RECEIVE_CAPACITY - c->receive_length;
            size_t take = data_length < room ? data_length : room;
            for (size_t i = 0; i < take; i++) {
                size_t at = (c->receive_start + c->receive_length + i)
                          % TCP_RECEIVE_CAPACITY;
                c->receive_buffer[at] = data[i];
            }
            c->receive_length += take;
            c->rcv_nxt += (uint32_t)take;
            in_order = take == data_length;
        }
    }
    if ((flags & TCP_FIN) != 0 && in_order
        && seq + (uint32_t)data_length == c->rcv_nxt && !c->remote_closed) {
        c->rcv_nxt++;
        c->remote_closed = true;
        send_ack = true;
        if (c->state == TCP_ESTABLISHED) {
            c->state = TCP_CLOSE_WAIT;
        } else if (c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2) {
            c->state = TCP_TIME_WAIT;
            c->time_wait_until = timer_ticks() + TCP_TIME_WAIT_TICKS;
            c->retransmit_at = 0;
        }
    }
    if (send_ack) {
        tcp_segment(c, c->snd_nxt, TCP_ACK, NULL, 0);
    }
    tcp_push(c, false);
}

static void tcp_poll(void) {
    uint64_t now = timer_ticks();
    for (size_t i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_table[i];
        if (!c->used) {
            continue;
        }
        if ((c->state == TCP_TIME_WAIT
             || (c->state == TCP_FIN_WAIT_2 && c->detached))
            && now >= c->time_wait_until) {
            c->state = TCP_CLOSED;
            if (c->detached) {
                tcp_free(c);
            }
            continue;
        }
        if (c->retransmit_at == 0 || now < c->retransmit_at) {
            continue;
        }
        c->retransmit_at = 0;
        if (++c->retries > TCP_RETRIES) {
            c->failed = true;
            c->state = TCP_CLOSED;
            if (c->detached) {
                tcp_free(c);
            }
            continue;
        }
        c->rto = c->rto * 2 > TCP_RTO_MAX ? TCP_RTO_MAX : c->rto * 2;
        if (c->state == TCP_SYN_SENT) {
            tcp_segment(c, c->iss, TCP_SYN, NULL, 0);
            tcp_arm(c);
            continue;
        }
        /* Go back to the oldest unacknowledged byte. */
        c->snd_nxt = c->snd_una;
        c->fin_sent = false;
        tcp_push(c, true);
        tcp_arm(c);
    }
}

int net_tcp_connect(const uint8_t ip[4], uint16_t port) {
    if (!configured) {
        return -1;
    }
    for (int i = 0; i < TCP_CONNECTIONS; i++) {
        struct tcp_connection *c = &tcp_table[i];
        if (c->used) {
            continue;
        }
        zero(c, sizeof(*c) - TCP_SEND_CAPACITY - TCP_RECEIVE_CAPACITY);
        c->used = true;
        copy(c->remote_ip, ip, 4);
        c->remote_port = port;
        if (tcp_next_port < 49152) {
            tcp_next_port = (uint16_t)(49152 + random32() % 8192);
        }
        c->local_port = tcp_next_port++;
        c->iss = random32();
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->mss = 536;
        c->rto = TCP_RTO_START;
        c->state = TCP_SYN_SENT;
        tcp_segment(c, c->iss, TCP_SYN, NULL, 0);
        tcp_arm(c);
        return i;
    }
    return -1;
}

static struct tcp_connection *tcp_handle(int handle) {
    if (handle < 0 || handle >= TCP_CONNECTIONS || !tcp_table[handle].used
        || tcp_table[handle].detached) {
        return NULL;
    }
    return &tcp_table[handle];
}

enum net_tcp_status net_tcp_status(int handle) {
    struct tcp_connection *c = tcp_handle(handle);
    if (c == NULL || c->failed) {
        return NET_TCP_FAILED;
    }
    if (c->state == TCP_SYN_SENT) {
        return NET_TCP_CONNECTING;
    }
    if (c->receive_length == 0 && (c->remote_closed || c->state == TCP_CLOSED)) {
        return NET_TCP_FINISHED;
    }
    return NET_TCP_OPEN;
}

size_t net_tcp_send(int handle, const void *data, size_t length) {
    struct tcp_connection *c = tcp_handle(handle);
    if (c == NULL || c->fin_queued || c->failed
        || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT
            && c->state != TCP_SYN_SENT)) {
        return 0;
    }
    size_t room = TCP_SEND_CAPACITY - c->send_length;
    if (length > room) {
        length = room;
    }
    copy(c->send_buffer + c->send_length, data, length);
    c->send_length += length;
    tcp_push(c, false);
    return length;
}

size_t net_tcp_receive(int handle, void *buffer, size_t capacity) {
    struct tcp_connection *c = tcp_handle(handle);
    if (c == NULL) {
        return 0;
    }
    size_t length = c->receive_length < capacity ? c->receive_length
                                                 : capacity;
    uint8_t *out = buffer;
    for (size_t i = 0; i < length; i++) {
        out[i] = c->receive_buffer[(c->receive_start + i)
                                   % TCP_RECEIVE_CAPACITY];
    }
    c->receive_start = (c->receive_start + length) % TCP_RECEIVE_CAPACITY;
    c->receive_length -= length;
    /* Tell the peer when a nearly closed window has opened again. */
    if (length != 0 && c->advertised < c->mss
        && tcp_window(c) >= c->mss && !c->remote_closed
        && c->state != TCP_CLOSED) {
        tcp_segment(c, c->snd_nxt, TCP_ACK, NULL, 0);
    }
    return length;
}

void net_tcp_close(int handle) {
    struct tcp_connection *c = tcp_handle(handle);
    if (c == NULL) {
        return;
    }
    c->detached = true;
    c->receive_length = 0;
    if (c->state == TCP_SYN_SENT || c->state == TCP_CLOSED || c->failed) {
        if (c->state == TCP_SYN_SENT) {
            tcp_segment(c, c->snd_nxt, TCP_RST, NULL, 0);
        }
        tcp_free(c);
        return;
    }
    c->fin_queued = true;
    tcp_push(c, false);
}

/* --------------------------------------------------------------- ICMP */

#define PING_ID 0x4f50
#define PING_COUNT 4
#define PING_TIMEOUT_TICKS (2 * TICKS_PER_SECOND)

static void icmp_receive(const uint8_t *ip, const uint8_t *icmp,
                         size_t length);

/* -------------------------------------------------------------- session */

enum session_kind {
    SESSION_NONE,
    SESSION_PING,
    SESSION_LOOKUP,
    SESSION_HTTP,
};

static enum session_kind session;
static bool session_resolving;
static char session_host[DNS_NAME_CAPACITY];
static uint8_t ping_target[4];
static uint16_t ping_sequence;  /* last sent */
static uint64_t ping_sent_tick;
static uint64_t ping_sent_us;
static bool ping_waiting;
static unsigned ping_received;
static uint64_t ping_total_us;

/* HTTP test fetch (SESSION_HTTP). */
#define HTTP_PATH_CAPACITY 128
#define HTTP_BODY_LINES 12
#define HTTP_TIMEOUT_TICKS (20 * TICKS_PER_SECOND)

static char http_path[HTTP_PATH_CAPACITY];
static int http_handle = -1;
static bool http_requested;
static bool http_in_body;
static char http_line[LINE_CAPACITY];
static size_t http_line_length;
static bool http_line_cut;
static uint8_t http_chunk[512];
static size_t http_chunk_at;
static size_t http_chunk_length;
static unsigned http_body_lines;
static uint64_t http_body_bytes;
static uint64_t http_started;

static void session_end(void) {
    if (http_handle >= 0) {
        net_tcp_close(http_handle);
        http_handle = -1;
    }
    session = SESSION_NONE;
    dns_active = false;
}

static void ping_send(void) {
    uint8_t *icmp = frame_out + ETHERNET_HEADER + IP_HEADER;
    ping_sequence++;
    icmp[0] = 8;
    icmp[1] = 0;
    put16(icmp + 2, 0);
    put16(icmp + 4, PING_ID);
    put16(icmp + 6, ping_sequence);
    for (int i = 0; i < 32; i++) {
        icmp[8 + i] = (uint8_t)('a' + i % 23);
    }
    put16(icmp + 2, checksum_finish(checksum_add(0, icmp, 40)));
    ping_sent_tick = timer_ticks();
    ping_sent_us = now_microseconds();
    ping_waiting = true;
    (void)ip_send(ping_target, IP_PROTOCOL_ICMP, 40);
}

static void ping_summary(void) {
    char line[LINE_CAPACITY];
    size_t length = append(line, sizeof(line), 0, "Sent ");
    length = append_number(line, sizeof(line), length, ping_sequence);
    length = append(line, sizeof(line), length, ", received ");
    length = append_number(line, sizeof(line), length, ping_received);
    if (ping_received != 0) {
        length = append(line, sizeof(line), length, ", average ");
        uint64_t average = ping_total_us / ping_received;
        length = append_number(line, sizeof(line), length, average / 1000);
        length = append(line, sizeof(line), length, ".");
        length = append_number(line, sizeof(line), length,
                               average / 100 % 10);
        length = append(line, sizeof(line), length, " ms");
    }
    output(line);
}

static void icmp_receive(const uint8_t *ip, const uint8_t *icmp,
                         size_t length) {
    if (length < 8 || checksum_finish(checksum_add(0, icmp, length)) != 0) {
        return;
    }
    if (icmp[0] == 8 && configured && same(ip + 16, our_ip, 4)) {
        /* Echo request: answer with the same identifier, sequence and
           data. */
        if (length > FRAME_CAPACITY - ETHERNET_HEADER - IP_HEADER) {
            return;
        }
        uint8_t source[4];
        copy(source, ip + 12, 4);
        uint8_t *reply = frame_out + ETHERNET_HEADER + IP_HEADER;
        copy(reply, icmp, length);
        reply[0] = 0;
        put16(reply + 2, 0);
        put16(reply + 2, checksum_finish(checksum_add(0, reply, length)));
        (void)ip_send(source, IP_PROTOCOL_ICMP, length);
        return;
    }
    if (icmp[0] == 0 && session == SESSION_PING && ping_waiting
        && get16(icmp + 4) == PING_ID && get16(icmp + 6) == ping_sequence
        && same(ip + 12, ping_target, 4)) {
        uint64_t elapsed = now_microseconds() - ping_sent_us;
        ping_waiting = false;
        ping_received++;
        ping_total_us += elapsed;
        char line[LINE_CAPACITY];
        size_t used = append(line, sizeof(line), 0, "Reply from ");
        used = net_format_ipv4(line, sizeof(line), used, ip + 12);
        used = append(line, sizeof(line), used, ": seq=");
        used = append_number(line, sizeof(line), used, ping_sequence);
        used = append(line, sizeof(line), used, " ttl=");
        used = append_number(line, sizeof(line), used, ip[8]);
        used = append(line, sizeof(line), used, " time=");
        used = append_number(line, sizeof(line), used, elapsed / 1000);
        used = append(line, sizeof(line), used, ".");
        used = append_number(line, sizeof(line), used, elapsed / 100 % 10);
        append(line, sizeof(line), used, " ms");
        output(line);
    }
}

static bool session_begin(enum session_kind kind, const char *host,
                          size_t length) {
    if (!nic_ready()) {
        output("No network card.");
        return false;
    }
    if (!configured) {
        output("No network address yet (DHCP is still asking).");
        return false;
    }
    while (length != 0 && host[length - 1] == ' ') {
        length--;
    }
    if (length == 0 || length >= DNS_NAME_CAPACITY) {
        output("Give a host name or address.");
        return false;
    }
    session_end();
    copy(session_host, host, length);
    session_host[length] = '\0';
    session = kind;
    ping_sequence = 0;
    ping_received = 0;
    ping_total_us = 0;
    ping_waiting = false;
    if (net_parse_ipv4(host, length, ping_target)) {
        session_resolving = false;
    } else if (dns_start(host, length)) {
        session_resolving = true;
    } else {
        output("Cannot look up names: no DNS server.");
        session_end();
        return false;
    }
    return true;
}

bool net_ping_start(const char *host, size_t length) {
    return session_begin(SESSION_PING, host, length);
}

bool net_lookup_start(const char *host, size_t length) {
    return session_begin(SESSION_LOOKUP, host, length);
}

bool net_http_start(const char *text, size_t length) {
    /* [http://]host[/path] */
    if (length >= 7 && same(text, "http://", 7)) {
        text += 7;
        length -= 7;
    }
    size_t host_length = 0;
    while (host_length < length && text[host_length] != '/'
           && text[host_length] != ' ') {
        host_length++;
    }
    size_t path_length = length - host_length;
    const char *path = text + host_length;
    while (path_length != 0 && *path == ' ') {
        path++;
        path_length--;
    }
    if (path_length >= HTTP_PATH_CAPACITY || (path_length != 0 && *path != '/')) {
        output("Usage: http HOST[/PATH]");
        return false;
    }
    http_path[0] = '/';
    http_path[1] = '\0';
    if (path_length != 0) {
        copy(http_path, path, path_length);
        http_path[path_length] = '\0';
    }
    http_requested = false;
    http_in_body = false;
    http_line_length = 0;
    http_chunk_at = 0;
    http_chunk_length = 0;
    http_body_lines = 0;
    http_body_bytes = 0;
    return session_begin(SESSION_HTTP, text, host_length);
}

bool net_session_active(void) {
    return session != SESSION_NONE;
}

void net_session_stop(void) {
    if (session == SESSION_PING && ping_sequence != 0) {
        ping_summary();
    }
    session_end();
}

/* Ends the current header or body line. */
static void http_line_end(void) {
    if (http_line_length != 0 && http_line[http_line_length - 1] == '\r') {
        http_line_length--;
    }
    http_line[http_line_length] = '\0';
    if (!http_in_body) {
        if (http_line_length == 0) {
            http_in_body = true;
        } else {
            output(http_line);
        }
    } else {
        if (http_body_lines == 0) {
            output("--- body ---");
        }
        if (http_body_lines < HTTP_BODY_LINES) {
            output(http_line);
        }
        http_body_lines++;
    }
    http_line_length = 0;
    http_line_cut = false;
}

static void http_poll(void) {
    char line[LINE_CAPACITY];
    if (http_handle < 0) {
        size_t used = append(line, sizeof(line), 0, "Connecting to ");
        used = net_format_ipv4(line, sizeof(line), used, ping_target);
        append(line, sizeof(line), used, " port 80...");
        output(line);
        http_handle = net_tcp_connect(ping_target, 80);
        http_started = timer_ticks();
        if (http_handle < 0) {
            output("No free connection.");
            session_end();
        }
        return;
    }
    enum net_tcp_status status = net_tcp_status(http_handle);
    if (status == NET_TCP_FAILED) {
        output(http_requested ? "Connection lost." : "Cannot connect.");
        session_end();
        return;
    }
    if (status == NET_TCP_CONNECTING) {
        if (timer_ticks() - http_started > HTTP_TIMEOUT_TICKS) {
            output("Connection timed out.");
            session_end();
        }
        return;
    }
    if (!http_requested) {
        char request[2 * DNS_NAME_CAPACITY + HTTP_PATH_CAPACITY + 64];
        size_t used = append(request, sizeof(request), 0, "GET ");
        used = append(request, sizeof(request), used, http_path);
        used = append(request, sizeof(request), used,
                      " HTTP/1.0\r\nHost: ");
        used = append(request, sizeof(request), used, session_host);
        used = append(request, sizeof(request), used,
                      "\r\nUser-Agent: OPOS\r\nConnection: close\r\n\r\n");
        (void)net_tcp_send(http_handle, request, used);
        http_requested = true;
        output("Connected; request sent.");
        return;
    }
    /* Stop while the output queue is nearly full; the rest of the chunk
       waits for the next tick. */
    while (line_count < LINE_COUNT - 1) {
        if (http_chunk_at == http_chunk_length) {
            http_chunk_at = 0;
            http_chunk_length = net_tcp_receive(http_handle, http_chunk,
                                                sizeof(http_chunk));
            if (http_chunk_length == 0) {
                break;
            }
        }
        char c = (char)http_chunk[http_chunk_at++];
        if (http_in_body) {
            http_body_bytes++;
        }
        if (c == '\n') {
            http_line_end();
        } else if (http_line_length + 1 < sizeof(http_line)) {
            http_line[http_line_length++] = c >= ' ' || c == '\r' ? c : '?';
        } else {
            http_line_cut = true;
        }
    }
    if (status == NET_TCP_FINISHED && http_chunk_at == http_chunk_length) {
        if (http_line_length != 0) {
            http_line_end();
        }
        size_t used = append(line, sizeof(line), 0, "Done: ");
        used = append_number(line, sizeof(line), used, http_body_bytes);
        used = append(line, sizeof(line), used, " bytes of body, ");
        used = append_number(line, sizeof(line), used, http_body_lines);
        append(line, sizeof(line), used, " lines.");
        output(line);
        session_end();
    }
}

static void session_poll(void) {
    if (session == SESSION_NONE) {
        return;
    }
    char line[LINE_CAPACITY];
    if (session_resolving) {
        if (!dns_done) {
            return;
        }
        session_resolving = false;
        dns_active = false;
        if (dns_failed) {
            size_t used = append(line, sizeof(line), 0, "Cannot find ");
            append(line, sizeof(line), used, session_host);
            output(line);
            session_end();
            return;
        }
        copy(ping_target, dns_result, 4);
        if (session == SESSION_LOOKUP) {
            size_t used = append(line, sizeof(line), 0, session_host);
            used = append(line, sizeof(line), used, " is ");
            net_format_ipv4(line, sizeof(line), used, ping_target);
            output(line);
            session_end();
            return;
        }
    }
    if (session == SESSION_LOOKUP) {
        size_t used = append(line, sizeof(line), 0, session_host);
        used = append(line, sizeof(line), used, " is ");
        net_format_ipv4(line, sizeof(line), used, ping_target);
        output(line);
        session_end();
        return;
    }
    if (session == SESSION_HTTP) {
        http_poll();
        return;
    }
    uint64_t now = timer_ticks();
    if (ping_sequence == 0) {
        size_t used = append(line, sizeof(line), 0, "Pinging ");
        used = net_format_ipv4(line, sizeof(line), used, ping_target);
        append(line, sizeof(line), used, " with 32 bytes of data:");
        output(line);
        ping_send();
        return;
    }
    if (ping_waiting && now - ping_sent_tick >= PING_TIMEOUT_TICKS) {
        ping_waiting = false;
        output("Request timed out.");
    }
    if (!ping_waiting && now - ping_sent_tick >= TICKS_PER_SECOND) {
        if (ping_sequence >= PING_COUNT) {
            ping_summary();
            session_end();
            return;
        }
        ping_send();
    }
}

/* -------------------------------------------------------------- driver */

void net_initialize(void) {
    if (!nic_initialize()) {
        return;
    }
    nic_mac_address(our_mac);
    dhcp_begin();
}

void net_poll(void) {
    if (!nic_ready()) {
        return;
    }
    /* A link that comes back may lead to another network: ask DHCP again
       (checked twice a second). */
    static bool link_was_up = true;
    static uint64_t link_checked;
    if (timer_ticks() - link_checked >= TICKS_PER_SECOND / 2) {
        link_checked = timer_ticks();
        bool link = nic_link_up();
        if (link && !link_was_up) {
            net_dhcp_restart();
        }
        link_was_up = link;
    }
    for (int frames = 0; frames < 32; frames++) {
        size_t length = nic_receive(frame_in, sizeof(frame_in));
        if (length == 0) {
            break;
        }
        if (length < ETHERNET_HEADER) {
            continue;
        }
        uint16_t type = get16(frame_in + 12);
        if (type == ETHERTYPE_ARP) {
            arp_receive(frame_in + ETHERNET_HEADER, length - ETHERNET_HEADER);
        } else if (type == ETHERTYPE_IPV4) {
            ip_receive(frame_in + ETHERNET_HEADER, length - ETHERNET_HEADER);
        }
    }
    arp_poll();
    tcp_poll();
    dhcp_poll();
    dns_poll();
    session_poll();
}

void net_get_status(struct net_status *status) {
    zero(status, sizeof(*status));
    status->card = nic_ready();
    if (!status->card) {
        status->state = "no network card";
        return;
    }
    status->link = nic_link_up();
    status->configured = configured;
    status->pci_id = nic_pci_id();
    copy(status->mac, our_mac, 6);
    copy(status->ip, our_ip, 4);
    copy(status->mask, our_mask, 4);
    copy(status->gateway, our_gateway, 4);
    copy(status->dns, our_dns, 4);
    status->lease_seconds = dhcp_lease;
    static const char *const names[] = {
        "off", "asking for an address", "requesting the address", "bound",
    };
    status->state = names[dhcp_state];
    status->sent = nic_sent_count();
    status->received = nic_received_count();
}

void net_describe(void (*print)(const char *line)) {
    struct net_status status;
    net_get_status(&status);
    char line[LINE_CAPACITY];
    if (!status.card) {
        print("No supported network card (Intel e1000/e1000e or VMware vmxnet3).");
        return;
    }
    static const char hex[] = "0123456789abcdef";
    size_t used = append(line, sizeof(line), 0, "Card: ");
    used = append(line, sizeof(line), used, nic_name());
    used = append(line, sizeof(line), used, " (");
    char vendor[6] = {hex[status.pci_id >> 28 & 15],
                      hex[status.pci_id >> 24 & 15],
                      hex[status.pci_id >> 20 & 15],
                      hex[status.pci_id >> 16 & 15], ':', '\0'};
    used = append(line, sizeof(line), used, vendor);
    char id[5] = {hex[status.pci_id >> 12 & 15], hex[status.pci_id >> 8 & 15],
                  hex[status.pci_id >> 4 & 15], hex[status.pci_id & 15], '\0'};
    used = append(line, sizeof(line), used, id);
    used = append(line, sizeof(line), used, "), MAC ");
    used = net_format_mac(line, sizeof(line), used, status.mac);
    append(line, sizeof(line), used, status.link ? ", link up" : ", link down");
    print(line);
    used = append(line, sizeof(line), 0, "DHCP: ");
    append(line, sizeof(line), used, status.state);
    print(line);
    if (!status.configured) {
        return;
    }
    used = append(line, sizeof(line), 0, "Address ");
    used = net_format_ipv4(line, sizeof(line), used, status.ip);
    used = append(line, sizeof(line), used, ", mask ");
    net_format_ipv4(line, sizeof(line), used, status.mask);
    print(line);
    used = append(line, sizeof(line), 0, "Gateway ");
    used = net_format_ipv4(line, sizeof(line), used, status.gateway);
    used = append(line, sizeof(line), used, ", DNS ");
    net_format_ipv4(line, sizeof(line), used, status.dns);
    print(line);
    used = append(line, sizeof(line), 0, "Frames sent ");
    used = append_number(line, sizeof(line), used, status.sent);
    used = append(line, sizeof(line), used, ", received ");
    append_number(line, sizeof(line), used, status.received);
    print(line);
}
