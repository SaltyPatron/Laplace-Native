/* AVX2 vertex scan: one 256-bit load per vertex (X, Y, Z, M), compared to the key on X, Y, Z. */
#include "../internal.h"
#include <immintrin.h>

size_t lp_scan_avx2(const uint8_t *v, size_t s, size_t n, const uint8_t key[24]){
    const __m256i k = _mm256_setr_epi64x(((const int64_t *)key)[0], ((const int64_t *)key)[1], ((const int64_t *)key)[2], 0);
    for (; s + 4 <= n; s += 4) {
        for (int u = 0; u < 4; u++) {
            __m256i x = _mm256_loadu_si256((const __m256i *)(v + (s + u) * LP_VERTEX_BYTES));
            int mask = _mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(x, k)));
            if ((mask & 7) == 7) return s + u;
        }
    }
    for (; s < n; s++) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(v + s * LP_VERTEX_BYTES));
        if ((_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpeq_epi64(x, k))) & 7) == 7) return s;
    }
    return n;
}
