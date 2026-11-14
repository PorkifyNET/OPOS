/*
 * pkg: installs and removes OPOS packages (.opk). Ships as /bin/pkg.
 *
 *   pkg available                 what the OPOS Store offers
 *   pkg install ID [options]      install a program from the OPOS Store
 *   pkg update [ID]               update programs from the OPOS Store
 *   pkg info FILE                 what a package is, without installing
 *   pkg install FILE [options]    install a package file for you (or
 *                                 --everyone), from outside the Store
 *   pkg remove ID [options]       remove it
 *   pkg list                      installed packages
 *
 * Options: --everyone (every account; administrators only), -y (do not
 * ask), --ignoreversion (reinstall or downgrade), --delete-data (remove:
 * also delete the program's data in your AppData folder), --source S
 * (the package repository: a URL or a folder; otherwise media with
 * opos-packages/, then /disk/OPOS/PACKAGE-SOURCE.TXT, then GitHub).
 *
 * The kernel checks every package; this program only shows the person
 * what they are installing and asks before passing OPOS_PACKAGE_CONFIRMED.
 * See docs/packages.md.
 */
#include <bearssl_hash.h>
#include <opos.h>
#include <opos_http.h>
#include <opos_tls.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PATH_MAX 256

/*
 * --gui: for the OPOS Store window (kernel/desktop.c). Nothing is asked;
 * the window's buttons are the confirmation. Besides the usual text it
 * prints lines the window reads:
 *   @phase TEXT        what is happening now
 *   @item ID\tVERSION\tNAME\tPUBLISHER\tSTATE\tINSTALLED\tNEEDS\tKB\tASKS\tDESCRIPTION
 *                      one per program (pkg available); STATE is new,
 *                      installed, update or needsopos; NEEDS is the OPOS
 *                      version a newer release needs, or empty; ASKS is
 *                      the index's asks line (capability names or none),
 *                      or empty in an index without one
 *   @progress N        percent of the current install or update (0-100):
 *                      downloading (straight to the cache) 0-75,
 *                      installing 75-100
 *   @updates N         how many installed programs have an update
 *   @done TEXT         what was done
 * Errors are the lines starting with "pkg: ".
 */
static bool gui;

static const char *error_text(long code) {
    switch (-code) {
    case OPOS_PACKAGE_ENOT_INSTALLED:
        return "packages can only be installed on an installed OPOS";
    case OPOS_PACKAGE_ENOT_ALLOWED:
        return "not allowed (installing for everyone needs an "
               "administrator; programs from a disk cannot install)";
    case OPOS_PACKAGE_EBAD:
        return "the package is damaged or breaks a rule";
    case OPOS_PACKAGE_EUNSUPPORTED:
        return "the package needs a newer OPOS";
    case OPOS_PACKAGE_ENEEDS_NEWER:
        return "the package needs a newer OPOS; update OPOS first";
    case OPOS_PACKAGE_EPLATFORM:
        return "OPOS cannot run programs for this platform";
    case OPOS_PACKAGE_ENOT_NEWER:
        return "the same or a newer version is installed "
               "(--ignoreversion reinstalls)";
    case OPOS_PACKAGE_EBUSY:
        return "another program is installing or updating; try again";
    case OPOS_PACKAGE_EDISK:
        return "a disk error stopped it";
    case OPOS_PACKAGE_ENOT_FOUND:
        return "not found";
    case OPOS_PACKAGE_ECONFLICT:
        return "it conflicts with what is installed";
    case OPOS_PACKAGE_ENOT_CONFIRMED:
        return "not confirmed";
    case OPOS_PACKAGE_EINVAL:
        return "invalid argument";
    case OPOS_ENOSYS:
        return "this OPOS cannot install packages";
    default:
        return "failed";
    }
}

static void fail(const char *what, long code,
                 const struct opos_package_info *info) {
    printf("pkg: %s: %s", what, error_text(code));
    if (info != NULL && info->reason[0] != '\0') {
        printf(" (%s)", info->reason);
    }
    printf("\n");
}

/* Makes a relative path absolute against the current folder. */
static bool absolute(const char *path, char *out) {
    if (path[0] == '/') {
        return snprintf(out, PATH_MAX, "%s", path) < PATH_MAX;
    }
    char folder[PATH_MAX];
    if (opos_getcwd(folder, sizeof(folder)) <= 0) {
        return false;
    }
    bool root = strcmp(folder, "/") == 0;
    return snprintf(out, PATH_MAX, "%s%s%s", folder, root ? "" : "/", path)
        < PATH_MAX;
}

static const char *platform_name(uint32_t platform) {
    return platform == OPOS_PACKAGE_PLATFORM_LINUX ? "Linux"
        : platform == OPOS_PACKAGE_PLATFORM_WINDOWS ? "Windows" : "OPOS";
}

