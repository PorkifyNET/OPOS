#include "iso9660.h"

/*
 * ISO 9660 (ECMA-119) reader with the Joliet and Rock Ridge name
 * extensions. Everything is read-only; directories are walked record by
 * record through a small sector cache.
 */

#define SECTOR OPTICAL_SECTOR_SIZE
#define DESCRIPTOR_FIRST 16
#define DESCRIPTOR_LIMIT 32
#define RECORD_MINIMUM 34
#define FLAG_HIDDEN 0x01
#define FLAG_DIRECTORY 0x02
#define FLAG_ASSOCIATED 0x04
#define FLAG_MULTI_EXTENT 0x80
/* Sectors per direct read when a file read covers whole sectors. */
#define DIRECT_CHUNK 32
#define IDENTITY_SIZE 64

struct iso_volume {
    enum iso_disc_state state;
    bool joliet;
    struct iso_node root;
    uint32_t generation;
    char label[33];
    uint8_t identity[IDENTITY_SIZE];
};

static struct iso_volume volumes[ISO_MAX_VOLUMES];
static uint32_t next_generation = 1;

#define CACHE_SLOTS 4
struct cache_tag {
    bool valid;
    size_t volume;
    uint32_t generation;
    uint32_t sector;
    uint32_t used;
};
static uint8_t cache_data[CACHE_SLOTS][SECTOR];
static struct cache_tag cache_tags[CACHE_SLOTS];
static uint32_t cache_clock;

static uint32_t le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
         | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static bool bytes_are(const uint8_t *bytes, const char *text) {
    for (size_t i = 0; text[i] != '\0'; i++) {
        if (bytes[i] != (uint8_t)text[i]) {
            return false;
        }
    }
    return true;
}

static bool usable(size_t volume) {
    return volume < ISO_MAX_VOLUMES && volume < optical_drive_count()
        && volumes[volume].state == ISO_DISC_MOUNTED;
}

/* One sector of a mounted disc, through the cache; NULL on a read error. */
static const uint8_t *read_sector(size_t volume, uint32_t sector) {
    uint32_t generation = volumes[volume].generation;
    size_t victim = 0;
    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        struct cache_tag *tag = &cache_tags[i];
        if (tag->valid && tag->volume == volume
            && tag->generation == generation && tag->sector == sector) {
            tag->used = ++cache_clock;
            return cache_data[i];
        }
        if (!tag->valid
            || (cache_tags[victim].valid && tag->used < cache_tags[victim].used)) {
            victim = i;
        }
    }
    struct cache_tag *tag = &cache_tags[victim];
    tag->valid = false;
    if (!optical_read(volume, sector, 1, cache_data[victim])) {
        return NULL;
    }
    *tag = (struct cache_tag){
        .valid = true,
        .volume = volume,
        .generation = generation,
        .sector = sector,
        .used = ++cache_clock,
    };
    return cache_data[victim];
}

static struct iso_node record_node(const uint8_t *record) {
    return (struct iso_node){
        .extent = le32(record + 2),
        .size = le32(record + 10),
        .directory = (record[25] & FLAG_DIRECTORY) != 0,
    };
}

/* What identifies a disc: label, size, creation date and root extent from
   the primary volume descriptor. A changed disc differs in one of them. */
static void disc_identity(const uint8_t *primary, uint8_t *identity) {
    size_t used = 0;
    for (size_t i = 0; i < 32; i++) {
        identity[used++] = primary[40 + i];       /* volume identifier */
    }
    for (size_t i = 0; i < 4; i++) {
        identity[used++] = primary[80 + i];       /* volume space size */
    }
    for (size_t i = 0; i < 17; i++) {
        identity[used++] = primary[813 + i];      /* creation date */
    }
    for (size_t i = 0; i < 4; i++) {
        identity[used++] = primary[156 + 2 + i];  /* root extent */
    }
    while (used < IDENTITY_SIZE) {
        identity[used++] = 0;
    }
}

static void drop(struct iso_volume *volume) {
    volume->state = ISO_DISC_NONE;
    volume->label[0] = '\0';
    volume->generation = next_generation++;
}

