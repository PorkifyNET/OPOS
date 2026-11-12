#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "accounts.h"
#include "package.h"
#include "permission_grants.h"
#include "permissions.h"
#include "sha256.h"
#include "vfs.h"

/* A few in-memory files and directories stand in for the disk. */
#define FILE_SLOTS 8
#define FILE_CAPACITY (PERMISSION_GRANTS_FILE_MAX + 64)

static struct {
    char path[128];
    uint8_t data[FILE_CAPACITY];
    size_t size;
    bool used;
} files[FILE_SLOTS];
static char directories[32][128];
static size_t directory_count;
static bool fail_writes;

size_t accounts_count(void) { return 2; }
size_t accounts_current(void) { return 0; }
bool accounts_is_admin(size_t account) { return account == 0; }
const char *accounts_name(size_t account) {
    return account == 0 ? "Alice" : "Bob";
}

/* Stands in for package_install.c: the package the next launch belongs
   to, or none. The kernel's version checks the record; this one records
   what it was asked. */
static const char *stub_package_id;
static bool stub_store;
static size_t stub_calls;
static size_t stub_account;
static uint8_t stub_hash[SHA256_DIGEST_SIZE];
bool package_identify(const char *path, size_t length, size_t account,
                      const uint8_t hash[32], char id[PACKAGE_ID_MAX + 1],
                      bool *store) {
    (void)path, (void)length;
    stub_calls++;
    stub_account = account;
    memcpy(stub_hash, hash, SHA256_DIGEST_SIZE);
    if (stub_package_id == NULL) {
        return false;
    }
    snprintf(id, PACKAGE_ID_MAX + 1, "%s", stub_package_id);
    *store = stub_store;
    return true;
}

bool vfs_disk_root_backing_path(const char *path, size_t length, char *backing,
                                size_t capacity, size_t *backing_length) {
    (void)path, (void)length, (void)backing, (void)capacity;
    (void)backing_length;
    return false;
}
bool vfs_disk_root_contains(const char *path, size_t length) {
    (void)path, (void)length;
    return false;
}
bool vfs_disk_root_ancestor(const char *path, size_t length) {
    (void)path, (void)length;
    return false;
}

static int find_file(const char *path, size_t length) {
    for (int i = 0; i < FILE_SLOTS; i++) {
        if (files[i].used && strlen(files[i].path) == length
            && strncmp(path, files[i].path, length) == 0) {
            return i;
        }
    }
    return -1;
}

static bool is_directory(const char *path, size_t length) {
    for (size_t i = 0; i < directory_count; i++) {
        if (strlen(directories[i]) == length
            && strncmp(path, directories[i], length) == 0) {
            return true;
        }
    }
    return false;
}

bool vfs_stat(const char *path, size_t length, struct vfs_metadata *metadata) {
    int file = find_file(path, length);
    if (file >= 0) {
        *metadata = (struct vfs_metadata){
            .type = VFS_NODE_FILE, .size = files[file].size,
            .inode = (uint64_t)file + 100
        };
        return true;
    }
    if (is_directory(path, length)) {
        *metadata = (struct vfs_metadata){.type = VFS_NODE_DIRECTORY};
        return true;
    }
    return false;
}

bool vfs_create_directory(const char *path, size_t length) {
    assert(directory_count < 32 && length < sizeof(directories[0]));
    memcpy(directories[directory_count], path, length);
    directories[directory_count++][length] = '\0';
    return true;
}

bool vfs_read_file(const char *path, size_t length, uint8_t *buffer,
                   size_t capacity, size_t *size) {
    int file = find_file(path, length);
    if (file < 0 || files[file].size > capacity) {
        return false;
    }
    memcpy(buffer, files[file].data, files[file].size);
    *size = files[file].size;
    return true;
}

bool vfs_write_file(const char *path, size_t length, const uint8_t *buffer,
                    size_t size) {
    if (fail_writes || length >= sizeof(files[0].path)
        || size > FILE_CAPACITY) {
        return false;
    }
    int file = find_file(path, length);
    for (int i = 0; file < 0 && i < FILE_SLOTS; i++) {
        if (!files[i].used) {
            file = i;
        }
    }
    assert(file >= 0);
    memcpy(files[file].path, path, length);
    files[file].path[length] = '\0';
    memcpy(files[file].data, buffer, size);
    files[file].size = size;
    files[file].used = true;
    return true;
}