static void show(const struct opos_package_info *info) {
    printf("  %s %s (%s)\n", info->name, info->version, info->id);
    printf("  Publisher: %s (as the package says; not checked)\n",
           info->publisher);
    printf("  Platform: %s\n", platform_name(info->platform));
    printf("  %u files, %lu KB, %u Start menu entr%s\n", info->file_count,
           (unsigned long)((info->total_size + 1023) / 1024),
           info->start_count, info->start_count == 1 ? "y" : "ies");
    if (info->requires_opos[0] != '\0') {
        printf("  Needs OPOS %s or newer\n", info->requires_opos);
    }
    if (info->installed == OPOS_PACKAGE_INSTALLED_ACCOUNT) {
        printf("  Installed for you: version %s\n", info->installed_version);
    } else if (info->installed == OPOS_PACKAGE_INSTALLED_EVERYONE) {
        printf("  Installed for everyone: version %s\n",
               info->installed_version);
    }
}

static bool ask(const char *question) {
    printf("%s Type YES to continue, or anything else to cancel: ",
           question);
    char answer[16];
    long length = opos_read(OPOS_STDIN, answer, sizeof(answer));
    size_t used = length > 0 ? (size_t)length : 0;
    while (used != 0
           && (answer[used - 1] == '\n' || answer[used - 1] == '\r')) {
        used--;
    }
    return used == 3 && memcmp(answer, "YES", 3) == 0;
}

static int info_command(const char *file) {
    char path[PATH_MAX];
    struct opos_package_info info;
    memset(&info, 0, sizeof(info));
    if (!absolute(file, path)) {
        printf("pkg: %s: path too long\n", file);
        return 1;
    }
    long result = opos_package_check(path, &info);
    if (info.id[0] != '\0') {
        show(&info);
    }
    if (result < 0) {
        fail(file, result, &info);
        return 1;
    }
    printf("  The package is intact and can be installed.\n");
    return 0;
}

static int install_command(const char *file, bool everyone, bool yes,
                           bool ignore_version) {
    char path[PATH_MAX];
    struct opos_package_info info;
    memset(&info, 0, sizeof(info));
    if (!absolute(file, path)) {
        printf("pkg: %s: path too long\n", file);
        return 1;
    }
    printf("Checking %s...\n", path);
    long result = opos_package_check(path, &info);
    if (info.id[0] != '\0') {
        show(&info);
    }
    if (result < 0) {
        fail(file, result, &info);
        return 1;
    }
    printf("\n\"%s\" (%s) is not from the OPOS Store.\n", info.name, info.id);
    printf("OPOS cannot check who made it. It will run with the limits "
           "every disk program has,\nand asks you before using the network "
           "or reading your files.\n");
    printf("It will be installed for %s.\n",
           everyone ? "every account on this computer" : "you only");
    if (!yes && !ask("Install anyway?")) {
        printf("Cancelled.\n");
        return 1;
    }
    uint32_t options = OPOS_PACKAGE_CONFIRMED
        | (everyone ? OPOS_PACKAGE_EVERYONE : 0)
        | (ignore_version ? OPOS_PACKAGE_IGNORE_VERSION : 0);
    result = opos_package_install(path, options, &info);
    if (result < 0) {
        fail(file, result, &info);
        return 1;
    }
    printf("Installed %s %s.%s\n", info.name, info.version,
           info.start_count != 0 ? " It is in Start > Programs." : "");
    return 0;
}

static int remove_command(const char *id, bool everyone, bool yes,
                          bool delete_data) {
    printf("Removing %s for %s.\n", id,
           everyone ? "every account" : "you only");
    printf("Its permissions are forgotten in your account. Other accounts "
           "keep theirs.\n");
    if (!yes && !ask("Remove it?")) {
        printf("Cancelled.\n");
        return 1;
    }
    if (!yes && !delete_data) {
        delete_data = ask("Also delete its settings and data in your AppData "
                          "folder?");
    }
    printf("Its settings and data in your AppData folder are %s.\n",
           delete_data ? "deleted" : "kept");
    uint32_t options = (everyone ? OPOS_PACKAGE_EVERYONE : 0)
        | (delete_data ? OPOS_PACKAGE_REMOVE_DATA : 0);
    long result = opos_package_remove(id, options);
    if (result < 0) {
        fail(id, result, NULL);
        return 1;
    }
    if (gui) {
        printf("@done REMOVED %s.\n", id);
    }
    printf("Removed %s.\n", id);
    return 0;
}

/* The value of "key: " in a record, into out. */
static void record_value(const char *text, const char *key, char *out,
                         size_t capacity) {
    out[0] = '\0';
    size_t key_length = strlen(key);
    const char *line = text;
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
        if (length > key_length + 1 && strncmp(line, key, key_length) == 0
            && line[key_length] == ':' && line[key_length + 1] == ' ') {
            size_t value = length - key_length - 2;
            if (value >= capacity) {
                value = capacity - 1;
            }
            memcpy(out, line + key_length + 2, value);
            out[value] = '\0';
            return;
        }
        if (end == NULL) {
            return;
        }
        line = end + 1;
    }
}

