#ifndef OPOS_VMXNET3_H
#define OPOS_VMXNET3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * VMware vmxnet3 paravirtual Ethernet (PCI 15ad:07b0), VMware's default
 * adapter for most guest types. Polled, one transmit and one receive
 * queue, device revision 1. Same calls as e1000.h.
 */
bool vmxnet3_initialize(void);
uint32_t vmxnet3_pci_id(void);
void vmxnet3_mac_address(uint8_t mac[6]);
bool vmxnet3_link_up(void);
bool vmxnet3_send(const void *frame, size_t length);
size_t vmxnet3_receive(void *buffer, size_t capacity);
uint64_t vmxnet3_sent_count(void);
uint64_t vmxnet3_received_count(void);

#endif