bool vfs_file_size(const char *path, size_t length, size_t *size) {
    int file = find_file(path, length);
    if (file < 0) {
        return false;
    }
    *size = files[file].size;
    return true;
}

bool vfs_read_at(const char *path, size_t length, size_t offset,
                 uint8_t *buffer, size_t count, size_t *bytes_read) {
    int file = find_file(path, length);
    if (file < 0 || offset > files[file].size) {
        return false;
    }
    /* Short reads, to exercise the loop. */
    size_t available = files[file].size - offset;
    size_t take = count < available ? count : available;
    take = take > 1000 ? 1000 : take;
    memcpy(buffer, files[file].data + offset, take);
    *bytes_read = take;
    return true;
}

/* Children of path: directories first, then files. */
static bool child_at(const char *path, size_t length, size_t index,
                     struct vfs_directory_entry *entry) {
    for (size_t pass = 0; pass < 2; pass++) {
        size_t limit = pass == 0 ? directory_count : FILE_SLOTS;
        for (size_t i = 0; i < limit; i++) {
            const char *candidate = pass == 0 ? directories[i]
                                              : files[i].path;
            if ((pass == 1 && !files[i].used)
                || strncmp(candidate, path, length) != 0
                || candidate[length] != '/'
                || strchr(candidate + length + 1, '/') != NULL) {
                continue;
            }
            if (index-- == 0) {
                if (entry != NULL) {
                    const char *name = candidate + length + 1;
                    *entry = (struct vfs_directory_entry){
                        .name_length = strlen(name),
                        .type = pass == 0 ? VFS_NODE_DIRECTORY
                                          : VFS_NODE_FILE,
                    };
                    strcpy(entry->name, name);
                }
                return true;
            }
        }
    }
    return false;
}

bool vfs_directory_count(const char *path, size_t length, size_t *count) {
    if (!is_directory(path, length)) {
        return false;
    }
    *count = 0;
    while (child_at(path, length, *count, NULL)) {
        (*count)++;
    }
    return true;
}

bool vfs_directory_entry_at(const char *path, size_t length, size_t index,
                            struct vfs_directory_entry *entry) {
    return child_at(path, length, index, entry);
}

static void program(uint8_t out[SHA256_DIGEST_SIZE], uint8_t seed) {
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out[i] = (uint8_t)(seed * 31 + i);
    }
}

static void hash_of(const uint8_t *data, size_t size,
                    uint8_t out[SHA256_DIGEST_SIZE]) {
    struct sha256_context context;
    sha256_init(&context);
    sha256_update(&context, data, size);
    sha256_final(&context, out);
}

static void put_file(const char *path, const uint8_t *data, size_t size) {
    assert(vfs_write_file(path, strlen(path), data, size));
}

static uint8_t image[FILE_CAPACITY];