struct dirent64 {
    uint64_t inode;
    int64_t offset;
    uint16_t length;
    uint8_t type;
    char name[];
};

/* Lists the records in folder; returns how many. */
static int list_folder(const char *folder, const char *scope) {
    long fd = opos_open(folder, OPOS_O_RDONLY | OPOS_O_DIRECTORY);
    if (fd < 0) {
        return 0;
    }
    static char entries[4096];
    static char text[4096 + 1];
    int count = 0;
    for (;;) {
        long got = opos_syscall3(217, fd, (long)entries, sizeof(entries));
        if (got <= 0) {
            break;
        }
        for (long at = 0; at < got;) {
            struct dirent64 *entry = (struct dirent64 *)(entries + at);
            at += entry->length;
            if (entry->name[0] == '.' || strchr(entry->name, '.') == NULL) {
                continue;
            }
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", folder, entry->name);
            long file = opos_open(path, OPOS_O_RDONLY);
            if (file < 0) {
                continue;
            }
            long length = opos_read((int)file, text, 4096);
            opos_close((int)file);
            text[length > 0 ? length : 0] = '\0';
            char name[32];
            char version[16];
            record_value(text, "name", name, sizeof(name));
            record_value(text, "version", version, sizeof(version));
            printf("  %-32s %-10s %-24s %s\n", entry->name, version, name,
                   scope);
            count++;
        }
    }
    opos_close((int)fd);
    return count;
}

static int list_command(void) {
    printf("  %-32s %-10s %-24s %s\n", "ID", "VERSION", "NAME", "FOR");
    int count = 0;
    struct opos_user_info user;
    if (opos_user_info(&user) == 0 && user.home[0] != '\0') {
        char folder[PATH_MAX];
        snprintf(folder, sizeof(folder), "%s/AppData/Packages", user.home);
        count += list_folder(folder, "you");
    }
    count += list_folder("/disk/OPOS/Packages", "everyone");
    if (count == 0) {
        printf("  (no packages installed)\n");
    }
    return 0;
}

/* ------------------------------------------------------- OPOS Store */

/*
 * The package repository (docs/packages.md, "Repository"): a folder or
 * URL holding index, index.sig and the .opk files. This program only
 * fetches them; the kernel checks the index signature and that the
 * package is the one the index lists.
 */
#define STORE_URL                                                          \
    "https://github.com/PorkifyNET/opos-packages/releases/latest/download/"
#define SOURCE_FILE "/disk/OPOS/PACKAGE-SOURCE.TXT"
#define DOWNLOAD_MAX (64u << 20)
#define ENTRIES_MAX 1024

struct entry {
    char id[64];
    char version[16];
    char name[32];
    char publisher[64];
    char platform[16];
    char requires[16];
    char file[96];
    unsigned long size;
    char sha256[65];
    char asks[64];
    char description[160];
};

static struct entry *entries;
static size_t entry_count;
static char *index_text;
static size_t index_size;
static uint8_t index_signature[OPOS_PACKAGE_SIGNATURE_MAX];
static size_t index_signature_size;
static char source[OPOS_HTTP_URL_MAX];

static bool is_url(const char *text) {
    return strncmp(text, "http://", 7) == 0
        || strncmp(text, "https://", 8) == 0;
}

static bool readable(const char *path) {
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        return false;
    }
    opos_close((int)fd);
    return true;
}

/* A CD, USB stick or second disk with opos-packages/index at its top. */
static bool media_source(char *out) {
    for (int kind = 0; kind < 2; kind++) {
        for (int number = 1; number <= 8; number++) {
            char root[16];
            if (kind == 0 && number == 1) {
                continue; /* /disk is OPOS's own volume */
            }
            if (number == 1) {
                snprintf(root, sizeof(root), "/cdrom");
            } else {
                snprintf(root, sizeof(root), "%s%d",
                         kind == 0 ? "/disk" : "/cdrom", number);
            }
            char path[64];
            snprintf(path, sizeof(path), "%s/opos-packages/index", root);
            if (readable(path)) {
                snprintf(out, OPOS_HTTP_URL_MAX, "%s/opos-packages/", root);
                return true;
            }
        }
    }
    return false;
}

/* SOURCE_FILE's first word, if it is a URL or a folder. */
static bool configured_source(char *out) {
    char text[OPOS_HTTP_URL_MAX];
    long fd = opos_open(SOURCE_FILE, OPOS_O_RDONLY);
    if (fd < 0) {
        return false;
    }
    long got = opos_read((int)fd, text, sizeof(text) - 1);
    opos_close((int)fd);
    if (got <= 0) {
        return false;
    }
    text[got] = '\0';
    size_t end = 0;
    while (text[end] != '\0' && text[end] != '\n' && text[end] != '\r'
           && text[end] != ' ') {
        end++;
    }
    text[end] = '\0';
    if (!is_url(text) && text[0] != '/') {
        return false;
    }
    snprintf(out, OPOS_HTTP_URL_MAX, "%s", text);
    return true;
}

