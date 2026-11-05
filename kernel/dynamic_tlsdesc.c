#include <stdint.h>

extern __thread uint64_t opos_base_tls_value;
extern __thread uint64_t opos_base_tls_zero;
extern __thread uint64_t opos_consumer_tls_value
    __attribute__((visibility("hidden")));
extern __thread uint64_t opos_consumer_tls_zero
    __attribute__((visibility("hidden")));
extern __thread uint64_t opos_optional_tls __attribute__((weak));

uint64_t opos_shared_tlsdesc_probe(void) {
    if (opos_base_tls_value != 17 || opos_base_tls_zero != 8
        || opos_consumer_tls_value != 23 || opos_consumer_tls_zero != 6) {
        return 0;
    }
    if (&opos_optional_tls != 0) return 0;
    opos_base_tls_zero = 9;
    opos_consumer_tls_zero = 7;
    return 54;
}