static void test_records(void) {
    struct permission_grants grants;
    struct permission_grants copy;
    uint8_t a[SHA256_DIGEST_SIZE];
    uint8_t b[SHA256_DIGEST_SIZE];
    program(a, 1);
    program(b, 2);

    /* Set, find, update. */
    permission_grants_clear(&grants);
    assert(permission_grants_find(&grants, a) == NULL);
    assert(permission_grants_set(&grants, a, "Editor", PERMISSION_CAP_NET, 0,
                                 0, 20261111120000ull));
    assert(permission_grants_set(&grants, b, "Game",
                                 PERMISSION_CAP_FILES_SHARED
                                     | PERMISSION_CAP_NET,
                                 PERMISSION_CAP_NET,
                                 PERMISSION_CAP_NET | PERMISSION_CAP_FILES_SHARED
                                     | PERMISSION_CAP_CLIPBOARD_READ,
                                 0));
    assert(grants.count == 2);
    struct permission_grant *game = permission_grants_find(&grants, b);
    assert(game != NULL && strcmp(game->name, "Game") == 0);
    /* Refusal wins over a grant of the same bit; decided bits are not
       askable. */
    assert(game->granted == PERMISSION_CAP_FILES_SHARED);
    assert(game->denied == PERMISSION_CAP_NET);
    assert(game->ask == PERMISSION_CAP_CLIPBOARD_READ);
    assert(permission_grants_set(&grants, a, "Editor 2",
                                 PERMISSION_CAP_CLIPBOARD_READ | 0x80000000u,
                                 0, 0, 5));
    assert(grants.count == 2);
    struct permission_grant *editor = permission_grants_find(&grants, a);
    assert(editor->granted == PERMISSION_CAP_CLIPBOARD_READ);
    assert(strcmp(editor->name, "Editor 2") == 0 && editor->updated == 5);
    char long_name[80];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    assert(permission_grants_set(&grants, a, long_name, 0, 0, 0, 0));
    assert(strlen(permission_grants_find(&grants, a)->name)
           == PERMISSION_GRANT_NAME_MAX);
    grants.flags = PERMISSION_GRANTS_MIGRATED;

    /* Round trip. */
    size_t size = permission_grants_encode(&grants, image, sizeof(image));
    assert(size == PERMISSION_GRANTS_HEADER_SIZE
                       + 2 * PERMISSION_GRANTS_RECORD_SIZE);
    assert(memcmp(image, "OPGR\x01", 5) == 0);
    assert(permission_grants_encode(&grants, image, size - 1) == 0);
    assert(permission_grants_decode(image, size, &copy));
    assert(copy.count == 2 && copy.flags == PERMISSION_GRANTS_MIGRATED);
    assert(memcmp(copy.records, grants.records,
                  2 * sizeof(grants.records[0])) == 0);

    /* Malformed files are rejected whole. */
    const size_t first = PERMISSION_GRANTS_HEADER_SIZE;
    const size_t second = first + PERMISSION_GRANTS_RECORD_SIZE;
    uint8_t bad[sizeof(image)];
    memcpy(bad, image, size);
    assert(!permission_grants_decode(bad, size - 1, &copy) && copy.count == 0);
    assert(!permission_grants_decode(bad, size + 1, &copy));
    assert(!permission_grants_decode(bad, 8, &copy));
    assert(!permission_grants_decode(NULL, 0, &copy));
    bad[0] = 'X';
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    bad[4] = 2;
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    bad[6] = 1;
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    bad[12] = 3; /* count disagrees with size */
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    bad[12] = 0xFF;
    bad[13] = 0xFF;
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* Second record's name loses its terminator. */
    memset(bad + second + 56, 'y', PERMISSION_GRANT_NAME_MAX + 1);
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* First record both grants and denies NET. */
    bad[first + 36] = PERMISSION_CAP_NET;
    bad[first + 32] = PERMISSION_CAP_NET;
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* Second record asks for a bit it was already granted. */
    bad[second + 40] = PERMISSION_CAP_FILES_SHARED;
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* Reserved bytes must stay zero. */
    bad[first + 45] = 1;
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* The same program twice. */
    memcpy(bad + second, bad + first, SHA256_DIGEST_SIZE);
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* Unknown capability bits from a future version are dropped. */
    bad[first + 35] = 0x80;
    assert(permission_grants_decode(bad, size, &copy));
    assert(copy.records[0].granted == 0);

    /* Remove keeps order and clears the freed slot. */
    assert(permission_grants_remove(&grants, a));
    assert(!permission_grants_remove(&grants, a));
    assert(grants.count == 1 && permission_grants_find(&grants, b) != NULL);
    assert(grants.records[1].name[0] == '\0');

    /* Full store. */
    permission_grants_clear(&grants);
    for (int i = 0; i < PERMISSION_GRANTS_MAX; i++) {
        uint8_t key[SHA256_DIGEST_SIZE];
        program(key, (uint8_t)(i + 10));
        assert(permission_grants_set(&grants, key, "p", 0, 0, 0, 0));
    }
    uint8_t extra[SHA256_DIGEST_SIZE];
    program(extra, 200);
    assert(!permission_grants_set(&grants, extra, "p", 0, 0, 0, 0));
    size = permission_grants_encode(&grants, image, sizeof(image));
    assert(size == PERMISSION_GRANTS_FILE_MAX);
    assert(permission_grants_decode(image, size, &copy)
           && copy.count == PERMISSION_GRANTS_MAX);
}

