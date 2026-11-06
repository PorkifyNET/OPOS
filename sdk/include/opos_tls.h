/*
 * TLS (HTTPS) client connections for OPOS applications, built on BearSSL
 * (bearssl/, MIT licence). Link build/libbearssl.a after libopos.a.
 *
 * The server's certificate is checked against the root certificates in
 * /etc/ssl/certs/ca-certificates.crt, its name against the host name, and
 * its dates against the clock. TLS 1.0 to 1.2; no TLS 1.3 (BearSSL has
 * none) and no client certificates.
 *
 *   struct opos_tls *tls;
 *   long error = opos_tls_connect("example.com", 443, &tls);
 *   if (error < 0) printf("%s\n", opos_tls_error_text(error));
 *   opos_tls_write(tls, request, length);
 *   while ((got = opos_tls_read(tls, buffer, sizeof(buffer))) > 0) ...
 *   opos_tls_close(tls);
 */
#ifndef OPOS_TLS_H
#define OPOS_TLS_H

#include <stdbool.h>

#include <opos_net.h>

#define OPOS_TLS_CA_FILE "/etc/ssl/certs/ca-certificates.crt"

/* TLS failures are -(OPOS_TLS_ERROR_BASE + BearSSL error code); other
   negative results are -errno from the network calls. */
#define OPOS_TLS_ERROR_BASE 1000

struct opos_tls;

/* Resolves host, connects and completes the TLS handshake. */
long opos_tls_connect(const char *host, uint16_t port, struct opos_tls **out);

/* Sends all of data; returns length or an error. */
long opos_tls_write(struct opos_tls *tls, const void *data, size_t length);

/* Returns bytes read, 0 when the server has closed, or an error. */
long opos_tls_read(struct opos_tls *tls, void *buffer, size_t capacity);

/* Ends the session, closes the socket and frees tls. */
void opos_tls_close(struct opos_tls *tls);

/* A short English description of an error from the calls above. */
const char *opos_tls_error_text(long error);

#endif
