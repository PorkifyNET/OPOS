#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "e1000.h"
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
    return e1000_send(frame_out, ETHERNET_HEADER + payload_length);
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
    if (!e1000_ready()) {
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

static void session_end(void) {
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
    if (!e1000_ready()) {
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

bool net_session_active(void) {
    return session != SESSION_NONE;
}

void net_session_stop(void) {
    if (session == SESSION_PING && ping_sequence != 0) {
        ping_summary();
    }
    session_end();
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
    if (!e1000_initialize()) {
        return;
    }
    e1000_mac_address(our_mac);
    dhcp_begin();
}

void net_poll(void) {
    if (!e1000_ready()) {
        return;
    }
    for (int frames = 0; frames < 32; frames++) {
        size_t length = e1000_receive(frame_in, sizeof(frame_in));
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
    dhcp_poll();
    dns_poll();
    session_poll();
}

void net_get_status(struct net_status *status) {
    zero(status, sizeof(*status));
    status->card = e1000_ready();
    if (!status->card) {
        status->state = "no network card";
        return;
    }
    status->link = e1000_link_up();
    status->configured = configured;
    status->pci_id = e1000_pci_id();
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
    status->sent = e1000_sent_count();
    status->received = e1000_received_count();
}

void net_describe(void (*print)(const char *line)) {
    struct net_status status;
    net_get_status(&status);
    char line[LINE_CAPACITY];
    if (!status.card) {
        print("No supported network card (Intel e1000/e1000e).");
        return;
    }
    size_t used = append(line, sizeof(line), 0, "Card: Intel e1000 (8086:");
    static const char hex[] = "0123456789abcdef";
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
