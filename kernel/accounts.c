#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "accounts.h"
#include "fat16.h"
#include "sha256.h"
#include "vfs.h"

/*
 * ACCOUNTS.DAT layout: magic "OPAC", version, account count, two reserved
 * bytes, then ACCOUNT_MAX records of RECORD_SIZE bytes:
 *   0  name, NUL-padded (ACCOUNT_NAME_MAX + 4 bytes)
 *   24 flags (ACCOUNT_FLAG_*)
 *   25 personal settings bits
 *   26 two reserved bytes
 *   28 salt (16 bytes)
 *   44 password hash (32 bytes)
 */
#define ACCOUNTS_DIRECTORY "/disk/OPOS"
#define ACCOUNTS_FILE "/disk/OPOS/ACCOUNTS.DAT"
#define ACCOUNTS_VERSION 1
#define HEADER_SIZE 8
#define NAME_FIELD (ACCOUNT_NAME_MAX + 4)
#define SALT_SIZE 16
#define RECORD_SIZE (NAME_FIELD + 4 + SALT_SIZE + SHA256_DIGEST_SIZE)
#define FILE_SIZE (HEADER_SIZE + ACCOUNT_MAX * RECORD_SIZE)
#define HASH_ROUNDS 20000

#define ACCOUNT_FLAG_ADMIN 0x01
#define ACCOUNT_FLAG_PASSWORD 0x02
#define ACCOUNT_FLAG_PERSONAL_SET 0x04

struct account {
    char name[ACCOUNT_NAME_MAX + 1];
    uint8_t flags;
    uint8_t personal;
    uint8_t salt[SALT_SIZE];
    uint8_t hash[SHA256_DIGEST_SIZE];
};

static struct account accounts[ACCOUNT_MAX];
static size_t account_count;
static size_t current_account = ACCOUNT_NONE;
static uint8_t file_buffer[FILE_SIZE];

static const char *const home_folders[] = {
    "Documents", "Pictures", "Music", "Videos", "Downloads",
};

/* ------------------------------------------------------------ helpers */

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static char upper(char character) {
    return character >= 'a' && character <= 'z'
        ? (char)(character - 32) : character;
}

static bool names_equal(const char *a, const char *b) {
    size_t i = 0;
    for (; a[i] != '\0' && b[i] != '\0'; i++) {
        if (upper(a[i]) != upper(b[i])) {
            return false;
        }
    }
    return a[i] == b[i];
}

static size_t append(char *buffer, size_t capacity, size_t offset,
                     const char *text) {
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    if (offset < capacity) {
        buffer[offset] = '\0';
    }
    return *text == '\0' ? offset : capacity;
}

static size_t home_of(const char *name, const char *sub, char *path,
                      size_t capacity) {
    size_t length = append(path, capacity, 0, ACCOUNT_HOME_ROOT "/");
    length = append(path, capacity, length, name);
    if (sub != NULL && length < capacity) {
        length = append(path, capacity, length, "/");
        length = append(path, capacity, length, sub);
    }
    return length < capacity ? length : 0;
}

static bool ensure_directory(const char *path) {
    struct vfs_metadata metadata;
    size_t length = text_length(path);
    if (vfs_stat(path, length, &metadata)) {
        return metadata.type == VFS_NODE_DIRECTORY;
    }
    return vfs_create_directory(path, length);
}

/* Salt bytes: RDRAND when the CPU has it, mixed with the TSC. */
static void random_bytes(uint8_t *buffer, size_t length) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1), "c"(0));
    bool has_rdrand = (ecx & (1u << 30)) != 0;
    struct sha256_context context;
    uint8_t digest[SHA256_DIGEST_SIZE];
    static uint64_t counter;
    for (size_t done = 0; done < length;) {
        uint32_t low, high;
        __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
        uint64_t seed[3] = {((uint64_t)high << 32) | low, ++counter, 0};
        if (has_rdrand) {
            for (int tries = 0; tries < 10; tries++) {
                uint8_t ok;
                __asm__ volatile ("rdrand %0; setc %1"
                                  : "=r"(seed[2]), "=qm"(ok));
                if (ok) {
                    break;
                }
            }
        }
        sha256_init(&context);
        sha256_update(&context, seed, sizeof(seed));
        sha256_final(&context, digest);
        for (size_t i = 0; i < SHA256_DIGEST_SIZE && done < length; i++) {
            buffer[done++] = digest[i];
        }
    }
}

