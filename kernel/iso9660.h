#ifndef OPOS_ISO9660_H
#define OPOS_ISO9660_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "optical.h"

/*
 * Read-only ISO 9660 discs in the CD/DVD drives. Volume n is the disc in
 * optical drive n, shown at /cdrom (n = 0) and /cdrom2../cdrom8. Names
 * come from Joliet when the disc has it, else from Rock Ridge, else the
 * plain ISO name without its ";1" version.
 */
#define ISO_MAX_VOLUMES OPTICAL_MAX_DRIVES

enum iso_disc_state {
    ISO_DISC_NONE,        /* no disc, or the drive does not answer */
    ISO_DISC_MOUNTED,
    ISO_DISC_UDF,         /* UDF only (most DVD video and Windows media) */
    ISO_DISC_UNKNOWN,     /* audio CD or an unknown format */
};

struct iso_node {
    uint32_t extent;      /* first 2048-byte sector */
    uint32_t size;        /* bytes */
    bool directory;
};

/* Checks every drive: mounts discs that were put in, drops discs that
   were taken out or changed. Cheap enough to call when Files lists the
   drives (one TEST UNIT READY and one sector read per drive). */
void iso_refresh(void);
enum iso_disc_state iso_state(size_t volume);
bool iso_mounted(size_t volume);
/* Volume label, trimmed; "" when it has none. */
const char *iso_label(size_t volume);
/* Changes on every mount, so open files can tell the disc was changed. */
uint32_t iso_generation(size_t volume);
/* Takes the disc out of the namespace and opens the tray. */
bool iso_eject(size_t volume);

/* path is relative to the disc root: "" for the root, "a/b" below it. */
bool iso_resolve(size_t volume, const char *path, size_t length,
                 struct iso_node *node);
/* The index-th entry of a directory ("." and ".." are skipped). */
bool iso_directory_entry_at(size_t volume, const struct iso_node *directory,
                            size_t index, char *name, size_t capacity,
                            size_t *name_length, struct iso_node *child);
bool iso_directory_count(size_t volume, const struct iso_node *directory,
                         size_t *count);
bool iso_read(size_t volume, const struct iso_node *node, size_t offset,
              void *buffer, size_t length, size_t *bytes_read);

#endif
