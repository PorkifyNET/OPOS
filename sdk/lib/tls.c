/*
 * TLS client on BearSSL. The trust-anchor loading follows BearSSL's own
 * tools/certs.c (MIT licence, Thomas Pornin).
 */
#include <stdbool.h>

#include <bearssl.h>
#include <opos_http.h>
#include <opos_tls.h>
#include <stdlib.h>
#include <string.h>

struct opos_tls {
    long fd;
    bool peer_closed;
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    br_sslio_context io;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
};

/* ------------------------------------------------- trust anchors */

struct bytes {
    unsigned char *data;
    size_t length;
    size_t capacity;
};

static bool bytes_append(struct bytes *b, const void *data, size_t length) {
    if (b->length + length > b->capacity) {
        size_t capacity = b->capacity != 0 ? b->capacity * 2 : 1024;
        while (capacity < b->length + length) {
            capacity *= 2;
        }
        unsigned char *grown = realloc(b->data, capacity);
        if (grown == NULL) {
            return false;
        }
        b->data = grown;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, data, length);
    b->length += length;
    return true;
}

static void bytes_append_callback(void *context, const void *data,
                                  size_t length) {
    (void)bytes_append(context, data, length);
}

static unsigned char *copy_bytes(const void *data, size_t length) {
    unsigned char *copy = malloc(length != 0 ? length : 1);
    if (copy != NULL) {
        memcpy(copy, data, length);
    }
    return copy;
}

static br_x509_trust_anchor *anchors;
static size_t anchor_count;
static size_t anchor_capacity;
static bool anchors_loaded;

/* Adds one DER certificate as a trust anchor (skipped if unusable). */
static void add_anchor(const unsigned char *der, size_t length) {
    struct bytes dn = {0};
    br_x509_decoder_context decoder;
    br_x509_decoder_init(&decoder, bytes_append_callback, &dn);
    br_x509_decoder_push(&decoder, der, length);
    br_x509_pkey *key = br_x509_decoder_get_pkey(&decoder);
    if (key == NULL || dn.data == NULL) {
        free(dn.data);
        return;
    }
    if (anchor_count == anchor_capacity) {
        size_t capacity = anchor_capacity != 0 ? anchor_capacity * 2 : 64;
        br_x509_trust_anchor *grown =
            realloc(anchors, capacity * sizeof(*anchors));
        if (grown == NULL) {
            free(dn.data);
            return;
        }
        anchors = grown;
        anchor_capacity = capacity;
    }
    br_x509_trust_anchor *anchor = &anchors[anchor_count];
    memset(anchor, 0, sizeof(*anchor));
    anchor->dn.data = dn.data;
    anchor->dn.len = dn.length;
    anchor->flags = br_x509_decoder_isCA(&decoder) ? BR_X509_TA_CA : 0;
    anchor->pkey.key_type = key->key_type;
    if (key->key_type == BR_KEYTYPE_RSA) {
        anchor->pkey.key.rsa.n = copy_bytes(key->key.rsa.n, key->key.rsa.nlen);
        anchor->pkey.key.rsa.nlen = key->key.rsa.nlen;
        anchor->pkey.key.rsa.e = copy_bytes(key->key.rsa.e, key->key.rsa.elen);
        anchor->pkey.key.rsa.elen = key->key.rsa.elen;
    } else if (key->key_type == BR_KEYTYPE_EC) {
        anchor->pkey.key.ec.curve = key->key.ec.curve;
        anchor->pkey.key.ec.q = copy_bytes(key->key.ec.q, key->key.ec.qlen);
        anchor->pkey.key.ec.qlen = key->key.ec.qlen;
    } else {
        free(dn.data);
        return;
    }
    anchor_count++;
}