static void test_storage(void) {
    struct permission_grants grants;
    struct permission_grants copy;
    uint8_t a[SHA256_DIGEST_SIZE];
    program(a, 1);

    char path[128];
    assert(permission_grants_path(0, path, sizeof(path)) != 0);
    assert(strcmp(path, "/disk/Users/Alice/AppData/Permissions.dat") == 0);
    assert(permission_grants_path(PERMISSION_NO_ACCOUNT, path,
                                  sizeof(path)) == 0);
    assert(permission_grants_path(5, path, sizeof(path)) == 0);
    assert(permission_grants_path(0, path, 20) == 0);

    assert(permission_grants_load(1, &copy) && copy.count == 0);
    assert(!permission_grants_load(PERMISSION_NO_ACCOUNT, &copy));
    permission_grants_clear(&grants);
    assert(permission_grants_set(&grants, a, "Editor", PERMISSION_CAP_NET,
                                 0, 0, 1));
    size_t directories_before = directory_count;
    assert(permission_grants_save(1, &grants));
    static const char bob[] = "/disk/Users/Bob/AppData/Permissions.dat";
    int file = find_file(bob, strlen(bob));
    assert(file >= 0);
    assert(directory_count == directories_before + 2);
    assert(is_directory("/disk/Users/Bob", 15));
    assert(is_directory("/disk/Users/Bob/AppData", 23));
    assert(permission_grants_load(1, &copy) && copy.count == 1);
    assert(permission_grants_find(&copy, a)->granted == PERMISSION_CAP_NET);
    assert(permission_grants_save(1, &grants)
           && directory_count == directories_before + 2);
    fail_writes = true;
    assert(!permission_grants_save(1, &grants));
    fail_writes = false;
    files[file].data[0] = 'Z';
    assert(!permission_grants_load(1, &copy) && copy.count == 0);
    /* A damaged store is left alone, not overwritten by a decision. */
    assert(!permission_grants_decide(1, a, "Editor", PERMISSION_CAP_NET,
                                     true, 2));
    assert(files[file].data[0] == 'Z');
    files[file].used = false;
    assert(!permission_grants_save(PERMISSION_NO_ACCOUNT, &grants));

    /* Program hash matches a one-shot SHA-256 of the same bytes. */
    for (size_t i = 0; i < 5000; i++) {
        image[i] = (uint8_t)(i * 7);
    }
    put_file("/disk/hashme", image, 5000);
    uint8_t expected[SHA256_DIGEST_SIZE];
    uint8_t actual[SHA256_DIGEST_SIZE];
    hash_of(image, 5000, expected);
    assert(permission_program_hash("/disk/hashme", 12, actual));
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
    assert(!permission_program_hash("/disk/none", 10, actual));
    files[find_file("/disk/hashme", 12)].used = false;
}