/* --source, then media, then SOURCE_FILE, then GitHub. Always ends in '/'. */
static bool pick_source(const char *given) {
    char found[OPOS_HTTP_URL_MAX];
    if (given != NULL) {
        if (!is_url(given) && given[0] != '/') {
            printf("pkg: --source needs a URL or a folder such as "
                   "/cdrom/opos-packages/\n");
            return false;
        }
        snprintf(found, sizeof(found), "%s", given);
    } else if (!media_source(found) && !configured_source(found)) {
        snprintf(found, sizeof(found), "%s", STORE_URL);
    }
    size_t length = strlen(found);
    if (length + 2 > sizeof(source)) {
        printf("pkg: the source is too long\n");
        return false;
    }
    snprintf(source, sizeof(source), "%s%s", found,
             length != 0 && found[length - 1] == '/' ? "" : "/");
    return true;
}

static void show_progress(void *context, size_t received, size_t total) {
    (void)context;
    static size_t shown;
    if (total == 0 || received < shown + (256u << 10)) {
        if (received >= total && total != 0) {
            shown = 0;
        }
        return;
    }
    shown = received;
    printf("  %lu of %lu KB\n", (unsigned long)(received / 1024),
           (unsigned long)(total / 1024));
}

/* --gui: the Store window's progress bar. A download (saved to the
   package cache as it arrives) is 0-75 percent. */
static void gui_progress(void *context, size_t received, size_t total) {
    (void)context;
    static int shown = -1;
    if (total == 0) {
        return;
    }
    int percent = (int)((uint64_t)(received > total ? total : received) * 75
                        / total);
    if (received == 0 || percent < shown) {
        shown = -1; /* a new download */
    }
    if (percent != shown) {
        shown = percent;
        printf("@progress %d\n", percent);
    }
}

