/*
 * Fetches a web page over HTTP or HTTPS and prints it.
 *
 *   run /bin/fetch example.com
 *   run /bin/fetch https://example.com/some/path
 *   run /bin/fetch -o /disk/page.html https://example.com
 *
 * Redirects are followed, and chunked and gzip coding are decoded (see
 * opos_http.h). With -o the body goes to a file instead of the screen.
 */
#include <opos_http.h>
#include <opos_tls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BODY (16u * 1024 * 1024)

/* Run from a disk, it asks once before it uses the network. */
OPOS_MANIFEST("Fetch", "1.0", OPOS_CAP_NET);

static void usage(void) {
    printf("Usage: fetch [-o FILE] [http://|https://]HOST[:PORT][/PATH]\n");
}

int main(int argc, char **argv) {
    const char *output = NULL;
    int argument = 1;
    if (argc > 2 && strcmp(argv[1], "-o") == 0) {
        output = argv[2];
        argument = 3;
    }
    if (argument >= argc) {
        usage();
        return 2;
    }
    char url[OPOS_HTTP_URL_MAX];
    if (strncmp(argv[argument], "http://", 7) == 0
        || strncmp(argv[argument], "https://", 8) == 0) {
        snprintf(url, sizeof(url), "%s", argv[argument]);
    } else {
        snprintf(url, sizeof(url), "http://%s", argv[argument]);
    }

    struct opos_http_response response;
    long result = opos_http_get(url, MAX_BODY, &response);
    if (result < 0) {
        printf("fetch: %s (error %ld)\n", opos_tls_error_text(result),
               -result);
        return 1;
    }
    printf("%s -> %ld %s (%u bytes)\n", response.url, response.status,
           response.content_type, (unsigned)response.length);
    int status = response.status == 200 ? 0 : 1;
    if (output != NULL) {
        long file = opos_open(output, OPOS_O_WRONLY | OPOS_O_CREAT
                                          | OPOS_O_TRUNC);
        /* One write call moves at most 4 KiB: loop until it is all out. */
        size_t written = 0;
        while (file >= 0 && written < response.length) {
            size_t piece = response.length - written;
            long put = opos_write((int)file, response.body + written,
                                  piece > 4096 ? 4096 : piece);
            if (put <= 0) {
                break;
            }
            written += (size_t)put;
        }
        if (file < 0 || written != response.length) {
            printf("fetch: cannot write %s\n", output);
            status = 1;
        } else {
            printf("Saved %u bytes to %s\n", (unsigned)response.length,
                   output);
        }
        if (file >= 0) {
            opos_close((int)file);
        }
    } else {
        /* printf cannot take huge strings: print in pieces. */
        for (size_t at = 0; at < response.length; at += 1024) {
            size_t take = response.length - at < 1024 ? response.length - at
                                                      : 1024;
            printf("%.*s", (int)take, (const char *)response.body + at);
        }
        printf("\n");
    }
    opos_http_free(&response);
    return status;
}