static void test_migration_and_launch(void) {
    /* /disk/Programs/old (ELF), /disk/Programs/Suite/tool (ELF),
       /disk/Programs/readme (not ELF). */
    static const uint8_t old_elf[] = {0x7F, 'E', 'L', 'F', 1, 2, 3};
    static const uint8_t tool_elf[] = {0x7F, 'E', 'L', 'F', 9, 9};
    static const uint8_t readme[] = "not a program";
    (void)vfs_create_directory("/disk/Programs", 14);
    (void)vfs_create_directory("/disk/Programs/Suite", 20);
    put_file("/disk/Programs/old", old_elf, sizeof(old_elf));
    put_file("/disk/Programs/Suite/tool", tool_elf, sizeof(tool_elf));
    put_file("/disk/Programs/readme", readme, sizeof(readme));
    uint8_t old_hash[SHA256_DIGEST_SIZE];
    uint8_t tool_hash[SHA256_DIGEST_SIZE];
    hash_of(old_elf, sizeof(old_elf), old_hash);
    hash_of(tool_elf, sizeof(tool_elf), tool_hash);

    struct permission_grants grants;
    assert(permission_grants_migrate(0, 0));
    assert(permission_grants_load(0, &grants));
    assert(grants.flags & PERMISSION_GRANTS_MIGRATED);
    assert(grants.count == 2);
    const struct permission_grant *old =
        permission_grants_find(&grants, old_hash);
    assert(old != NULL && strcmp(old->name, "old") == 0);
    assert(old->ask == PERMISSION_CAP_FILES_SHARED);
    assert(old->granted == 0 && old->denied == 0);
    assert(permission_grants_find(&grants, tool_hash) != NULL);

    /* Once only: a program installed afterwards gets no record. */
    static const uint8_t new_elf[] = {0x7F, 'E', 'L', 'F', 4, 4};
    put_file("/disk/Programs/new", new_elf, sizeof(new_elf));
    assert(permission_grants_migrate(0, 0));
    assert(permission_grants_load(0, &grants) && grants.count == 2);

    /* Launching: a migrated program may ask; a new one may not. */
    struct permission_identity session = permission_session_identity();
    struct permission_identity identity = session;
    permission_apply_program(&identity, "/disk/Programs/old", 18, old_elf,
                             sizeof(old_elf));
    assert(identity.confined && identity.program_known);
    assert(memcmp(identity.program, old_hash, SHA256_DIGEST_SIZE) == 0);
    assert(identity.capabilities == PERMISSION_CAPS_DISK_DEFAULT);
    assert(identity.askable
           == (PERMISSION_CAP_FILES_SHARED | PERMISSION_CAPS_ASK_ON_USE));
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/new", 18, new_elf,
                             sizeof(new_elf));
    assert(identity.capabilities == PERMISSION_CAPS_DISK_DEFAULT);
    /* Any program may ask for the network and the clipboard. */
    assert(identity.askable == PERMISSION_CAPS_ASK_ON_USE);

    /* Built-in programs are not confined or recorded; they only narrow to
       their job (see test_builtin_launch). */
    identity = session;
    permission_apply_program(&identity, "/bin/hello", 10, new_elf,
                             sizeof(new_elf));
    assert(!identity.confined && !identity.program_known);
    assert(identity.capabilities
           == permission_builtin_capabilities("/bin/hello", 10));

    /* Allowing stores the grant; the next launch has it. */
    assert(permission_grants_decide(0, old_hash, "old",
                                    PERMISSION_CAP_FILES_SHARED, true, 7));
    assert(permission_grants_load(0, &grants));
    old = permission_grants_find(&grants, old_hash);
    assert(old->granted == PERMISSION_CAP_FILES_SHARED && old->ask == 0
           && old->updated == 7 && strcmp(old->name, "old") == 0);
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/old", 18, old_elf,
                             sizeof(old_elf));
    assert(identity.capabilities
           == (PERMISSION_CAPS_DISK_DEFAULT | PERMISSION_CAP_FILES_SHARED));
    assert(identity.askable == PERMISSION_CAPS_ASK_ON_USE);

    /* A parent without the capability bounds the child, which cannot ask
       for it either. */
    struct permission_identity parent = session;
    parent.capabilities &= ~(PERMISSION_CAP_FILES_SHARED
                             | PERMISSION_CAP_NET);
    permission_apply_program(&parent, "/disk/Programs/old", 18, old_elf,
                             sizeof(old_elf));
    assert(!permission_has(&parent, PERMISSION_CAP_FILES_SHARED));
    assert(parent.askable == PERMISSION_CAP_CLIPBOARD_READ);

    /* Allowing the network keeps it; refusing the clipboard stops asks. */
    assert(permission_grants_decide(0, old_hash, NULL, PERMISSION_CAP_NET,
                                    true, 10));
    assert(permission_grants_decide(0, old_hash, NULL,
                                    PERMISSION_CAP_CLIPBOARD_READ, false, 11));
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/old", 18, old_elf,
                             sizeof(old_elf));
    assert(permission_has(&identity, PERMISSION_CAP_NET));
    assert(!permission_has(&identity, PERMISSION_CAP_CLIPBOARD_READ));
    assert(identity.askable == 0);
    assert(permission_grants_decide(0, old_hash, NULL, PERMISSION_CAP_NET,
                                    false, 12));
    assert(permission_grants_decide(0, old_hash, NULL,
                                    PERMISSION_CAP_CLIPBOARD_READ, false, 13));

    /* Refusing replaces it and is not asked again; NULL keeps the name. */
    assert(permission_grants_decide(0, old_hash, NULL,
                                    PERMISSION_CAP_FILES_SHARED, false, 8));
    assert(permission_grants_load(0, &grants));
    old = permission_grants_find(&grants, old_hash);
    assert(old->granted == 0
           && old->denied == (PERMISSION_CAP_FILES_SHARED | PERMISSION_CAP_NET
                              | PERMISSION_CAP_CLIPBOARD_READ)
           && old->ask == 0 && strcmp(old->name, "old") == 0);
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/old", 18, old_elf,
                             sizeof(old_elf));
    assert(identity.capabilities == PERMISSION_CAPS_DISK_DEFAULT
           && identity.askable == 0);

    /* Denying a default capability removes it. */
    assert(permission_grants_decide(0, tool_hash, NULL,
                                    PERMISSION_CAP_CLIPBOARD_WRITE, false, 9));
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/Suite/tool", 25,
                             tool_elf, sizeof(tool_elf));
    assert(!permission_has(&identity, PERMISSION_CAP_CLIPBOARD_WRITE));
    assert(identity.askable
           == (PERMISSION_CAP_FILES_SHARED | PERMISSION_CAPS_ASK_ON_USE));

    /* Without an account nothing is loaded, but the program may ask; the
       answer lasts for that process only. */
    struct permission_identity guest = {
        .account = PERMISSION_NO_ACCOUNT, .capabilities = PERMISSION_CAPS_ALL
    };
    permission_apply_program(&guest, "/disk/Programs/old", 18, old_elf,
                             sizeof(old_elf));
    assert(guest.capabilities == PERMISSION_CAPS_DISK_DEFAULT
           && guest.askable == PERMISSION_CAPS_ASK_ON_USE);
    assert(!permission_grants_decide(PERMISSION_NO_ACCOUNT, old_hash, NULL,
                                     PERMISSION_CAP_NET, true, 1));

    /* A damaged store makes nothing askable. */
    static const char alice[] = "/disk/Users/Alice/AppData/Permissions.dat";
    int store = find_file(alice, strlen(alice));
    assert(store >= 0);
    files[store].data[0] = 'Z';
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/new", 18, new_elf,
                             sizeof(new_elf));
    assert(identity.capabilities == PERMISSION_CAPS_DISK_DEFAULT
           && identity.askable == 0);
    files[store].data[0] = 'O';
}

