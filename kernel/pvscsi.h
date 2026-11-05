#ifndef OPOS_PVSCSI_H
#define OPOS_PVSCSI_H

#include <stdbool.h>
#include <stddef.h>

/*
 * VMware Paravirtual SCSI (PVSCSI, PCI 15ad:07c0) host driver. The first
 * controller is used; its disks on targets 0-15 (LUN 0) are added to the
 * SCSI disk list in scsi.h. Commands are polled; interrupts stay masked.
 */
bool pvscsi_initialize(void);
/* Disks this controller added to the SCSI disk list. */
size_t pvscsi_disk_count(void);

#endif