/* Reads the volume descriptors of the disc in `drive`. */
static void mount(size_t drive) {
    static uint8_t sector[SECTOR];
    struct iso_volume *volume = &volumes[drive];
    drop(volume);
    bool primary = false;
    bool udf = false;
    struct iso_node joliet_root = {0};
    bool joliet = false;
    for (uint32_t lba = DESCRIPTOR_FIRST;
         lba < DESCRIPTOR_FIRST + DESCRIPTOR_LIMIT; lba++) {
        if (!optical_read(drive, lba, 1, sector)) {
            break;
        }
        if (bytes_are(sector + 1, "BEA01") || bytes_are(sector + 1, "NSR02")
            || bytes_are(sector + 1, "NSR03")) {
            udf = true;
            continue;
        }
        if (bytes_are(sector + 1, "TEA01")) {
            break;
        }
        if (!bytes_are(sector + 1, "CD001")) {
            if (primary) {
                break;
            }
            continue;
        }
        if (sector[0] == 1 && !primary) {
            primary = true;
            volume->root = record_node(sector + 156);
            disc_identity(sector, volume->identity);
            size_t length = 0;
            for (size_t i = 0; i < 32; i++) {
                uint8_t character = sector[40 + i];
                volume->label[length++] = character >= ' ' && character <= '~'
                    ? (char)character : '?';
            }
            while (length > 0 && volume->label[length - 1] == ' ') {
                length--;
            }
            volume->label[length] = '\0';
        } else if (sector[0] == 2 && sector[88] == '%' && sector[89] == '/'
                   && (sector[90] == '@' || sector[90] == 'C'
                       || sector[90] == 'E')) {
            /* Joliet: UCS-2 names in a second directory tree. */
            joliet = true;
            joliet_root = record_node(sector + 156);
        }
    }
    if (primary && volume->root.directory) {
        volume->joliet = joliet && joliet_root.directory;
        if (volume->joliet) {
            volume->root = joliet_root;
        }
        volume->state = ISO_DISC_MOUNTED;
    } else {
        volume->state = udf ? ISO_DISC_UDF : ISO_DISC_UNKNOWN;
    }
}

void iso_refresh(void) {
    static uint8_t sector[SECTOR];
    for (size_t drive = 0;
         drive < optical_drive_count() && drive < ISO_MAX_VOLUMES; drive++) {
        struct iso_volume *volume = &volumes[drive];
        if (!optical_check(drive)) {
            if (volume->state != ISO_DISC_NONE) {
                drop(volume);
            }
            continue;
        }
        if (volume->state == ISO_DISC_MOUNTED
            && optical_read(drive, DESCRIPTOR_FIRST, 1, sector)) {
            uint8_t identity[IDENTITY_SIZE];
            disc_identity(sector, identity);
            bool same = true;
            for (size_t i = 0; i < IDENTITY_SIZE; i++) {
                same = same && identity[i] == volume->identity[i];
            }
            if (same) {
                continue;
            }
        }
        mount(drive);
    }
}

enum iso_disc_state iso_state(size_t volume) {
    return volume < ISO_MAX_VOLUMES ? volumes[volume].state : ISO_DISC_NONE;
}

bool iso_mounted(size_t volume) {
    return usable(volume);
}

const char *iso_label(size_t volume) {
    return usable(volume) ? volumes[volume].label : "";
}

uint32_t iso_generation(size_t volume) {
    return volume < ISO_MAX_VOLUMES ? volumes[volume].generation : 0;
}

bool iso_eject(size_t volume) {
    if (volume >= ISO_MAX_VOLUMES || volume >= optical_drive_count()) {
        return false;
    }
    drop(&volumes[volume]);
    return optical_eject(volume);
}

/*
 * Directory walking. *offset is the byte position in the directory; the
 * call returns the next record (".", ".." and continuation records of
 * multi-extent files included) and advances *offset past it.
 */
static const uint8_t *next_record(size_t volume,
                                  const struct iso_node *directory,
                                  uint32_t *offset) {
    while (*offset < directory->size) {
        uint32_t within = *offset % SECTOR;
        const uint8_t *data = read_sector(volume,
                                          directory->extent + *offset / SECTOR);
        if (data == NULL) {
            return NULL;
        }
        uint8_t length = data[within];
        if (length == 0) {
            /* Records never cross sectors; the rest is padding. */
            *offset += SECTOR - within;
            continue;
        }
        if (length < RECORD_MINIMUM || within + length > SECTOR
            || RECORD_MINIMUM - 1 + data[within + 32] > length) {
            return NULL; /* damaged directory */
        }
        *offset += length;
        return data + within;
    }
    return NULL;
}

/* Name of a record into name (NUL-terminated); returns its length. */
static size_t record_name(const struct iso_volume *volume,
                          const uint8_t *record, char *name,
                          size_t capacity) {
    size_t id_length = record[32];
    const uint8_t *id = record + 33;
    size_t length = 0;
    if (capacity == 0) {
        return 0;
    }
    if (volume->joliet) {
        for (size_t i = 0; i + 1 < id_length && length + 1 < capacity;
             i += 2) {
            uint16_t character = (uint16_t)((id[i] << 8) | id[i + 1]);
            if (character == ';') {
                break;
            }
            name[length++] = character >= ' ' && character <= '~'
                ? (char)character : '_';
        }
        name[length] = '\0';
        return length;
    }
    /* Rock Ridge: "NM" entries in the system use area after the name. */
    const uint8_t *use = id + id_length + ((id_length & 1) == 0 ? 1 : 0);
    const uint8_t *end = record + record[0];
    bool rock_ridge = false;
    while (use + 4 <= end) {
        uint8_t entry_length = use[2];
        if (entry_length < 4 || use + entry_length > end) {
            break;
        }
        if (use[0] == 'N' && use[1] == 'M' && entry_length >= 5
            && (use[4] & 0x06) == 0) {
            rock_ridge = true;
            for (size_t i = 5; i < entry_length && length + 1 < capacity;
                 i++) {
                uint8_t character = use[i];
                name[length++] = character >= ' ' && character <= '~'
                    ? (char)character : '_';
            }
        }
        use += entry_length;
    }
    if (rock_ridge && length != 0) {
        name[length] = '\0';
        return length;
    }
    length = 0;
    for (size_t i = 0; i < id_length && id[i] != ';' && length + 1 < capacity;
         i++) {
        name[length++] = (char)id[i];
    }
    /* "README." (no extension) is shown as "README". */
    if (length > 1 && name[length - 1] == '.') {
        length--;
    }
    name[length] = '\0';
    return length;
}

