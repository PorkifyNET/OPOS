#ifndef OPOS_NVME_H
#define OPOS_NVME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NVME_MAX_NAMESPACES 8
#define NVME_MODEL_LENGTH 40

/* Describes the first active namespace, whatever its block size. */
struct nvme_status {
    bool controller_ready;
    uint32_t namespace_id;
    uint32_t logical_block_size;
    uint64_t namespace_blocks;
};

/* Sets up the first NVMe controller. Every active namespace with 512-byte
   blocks becomes a disk, numbered 0..nvme_disk_count()-1. */
bool nvme_initialize(void);
void nvme_get_status(struct nvme_status *status);
size_t nvme_disk_count(void);
uint64_t nvme_sector_count(size_t disk);
const char *nvme_model(size_t disk);
uint32_t nvme_namespace_id(size_t disk);
bool nvme_read_sectors(size_t disk, uint32_t first_sector,
                       uint8_t sector_count, void *buffer);
/* Batch writes: skip the flush after each write until nvme_flush. */
void nvme_set_flush_deferred(bool deferred);
bool nvme_flush(size_t disk);
bool nvme_write_sectors(size_t disk, uint32_t first_sector,
                        uint8_t sector_count, const void *buffer);

#endif
