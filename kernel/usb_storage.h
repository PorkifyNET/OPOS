#ifndef OPOS_USB_STORAGE_H
#define OPOS_USB_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * USB mass storage, Bulk-Only Transport with SCSI commands (class 8,
 * subclass 6, protocol 0x50): USB sticks, card readers, USB disks. Each
 * device becomes a target of the "USB" SCSI host, so scsi.c finds its
 * disk and the block layer lists it like any other disk.
 */
#define USB_STORAGE_MAX 4

/* A configured xHCI device with a mass-storage interface; true when its
   disk was added. */
bool usb_storage_attach(size_t device, uint8_t interface);
/* The xHCI device was unplugged. */
void usb_storage_detach(size_t device);

#endif