static void hash_password(const uint8_t salt[SALT_SIZE], const char *password,
                          uint8_t hash[SHA256_DIGEST_SIZE]) {
    struct sha256_context context;
    sha256_init(&context);
    sha256_update(&context, salt, SALT_SIZE);
    sha256_update(&context, password, text_length(password));
    sha256_final(&context, hash);
    for (int round = 1; round < HASH_ROUNDS; round++) {
        sha256_init(&context);
        sha256_update(&context, hash, SHA256_DIGEST_SIZE);
        sha256_update(&context, salt, SALT_SIZE);
        sha256_final(&context, hash);
    }
}

static void set_password(struct account *account, const char *password) {
    if (password == NULL || password[0] == '\0') {
        account->flags = (uint8_t)(account->flags & ~ACCOUNT_FLAG_PASSWORD);
        for (size_t i = 0; i < SALT_SIZE; i++) {
            account->salt[i] = 0;
        }
        for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
            account->hash[i] = 0;
        }
        return;
    }
    account->flags |= ACCOUNT_FLAG_PASSWORD;
    random_bytes(account->salt, SALT_SIZE);
    hash_password(account->salt, password, account->hash);
}

/* -------------------------------------------------------------- storage */

void accounts_load(void) {
    account_count = 0;
    current_account = ACCOUNT_NONE;
    size_t size = 0;
    if (!fat16_system_mounted()
        || !vfs_read_file(ACCOUNTS_FILE, text_length(ACCOUNTS_FILE),
                          file_buffer, sizeof(file_buffer), &size)
        || size != FILE_SIZE || file_buffer[0] != 'O' || file_buffer[1] != 'P'
        || file_buffer[2] != 'A' || file_buffer[3] != 'C'
        || file_buffer[4] != ACCOUNTS_VERSION
        || file_buffer[5] > ACCOUNT_MAX) {
        return;
    }
    size_t count = file_buffer[5];
    for (size_t i = 0; i < count; i++) {
        const uint8_t *record = file_buffer + HEADER_SIZE + i * RECORD_SIZE;
        struct account *account = &accounts[account_count];
        for (size_t j = 0; j <= ACCOUNT_NAME_MAX; j++) {
            account->name[j] = j < ACCOUNT_NAME_MAX ? (char)record[j] : '\0';
        }
        if (!accounts_name_valid(account->name)) {
            continue;
        }
        account->flags = record[NAME_FIELD];
        account->personal = record[NAME_FIELD + 1];
        for (size_t j = 0; j < SALT_SIZE; j++) {
            account->salt[j] = record[NAME_FIELD + 4 + j];
        }
        for (size_t j = 0; j < SHA256_DIGEST_SIZE; j++) {
            account->hash[j] = record[NAME_FIELD + 4 + SALT_SIZE + j];
        }
        account_count++;
    }
}

static bool accounts_save(void) {
    for (size_t i = 0; i < sizeof(file_buffer); i++) {
        file_buffer[i] = 0;
    }
    file_buffer[0] = 'O';
    file_buffer[1] = 'P';
    file_buffer[2] = 'A';
    file_buffer[3] = 'C';
    file_buffer[4] = ACCOUNTS_VERSION;
    file_buffer[5] = (uint8_t)account_count;
    for (size_t i = 0; i < account_count; i++) {
        uint8_t *record = file_buffer + HEADER_SIZE + i * RECORD_SIZE;
        const struct account *account = &accounts[i];
        for (size_t j = 0; j < ACCOUNT_NAME_MAX && account->name[j] != '\0';
             j++) {
            record[j] = (uint8_t)account->name[j];
        }
        record[NAME_FIELD] = account->flags;
        record[NAME_FIELD + 1] = account->personal;
        for (size_t j = 0; j < SALT_SIZE; j++) {
            record[NAME_FIELD + 4 + j] = account->salt[j];
        }
        for (size_t j = 0; j < SHA256_DIGEST_SIZE; j++) {
            record[NAME_FIELD + 4 + SALT_SIZE + j] = account->hash[j];
        }
    }
    return fat16_system_mounted() && ensure_directory(ACCOUNTS_DIRECTORY)
        && vfs_write_file(ACCOUNTS_FILE, text_length(ACCOUNTS_FILE),
                          file_buffer, sizeof(file_buffer));
}

