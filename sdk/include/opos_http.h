/*
 * HTTP and HTTPS client for OPOS applications (HTTP/1.1, one request per
 * connection, no keep-alive).
 *
 * opos_http_get downloads a whole page into memory. It follows redirects,
 * decodes chunked transfer coding and gzip/deflate content coding, and
 * returns the final URL so relative links can be resolved against it.
 * Link build/libbearssl.a after libopos.a when you use it.
 *
 *   struct opos_http_response response;
 *   long error = opos_http_get("https://example.com/", 4 << 20, &response);
 *   if (error == 0) { use(response.body, response.length); }
 *   opos_http_free(&response);
 *
 * Results: 0 when a response arrived (check response.status: a 404 page is
 * still a successful call), or a negative error number (-OPOS_ENOENT: name
 * not found, -OPOS_EFBIG: body larger than max_body, a TLS error, ...; see
 * opos_tls_error_text).
 */
#ifndef OPOS_HTTP_H
#define OPOS_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OPOS_EFBIG 27
#define OPOS_HTTP_URL_MAX 2048
#define OPOS_HTTP_MAX_REDIRECTS 8

struct opos_http_response {
    long status;                          /* 200, 404, ... */
    char url[OPOS_HTTP_URL_MAX];          /* the URL after redirects */
    char content_type[128];               /* "text/html; charset=utf-8" */
    uint8_t *body;                        /* malloc'd, always NUL-terminated */
    size_t length;                        /* bytes in body, without the NUL */
};

/* Downloads url. body is NULL on error. Never returns more than max_body
   bytes: a larger body fails with -OPOS_EFBIG. */
long opos_http_get(const char *url, size_t max_body,
                   struct opos_http_response *response);

/* Sends body with a POST request (content_type such as
   "application/x-www-form-urlencoded") and reads the response like
   opos_http_get. A 301, 302 or 303 redirect is followed with a GET. */
long opos_http_post(const char *url, const char *content_type,
                    const void *body, size_t length, size_t max_body,
                    struct opos_http_response *response);

/* Receives a downloaded body piece by piece. A negative result stops the
   download, and opos_http_download returns it. */
typedef long (*opos_http_sink)(void *context, const uint8_t *data,
                               size_t length);

/* Like opos_http_get, but a 2xx body goes to sink as it arrives instead
   of into memory, so files larger than the heap can be downloaded. It asks
   for no content coding (gzip is not undone here). response->body is then
   empty and response->length is the number of bytes sent to sink. A
   non-2xx body (an error page) is still read into response->body. */
long opos_http_download(const char *url, size_t max_body,
                        opos_http_sink sink, void *context,
                        struct opos_http_response *response);

void opos_http_free(struct opos_http_response *response);

/* Optional progress report while the body arrives: bytes so far and the
   announced total (0 when unknown). Pass NULL to turn it off. */
typedef void (*opos_http_progress)(void *context, size_t received,
                                   size_t total);
void opos_http_set_progress(opos_http_progress callback, void *context);

/* Resolves reference (an absolute URL, "//host/path", "/path", "path",
   "?query", "#fragment") against the absolute URL base, removing "." and
   ".." segments. Returns false when it does not fit or base is not an
   http(s) URL. */
bool opos_url_resolve(const char *base, const char *reference, char *out,
                      size_t capacity);

/* Decompresses a gzip (RFC 1952), zlib (RFC 1950) or raw deflate
   (RFC 1951) stream. format: 0 detects gzip/zlib/raw by the header, 1 is
   raw deflate. On success *out is malloc'd and NUL-terminated and the
   result is 0; otherwise -OPOS_EINVAL (corrupt data) or -OPOS_EFBIG (more
   than max_output bytes). */
long opos_inflate(const uint8_t *input, size_t length, int format,
                  size_t max_output, uint8_t **out, size_t *out_length);

#endif
