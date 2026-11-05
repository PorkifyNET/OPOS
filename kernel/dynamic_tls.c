#include <stdint.h>

extern __thread uint64_t opos_base_tls_value;
extern __thread uint64_t opos_base_tls_zero;
extern __thread uint64_t opos_consumer_tls_value
    __attribute__((tls_model("local-dynamic"), visibility("hidden")));
extern __thread uint64_t opos_consumer_tls_zero
    __attribute__((tls_model("local-dynamic"), visibility("hidden")));

uint64_t opos_shared_dynamic_tls_probe(void) {
    if (opos_base_tls_value != 17 || opos_base_tls_zero != 7
        || opos_consumer_tls_value != 23 || opos_consumer_tls_zero != 5) {
        return 0;
    }
    opos_base_tls_zero = 8;
    opos_consumer_tls_zero = 6;
    return opos_base_tls_value + opos_base_tls_zero
        + opos_consumer_tls_value + opos_consumer_tls_zero;
}
