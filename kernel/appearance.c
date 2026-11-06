#include "appearance.h"

#include "accounts.h"
#include "fat16.h"
#include "installer.h"
#include "memory.h"
#include "permissions.h"
#include "vfs.h"

/* ------------------------------------------------------------- helpers */

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static size_t append(char *buffer, size_t capacity, size_t offset,
                     const char *text) {
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    if (offset < capacity) {
        buffer[offset] = '\0';
    }
    return offset;
}

static bool text_same(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static char upper(char c) {
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static bool same_ignoring_case(const char *a, size_t length, const char *b) {
    size_t i = 0;
    for (; i < length && b[i] != '\0'; i++) {
        if (upper(a[i]) != upper(b[i])) {
            return false;
        }
    }
    return i == length && b[i] == '\0';
}

/* ---------------------------------------------------------------- pools */

/*
 * Big buffers come from contiguous physical pages, mapped through the
 * HHDM. A pool only grows; the pages of an outgrown buffer go back to the
 * single-page free list.
 */
struct pool {
    uint8_t *base;
    uint64_t physical;
    size_t size;
};

#define POOL_ROUND (1024u * 1024u)
/* Leave this much memory to everything else. */
#define POOL_RESERVE_PAGES (32u * 1024u * 1024u / 4096u)

static void *pool_reserve(struct pool *pool, size_t bytes) {
    if (bytes == 0) {
        bytes = 1;
    }
    if (bytes <= pool->size) {
        return pool->base;
    }
    size_t rounded = (bytes + POOL_ROUND - 1) / POOL_ROUND * POOL_ROUND;
    uint64_t pages = rounded / 4096;
    if (pages + POOL_RESERVE_PAGES > memory_free_pages()) {
        return NULL;
    }
    uint64_t physical = physical_pages_alloc_contiguous(pages);
    if (physical == 0) {
        return NULL;
    }
    if (pool->physical != 0) {
        for (size_t offset = 0; offset < pool->size; offset += 4096) {
            (void)physical_page_free(pool->physical + offset);
        }
    }
    pool->physical = physical;
    pool->base = physical_page_to_hhdm(physical);
    pool->size = rounded;
    return pool->base;
}

static struct pool file_pool;
static struct pool decode_pools[IMAGE_SLOT_COUNT];
static struct pool scale_pools[IMAGE_SLOT_COUNT];
static struct pool wallpaper_pool;

#define IMAGE_FILE_MAX (64u * 1024u * 1024u)

/* Path of the image currently decoded in the WORK slot ("" if none). */
static char work_path[256];
static struct image work_image;

enum image_result appearance_image_load(enum image_slot slot,
                                        const char *path, size_t length,
                                        struct image *image,
                                        enum image_format *format) {
    if (slot == IMAGE_SLOT_WORK) {
        work_path[0] = '\0';
    }
    size_t size = 0;
    if (!vfs_file_size(path, length, &size) || size == 0) {
        return IMAGE_CORRUPT;
    }
    if (size > IMAGE_FILE_MAX) {
        return IMAGE_TOO_LARGE;
    }
    uint8_t *data = pool_reserve(&file_pool, size);
    if (data == NULL) {
        return IMAGE_NO_MEMORY;
    }
    size_t read = 0;
    if (!vfs_read_file(path, length, data, size, &read) || read != size) {
        return IMAGE_CORRUPT;
    }
    struct image_info info;
    enum image_result result = image_probe(data, size, &info);
    if (format != NULL) {
        *format = info.format;
    }
    if (result != IMAGE_OK) {
        return result;
    }
    void *scratch = pool_reserve(&decode_pools[slot], info.scratch_bytes);
    if (scratch == NULL) {
        return IMAGE_NO_MEMORY;
    }
    result = image_decode(data, size, scratch, decode_pools[slot].size, image);
    if (result == IMAGE_OK && slot == IMAGE_SLOT_WORK
        && length < sizeof(work_path)) {
        for (size_t i = 0; i < length; i++) {
            work_path[i] = path[i];
        }
        work_path[length] = '\0';
        work_image = *image;
    }
    return result;
}

uint32_t *appearance_scale_buffer(enum image_slot slot, size_t pixels) {
    return pool_reserve(&scale_pools[slot], pixels * 4);
}

bool appearance_is_image_name(const char *name, size_t length) {
    static const char *const extensions[] = {".PNG", ".JPG", ".JPEG", ".BMP"};
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
        size_t extension = text_length(extensions[i]);
        if (length > extension
            && same_ignoring_case(name + length - extension, extension,
                                  extensions[i])) {
            return true;
        }
    }
    return false;
}

/* --------------------------------------------------------------- themes */

static const char *const theme_names[THEME_COUNT] = {
    "OCEAN", "EMERALD", "VIOLET", "ROSE", "GRAPHITE", "LIGHT",
};

/* Hue in 1/1536 turns (256 per 60 degrees), saturation and lightness
   0-255. */
struct hsl {
    int32_t hue;
    int32_t saturation;
    int32_t lightness;
};

static struct hsl to_hsl(int32_t r, int32_t g, int32_t b) {
    int32_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int32_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    struct hsl hsl = {0, 0, (max + min) / 2};
    int32_t delta = max - min;
    if (delta == 0) {
        return hsl;
    }
    hsl.saturation = hsl.lightness < 128
        ? delta * 255 / (max + min)
        : delta * 255 / (510 - max - min);
    if (max == r) {
        hsl.hue = (g - b) * 256 / delta;
    } else if (max == g) {
        hsl.hue = 512 + (b - r) * 256 / delta;
    } else {
        hsl.hue = 1024 + (r - g) * 256 / delta;
    }
    hsl.hue = (hsl.hue % 1536 + 1536) % 1536;
    return hsl;
}

static int32_t hue_channel(int32_t p, int32_t q, int32_t t) {
    t = (t % 1536 + 1536) % 1536;
    if (t < 256) {
        return p + (q - p) * t / 256;
    }
    if (t < 768) {
        return q;
    }
    if (t < 1024) {
        return p + (q - p) * (1024 - t) / 256;
    }
    return p;
}

static void from_hsl(struct hsl hsl, uint8_t *r, uint8_t *g, uint8_t *b) {
    int32_t l = hsl.lightness < 0 ? 0 : hsl.lightness > 255 ? 255
              : hsl.lightness;
    int32_t s = hsl.saturation < 0 ? 0 : hsl.saturation > 255 ? 255
              : hsl.saturation;
    if (s == 0) {
        *r = *g = *b = (uint8_t)l;
        return;
    }
    int32_t q = l < 128 ? l * (255 + s) / 255 : l + s - l * s / 255;
    int32_t p = 2 * l - q;
    *r = (uint8_t)hue_channel(p, q, hsl.hue + 512);
    *g = (uint8_t)hue_channel(p, q, hsl.hue);
    *b = (uint8_t)hue_channel(p, q, hsl.hue - 512);
}

/* The Ocean palette's blues sit between 170 and 265 degrees. */
#define CHROME_HUE_LOW (170 * 1536 / 360)
#define CHROME_HUE_HIGH (265 * 1536 / 360)

static enum theme current_theme = THEME_OCEAN;

void appearance_map_color(uint8_t *red, uint8_t *green, uint8_t *blue) {
    if (current_theme == THEME_OCEAN) {
        return;
    }
    struct hsl hsl = to_hsl(*red, *green, *blue);
    bool chrome = hsl.saturation != 0 && hsl.hue >= CHROME_HUE_LOW
               && hsl.hue <= CHROME_HUE_HIGH;
    switch (current_theme) {
    case THEME_EMERALD:
    case THEME_VIOLET:
    case THEME_ROSE: {
        if (!chrome) {
            return;
        }
        static const int32_t shift[THEME_COUNT] = {
            0, -70 * 1536 / 360, 50 * 1536 / 360, 115 * 1536 / 360, 0, 0};
        hsl.hue += shift[current_theme];
        break;
    }
    case THEME_GRAPHITE:
        if (!chrome) {
            return;
        }
        hsl.saturation = hsl.saturation / 8;
        break;
    case THEME_LIGHT:
        if (chrome || hsl.saturation < 40) {
            /* Dark surfaces turn light and light text turns dark. */
            hsl.lightness = 255 - hsl.lightness;
            if (chrome && hsl.lightness > 160) {
                /* Light surfaces: a softer tint. */
                hsl.saturation = hsl.saturation * 2 / 3;
            }
        } else if (hsl.lightness > 140) {
            /* Status colours stay readable on a light background. */
            hsl.lightness = hsl.lightness * 9 / 16;
        } else {
            return;
        }
        break;
    default:
        return;
    }
    from_hsl(hsl, red, green, blue);
}

enum theme appearance_theme(void) {
    return current_theme;
}

const char *appearance_theme_name(enum theme theme) {
    return theme < THEME_COUNT ? theme_names[theme] : "?";
}

/* --------------------------------------------------------------- config */

static const char *const style_names[WALLPAPER_STYLE_COUNT] = {
    "FILL", "FIT", "STRETCH", "CENTER", "TILE",
};

#define WALLPAPER_PATH_MAX 192
#define DEFAULT_WALLPAPER WALLPAPER_DIRECTORY "/Dusk.png"

static enum wallpaper_style current_style = WALLPAPER_FILL;
static enum taskbar_position current_taskbar = TASKBAR_BOTTOM;
static bool taskbar_autohide;
static const char *const taskbar_names[TASKBAR_POSITION_COUNT] = {
    "BOTTOM", "TOP", "LEFT", "RIGHT",
};
static char wallpaper_path[WALLPAPER_PATH_MAX] = DEFAULT_WALLPAPER;
/* Bumped by every change that alters the rendered background. */
static uint32_t wallpaper_generation = 1;

#define CONFIG_NAME "DESKTOP.CFG"
#define PICTURE_NAME "PICTURE.RAW"
#define MACHINE_CONFIG "/disk/OPOS/" CONFIG_NAME

/* Where the settings of this session live; 0 when they stay in memory. */
static size_t config_path(char *path, size_t capacity, bool for_writing) {
    if (installer_available() || !fat16_system_mounted()) {
        return 0;
    }
    if (accounts_current() != ACCOUNT_NONE) {
        size_t length = accounts_home_path("AppData", path, capacity);
        if (length == 0) {
            return 0;
        }
        length = append(path, capacity, length, "/" CONFIG_NAME);
        return length + 1 < capacity ? length : 0;
    }
    if (for_writing && accounts_count() != 0) {
        return 0; /* the login screen changes nothing */
    }
    size_t length = append(path, capacity, 0, MACHINE_CONFIG);
    return length + 1 < capacity ? length : 0;
}

/* Touchpad gestures, kept with the rest of the account's desktop choices. */
static struct tp_options touchpad_options;

static void config_defaults(void) {
    touchpad_options = (struct tp_options){
        .tap_to_click = true, .natural_scroll = true,
        .speed = TP_SPEED_DEFAULT,
    };
    current_theme = THEME_OCEAN;
    current_style = WALLPAPER_FILL;
    current_taskbar = TASKBAR_BOTTOM;
    taskbar_autohide = false;
    append(wallpaper_path, sizeof(wallpaper_path), 0, DEFAULT_WALLPAPER);
}

static bool parse_name(const char *value, size_t length,
                       const char *const *names, size_t count,
                       size_t *index) {
    for (size_t i = 0; i < count; i++) {
        if (same_ignoring_case(value, length, names[i])) {
            *index = i;
            return true;
        }
    }
    return false;
}

/* Lines of "key=value"; unknown keys are ignored. */
static void config_parse(const char *text, size_t size) {
    size_t line = 0;
    while (line < size) {
        size_t end = line;
        while (end < size && text[end] != '\n') {
            end++;
        }
        size_t stop = end;
        if (stop > line && text[stop - 1] == '\r') {
            stop--;
        }
        size_t equals = line;
        while (equals < stop && text[equals] != '=') {
            equals++;
        }
        if (equals < stop) {
            const char *key = text + line;
            size_t key_length = equals - line;
            const char *value = text + equals + 1;
            size_t value_length = stop - equals - 1;
            size_t index;
            if (same_ignoring_case(key, key_length, "theme")
                && parse_name(value, value_length, theme_names, THEME_COUNT,
                              &index)) {
                current_theme = (enum theme)index;
            } else if (same_ignoring_case(key, key_length, "style")
                       && parse_name(value, value_length, style_names,
                                     WALLPAPER_STYLE_COUNT, &index)) {
                current_style = (enum wallpaper_style)index;
            } else if (same_ignoring_case(key, key_length, "taskbar")
                       && parse_name(value, value_length, taskbar_names,
                                     TASKBAR_POSITION_COUNT, &index)) {
                current_taskbar = (enum taskbar_position)index;
            } else if (same_ignoring_case(key, key_length, "autohide")) {
                taskbar_autohide =
                    same_ignoring_case(value, value_length, "on");
            } else if (same_ignoring_case(key, key_length, "tap")) {
                touchpad_options.tap_to_click =
                    !same_ignoring_case(value, value_length, "off");
            } else if (same_ignoring_case(key, key_length, "scroll")) {
                touchpad_options.natural_scroll =
                    !same_ignoring_case(value, value_length, "traditional");
            } else if (same_ignoring_case(key, key_length, "pointer")
                       && value_length > 0 && value_length <= 2) {
                uint32_t speed = 0;
                for (size_t i = 0; i < value_length; i++) {
                    speed = speed * 10 + (uint32_t)(value[i] - '0');
                    if (value[i] < '0' || value[i] > '9') {
                        speed = 0;
                        break;
                    }
                }
                if (speed >= TP_SPEED_MIN && speed <= TP_SPEED_MAX) {
                    touchpad_options.speed = (uint8_t)speed;
                }
            } else if (same_ignoring_case(key, key_length, "wallpaper")
                       && value_length < WALLPAPER_PATH_MAX) {
                bool printable = true;
                for (size_t i = 0; i < value_length; i++) {
                    printable &= value[i] >= ' ' && value[i] <= '~';
                }
                if (printable && (value_length == 0 || value[0] == '/')) {
                    for (size_t i = 0; i < value_length; i++) {
                        wallpaper_path[i] = value[i];
                    }
                    wallpaper_path[value_length] = '\0';
                }
            }
        }
        line = end + 1;
    }
}

static void picture_cache_clear(void);

void appearance_load(void) {
    config_defaults();
    char path[128];
    size_t length = config_path(path, sizeof(path), false);
    static char text[512];
    size_t size = 0;
    if (length != 0 && vfs_file_size(path, length, &size) && size != 0
        && size <= sizeof(text)
        && vfs_read_file(path, length, (uint8_t *)text, sizeof(text), &size)) {
        config_parse(text, size);
    }
    /* A background another account owns is not shown. */
    size_t wallpaper_length = text_length(wallpaper_path);
    if (wallpaper_length != 0) {
        char canonical[WALLPAPER_PATH_MAX];
        size_t canonical_length = permission_canonicalize(
            wallpaper_path, wallpaper_length, canonical, sizeof(canonical));
        if (canonical_length == 0
            || !permission_ui_allows(canonical, canonical_length,
                                     PERMISSION_READ)) {
            wallpaper_path[0] = '\0';
        }
    }
    wallpaper_generation++;
    picture_cache_clear();
    tp_set_options(&touchpad_options);
}

static bool ensure_directory(const char *path, size_t length) {
    struct vfs_metadata metadata;
    if (vfs_stat(path, length, &metadata)) {
        return metadata.type == VFS_NODE_DIRECTORY;
    }
    return vfs_create_directory(path, length);
}

/* Directory part of a path (up to the last '/'). */
static size_t parent_length(const char *path, size_t length) {
    while (length > 0 && path[length - 1] != '/') {
        length--;
    }
    return length > 1 ? length - 1 : length;
}

static void config_save(void) {
    char path[128];
    size_t length = config_path(path, sizeof(path), true);
    if (length == 0 || !ensure_directory(path, parent_length(path, length))) {
        return;
    }
    char text[384];
    size_t used = append(text, sizeof(text), 0, "theme=");
    used = append(text, sizeof(text), used, theme_names[current_theme]);
    used = append(text, sizeof(text), used, "\nwallpaper=");
    used = append(text, sizeof(text), used, wallpaper_path);
    used = append(text, sizeof(text), used, "\nstyle=");
    used = append(text, sizeof(text), used, style_names[current_style]);
    used = append(text, sizeof(text), used, "\ntaskbar=");
    used = append(text, sizeof(text), used, taskbar_names[current_taskbar]);
    used = append(text, sizeof(text), used, "\nautohide=");
    used = append(text, sizeof(text), used, taskbar_autohide ? "on" : "off");
    used = append(text, sizeof(text), used, "\ntap=");
    used = append(text, sizeof(text), used,
                  touchpad_options.tap_to_click ? "on" : "off");
    used = append(text, sizeof(text), used, "\nscroll=");
    used = append(text, sizeof(text), used,
                  touchpad_options.natural_scroll ? "natural" : "traditional");
    char speed[3] = {(char)('0' + touchpad_options.speed / 10),
                     (char)('0' + touchpad_options.speed % 10), '\0'};
    used = append(text, sizeof(text), used, "\npointer=");
    used = append(text, sizeof(text), used,
                  touchpad_options.speed >= 10 ? speed : speed + 1);
    used = append(text, sizeof(text), used, "\n");
    (void)vfs_write_file(path, length, (const uint8_t *)text, used);
}

void appearance_set_theme(enum theme theme) {
    if (theme >= THEME_COUNT || theme == current_theme) {
        return;
    }
    current_theme = theme;
    wallpaper_generation++;
    config_save();
}

/* ------------------------------------------------------------ wallpaper */

const char *appearance_wallpaper_path(void) {
    return wallpaper_path;
}

enum wallpaper_style appearance_wallpaper_style(void) {
    return current_style;
}

const char *appearance_wallpaper_style_name(enum wallpaper_style style) {
    return style < WALLPAPER_STYLE_COUNT ? style_names[style] : "?";
}

enum taskbar_position appearance_taskbar(void) {
    return current_taskbar;
}

const char *appearance_taskbar_name(enum taskbar_position position) {
    return position < TASKBAR_POSITION_COUNT ? taskbar_names[position] : "?";
}

void appearance_set_taskbar(enum taskbar_position position) {
    if (position >= TASKBAR_POSITION_COUNT || position == current_taskbar) {
        return;
    }
    current_taskbar = position;
    config_save();
}

bool appearance_taskbar_autohide(void) {
    return taskbar_autohide;
}

void appearance_set_taskbar_autohide(bool hide) {
    if (hide == taskbar_autohide) {
        return;
    }
    taskbar_autohide = hide;
    config_save();
}

void appearance_touchpad(struct tp_options *options) {
    *options = touchpad_options;
}

void appearance_set_touchpad(const struct tp_options *options) {
    touchpad_options = *options;
    if (touchpad_options.speed < TP_SPEED_MIN
        || touchpad_options.speed > TP_SPEED_MAX) {
        touchpad_options.speed = TP_SPEED_DEFAULT;
    }
    tp_set_options(&touchpad_options);
    config_save();
}

void appearance_set_wallpaper_style(enum wallpaper_style style) {
    if (style >= WALLPAPER_STYLE_COUNT || style == current_style) {
        return;
    }
    current_style = style;
    wallpaper_generation++;
    config_save();
}

static bool is_bundled(const char *path) {
    static const char prefix[] = WALLPAPER_DIRECTORY "/";
    for (size_t i = 0; i + 1 < sizeof(prefix); i++) {
        if (path[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

void appearance_wallpaper_label(char *text, size_t capacity) {
    if (wallpaper_path[0] == '\0') {
        append(text, capacity, 0, "PLAIN");
        return;
    }
    size_t length = text_length(wallpaper_path);
    size_t start = length;
    while (start > 0 && wallpaper_path[start - 1] != '/') {
        start--;
    }
    size_t end = length;
    if (is_bundled(wallpaper_path)) {
        while (end > start && wallpaper_path[end - 1] != '.') {
            end--;
        }
        end = end > start ? end - 1 : length;
    }
    size_t used = 0;
    for (size_t i = start; i < end && used + 1 < capacity; i++) {
        text[used++] = upper(wallpaper_path[i]);
    }
    text[used] = '\0';
}

enum image_result appearance_set_wallpaper(const char *path, size_t length) {
    if (length >= WALLPAPER_PATH_MAX) {
        return IMAGE_TOO_LARGE;
    }
    if (length != 0) {
        struct image image;
        enum image_result result = appearance_image_load(
            IMAGE_SLOT_WORK, path, length, &image, NULL);
        if (result != IMAGE_OK) {
            return result;
        }
    }
    for (size_t i = 0; i < length; i++) {
        wallpaper_path[i] = path[i];
    }
    wallpaper_path[length] = '\0';
    wallpaper_generation++;
    config_save();
    return IMAGE_OK;
}

#define BUNDLED_MAX 16

/* Bundled pictures in name order. */
static size_t bundled_list(char names[BUNDLED_MAX][64]) {
    static const char directory[] = WALLPAPER_DIRECTORY;
    size_t total = 0;
    size_t count = 0;
    if (!vfs_directory_count(directory, sizeof(directory) - 1, &total)) {
        return 0;
    }
    for (size_t i = 0; i < total && count < BUNDLED_MAX; i++) {
        struct vfs_directory_entry entry;
        if (!vfs_directory_entry_at(directory, sizeof(directory) - 1, i,
                                    &entry)
            || entry.type != VFS_NODE_FILE || entry.name_length >= 64
            || !appearance_is_image_name(entry.name, entry.name_length)) {
            continue;
        }
        size_t at = count++;
        while (at > 0) {
            /* insertion sort */
            const char *a = names[at - 1];
            size_t j = 0;
            while (a[j] != '\0' && a[j] == entry.name[j]) {
                j++;
            }
            if ((uint8_t)a[j] <= (uint8_t)entry.name[j]) {
                break;
            }
            for (size_t k = 0; k < 64; k++) {
                names[at][k] = names[at - 1][k];
            }
            at--;
        }
        for (size_t k = 0; k <= entry.name_length; k++) {
            names[at][k] = k < entry.name_length ? entry.name[k] : '\0';
        }
    }
    return count;
}

/* Choices: 0 = plain, 1..n = bundled, n+1 = the custom picture (if any). */
void appearance_cycle_wallpaper(int delta) {
    static char names[BUNDLED_MAX][64];
    static char custom[WALLPAPER_PATH_MAX];
    size_t bundled = bundled_list(names);
    bool has_custom = wallpaper_path[0] != '\0' && !is_bundled(wallpaper_path);
    if (has_custom) {
        append(custom, sizeof(custom), 0, wallpaper_path);
    }
    size_t choices = 1 + bundled + (has_custom ? 1 : 0);
    size_t current = 0;
    if (has_custom) {
        current = choices - 1;
    } else if (wallpaper_path[0] != '\0') {
        for (size_t i = 0; i < bundled; i++) {
            char path[WALLPAPER_PATH_MAX];
            size_t length = append(path, sizeof(path), 0,
                                   WALLPAPER_DIRECTORY "/");
            append(path, sizeof(path), length, names[i]);
            if (text_same(path, wallpaper_path)) {
                current = i + 1;
            }
        }
    }
    /* Try each choice in turn; skip pictures that fail to load. */
    for (size_t step = 1; step < choices; step++) {
        size_t offset = delta < 0 ? choices - step : step;
        size_t choice = (current + offset) % choices;
        char path[WALLPAPER_PATH_MAX];
        size_t length = 0;
        if (choice == 0) {
            path[0] = '\0';
        } else if (choice <= bundled) {
            length = append(path, sizeof(path), 0, WALLPAPER_DIRECTORY "/");
            length = append(path, sizeof(path), length, names[choice - 1]);
        } else {
            length = append(path, sizeof(path), 0, custom);
        }
        if (appearance_set_wallpaper(path, length) == IMAGE_OK) {
            return;
        }
    }
}

static uint32_t rendered_generation;
static uint32_t rendered_width;
static uint32_t rendered_height;
static uint32_t *rendered_pixels;
static bool rendered_failed;

static uint32_t mapped_color(uint8_t r, uint8_t g, uint8_t b) {
    appearance_map_color(&r, &g, &b);
    return (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

static uint32_t blend_on(uint32_t pixel, uint32_t background) {
    uint32_t alpha = pixel >> 24;
    if (alpha == 255) {
        return pixel & 0x00ffffffu;
    }
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        uint32_t a = (pixel >> shift) & 0xff;
        uint32_t b = (background >> shift) & 0xff;
        result |= ((a * alpha + b * (255 - alpha) + 127) / 255) << shift;
    }
    return result;
}

static bool wallpaper_render(uint32_t width, uint32_t height) {
    size_t length = text_length(wallpaper_path);
    struct image image;
    if (text_same(work_path, wallpaper_path) && work_image.pixels != NULL) {
        image = work_image;
    } else if (appearance_image_load(IMAGE_SLOT_WORK, wallpaper_path, length,
                                     &image, NULL) != IMAGE_OK) {
        return false;
    }
    uint32_t *out = pool_reserve(&wallpaper_pool, (size_t)width * height * 4);
    if (out == NULL) {
        return false;
    }
    uint32_t background = mapped_color(14, 20, 38);
    uint32_t sw = image.width;
    uint32_t sh = image.height;
    switch (current_style) {
    case WALLPAPER_STRETCH:
        image_resample(image.pixels, sw, 0, 0, sw, sh, out, width, width,
                       height);
        break;
    case WALLPAPER_FILL: {
        uint32_t cx = 0, cy = 0, cw = sw, ch = sh;
        if ((uint64_t)sw * height > (uint64_t)sh * width) {
            cw = (uint32_t)((uint64_t)sh * width / height);
            cw = cw == 0 ? 1 : cw;
            cx = (sw - cw) / 2;
        } else {
            ch = (uint32_t)((uint64_t)sw * height / width);
            ch = ch == 0 ? 1 : ch;
            cy = (sh - ch) / 2;
        }
        image_resample(image.pixels, sw, cx, cy, cw, ch, out, width, width,
                       height);
        break;
    }
    case WALLPAPER_FIT: {
        uint32_t dw = width, dh = height;
        if ((uint64_t)sw * height > (uint64_t)sh * width) {
            dh = (uint32_t)((uint64_t)sh * width / sw);
        } else {
            dw = (uint32_t)((uint64_t)sw * height / sh);
        }
        dw = dw == 0 ? 1 : dw;
        dh = dh == 0 ? 1 : dh;
        for (size_t i = 0; i < (size_t)width * height; i++) {
            out[i] = 0xff000000u | background;
        }
        uint32_t x = (width - dw) / 2;
        uint32_t y = (height - dh) / 2;
        image_resample(image.pixels, sw, 0, 0, sw, sh,
                       out + (size_t)y * width + x, width, dw, dh);
        break;
    }
    case WALLPAPER_CENTER:
    case WALLPAPER_TILE: {
        bool tile = current_style == WALLPAPER_TILE;
        int64_t offset_x = tile ? 0 : ((int64_t)width - sw) / 2;
        int64_t offset_y = tile ? 0 : ((int64_t)height - sh) / 2;
        for (uint32_t y = 0; y < height; y++) {
            int64_t source_y = (int64_t)y - offset_y;
            if (tile) {
                source_y %= sh;
            }
            uint32_t *row = out + (size_t)y * width;
            for (uint32_t x = 0; x < width; x++) {
                int64_t source_x = (int64_t)x - offset_x;
                if (tile) {
                    source_x %= sw;
                }
                row[x] = source_x >= 0 && source_x < sw && source_y >= 0
                              && source_y < sh
                    ? image.pixels[(size_t)source_y * sw + (size_t)source_x]
                    : 0xff000000u | background;
            }
        }
        break;
    }
    default:
        return false;
    }
    for (size_t i = 0; i < (size_t)width * height; i++) {
        out[i] = blend_on(out[i], background);
    }
    rendered_pixels = out;
    return true;
}

const uint32_t *appearance_wallpaper_pixels(uint32_t width, uint32_t height) {
    if (wallpaper_path[0] == '\0' || width == 0 || height == 0) {
        return NULL;
    }
    if (rendered_generation != wallpaper_generation || rendered_width != width
        || rendered_height != height) {
        rendered_generation = wallpaper_generation;
        rendered_width = width;
        rendered_height = height;
        rendered_failed = !wallpaper_render(width, height);
    }
    return rendered_failed ? NULL : rendered_pixels;
}

/* -------------------------------------------------------- account pictures */

#define PICTURE_PIXELS (ACCOUNT_PICTURE_SIZE * ACCOUNT_PICTURE_SIZE)
#define PICTURE_FILE_SIZE (8 + PICTURE_PIXELS * 4)

enum picture_state {
    PICTURE_UNKNOWN,
    PICTURE_NONE,
    PICTURE_LOADED,
};

static uint32_t pictures[ACCOUNT_MAX][PICTURE_PIXELS];
static enum picture_state picture_states[ACCOUNT_MAX];
static char picture_owner[ACCOUNT_MAX][ACCOUNT_NAME_MAX + 1];
static uint8_t picture_file[PICTURE_FILE_SIZE];

static void picture_cache_clear(void) {
    for (size_t i = 0; i < ACCOUNT_MAX; i++) {
        picture_states[i] = PICTURE_UNKNOWN;
    }
}

void appearance_pictures_changed(void) {
    picture_cache_clear();
}

static size_t picture_path(const char *name, char *path, size_t capacity) {
    size_t length = append(path, capacity, 0, ACCOUNT_HOME_ROOT "/");
    length = append(path, capacity, length, name);
    length = append(path, capacity, length, "/AppData/" PICTURE_NAME);
    return length + 1 < capacity ? length : 0;
}

const uint32_t *appearance_account_picture(size_t account) {
    if (account >= accounts_count() || account >= ACCOUNT_MAX) {
        return NULL;
    }
    const char *name = accounts_name(account);
    if (picture_states[account] != PICTURE_UNKNOWN
        && text_same(picture_owner[account], name)) {
        return picture_states[account] == PICTURE_LOADED ? pictures[account]
                                                         : NULL;
    }
    append(picture_owner[account], sizeof(picture_owner[account]), 0, name);
    picture_states[account] = PICTURE_NONE;
    if (installer_available() || !fat16_system_mounted()) {
        return NULL;
    }
    char path[128];
    size_t length = picture_path(name, path, sizeof(path));
    size_t size = 0;
    if (length == 0 || !vfs_file_size(path, length, &size)
        || size != PICTURE_FILE_SIZE
        || !vfs_read_file(path, length, picture_file, sizeof(picture_file),
                          &size)
        || size != PICTURE_FILE_SIZE || picture_file[0] != 'O'
        || picture_file[1] != 'P' || picture_file[2] != 'P'
        || picture_file[3] != 'I' || picture_file[4] != ACCOUNT_PICTURE_SIZE
        || picture_file[5] != 0) {
        return NULL;
    }
    for (size_t i = 0; i < PICTURE_PIXELS; i++) {
        const uint8_t *at = picture_file + 8 + i * 4;
        pictures[account][i] = (uint32_t)at[0] | (uint32_t)at[1] << 8
                             | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
    }
    picture_states[account] = PICTURE_LOADED;
    return pictures[account];
}

enum image_result appearance_set_account_picture(const char *path,
                                                 size_t length) {
    size_t account = accounts_current();
    if (account == ACCOUNT_NONE || installer_available()
        || !fat16_system_mounted()) {
        return IMAGE_UNSUPPORTED;
    }
    struct image image;
    enum image_result result = appearance_image_load(IMAGE_SLOT_WORK, path,
                                                     length, &image, NULL);
    if (result != IMAGE_OK) {
        return result;
    }
    uint32_t side = image.width < image.height ? image.width : image.height;
    static uint32_t scaled[PICTURE_PIXELS];
    image_resample(image.pixels, image.width, (image.width - side) / 2,
                   (image.height - side) / 2, side, side, scaled,
                   ACCOUNT_PICTURE_SIZE, ACCOUNT_PICTURE_SIZE,
                   ACCOUNT_PICTURE_SIZE);
    picture_file[0] = 'O';
    picture_file[1] = 'P';
    picture_file[2] = 'P';
    picture_file[3] = 'I';
    picture_file[4] = ACCOUNT_PICTURE_SIZE;
    picture_file[5] = 0;
    picture_file[6] = 1; /* version */
    picture_file[7] = 0;
    for (size_t i = 0; i < PICTURE_PIXELS; i++) {
        uint8_t *at = picture_file + 8 + i * 4;
        at[0] = (uint8_t)scaled[i];
        at[1] = (uint8_t)(scaled[i] >> 8);
        at[2] = (uint8_t)(scaled[i] >> 16);
        at[3] = (uint8_t)(scaled[i] >> 24);
    }
    char file[128];
    size_t file_length = picture_path(accounts_name(account), file,
                                      sizeof(file));
    if (file_length == 0
        || !ensure_directory(file, parent_length(file, file_length))
        || !vfs_write_file(file, file_length, picture_file,
                           sizeof(picture_file))) {
        return IMAGE_SAVE_FAILED;
    }
    picture_states[account] = PICTURE_UNKNOWN;
    return IMAGE_OK;
}

bool appearance_remove_account_picture(void) {
    size_t account = accounts_current();
    if (account == ACCOUNT_NONE || !fat16_system_mounted()) {
        return false;
    }
    char file[128];
    size_t length = picture_path(accounts_name(account), file, sizeof(file));
    bool not_empty = false;
    bool removed = length != 0 && vfs_remove(file, length, false, &not_empty);
    picture_states[account] = PICTURE_UNKNOWN;
    return removed;
}