/* Reads name from the source into a malloc'd buffer (NUL-terminated). */
static bool source_read(const char *name, size_t max, uint8_t **data,
                        size_t *size) {
    char location[OPOS_HTTP_URL_MAX];
    if (snprintf(location, sizeof(location), "%s%s", source, name)
        >= (int)sizeof(location)) {
        return false;
    }
    if (is_url(location)) {
        struct opos_http_response response;
        opos_http_set_progress(gui && max == DOWNLOAD_MAX ? gui_progress
                               : max > (1u << 20) ? show_progress : NULL,
                               NULL);
        long result = opos_http_get(location, max, &response);
        opos_http_set_progress(NULL, NULL);
        if (result < 0) {
            printf("pkg: %s: %s\n", location, opos_tls_error_text(result));
            return false;
        }
        if (response.status != 200) {
            printf("pkg: %s: the server answered %ld\n", location,
                   response.status);
            opos_http_free(&response);
            return false;
        }
        *data = response.body;
        *size = response.length;
        return true;
    }
    long fd = opos_open(location, OPOS_O_RDONLY);
    if (fd < 0) {
        printf("pkg: %s: not found\n", location);
        return false;
    }
    size_t capacity = 65536;
    size_t used = 0;
    uint8_t *buffer = malloc(capacity + 1);
    for (;;) {
        if (buffer == NULL) {
            opos_close((int)fd);
            printf("pkg: out of memory\n");
            return false;
        }
        if (used == capacity) {
            if (capacity >= max) {
                free(buffer);
                opos_close((int)fd);
                printf("pkg: %s: too large\n", location);
                return false;
            }
            capacity *= 2;
            buffer = realloc(buffer, capacity + 1);
            continue;
        }
        size_t want = capacity - used < 4096 ? capacity - used : 4096;
        long got = opos_read((int)fd, buffer + used, want);
        if (got < 0) {
            free(buffer);
            opos_close((int)fd);
            printf("pkg: %s: could not read it\n", location);
            return false;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
    }
    opos_close((int)fd);
    buffer[used] = '\0';
    *data = buffer;
    *size = used;
    return true;
}

static void copy_value(char *out, size_t capacity, const char *value,
                       size_t length) {
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(out, value, length);
    out[length] = '\0';
}

/* Splits the index into entries. The kernel checks it again; this only
   needs to be good enough to show and choose. */
static void parse_entries(void) {
    entry_count = 0;
    struct entry *current = NULL;
    size_t line = 0;
    while (line < index_size) {
        size_t end = line;
        while (end < index_size && index_text[end] != '\n') {
            end++;
        }
        const char *text = index_text + line;
        size_t length = end - line;
        const char *colon = memchr(text, ':', length);
        if (colon != NULL && (size_t)(colon - text) + 2 <= length
            && colon[1] == ' ') {
            size_t key = (size_t)(colon - text);
            const char *value = colon + 2;
            size_t value_length = length - key - 2;
            if (key == 5 && strncmp(text, "entry", 5) == 0) {
                current = entry_count < ENTRIES_MAX ? &entries[entry_count++]
                                                    : NULL;
                if (current != NULL) {
                    memset(current, 0, sizeof(*current));
                    const char *space = memchr(value, ' ', value_length);
                    size_t id_length = space != NULL
                        ? (size_t)(space - value) : value_length;
                    copy_value(current->id, sizeof(current->id), value,
                               id_length);
                    if (space != NULL) {
                        copy_value(current->version, sizeof(current->version),
                                   space + 1,
                                   value_length - id_length - 1);
                    }
                }
            } else if (current != NULL) {
#define FIELD(name, member)                                                 \
    if (key == sizeof(name) - 1 && strncmp(text, name, key) == 0) {         \
        copy_value(current->member, sizeof(current->member), value,         \
                   value_length);                                           \
    }
                FIELD("name", name)
                FIELD("publisher", publisher)
                FIELD("platform", platform)
                FIELD("requires-opos", requires)
                FIELD("description", description)
                FIELD("asks", asks)
#undef FIELD
                if (key == 4 && strncmp(text, "file", 4) == 0) {
                    char copy[256];
                    copy_value(copy, sizeof(copy), value, value_length);
                    char *size_text = strchr(copy, ' ');
                    char *hash = size_text != NULL
                        ? strchr(size_text + 1, ' ') : NULL;
                    if (hash != NULL) {
                        *size_text++ = '\0';
                        *hash++ = '\0';
                        copy_value(current->file, sizeof(current->file), copy,
                                   strlen(copy));
                        current->size = strtoul(size_text, NULL, 10);
                        copy_value(current->sha256, sizeof(current->sha256),
                                   hash, strlen(hash));
                    }
                }
            }
        }
        line = end + 1;
    }
}

/* Fetches and splits the index and its signature from the source. */
static bool load_index(const char *given) {
    if (!pick_source(given)) {
        return false;
    }
    printf(gui ? "@phase READING THE STORE INDEX\n"
               : "Reading the OPOS Store index from %s\n", source);
    uint8_t *data;
    size_t size;
    if (!source_read("index", OPOS_PACKAGE_INDEX_MAX, &data, &size)) {
        return false;
    }
    uint8_t *signature;
    size_t signature_size;
    if (!source_read("index.sig", OPOS_PACKAGE_SIGNATURE_MAX, &signature,
                     &signature_size)) {
        free(data);
        return false;
    }
    if (signature_size == 0 || signature_size > sizeof(index_signature)) {
        printf("pkg: index.sig is not a signature\n");
        free(data);
        free(signature);
        return false;
    }
    memcpy(index_signature, signature, signature_size);
    index_signature_size = signature_size;
    free(signature);
    index_text = (char *)data;
    index_size = size;
    entries = malloc(sizeof(struct entry) * ENTRIES_MAX);
    if (entries == NULL) {
        printf("pkg: out of memory\n");
        return false;
    }
    parse_entries();
    return true;
}

static int compare_versions(const char *a, const char *b) {
    for (int part = 0; part < 4; part++) {
        unsigned long x = strtoul(a, (char **)&a, 10);
        unsigned long y = strtoul(b, (char **)&b, 10);
        if (x != y) {
            return x < y ? -1 : 1;
        }
        a += *a == '.';
        b += *b == '.';
    }
    return 0;
}

static const char *opos_version(void) {
    /* uname(2): release is the third 65-byte field. */
    static char name[390];
    static bool loaded;
    if (!loaded) {
        if (opos_syscall1(63, (long)name) < 0) {
            strcpy(name + 130, "0");
        }
        loaded = true;
    }
    return name + 130;
}

/* Linux programs run through the Linux layer; Windows ones cannot yet. */
static bool platform_supported(const char *platform) {
    return strcmp(platform, "opos") == 0 || strcmp(platform, "linux") == 0;
}

/*
 * The newest version of id this OPOS can run, or NULL. *newer_needed is
 * the newest entry that needs a newer OPOS (for "Needs OPOS x"), or NULL.
 */
static const struct entry *best_entry(const char *id,
                                      const struct entry **newer_needed) {
    const struct entry *best = NULL;
    const struct entry *blocked = NULL;
    for (size_t i = 0; i < entry_count; i++) {
        const struct entry *e = &entries[i];
        if (strcmp(e->id, id) != 0 || !platform_supported(e->platform)) {
            continue;
        }
        if (e->requires[0] != '\0'
            && compare_versions(e->requires, opos_version()) > 0) {
            if (blocked == NULL
                || compare_versions(e->version, blocked->version) > 0) {
                blocked = e;
            }
        } else if (best == NULL
                   || compare_versions(e->version, best->version) > 0) {
            best = e;
        }
    }
    if (newer_needed != NULL) {
        *newer_needed = blocked != NULL
            && (best == NULL
                || compare_versions(blocked->version, best->version) > 0)
            ? blocked : NULL;
    }
    return best;
}

/* What is installed for id, as this account sees it. */
struct installed {
    bool found;
    bool everyone;
    bool store;
    char version[16];
};

static bool read_record(const char *path, struct installed *out) {
    static char text[4096 + 1];
    long fd = opos_open(path, OPOS_O_RDONLY);
    if (fd < 0) {
        return false;
    }
    long got = opos_read((int)fd, text, 4096);
    opos_close((int)fd);
    text[got > 0 ? got : 0] = '\0';
    out->found = true;
    out->store = strncmp(text, "origin: store\n", 14) == 0;
    record_value(text, "version", out->version, sizeof(out->version));
    return true;
}

static struct installed installed_copy(const char *id) {
    struct installed result;
    memset(&result, 0, sizeof(result));
    struct opos_user_info user;
    char path[PATH_MAX];
    if (opos_user_info(&user) == 0 && user.home[0] != '\0') {
        snprintf(path, sizeof(path), "%s/AppData/Packages/%s", user.home, id);
        if (read_record(path, &result)) {
            return result;
        }
    }
    snprintf(path, sizeof(path), "/disk/OPOS/Packages/%s", id);
    if (read_record(path, &result)) {
        result.everyone = true;
    }
    return result;
}

static int available_command(const char *given) {
    if (!load_index(given)) {
        return 1;
    }
    if (!gui) {
        printf("  %-32s %-10s %-24s %s\n", "ID", "VERSION", "NAME", "STATE");
    }
    int shown = 0;
    int updates = 0;
    for (size_t i = 0; i < entry_count; i++) {
        const struct entry *e = &entries[i];
        bool first = true;
        for (size_t j = 0; j < i && first; j++) {
            first = strcmp(entries[j].id, e->id) != 0;
        }
        if (!first) {
            continue;
        }
        const struct entry *blocked;
        const struct entry *best = best_entry(e->id, &blocked);
        if (best == NULL && blocked == NULL) {
            continue; /* a platform this OPOS cannot run */
        }
        const struct entry *show_entry = best != NULL ? best : blocked;
        struct installed copy = installed_copy(e->id);
        char state[64] = "";
        if (copy.found && best != NULL
            && compare_versions(best->version, copy.version) > 0) {
            snprintf(state, sizeof(state), "update from %s", copy.version);
        } else if (copy.found) {
            snprintf(state, sizeof(state), "installed");
        }
        if (blocked != NULL) {
            snprintf(state + strlen(state), sizeof(state) - strlen(state),
                     "%sneeds OPOS %s for %s", state[0] != '\0' ? "; " : "",
                     blocked->requires, blocked->version);
        }
        if (gui) {
            bool update = copy.found && best != NULL
                && compare_versions(best->version, copy.version) > 0;
            updates += update;
            printf("@item %s\t%s\t%s\t%s\t%s\t%s\t%s\t%lu\t%s\t%s\n",
                   show_entry->id, show_entry->version, show_entry->name,
                   show_entry->publisher,
                   update ? "update" : copy.found ? "installed"
                   : best == NULL ? "needsopos" : "new",
                   copy.found ? copy.version : "",
                   blocked != NULL ? blocked->requires : "",
                   (show_entry->size + 1023) / 1024, show_entry->asks,
                   show_entry->description);
        } else {
            printf("  %-32s %-10s %-24s %s\n", show_entry->id,
                   show_entry->version, show_entry->name, state);
            if (show_entry->description[0] != '\0') {
                printf("      %s\n", show_entry->description);
            }
        }
        shown++;
    }
    if (gui) {
        printf("@updates %d\n", updates);
    } else if (shown == 0) {
        printf("  (nothing this OPOS can run)\n");
    }
    return 0;
}

/* Downloads e's package to a cache file and checks its size and SHA-256
   against the index, so a broken download is reported before the kernel
   reads it. */
/* opos_http_download's sink: saves and hashes each piece as it arrives. */
struct download_file {
    long fd;
    br_sha256_context hash;
    size_t size;
    bool write_failed;
};

static long download_write(void *context, const uint8_t *data,
                           size_t length) {
    struct download_file *file = context;
    br_sha256_update(&file->hash, data, length);
    file->size += length;
    size_t written = 0;
    while (written < length) {
        size_t want = length - written < 4096 ? length - written : 4096;
        long put = opos_write((int)file->fd, data + written, want);
        if (put <= 0) {
            file->write_failed = true;
            return -OPOS_EIO;
        }
        written += (size_t)put;
    }
    return 0;
}

static bool download_package(const struct entry *e, char *path) {
    struct opos_user_info user;
    char folder[PATH_MAX];
    if (opos_user_info(&user) == 0 && user.home[0] != '\0') {
        snprintf(folder, sizeof(folder), "%s/AppData/PackageCache",
                 user.home);
    } else {
        snprintf(folder, sizeof(folder), "/disk/PackageCache");
    }
    (void)opos_mkdir(folder);
    snprintf(path, PATH_MAX, "%s/%s", folder, e->file);
    if (gui) {
        printf("@phase DOWNLOADING %s (%lu KB)\n", e->name,
               (e->size + 1023) / 1024);
        printf("@progress 0\n");
    } else {
        printf("Downloading %s (%lu KB)...\n", e->file,
               (e->size + 1023) / 1024);
    }
    char location[OPOS_HTTP_URL_MAX];
    if (snprintf(location, sizeof(location), "%s%s", source, e->file)
        >= (int)sizeof(location)) {
        printf("pkg: the address is too long\n");
        return false;
    }
    struct download_file file = {
        opos_open(path, OPOS_O_WRONLY | OPOS_O_CREAT | OPOS_O_TRUNC),
        {0}, 0, false,
    };
    if (file.fd < 0) {
        printf("pkg: %s: could not save the download\n", path);
        return false;
    }
    br_sha256_init(&file.hash);
    /* Streamed to the cache file: a package can be larger than this
       program's heap (Freedoom is over 32 MB). */
    struct opos_http_response response;
    opos_http_set_progress(gui ? gui_progress : show_progress, NULL);
    long result = opos_http_download(location, DOWNLOAD_MAX, download_write,
                                     &file, &response);
    opos_http_set_progress(NULL, NULL);
    opos_close((int)file.fd);
    bool ok = false;
    if (file.write_failed) {
        printf("pkg: %s: could not save the download\n", path);
    } else if (result < 0) {
        printf("pkg: %s: %s\n", location, opos_tls_error_text(result));
    } else if (response.status < 200 || response.status >= 300) {
        printf("pkg: %s: the server answered %ld\n", location,
               response.status);
    } else {
        uint8_t digest[32];
        br_sha256_out(&file.hash, digest);
        char hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(hex + i * 2, 3, "%02x", digest[i]);
        }
        ok = file.size == e->size && strcmp(hex, e->sha256) == 0;
        if (!ok) {
            printf("pkg: %s: the download differs from the index\n",
                   e->file);
        }
    }
    if (result == 0) {
        opos_http_free(&response);
    }
    if (!ok) {
        (void)opos_unlink(path);
    }
    return ok;
}