static bool create_home(const char *name) {
    char path[96];
    if (!ensure_directory(ACCOUNT_HOME_ROOT)
        || home_of(name, NULL, path, sizeof(path)) == 0
        || !ensure_directory(path)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(home_folders) / sizeof(home_folders[0]);
         i++) {
        if (home_of(name, home_folders[i], path, sizeof(path)) == 0
            || !ensure_directory(path)) {
            return false;
        }
    }
    return true;
}

/* -------------------------------------------------------------- queries */

size_t accounts_count(void) {
    return account_count;
}

const char *accounts_name(size_t index) {
    return index < account_count ? accounts[index].name : "";
}

bool accounts_has_password(size_t index) {
    return index < account_count
        && (accounts[index].flags & ACCOUNT_FLAG_PASSWORD) != 0;
}

bool accounts_is_admin(size_t index) {
    return index < account_count
        && (accounts[index].flags & ACCOUNT_FLAG_ADMIN) != 0;
}

size_t accounts_find(const char *name) {
    for (size_t i = 0; i < account_count; i++) {
        if (names_equal(accounts[i].name, name)) {
            return i;
        }
    }
    return ACCOUNT_NONE;
}

bool accounts_login_required(void) {
    return account_count > 1
        || (account_count == 1 && accounts_has_password(0));
}

bool accounts_login(size_t index, const char *password) {
    if (index >= account_count) {
        return false;
    }
    if (accounts_has_password(index)) {
        uint8_t hash[SHA256_DIGEST_SIZE];
        hash_password(accounts[index].salt, password != NULL ? password : "",
                      hash);
        uint8_t difference = 0;
        for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
            difference |= (uint8_t)(hash[i] ^ accounts[index].hash[i]);
        }
        if (difference != 0) {
            return false;
        }
    }
    current_account = index;
    /* Recreates missing home folders (deleted, or an older install). */
    (void)create_home(accounts[index].name);
    return true;
}

void accounts_logout(void) {
    current_account = ACCOUNT_NONE;
}

size_t accounts_current(void) {
    return current_account;
}

size_t accounts_home_path(const char *sub, char *path, size_t capacity) {
    if (current_account >= account_count) {
        return 0;
    }
    return home_of(accounts[current_account].name, sub, path, capacity);
}

/* 1-20 characters: letters, digits, space, '-', '_' and '.', not starting
   or ending with a space or dot. */