static bool visible_record(const uint8_t *record, bool after_multi_extent) {
    uint8_t id_length = record[32];
    if (id_length == 1 && (record[33] == 0 || record[33] == 1)) {
        return false; /* "." and ".." */
    }
    return (record[25] & FLAG_ASSOCIATED) == 0 && !after_multi_extent;
}

bool iso_directory_entry_at(size_t volume, const struct iso_node *directory,
                            size_t index, char *name, size_t capacity,
                            size_t *name_length, struct iso_node *child) {
    if (!usable(volume) || directory == NULL || !directory->directory) {
        return false;
    }
    uint32_t offset = 0;
    bool after_multi_extent = false;
    const uint8_t *record;
    while ((record = next_record(volume, directory, &offset)) != NULL) {
        bool visible = visible_record(record, after_multi_extent);
        after_multi_extent = (record[25] & FLAG_MULTI_EXTENT) != 0;
        if (!visible || index-- != 0) {
            continue;
        }
        size_t length = record_name(&volumes[volume], record, name, capacity);
        if (name_length != NULL) {
            *name_length = length;
        }
        if (child != NULL) {
            *child = record_node(record);
        }
        return true;
    }
    return false;
}

bool iso_directory_count(size_t volume, const struct iso_node *directory,
                         size_t *count) {
    if (!usable(volume) || directory == NULL || !directory->directory
        || count == NULL) {
        return false;
    }
    *count = 0;
    uint32_t offset = 0;
    bool after_multi_extent = false;
    const uint8_t *record;
    while ((record = next_record(volume, directory, &offset)) != NULL) {
        if (visible_record(record, after_multi_extent)) {
            (*count)++;
        }
        after_multi_extent = (record[25] & FLAG_MULTI_EXTENT) != 0;
    }
    return true;
}

static bool name_equal(const char *left, size_t left_length,
                       const char *right, size_t right_length) {
    if (left_length != right_length) {
        return false;
    }
    for (size_t i = 0; i < left_length; i++) {
        char a = left[i];
        char b = right[i];
        a = a >= 'a' && a <= 'z' ? (char)(a - 32) : a;
        b = b >= 'a' && b <= 'z' ? (char)(b - 32) : b;
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool iso_resolve(size_t volume, const char *path, size_t length,
                 struct iso_node *node) {
    if (!usable(volume) || node == NULL) {
        return false;
    }
    struct iso_node current = volumes[volume].root;
    size_t position = 0;
    while (position < length) {
        while (position < length && path[position] == '/') {
            position++;
        }
        size_t start = position;
        while (position < length && path[position] != '/') {
            position++;
        }
        if (position == start) {
            break;
        }
        if (!current.directory) {
            return false;
        }
        bool found = false;
        char name[256];
        size_t name_length;
        struct iso_node child;
        for (size_t index = 0;
             iso_directory_entry_at(volume, &current, index, name,
                                    sizeof(name), &name_length, &child);
             index++) {
            if (name_equal(name, name_length, path + start,
                           position - start)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
        current = child;
    }
    *node = current;
    return true;
}

bool iso_read(size_t volume, const struct iso_node *node, size_t offset,
              void *buffer, size_t length, size_t *bytes_read) {
    if (bytes_read != NULL) {
        *bytes_read = 0;
    }
    if (!usable(volume) || node == NULL || node->directory
        || (buffer == NULL && length != 0)) {
        return false;
    }
    if (offset >= node->size) {
        return true;
    }
    if (length > node->size - offset) {
        length = node->size - offset;
    }
    uint8_t *destination = buffer;
    size_t done = 0;
    while (done < length) {
        size_t position = offset + done;
        uint32_t sector = node->extent + (uint32_t)(position / SECTOR);
        size_t within = position % SECTOR;
        size_t remaining = length - done;
        if (within == 0 && remaining >= SECTOR) {
            uint32_t count = (uint32_t)(remaining / SECTOR);
            if (count > DIRECT_CHUNK) {
                count = DIRECT_CHUNK;
            }
            if (!optical_read(volume, sector, count, destination + done)) {
                return false;
            }
            done += (size_t)count * SECTOR;
            continue;
        }
        const uint8_t *data = read_sector(volume, sector);
        if (data == NULL) {
            return false;
        }
        size_t part = SECTOR - within;
        if (part > remaining) {
            part = remaining;
        }
        for (size_t i = 0; i < part; i++) {
            destination[done + i] = data[within + i];
        }
        done += part;
    }
    if (bytes_read != NULL) {
        *bytes_read = done;
    }
    return true;
}