/* Reads the PEM bundle once per program. */
static void load_anchors(void) {
    if (anchors_loaded) {
        return;
    }
    anchors_loaded = true;
    long fd = opos_open(OPOS_TLS_CA_FILE, 0);
    if (fd < 0) {
        return;
    }
    br_pem_decoder_context pem;
    br_pem_decoder_init(&pem);
    struct bytes der = {0};
    bool in_certificate = false;
    unsigned char chunk[4096];
    bool end = false;
    while (!end) {
        long got = opos_read((int)fd, chunk, sizeof(chunk));
        size_t length;
        if (got <= 0) {
            /* A final newline makes the decoder finish the last line. */
            chunk[0] = '\n';
            length = 1;
            end = true;
        } else {
            length = (size_t)got;
        }
        const unsigned char *at = chunk;
        while (length > 0) {
            size_t used = br_pem_decoder_push(&pem, at, length);
            at += used;
            length -= used;
            switch (br_pem_decoder_event(&pem)) {
            case BR_PEM_BEGIN_OBJ:
                in_certificate = strcmp(br_pem_decoder_name(&pem),
                                        "CERTIFICATE") == 0
                    || strcmp(br_pem_decoder_name(&pem),
                              "X509 CERTIFICATE") == 0;
                der.length = 0;
                br_pem_decoder_setdest(&pem,
                                       in_certificate ? bytes_append_callback
                                                      : NULL,
                                       &der);
                break;
            case BR_PEM_END_OBJ:
                if (in_certificate && der.length != 0) {
                    add_anchor(der.data, der.length);
                }
                in_certificate = false;
                break;
            case BR_PEM_ERROR:
                br_pem_decoder_init(&pem);
                in_certificate = false;
                break;
            default:
                break;
            }
        }
    }
    free(der.data);
    opos_close((int)fd);
}

/* -------------------------------------------------- connections */

static int socket_read(void *context, unsigned char *buffer, size_t length) {
    struct opos_tls *tls = context;
    long got = opos_recv(tls->fd, buffer, length);
    if (got <= 0) {
        tls->peer_closed = got == 0;
        return -1;
    }
    return (int)got;
}

static int socket_write(void *context, const unsigned char *data,
                        size_t length) {
    struct opos_tls *tls = context;
    long sent = opos_send(tls->fd, data, length);
    return sent <= 0 ? -1 : (int)sent;
}

static long tls_failure(struct opos_tls *tls, long fallback) {
    int error = br_ssl_engine_last_error(&tls->client.eng);
    return error != BR_ERR_OK ? -(OPOS_TLS_ERROR_BASE + error) : fallback;
}

