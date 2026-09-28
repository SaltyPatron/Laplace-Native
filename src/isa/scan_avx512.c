/* AVX-512 vertex scan: two vertices per 512-bit load, compared to the key on X, Y, Z of each. */
#include "../internal.h"
#include <immintrin.h>

size_t lp_scan_avx512(const uint8_t *v, size_t s, size_t n, const uint8_t key[24]){
    const int64_t *k = (const int64_t *)key;
    const __m512i kk = _mm512_setr_epi64(k[0], k[1], k[2], 0, k[0], k[1], k[2], 0);
    for (; s + 2 <= n; s += 2) {
        __m512i x = _mm512_loadu_si512((const void *)(v + s * LP_VERTEX_BYTES));
        __mmask8 m = _mm512_mask_cmpeq_epi64_mask(0x77, x, kk);
        if ((m & 0x07) == 0x07) return s;
        if ((m & 0x70) == 0x70) return s + 1;
    }
    if (s < n) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(v + s * LP_VERTEX_BYTES));
        __m256i kh = _mm512_castsi512_si256(kk);
        if ((_mm256_mask_cmpeq_epi64_mask(0x7, x, kh) & 7) == 7) return s;
        s++;
    }
    return n;
}