bool accounts_name_valid(const char *name) {
    size_t length = text_length(name);
    if (length == 0 || length > ACCOUNT_NAME_MAX || name[0] == ' '
        || name[0] == '.' || name[length - 1] == ' '
        || name[length - 1] == '.') {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_'
            || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

const char *accounts_result_text(enum account_result result) {
    switch (result) {
    case ACCOUNT_OK:
        return "DONE";
    case ACCOUNT_BAD_NAME:
        return "NAME: 1-20 LETTERS, DIGITS, SPACE - _ .";
    case ACCOUNT_NAME_TAKEN:
        return "THAT NAME IS ALREADY USED";
    case ACCOUNT_FULL:
        return "NO ROOM FOR MORE ACCOUNTS (MAX 8)";
    case ACCOUNT_NOT_FOUND:
        return "ACCOUNT NOT FOUND";
    case ACCOUNT_LAST_ADMIN:
        return "AT LEAST ONE ADMINISTRATOR IS NEEDED";
    default:
        return "THE DISK COULD NOT BE WRITTEN";
    }
}

/* ------------------------------------------------------------- changes */

enum account_result accounts_create(const char *name, const char *password,
                                    bool admin) {
    if (!accounts_name_valid(name)) {
        return ACCOUNT_BAD_NAME;
    }
    if (accounts_find(name) != ACCOUNT_NONE) {
        return ACCOUNT_NAME_TAKEN;
    }
    if (account_count >= ACCOUNT_MAX) {
        return ACCOUNT_FULL;
    }
    if (!fat16_system_mounted()) {
        return ACCOUNT_DISK_ERROR;
    }
    struct account *account = &accounts[account_count];
    *account = (struct account){0};
    for (size_t i = 0; name[i] != '\0'; i++) {
        account->name[i] = name[i];
    }
    /* The first account is always an administrator. */
    account->flags = admin || account_count == 0 ? ACCOUNT_FLAG_ADMIN : 0;
    set_password(account, password);
    account_count++;
    if (!create_home(name) || !accounts_save()) {
        account_count--;
        return ACCOUNT_DISK_ERROR;
    }
    return ACCOUNT_OK;
}

enum account_result accounts_set_password(size_t index, const char *password) {
    if (index >= account_count) {
        return ACCOUNT_NOT_FOUND;
    }
    struct account saved = accounts[index];
    set_password(&accounts[index], password);
    if (!accounts_save()) {
        accounts[index] = saved;
        return ACCOUNT_DISK_ERROR;
    }
    return ACCOUNT_OK;
}

enum account_result accounts_rename(size_t index, const char *name) {
    if (index >= account_count) {
        return ACCOUNT_NOT_FOUND;
    }
    if (!accounts_name_valid(name)) {
        return ACCOUNT_BAD_NAME;
    }
    size_t existing = accounts_find(name);
    if (existing != ACCOUNT_NONE && existing != index) {
        return ACCOUNT_NAME_TAKEN;
    }
    char old_path[96];
    char new_path[96];
    size_t old_length = home_of(accounts[index].name, NULL, old_path,
                                sizeof(old_path));
    size_t new_length = home_of(name, NULL, new_path, sizeof(new_path));
    struct vfs_metadata metadata;
    if (old_length != 0 && new_length != 0
        && vfs_stat(old_path, old_length, &metadata)
        && vfs_rename(old_path, old_length, new_path, new_length)
               != VFS_RENAME_OK) {
        return ACCOUNT_DISK_ERROR;
    }
    struct account saved = accounts[index];
    for (size_t i = 0; i <= ACCOUNT_NAME_MAX; i++) {
        accounts[index].name[i] = '\0';
    }
    for (size_t i = 0; name[i] != '\0'; i++) {
        accounts[index].name[i] = name[i];
    }
    if (!accounts_save()) {
        accounts[index] = saved;
        (void)vfs_rename(new_path, new_length, old_path, old_length);
        return ACCOUNT_DISK_ERROR;
    }
    return ACCOUNT_OK;
}

static size_t admin_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < account_count; i++) {
        count += accounts_is_admin(i) ? 1 : 0;
    }
    return count;
}

enum account_result accounts_set_admin(size_t index, bool admin) {
    if (index >= account_count) {
        return ACCOUNT_NOT_FOUND;
    }
    if (!admin && accounts_is_admin(index) && admin_count() == 1) {
        return ACCOUNT_LAST_ADMIN;
    }
    uint8_t saved = accounts[index].flags;
    accounts[index].flags = admin
        ? (uint8_t)(saved | ACCOUNT_FLAG_ADMIN)
        : (uint8_t)(saved & ~ACCOUNT_FLAG_ADMIN);
    if (!accounts_save()) {
        accounts[index].flags = saved;
        return ACCOUNT_DISK_ERROR;
    }
    return ACCOUNT_OK;
}

enum account_result accounts_delete(size_t index) {
    if (index >= account_count) {
        return ACCOUNT_NOT_FOUND;
    }
    if (accounts_is_admin(index) && admin_count() == 1) {
        return ACCOUNT_LAST_ADMIN;
    }
    struct account removed = accounts[index];
    for (size_t i = index; i + 1 < account_count; i++) {
        accounts[i] = accounts[i + 1];
    }
    account_count--;
    if (!accounts_save()) {
        for (size_t i = account_count; i > index; i--) {
            accounts[i] = accounts[i - 1];
        }
        accounts[index] = removed;
        account_count++;
        return ACCOUNT_DISK_ERROR;
    }
    if (current_account == index) {
        current_account = ACCOUNT_NONE;
    } else if (current_account != ACCOUNT_NONE && current_account > index) {
        current_account--;
    }
    return ACCOUNT_OK;
}

bool accounts_personal_settings(uint8_t *flags) {
    if (current_account >= account_count
        || (accounts[current_account].flags & ACCOUNT_FLAG_PERSONAL_SET) == 0) {
        return false;
    }
    *flags = accounts[current_account].personal;
    return true;
}

void accounts_set_personal_settings(uint8_t flags) {
    if (current_account >= account_count) {
        return;
    }
    accounts[current_account].personal = flags;
    accounts[current_account].flags |= ACCOUNT_FLAG_PERSONAL_SET;
    (void)accounts_save();
}