/* A minimal ELF64 image whose PT_NOTE holds one manifest note. */
static size_t manifest_elf(uint8_t *out, const char *name,
                           uint32_t capabilities) {
    memset(out, 0, 256);
    memcpy(out, "\x7f" "ELF", 4);
    out[4] = 2;
    out[5] = 1;
    out[32] = 64;  /* program headers at 64 */
    out[54] = 56;  /* entry size */
    out[56] = 1;   /* one entry */
    out[64] = 4;   /* PT_NOTE */
    out[64 + 8] = 120;
    out[64 + 32] = 76;
    uint8_t *note = out + 120;
    note[0] = 5;
    note[4] = 56;
    note[8] = 2;
    memcpy(note + 12, "OPOS", 5);
    note[20] = 1; /* format */
    memcpy(note + 24, &capabilities, 4);
    strcpy((char *)note + 28, name);
    strcpy((char *)note + 60, "2.0");
    return 196;
}

static void test_manifest_launch(void) {
    struct permission_identity session = permission_session_identity();
    uint8_t elf[256];

    /* Declaring files.shared lets a new program ask for it, and only it. */
    size_t size = manifest_elf(elf, "Viewer", PERMISSION_CAP_FILES_SHARED);
    put_file("/disk/Programs/viewer", elf, size);
    struct permission_identity identity = session;
    permission_apply_program(&identity, "/disk/Programs/viewer", 21, elf,
                             size);
    assert(strcmp(identity.program_name, "Viewer") == 0);
    assert(identity.askable == PERMISSION_CAP_FILES_SHARED);
    assert(identity.capabilities == PERMISSION_CAPS_DISK_DEFAULT);

    /* Declaring nothing: never asks, even with a migration record. */
    size = manifest_elf(elf, "Quiet", 0);
    uint8_t hash[SHA256_DIGEST_SIZE];
    hash_of(elf, size, hash);
    struct permission_grants grants;
    assert(permission_grants_load(0, &grants));
    assert(permission_grants_set(&grants, hash, "Quiet", 0, 0,
                                 PERMISSION_CAP_FILES_SHARED, 0));
    assert(permission_grants_save(0, &grants));
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/quiet", 20, elf,
                             size);
    assert(identity.askable == 0);

    /* Defaults are not "asked for", and decided bits stay decided. */
    size = manifest_elf(elf, "Chat", PERMISSION_CAP_NET
                                         | PERMISSION_CAP_WINDOW
                                         | PERMISSION_CAP_CLIPBOARD_READ);
    hash_of(elf, size, hash);
    assert(permission_grants_decide(0, hash, NULL, PERMISSION_CAP_NET,
                                    false, 1));
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/chat", 19, elf,
                             size);
    assert(identity.askable == PERMISSION_CAP_CLIPBOARD_READ);
    assert(!permission_has(&identity, PERMISSION_CAP_NET));

    /* A broken manifest counts as none; the name is cleared on the next
       program. */
    size = manifest_elf(elf, "Broken", PERMISSION_CAP_FILES_SHARED);
    elf[120 + 4] = 52;
    identity = session;
    permission_apply_program(&identity, "/disk/Programs/broken", 21, elf,
                             size);
    assert(identity.program_name[0] == '\0');
    assert(identity.askable == PERMISSION_CAPS_ASK_ON_USE);
}

