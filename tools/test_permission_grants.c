#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "accounts.h"
#include "permission_grants.h"
#include "permissions.h"
#include "sha256.h"
#include "vfs.h"

/* One in-memory file plus a list of directories stand in for the disk. */
static char directories[8][128];
static size_t directory_count;
static char file_path[128];
static uint8_t file_data[PERMISSION_GRANTS_FILE_MAX + 64];
static size_t file_size;
static bool file_exists;
static bool fail_writes;

size_t accounts_count(void) { return 2; }
const char *accounts_name(size_t account) {
    return account == 0 ? "Alice" : "Bob";
}

static bool is_file(const char *path, size_t length) {
    return file_exists && strlen(file_path) == length
        && strncmp(path, file_path, length) == 0;
}

bool vfs_stat(const char *path, size_t length, struct vfs_metadata *metadata) {
    if (is_file(path, length)) {
        *metadata = (struct vfs_metadata){.type = VFS_NODE_FILE};
        return true;
    }
    for (size_t i = 0; i < directory_count; i++) {
        if (strlen(directories[i]) == length
            && strncmp(path, directories[i], length) == 0) {
            *metadata = (struct vfs_metadata){.type = VFS_NODE_DIRECTORY};
            return true;
        }
    }
    return false;
}

bool vfs_create_directory(const char *path, size_t length) {
    assert(directory_count < 8 && length < sizeof(directories[0]));
    memcpy(directories[directory_count], path, length);
    directories[directory_count++][length] = '\0';
    return true;
}

bool vfs_read_file(const char *path, size_t length, uint8_t *buffer,
                   size_t capacity, size_t *size) {
    if (!is_file(path, length) || file_size > capacity) {
        return false;
    }
    memcpy(buffer, file_data, file_size);
    *size = file_size;
    return true;
}

bool vfs_write_file(const char *path, size_t length, const uint8_t *buffer,
                    size_t size) {
    if (fail_writes || length >= sizeof(file_path)
        || size > sizeof(file_data)) {
        return false;
    }
    memcpy(file_path, path, length);
    file_path[length] = '\0';
    memcpy(file_data, buffer, size);
    file_size = size;
    file_exists = true;
    return true;
}

bool vfs_file_size(const char *path, size_t length, size_t *size) {
    if (!is_file(path, length)) {
        return false;
    }
    *size = file_size;
    return true;
}

bool vfs_read_at(const char *path, size_t length, size_t offset,
                 uint8_t *buffer, size_t count, size_t *bytes_read) {
    if (!is_file(path, length) || offset > file_size) {
        return false;
    }
    /* Short reads, to exercise the loop. */
    size_t available = file_size - offset;
    size_t take = count < available ? count : available;
    take = take > 1000 ? 1000 : take;
    memcpy(buffer, file_data + offset, take);
    *bytes_read = take;
    return true;
}

static void program(uint8_t out[SHA256_DIGEST_SIZE], uint8_t seed) {
    for (size_t i = 0; i < SHA256_DIGEST_SIZE; i++) {
        out[i] = (uint8_t)(seed * 31 + i);
    }
}

static uint8_t image[PERMISSION_GRANTS_FILE_MAX + 64];

