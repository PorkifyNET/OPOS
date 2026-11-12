#ifndef OPOS_SIGNATURE_H
#define OPOS_SIGNATURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * True when signature (ASN.1 DER, as `openssl dgst -sha256 -sign` writes
 * it) is a valid ECDSA P-256 signature of SHA-256(text) by public_key, an
 * uncompressed point (04 || X || Y). Used for system updates
 * (update_key.h) and the OPOS Store index (store_key.h), which have
 * separate keys.
 */
bool signature_valid(const uint8_t public_key[65], const uint8_t *text,
                     size_t length, const uint8_t *signature,
                     size_t signature_length);

#endif