static void test_builtin_launch(void) {
    struct permission_identity identity = permission_session_identity();
    static const uint8_t elf[] = {0x7F, 'E', 'L', 'F', 2, 1};
    permission_apply_program(&identity, "/bin/notepad", 12, elf,
                             sizeof(elf));
    assert(!identity.confined);
    assert(!permission_has(&identity, PERMISSION_CAP_NET));
    assert(!permission_has(&identity, PERMISSION_CAP_CLIPBOARD_READ));
    assert(permission_has(&identity, PERMISSION_CAP_CLIPBOARD_WRITE));
    /* notepad starting fetch cannot regain the network. */
    permission_apply_program(&identity, "/bin/fetch", 10, elf, sizeof(elf));
    assert(!permission_has(&identity, PERMISSION_CAP_NET));

    /* A disk program starting the browser keeps its own limits and may
       still ask for what both allow. */
    identity = permission_session_identity();
    identity.askable = PERMISSION_CAPS_ASK_ON_USE;
    identity.capabilities = PERMISSION_CAPS_DISK_DEFAULT;
    permission_apply_program(&identity, "/bin/notepad", 12, elf,
                             sizeof(elf));
    assert(identity.askable == 0);
    identity.askable = PERMISSION_CAPS_ASK_ON_USE;
    permission_apply_program(&identity, "/bin/browser", 12, elf,
                             sizeof(elf));
    assert(identity.askable == PERMISSION_CAPS_ASK_ON_USE);
    assert(!permission_has(&identity, PERMISSION_CAP_NET));

    /* No path (kernel-embedded tests): unchanged. */
    identity = permission_session_identity();
    permission_apply_program(&identity, NULL, 0, elf, sizeof(elf));
    assert(identity.capabilities == PERMISSION_CAPS_ALL);
}

