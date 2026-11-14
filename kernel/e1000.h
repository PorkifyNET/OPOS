#ifndef OPOS_E1000_H
#define OPOS_E1000_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Intel PRO/1000 (e1000 family) Ethernet driver: the 8254x cards QEMU and
 * VMware emulate ("e1000", 82540EM/82545EM) and the 82574L ("e1000e").
 * Polled; frames are whole Ethernet frames without the CRC.
 */

/* Finds and starts the first supported card. */
bool e1000_initialize(void);
bool e1000_ready(void);
/* PCI vendor:device of the card in use, or 0. */
uint32_t e1000_pci_id(void);
void e1000_mac_address(uint8_t mac[6]);
bool e1000_link_up(void);
/* Receive (and transmit) ring entries. */
#define E1000_RX_FRAMES 256
/* Sends one frame (14-byte header + payload, at most 1514 bytes). Returns
   false when the card is missing or the transmit ring stays full. */
bool e1000_send(const void *frame, size_t length);
/* Copies the next received frame into buffer and returns its length, or 0
   when nothing is waiting. Longer frames are cut to capacity. */
size_t e1000_receive(void *buffer, size_t capacity);
/* Frames counted since boot. */
uint64_t e1000_sent_count(void);
uint64_t e1000_received_count(void);

#endif
