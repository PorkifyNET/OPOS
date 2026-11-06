#ifndef OPOS_XHCI_H
#define OPOS_XHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * USB 3 (xHCI) host controller, polled (no interrupts). It enumerates the
 * devices on the root ports, configures USB mass-storage devices and hands
 * them to usb_storage.c. Hubs and other device classes are not supported
 * yet; their ports are noticed but left alone.
 */
#define XHCI_MAX_DEVICES 16
/* The shared bulk data buffer: 64 KiB, physically contiguous, aligned so
   one transfer never crosses a 64 KiB boundary. */
#define XHCI_TRANSFER_BUFFER_SIZE 65536

enum usb_result {
    USB_OK,
    USB_STALL,       /* the endpoint halted (xhci_clear_halt recovers) */
    USB_ERROR,       /* transaction error, timeout, device gone */
};

/* Finds and starts the first xHCI controller and attaches the devices
   that are already plugged in. Only runs under a hypervisor: on real
   hardware the firmware's USB keyboard emulation would stop, and OPOS has
   no USB keyboard driver yet. */
bool xhci_initialize(void);
bool xhci_ready(void);
/* Looks at the root ports: attaches new devices, detaches removed ones.
   True when something changed. */
bool xhci_poll(void);

uint8_t *xhci_transfer_buffer(void);
/* A small DMA buffer (512 bytes) for command and status blocks. */
uint8_t *xhci_small_buffer(void);
/* Bulk transfer on a configured mass-storage device, in the transfer
   buffer (small = false) or the small buffer. *done gets the bytes moved;
   a short IN transfer is USB_OK. */
enum usb_result xhci_bulk(size_t device, bool in, bool small, uint32_t length,
                          uint32_t *done);
/* Clears a halted bulk endpoint (after USB_STALL). */
bool xhci_clear_halt(size_t device, bool in);
/* Class or vendor request without a data stage (e.g. the mass-storage
   reset). */
bool xhci_control_no_data(size_t device, uint8_t request_type,
                          uint8_t request, uint16_t value, uint16_t index);
/* Root port (1-based) the device is plugged into. */
uint8_t xhci_device_port(size_t device);

#endif
