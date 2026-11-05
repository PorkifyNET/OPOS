#ifndef OPOS_BUSLOGIC_H
#define OPOS_BUSLOGIC_H

#include <stdbool.h>
#include <stddef.h>

/*
 * BusLogic MultiMaster host driver (BT-958, PCI 104b:1040; VMware's
 * "BusLogic" SCSI controller). The first controller is used; its disks
 * (LUN 0) are added to the SCSI disk list in scsi.h. Commands are polled.
 */
bool buslogic_initialize(void);
/* Disks this controller added to the SCSI disk list. */
size_t buslogic_disk_count(void);

#endif