int main(void) {
    struct permission_grants grants;
    struct permission_grants copy;
    uint8_t a[SHA256_DIGEST_SIZE];
    uint8_t b[SHA256_DIGEST_SIZE];
    program(a, 1);
    program(b, 2);

    /* Set, find, update. */
    permission_grants_clear(&grants);
    assert(permission_grants_find(&grants, a) == NULL);
    assert(permission_grants_set(&grants, a, "Editor",
                                 PERMISSION_CAP_NET, 0, 20261111120000ull));
    assert(permission_grants_set(&grants, b, "Game",
                                 PERMISSION_CAP_FILES_SHARED
                                     | PERMISSION_CAP_NET,
                                 PERMISSION_CAP_NET, 0));
    assert(grants.count == 2);
    struct permission_grant *game = permission_grants_find(&grants, b);
    assert(game != NULL && strcmp(game->name, "Game") == 0);
    /* Refusal wins over a grant of the same bit. */
    assert(game->granted == PERMISSION_CAP_FILES_SHARED);
    assert(game->denied == PERMISSION_CAP_NET);
    assert(permission_grants_set(&grants, a, "Editor 2",
                                 PERMISSION_CAP_CLIPBOARD_READ | 0x80000000u,
                                 0, 5));
    assert(grants.count == 2);
    struct permission_grant *editor = permission_grants_find(&grants, a);
    assert(editor->granted == PERMISSION_CAP_CLIPBOARD_READ);
    assert(strcmp(editor->name, "Editor 2") == 0 && editor->updated == 5);
    char long_name[80];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    assert(permission_grants_set(&grants, a, long_name, 0, 0, 0));
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
    memset(bad + PERMISSION_GRANTS_HEADER_SIZE + PERMISSION_GRANTS_RECORD_SIZE
               + 48, 'y', PERMISSION_GRANT_NAME_MAX + 1);
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* First record both grants and denies NET. */
    bad[PERMISSION_GRANTS_HEADER_SIZE + 36] = PERMISSION_CAP_NET;
    bad[PERMISSION_GRANTS_HEADER_SIZE + 32] = PERMISSION_CAP_NET;
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* The same program twice. */
    memcpy(bad + PERMISSION_GRANTS_HEADER_SIZE + PERMISSION_GRANTS_RECORD_SIZE,
           bad + PERMISSION_GRANTS_HEADER_SIZE, SHA256_DIGEST_SIZE);
    assert(!permission_grants_decode(bad, size, &copy));
    memcpy(bad, image, size);
    /* Unknown capability bits from a future version are dropped. */
    bad[PERMISSION_GRANTS_HEADER_SIZE + 35] = 0x80;
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
        assert(permission_grants_set(&grants, key, "p", 0, 0, 0));
    }
    uint8_t extra[SHA256_DIGEST_SIZE];
    program(extra, 200);
    assert(!permission_grants_set(&grants, extra, "p", 0, 0, 0));
    size = permission_grants_encode(&grants, image, sizeof(image));
    assert(size == PERMISSION_GRANTS_FILE_MAX);
    assert(permission_grants_decode(image, size, &copy)
           && copy.count == PERMISSION_GRANTS_MAX);

    /* Paths. */
    char path[128];
    assert(permission_grants_path(0, path, sizeof(path)) != 0);
    assert(strcmp(path, "/disk/Users/Alice/AppData/Permissions.dat") == 0);
    assert(permission_grants_path(PERMISSION_NO_ACCOUNT, path,
                                  sizeof(path)) == 0);
    assert(permission_grants_path(5, path, sizeof(path)) == 0);
    assert(permission_grants_path(0, path, 20) == 0);

    /* Load and save. */
    assert(permission_grants_load(1, &copy) && copy.count == 0);
    assert(!permission_grants_load(PERMISSION_NO_ACCOUNT, &copy));
    permission_grants_clear(&grants);
    assert(permission_grants_set(&grants, a, "Editor", PERMISSION_CAP_NET,
                                 0, 1));
    assert(permission_grants_save(1, &grants));
    assert(strcmp(file_path, "/disk/Users/Bob/AppData/Permissions.dat") == 0);
    assert(directory_count == 2);
    assert(strcmp(directories[0], "/disk/Users/Bob") == 0);
    assert(strcmp(directories[1], "/disk/Users/Bob/AppData") == 0);
    assert(permission_grants_load(1, &copy) && copy.count == 1);
    assert(permission_grants_find(&copy, a)->granted == PERMISSION_CAP_NET);
    assert(permission_grants_save(1, &grants) && directory_count == 2);
    fail_writes = true;
    assert(!permission_grants_save(1, &grants));
    fail_writes = false;
    file_data[0] = 'Z';
    assert(!permission_grants_load(1, &copy) && copy.count == 0);
    assert(!permission_grants_save(PERMISSION_NO_ACCOUNT, &grants));

    /* Program hash matches a one-shot SHA-256 of the same bytes. */
    for (size_t i = 0; i < 5000; i++) {
        image[i] = (uint8_t)(i * 7);
    }
    assert(vfs_write_file("/disk/Programs/app", 18, image, 5000));
    uint8_t expected[SHA256_DIGEST_SIZE];
    uint8_t actual[SHA256_DIGEST_SIZE];
    struct sha256_context context;
    sha256_init(&context);
    sha256_update(&context, image, 5000);
    sha256_final(&context, expected);
    assert(permission_program_hash("/disk/Programs/app", 18, actual));
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
    assert(!permission_program_hash("/disk/Programs/none", 19, actual));

    puts("permission grants: passed");
    return 0;
}
