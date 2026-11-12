#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <bearssl_ec.h>

#include "sha256.h"
#include "signature.h"

bool signature_valid(const uint8_t public_key[65], const uint8_t *text,
                     size_t length, const uint8_t *signature,
                     size_t signature_length) {
    if (public_key == NULL || text == NULL || signature == NULL
        || signature_length == 0) {
        return false;
    }
    uint8_t hash[SHA256_DIGEST_SIZE];
    struct sha256_context context;
    sha256_init(&context);
    sha256_update(&context, text, length);
    sha256_final(&context, hash);
    br_ec_public_key key = {BR_EC_secp256r1, (unsigned char *)public_key,
                            65};
    return br_ecdsa_i31_vrfy_asn1(&br_ec_p256_m31, hash, sizeof(hash), &key,
                                  signature, signature_length) == 1;
}
