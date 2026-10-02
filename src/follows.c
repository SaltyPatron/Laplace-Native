/* The scalar vertex scan, the one every SIMD scan (isa/) must agree with; continuations themselves are path.c's. */
#include "laplace/laplace.h"
#include "internal.h"
#include <string.h>

size_t lp_scan_scalar(const uint8_t *v, size_t s, size_t n, const uint8_t key[24]){
    for (; s < n; s++) if (!memcmp(v + s * LP_VERTEX_BYTES, key, 24)) return s;
    return n;
}