/* Runs the engine until the handshake is over (or failed). */
static long handshake(struct opos_tls *tls) {
    br_ssl_engine_context *engine = &tls->client.eng;
    for (;;) {
        unsigned state = br_ssl_engine_current_state(engine);
        if ((state & BR_SSL_CLOSED) != 0) {
            return tls_failure(tls, -OPOS_ECONNRESET);
        }
        if ((state & BR_SSL_SENDREC) != 0) {
            size_t length;
            unsigned char *data = br_ssl_engine_sendrec_buf(engine, &length);
            int sent = socket_write(tls, data, length);
            if (sent < 0) {
                return tls_failure(tls, -OPOS_ECONNRESET);
            }
            br_ssl_engine_sendrec_ack(engine, (size_t)sent);
            continue;
        }
        if ((state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0) {
            return 0;
        }
        if ((state & BR_SSL_RECVREC) != 0) {
            size_t length;
            unsigned char *data = br_ssl_engine_recvrec_buf(engine, &length);
            int got = socket_read(tls, data, length);
            if (got < 0) {
                return tls_failure(tls, -OPOS_ECONNRESET);
            }
            br_ssl_engine_recvrec_ack(engine, (size_t)got);
            continue;
        }
        br_ssl_engine_flush(engine, 0);
    }
}

long opos_tls_connect(const char *host, uint16_t port,
                      struct opos_tls **out) {
    *out = NULL;
    load_anchors();
    if (anchor_count == 0) {
        return -(OPOS_TLS_ERROR_BASE + BR_ERR_X509_NOT_TRUSTED);
    }
    uint8_t ip[4];
    long result = opos_net_resolve(host, ip);
    if (result < 0) {
        return result;
    }
    struct opos_tls *tls = malloc(sizeof(*tls));
    if (tls == NULL) {
        return -OPOS_ENOMEM;
    }
    memset(tls, 0, sizeof(*tls) - sizeof(tls->buffer));
    tls->fd = opos_socket(OPOS_SOCK_STREAM);
    if (tls->fd < 0) {
        result = tls->fd;
        free(tls);
        return result;
    }
    struct opos_sockaddr address = opos_net_address(ip, port);
    result = opos_connect(tls->fd, &address);
    if (result < 0) {
        opos_close((int)tls->fd);
        free(tls);
        return result;
    }

    br_ssl_client_init_full(&tls->client, &tls->x509, anchors, anchor_count);
    /* Certificate dates: days since 1 January of year 0, and seconds. */
    struct {
        int64_t seconds;
        int64_t nanoseconds;
    } now = {0, 0};
    if (opos_syscall2(228, 0 /* CLOCK_REALTIME */, (long)&now) == 0
        && now.seconds > 0) {
        br_x509_minimal_set_time(&tls->x509,
                                 (uint32_t)(now.seconds / 86400 + 719528),
                                 (uint32_t)(now.seconds % 86400));
    }
    br_ssl_engine_set_buffer(&tls->client.eng, tls->buffer,
                             sizeof(tls->buffer), 1);
    unsigned char seed[32];
    if (opos_syscall3(318, (long)seed, sizeof(seed), 0) != sizeof(seed)) {
        opos_close((int)tls->fd);
        free(tls);
        return -OPOS_EIO;
    }
    br_ssl_engine_inject_entropy(&tls->client.eng, seed, sizeof(seed));
    if (!br_ssl_client_reset(&tls->client, host, 0)) {
        result = tls_failure(tls, -OPOS_EINVAL);
        opos_close((int)tls->fd);
        free(tls);
        return result;
    }
    br_sslio_init(&tls->io, &tls->client.eng, socket_read, tls, socket_write,
                  tls);
    result = handshake(tls);
    if (result < 0) {
        opos_close((int)tls->fd);
        free(tls);
        return result;
    }
    *out = tls;
    return 0;
}

long opos_tls_write(struct opos_tls *tls, const void *data, size_t length) {
    if (br_sslio_write_all(&tls->io, data, length) < 0
        || br_sslio_flush(&tls->io) < 0) {
        return tls_failure(tls, -OPOS_ECONNRESET);
    }
    return (long)length;
}

long opos_tls_read(struct opos_tls *tls, void *buffer, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    int got = br_sslio_read(&tls->io, buffer, capacity);
    if (got > 0) {
        return got;
    }
    int error = br_ssl_engine_last_error(&tls->client.eng);
    /* A clean close_notify, or a server that just closed the connection
       (common with HTTP, which marks the end of the body itself). */
    if (error == BR_ERR_OK || (error == BR_ERR_IO && tls->peer_closed)) {
        return 0;
    }
    return -(OPOS_TLS_ERROR_BASE + error);
}

void opos_tls_close(struct opos_tls *tls) {
    if (tls == NULL) {
        return;
    }
    if (!tls->peer_closed
        && (br_ssl_engine_current_state(&tls->client.eng)
            & BR_SSL_CLOSED) == 0) {
        br_ssl_engine_close(&tls->client.eng);
        (void)br_sslio_flush(&tls->io);
    }
    opos_close((int)tls->fd);
    free(tls);
}

const char *opos_tls_error_text(long error) {
    if (error <= -OPOS_TLS_ERROR_BASE) {
        switch (-error - OPOS_TLS_ERROR_BASE) {
        case BR_ERR_X509_NOT_TRUSTED:
            return "certificate not issued by a trusted authority";
        case BR_ERR_X509_EXPIRED:
            return "certificate expired or not yet valid (check the clock)";
        case BR_ERR_X509_BAD_SERVER_NAME:
            return "certificate is for a different host name";
        case BR_ERR_X509_TIME_UNKNOWN:
            return "the clock is not set";
        case BR_ERR_BAD_VERSION:
        case BR_ERR_BAD_CIPHER_SUITE:
            return "the server needs a TLS version or cipher we lack";
        case BR_ERR_IO:
            return "connection lost";
        default:
            if (-error - OPOS_TLS_ERROR_BASE >= BR_ERR_SEND_FATAL_ALERT) {
                return "the server sent a fatal alert";
            }
            return "TLS failure";
        }
    }
    switch (-error) {
    case OPOS_ENOENT:
        return "host not found";
    case OPOS_ETIMEDOUT:
        return "timed out";
    case OPOS_ECONNREFUSED:
        return "connection refused";
    case OPOS_ECONNRESET:
        return "connection reset";
    case OPOS_ENETUNREACH:
        return "network is down";
    case OPOS_ENOMEM:
        return "out of memory";
    case OPOS_EFBIG:
        return "larger than expected";
    default:
        return "network error";
    }
}
