#ifndef OPOS_NIC_H
#define OPOS_NIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The network card in use, whichever driver runs it: Intel e1000/e1000e
 * (e1000.c) or VMware vmxnet3 (vmxnet3.c). Frames are whole Ethernet
 * frames without the CRC, at most 1514 bytes.
 */

/* Starts the first supported card. */
bool nic_initialize(void);
bool nic_ready(void);
/* "Intel e1000" or "VMware vmxnet3"; "" without a card. */
const char *nic_name(void);
/* PCI vendor << 16 | device, or 0. */
uint32_t nic_pci_id(void);
void nic_mac_address(uint8_t mac[6]);
bool nic_link_up(void);
bool nic_send(const void *frame, size_t length);
size_t nic_receive(void *buffer, size_t capacity);
uint64_t nic_sent_count(void);
uint64_t nic_received_count(void);
/* Frames the card can hold between two polls; TCP keeps its window to
   about this many full segments so a burst does not overflow the ring. */
size_t nic_receive_frames(void);

#endif
