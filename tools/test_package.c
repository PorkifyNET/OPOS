/*
 * Host tests for kernel/package.c.
 *
 *   test_package              run the built-in tests
 *   test_package FILE         parse FILE as PACKAGE text and print
 *                             "<result> <reason>" (used by
 *                             tools/test_package_parity.py to compare
 *                             with sdk/tools/mkopk.py)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "package.h"

static int failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static struct package package;
static char reason[PACKAGE_REASON_MAX + 1];

static const char good[] =
    "format: 1\n"
    "id: org.example.paint\n"
    "name: Paint\n"
    "version: 1.2.0\n"
    "publisher: Example Ltd\n"
    "platform: opos\n"
    "requires-opos: 0.1.5\n"
    "file: files/paint "
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 100 "
    "exec\n"
    "file: files/data/brushes.dat "
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210 4096\n"
    "start: files/paint Paint Program\n";

static enum package_result parse(const char *text, bool record) {
    return package_parse(text, strlen(text), record, &package, reason);
}

static void test_good(void) {
    CHECK(parse(good, false) == PACKAGE_OK);
    CHECK(strcmp(package.id, "org.example.paint") == 0);
    CHECK(strcmp(package.name, "Paint") == 0);
    CHECK(strcmp(package.version, "1.2.0") == 0);
    CHECK(strcmp(package.requires_opos, "0.1.5") == 0);
    CHECK(package.platform == PACKAGE_PLATFORM_OPOS);
    CHECK(package.file_count == 2);
    CHECK(package.files[0].exec && !package.files[1].exec);
    CHECK(package.files[0].size == 100 && package.files[1].size == 4096);
    CHECK(package.files[0].hash[0] == 0x01 && package.files[0].hash[31] == 0xef);
    CHECK(package.total_size == 4196);
    CHECK(package.start_count == 1 && package.starts[0].file == 0);
    CHECK(strncmp(good + package.starts[0].label.offset, "Paint Program",
                  package.starts[0].label.length) == 0);
    CHECK(package_find_file(&package, good, "files/data/brushes.dat", 22) == 1);
    CHECK(package_find_file(&package, good, "files/Paint", 11) == -1);
}

static void test_records(void) {
    char record[sizeof(good) + 32];
    snprintf(record, sizeof(record), "origin: store\n%s", good);
    CHECK(parse(record, true) == PACKAGE_OK && package.store);
    snprintf(record, sizeof(record), "origin: sideloaded\n%s", good);
    CHECK(parse(record, true) == PACKAGE_OK && !package.store);
    CHECK(parse(record, false) == PACKAGE_UNSUPPORTED); /* not in PACKAGE */
    CHECK(parse(good, true) == PACKAGE_BAD);            /* no origin */
    snprintf(record, sizeof(record), "origin: web\n%s", good);
    CHECK(parse(record, true) == PACKAGE_BAD);
}

static void test_versions(void) {
    CHECK(package_version_valid("1", 1));
    CHECK(package_version_valid("1.2.3.4", 7));
    CHECK(package_version_valid("99999.0", 7));
    CHECK(!package_version_valid("1.2.3.4.5", 9));
    CHECK(!package_version_valid("123456", 6));
    CHECK(!package_version_valid("1.", 2));
    CHECK(!package_version_valid(".1", 2));
    CHECK(!package_version_valid("", 0));
    CHECK(package_compare_versions("1.2.10", "1.2.9") == 1);
    CHECK(package_compare_versions("1.2", "1.2.0") == 0);
    CHECK(package_compare_versions("0.9", "1") == -1);
}

static void header(uint8_t block[512], const char *name, unsigned size,
                   char kind) {
    memset(block, 0, 512);
    memcpy(block, name, strlen(name));
    snprintf((char *)block + 100, 8, "%07o", 0644);
    snprintf((char *)block + 124, 12, "%011o", size);
    block[156] = (uint8_t)kind;
    memcpy(block + 257, "ustar", 6);
    memcpy(block + 263, "00", 2);
    memset(block + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) {
        sum += block[i];
    }
    snprintf((char *)block + 148, 8, "%06o", sum);
    block[155] = ' ';
}

