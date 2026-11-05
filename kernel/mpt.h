#ifndef OPOS_MPT_H
#define OPOS_MPT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * LSI Fusion-MPT host driver for VMware's "LSI Logic Parallel" (53C1030,
 * PCI 1000:0030) and "LSI Logic SAS" (SAS1068, 1000:0054) controllers, and
 * QEMU's mptsas1068. The first controller is used; its disks (LUN 0) are
 * added to the SCSI disk list in scsi.h. Commands are polled.
 */
bool mpt_initialize(void);
/* Disks this controller added to the SCSI disk list. */
size_t mpt_disk_count(void);

#endif