/* Installs (or updates to) entry e from the Store. */
static int store_install(const struct entry *e, bool everyone,
                         bool ignore_version) {
    char path[PATH_MAX];
    bool downloaded = is_url(source);
    if (downloaded) {
        if (!download_package(e, path)) {
            return 1;
        }
    } else if (snprintf(path, sizeof(path), "%s%s", source, e->file)
               >= (int)sizeof(path)) {
        printf("pkg: the path is too long\n");
        return 1;
    }
    struct opos_package_index index = {index_text, index_size,
                                       index_signature, index_signature_size};
    struct opos_package_info info;
    memset(&info, 0, sizeof(info));
    uint32_t options = (everyone ? OPOS_PACKAGE_EVERYONE : 0)
        | (ignore_version ? OPOS_PACKAGE_IGNORE_VERSION : 0);
    if (gui) {
        printf("@phase INSTALLING %s %s\n", e->name, e->version);
        printf("@progress 75\n");
    }
    long result = opos_package_install_store(path, options, &info, &index);
    if (downloaded) {
        (void)opos_unlink(path);
    }
    if (result < 0) {
        fail(e->id, result, &info);
        return 1;
    }
    if (gui) {
        printf("@progress 100\n");
        printf("@done INSTALLED %s %s.\n", info.name, info.version);
    }
    printf("Installed %s %s from the OPOS Store.%s\n", info.name,
           info.version,
           info.start_count != 0 ? " It is in Start > Programs." : "");
    return 0;
}