static void test_ustar(void) {
    uint8_t block[512];
    char name[PACKAGE_PATH_MAX + 1];
    uint64_t size;
    bool end;
    header(block, "files/paint", 1234, '0');
    CHECK(package_ustar_header(block, name, &size, &end, reason) && !end);
    CHECK(strcmp(name, "files/paint") == 0 && size == 1234);
    header(block, "files/paint", 0, 0);
    CHECK(package_ustar_header(block, name, &size, &end, reason));
    memset(block, 0, sizeof(block));
    CHECK(package_ustar_header(block, name, &size, &end, reason) && end);
    const char kinds[] = {'1', '2', '5', '3'};
    for (size_t i = 0; i < sizeof(kinds); i++) {
        header(block, "files/x", 0, kinds[i]);
        CHECK(!package_ustar_header(block, name, &size, &end, reason));
    }
    header(block, "files/x", 0, '0');
    block[0] ^= 1;
    CHECK(!package_ustar_header(block, name, &size, &end, reason));
    CHECK(strstr(reason, "checksum") != NULL);
    header(block, "files/x", 0, '0');
    block[262] = ' ';
    CHECK(!package_ustar_header(block, name, &size, &end, reason));
    /* prefix + name */
    header(block, "x.dat", 1, '0');
    memcpy(block + 345, "files/deep", 10);
    memset(block + 148, ' ', 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) {
        sum += block[i];
    }
    snprintf((char *)block + 148, 8, "%06o", sum);
    block[155] = ' ';
    CHECK(package_ustar_header(block, name, &size, &end, reason));
    CHECK(strcmp(name, "files/deep/x.dat") == 0);
}

static int parse_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror(path);
        return 2;
    }
    static char text[PACKAGE_TEXT_MAX * 2];
    size_t length = fread(text, 1, sizeof(text), file);
    fclose(file);
    enum package_result result = package_parse(text, length, false, &package,
                                               reason);
    printf("%d %s\n", (int)result, reason);
    return 0;
}

/* "test_package --index FILE [ID VERSION]": "<result> <hash or -> <reason>" */
static int index_file(const char *path, const char *id, const char *version) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror(path);
        return 2;
    }
    static char text[PACKAGE_INDEX_MAX * 2];
    size_t length = fread(text, 1, sizeof(text), file);
    fclose(file);
    uint8_t hash[32] = {0};
    enum package_result result = package_index_find(text, length, id, version,
                                                    hash, reason);
    printf("%d ", (int)result);
    if (result == PACKAGE_OK && id != NULL) {
        for (int i = 0; i < 32; i++) {
            printf("%02x", hash[i]);
        }
    } else {
        printf("-");
    }
    printf(" %s\n", reason);
    return 0;
}

static void test_index(void) {
    static const char index[] =
        "format: 1\n"
        "\n"
        "entry: org.example.paint 1.2.0\n"
        "package: "
        "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n"
        "name: Paint\n"
        "description: Draws pictures \xc3\xa9\n"
        "entry: org.example.paint 1.10.0\n"
        "package: "
        "ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100\n"
        "entry: org.example.zoo 1.0\n"
        "signer: someone\n"
        "package: "
        "0000000000000000000000000000000000000000000000000000000000000001\n";
    uint8_t hash[32];
    CHECK(package_index_find(index, sizeof(index) - 1, NULL, NULL, NULL,
                             reason) == PACKAGE_OK);
    CHECK(package_index_find(index, sizeof(index) - 1, "org.example.paint",
                             "1.10.0", hash, reason) == PACKAGE_OK);
    CHECK(hash[0] == 0xff && hash[31] == 0x00);
    CHECK(package_index_find(index, sizeof(index) - 1, "org.example.paint",
                             "1.2.0", hash, reason) == PACKAGE_OK);
    CHECK(hash[0] == 0x00 && hash[31] == 0xff);
    CHECK(package_index_find(index, sizeof(index) - 1, "org.example.zoo",
                             "1.0", hash, reason) == PACKAGE_OK);
    CHECK(hash[31] == 0x01);
    /* Exact spelling: 1.10 is not 1.10.0, and ids are not prefixes. */
    CHECK(package_index_find(index, sizeof(index) - 1, "org.example.paint",
                             "1.10", hash, reason) == PACKAGE_NOT_FOUND);
    CHECK(package_index_find(index, sizeof(index) - 1, "org.example.pain",
                             "1.2.0", hash, reason) == PACKAGE_NOT_FOUND);
    /* A cut index loses its last line break. */
    CHECK(package_index_find(index, sizeof(index) - 2, NULL, NULL, NULL,
                             reason) == PACKAGE_BAD);
    CHECK(package_index_find("format: 2\n", 10, NULL, NULL, NULL, reason)
          == PACKAGE_UNSUPPORTED);
    CHECK(package_index_find("format: 1\n", 10, NULL, NULL, NULL, reason)
          == PACKAGE_OK);
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--index") == 0) {
        return index_file(argv[2], argc >= 5 ? argv[3] : NULL,
                          argc >= 5 ? argv[4] : NULL);
    }
    if (argc == 2) {
        return parse_file(argv[1]);
    }
    test_index();
    test_good();
    test_records();
    test_versions();
    test_ustar();
    if (failures != 0) {
        printf("test_package: %d failures\n", failures);
        return 1;
    }
    printf("test_package: ok\n");
    return 0;
}
