#ifndef OPOS_PCNET_H
#define OPOS_PCNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * AMD PCnet-PCI II/III (Am79C970A/Am79C973, PCI 1022:2000), VMware's
 * "vlance" adapter and QEMU's "pcnet". Polled, 32-bit software style,
 * through the I/O ports. Same calls as e1000.h.
 */
bool pcnet_initialize(void);
uint32_t pcnet_pci_id(void);
void pcnet_mac_address(uint8_t mac[6]);
bool pcnet_link_up(void);
bool pcnet_send(const void *frame, size_t length);
size_t pcnet_receive(void *buffer, size_t capacity);
uint64_t pcnet_sent_count(void);
uint64_t pcnet_received_count(void);

#endif