static int store_install_command(const char *id, bool everyone, bool yes,
                                 bool ignore_version, const char *given) {
    if (!load_index(given)) {
        return 1;
    }
    const struct entry *blocked;
    const struct entry *e = best_entry(id, &blocked);
    if (e == NULL) {
        if (blocked != NULL) {
            printf("pkg: %s %s needs OPOS %s or newer (this is %s). Update "
                   "OPOS first: Settings > UPDATES.\n", id, blocked->version,
                   blocked->requires, opos_version());
        } else {
            printf("pkg: %s is not in the OPOS Store, or OPOS cannot run "
                   "it\n", id);
        }
        return 1;
    }
    printf("  %s %s (%s)\n", e->name, e->version, e->id);
    printf("  Publisher: %s\n", e->publisher);
    if (e->description[0] != '\0') {
        printf("  %s\n", e->description);
    }
    printf("  %lu KB, from the OPOS Store\n", (e->size + 1023) / 1024);
    struct installed copy = installed_copy(id);
    if (copy.found && !ignore_version
        && compare_versions(e->version, copy.version) <= 0) {
        printf("%s %s is installed and up to date.\n", e->name, copy.version);
        return 0;
    }
    printf("It will be installed for %s.\n",
           everyone ? "every account on this computer" : "you only");
    if (!yes && !ask(copy.found ? "Update it?" : "Install it?")) {
        printf("Cancelled.\n");
        return 1;
    }
    return store_install(e, everyone, ignore_version);
}

