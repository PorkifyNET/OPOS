/*
 * pkg: installs and removes OPOS packages (.opk). Ships as /bin/pkg.
 *
 *   pkg info FILE                 what a package is, without installing
 *   pkg install FILE [options]    install it for you (or --everyone)
 *   pkg remove ID [options]       remove it
 *   pkg list                      installed packages
 *
 * Options: --everyone (every account; administrators only), -y (do not
 * ask), --ignoreversion (reinstall or downgrade), --delete-data (remove:
 * also delete the program's data in your AppData folder).
 *
 * The kernel checks every package; this program only shows the person
 * what they are installing and asks before passing OPOS_PACKAGE_CONFIRMED.
 * See docs/packages.md.
 */
#include <opos.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define PATH_MAX 256

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

static int usage(void) {
    printf("Usage: pkg info FILE\n"
           "       pkg install FILE [--everyone] [-y] [--ignoreversion]\n"
           "       pkg remove ID [--everyone] [-y] [--delete-data]\n"
           "       pkg list\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return usage();
    }
    bool everyone = false;
    bool yes = false;
    bool ignore_version = false;
    bool delete_data = false;
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
        } else if (operand == NULL && argv[i][0] != '-') {
            operand = argv[i];
        } else {
            return usage();
        }
    }
    if (strcmp(argv[1], "list") == 0 && operand == NULL) {
        return list_command();
    }
    if (operand == NULL) {
        return usage();
    }
    if (strcmp(argv[1], "info") == 0) {
        return info_command(operand);
    }
    if (strcmp(argv[1], "install") == 0) {
        return install_command(operand, everyone, yes, ignore_version);
    }
    if (strcmp(argv[1], "remove") == 0) {
        return remove_command(operand, everyone, yes, delete_data);
    }
    return usage();
}