static void test_package_identity(void) {
    struct permission_identity session = permission_session_identity();
    static const uint8_t v1[] = {0x7F, 'E', 'L', 'F', 1, 0, 1};
    static const uint8_t v2[] = {0x7F, 'E', 'L', 'F', 1, 0, 2};
    static const char path[] = "/disk/Programs/org.example.paint/paint";
    const size_t length = sizeof(path) - 1;
    uint8_t v1_hash[SHA256_DIGEST_SIZE];
    uint8_t v2_hash[SHA256_DIGEST_SIZE];
    uint8_t key[SHA256_DIGEST_SIZE];
    hash_of(v1, sizeof(v1), v1_hash);
    hash_of(v2, sizeof(v2), v2_hash);
    permission_store_key("org.example.paint", key);
    static const char store_text[] = "store:org.example.paint";
    uint8_t expected[SHA256_DIGEST_SIZE];
    hash_of((const uint8_t *)store_text, sizeof(store_text) - 1, expected);
    assert(memcmp(key, expected, SHA256_DIGEST_SIZE) == 0);

    /* A Store package: grants keyed by store:<id>, storage by id. */
    stub_calls = 0;
    stub_package_id = "org.example.paint";
    stub_store = true;
    struct permission_identity identity = session;
    permission_apply_program(&identity, path, length, v1, sizeof(v1));
    assert(identity.program_known);
    assert(memcmp(identity.program, key, SHA256_DIGEST_SIZE) == 0);
    assert(strcmp(identity.private_path,
                  "/disk/Users/Alice/AppData/Programs/org.example.paint")
           == 0);
    assert(stub_calls == 1 && stub_account == 0);
    assert(memcmp(stub_hash, v1_hash, SHA256_DIGEST_SIZE) == 0);
    assert(permission_grants_decide(0, identity.program, "Paint",
                                    PERMISSION_CAP_NET, true, 3));

    /* The update: new bytes, same grants and storage. */
    identity = session;
    permission_apply_program(&identity, path, length, v2, sizeof(v2));
    assert(memcmp(stub_hash, v2_hash, SHA256_DIGEST_SIZE) == 0);
    assert(memcmp(identity.program, key, SHA256_DIGEST_SIZE) == 0);
    assert(permission_has(&identity, PERMISSION_CAP_NET));
    assert(strcmp(identity.private_path,
                  "/disk/Users/Alice/AppData/Programs/org.example.paint")
           == 0);

    /* Sideloaded: same storage, but grants follow the executable's hash,
       so the update is a new program for grants. */
    stub_store = false;
    identity = session;
    permission_apply_program(&identity, path, length, v2, sizeof(v2));
    assert(memcmp(identity.program, v2_hash, SHA256_DIGEST_SIZE) == 0);
    assert(!permission_has(&identity, PERMISSION_CAP_NET));
    assert(strcmp(identity.private_path,
                  "/disk/Users/Alice/AppData/Programs/org.example.paint")
           == 0);

    /* Not the package's file (or a changed one): a loose program, with
       hash identity and storage named by its path. */
    stub_package_id = NULL;
    identity = session;
    permission_apply_program(&identity, path, length, v2, sizeof(v2));
    assert(memcmp(identity.program, v2_hash, SHA256_DIGEST_SIZE) == 0);
    assert(!permission_has(&identity, PERMISSION_CAP_NET));
    assert(strncmp(identity.private_path,
                   "/disk/Users/Alice/AppData/Programs/", 35) == 0);
    assert(strlen(identity.private_path) == 35 + 16);

    /* No image: nothing to identify, nothing asked. */
    stub_calls = 0;
    stub_package_id = "org.example.paint";
    identity = session;
    permission_apply_program(&identity, path, length, NULL, 0);
    assert(stub_calls == 0 && !identity.program_known);
    stub_package_id = NULL;
}

static void test_forget(void) {
    uint8_t program[SHA256_DIGEST_SIZE];
    memset(program, 0x5A, sizeof(program));
    assert(!permission_grants_forget(0, program));
    assert(permission_grants_decide(0, program, "Gone", PERMISSION_CAP_NET,
                                    false, 7));
    assert(permission_grants_forget(0, program));
    struct permission_grants grants;
    assert(permission_grants_load(0, &grants));
    assert(permission_grants_find(&grants, program) == NULL);
    assert(!permission_grants_forget(0, program));
    assert(!permission_grants_forget(0, NULL));
}

int main(void) {
    test_records();
    test_storage();
    test_migration_and_launch();
    test_manifest_launch();
    test_forget();
    test_builtin_launch();
    test_package_identity();
    puts("permission grants: passed");
    return 0;
}