/* Updates the Store packages this account can update: its own, and
   everyone's for an administrator. */
static int update_command(const char *only, bool yes, const char *given) {
    if (!load_index(given)) {
        return 1;
    }
    struct opos_user_info user;
    bool admin = opos_user_info(&user) == 0 && (user.flags & OPOS_USER_ADMIN);
    const struct entry *updates[ENTRIES_MAX];
    bool for_everyone[ENTRIES_MAX];
    size_t count = 0;
    for (size_t i = 0; i < entry_count; i++) {
        const struct entry *e = &entries[i];
        bool first = true;
        for (size_t j = 0; j < i && first; j++) {
            first = strcmp(entries[j].id, e->id) != 0;
        }
        if (!first || (only != NULL && strcmp(only, e->id) != 0)) {
            continue;
        }
        struct installed copy = installed_copy(e->id);
        const struct entry *best = best_entry(e->id, NULL);
        if (!copy.found || !copy.store || best == NULL
            || compare_versions(best->version, copy.version) <= 0
            || (copy.everyone && !admin)) {
            continue;
        }
        printf("  %-32s %s -> %s%s\n", best->id, copy.version, best->version,
               copy.everyone ? " (every account)" : "");
        updates[count] = best;
        for_everyone[count++] = copy.everyone;
    }
    if (count == 0) {
        if (gui) {
            printf("@done EVERY PROGRAM IS UP TO DATE.\n");
        }
        printf("Every program from the OPOS Store is up to date.\n");
        return 0;
    }
    if (!yes && !ask(count == 1 ? "Update it?" : "Update them?")) {
        printf("Cancelled.\n");
        return 1;
    }
    int failures = 0;
    for (size_t i = 0; i < count; i++) {
        failures += store_install(updates[i], for_everyone[i], false);
    }
    return failures != 0;
}

static int usage(void) {
    printf("Usage: pkg available [--source S]\n"
           "       pkg install ID [--everyone] [-y] [--source S]\n"
           "       pkg update [ID] [-y] [--source S]\n"
           "       pkg install FILE.opk [--everyone] [-y] "
           "[--ignoreversion]\n"
           "       pkg info FILE.opk\n"
           "       pkg remove ID [--everyone] [-y] [--delete-data]\n"
           "       pkg list\n"
           "S is a URL or a folder holding index, index.sig and the "
           "packages.\n");
    return 2;
}

/* A file (installed from outside the Store) rather than a Store id. */
static bool names_file(const char *operand) {
    size_t length = strlen(operand);
    return strchr(operand, '/') != NULL
        || (length > 4 && strcmp(operand + length - 4, ".opk") == 0);
}

int main(int argc, char **argv) {
    /* "pkg --gui COMMAND ...": the flag may come first. */
    if (argc >= 3 && strcmp(argv[1], "--gui") == 0) {
        gui = true;
        argv[1] = argv[2];
        argv[2] = "--gui";
    }
    if (argc < 2) {
        return usage();
    }
    bool everyone = false;
    bool yes = false;
    bool ignore_version = false;
    bool delete_data = false;
    const char *given_source = NULL;
    const char *operand = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--everyone") == 0) {
            everyone = true;
        } else if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--y") == 0) {
            yes = true;
        } else if (strcmp(argv[i], "--ignoreversion") == 0) {
            ignore_version = true;
        } else if (strcmp(argv[i], "--delete-data") == 0) {
            delete_data = true;
        } else if (strcmp(argv[i], "--gui") == 0) {
            gui = true;
            yes = true;
        } else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
            given_source = argv[++i];
        } else if (operand == NULL && argv[i][0] != '-') {
            operand = argv[i];
        } else {
            return usage();
        }
    }
    const char *command = argv[1];
    if (strcmp(command, "list") == 0 && operand == NULL) {
        return list_command();
    }
    if ((strcmp(command, "available") == 0 || strcmp(command, "search") == 0)
        && operand == NULL) {
        return available_command(given_source);
    }
    if (strcmp(command, "update") == 0) {
        return update_command(operand, yes, given_source);
    }
    if (operand == NULL) {
        return usage();
    }
    if (strcmp(command, "info") == 0) {
        return info_command(operand);
    }
    if (strcmp(command, "install") == 0) {
        return names_file(operand)
            ? install_command(operand, everyone, yes, ignore_version)
            : store_install_command(operand, everyone, yes, ignore_version,
                                    given_source);
    }
    if (strcmp(command, "remove") == 0) {
        /* The window removes whichever copy this account sees. */
        if (gui && !everyone) {
            struct installed copy = installed_copy(operand);
            everyone = copy.found && copy.everyone;
        }
        return remove_command(operand, everyone, yes, delete_data);
    }
    return usage();
}
