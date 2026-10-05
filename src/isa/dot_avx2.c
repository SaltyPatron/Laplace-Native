/* AVX2 int8 dot: each byte widened to 16 bits, pairs multiplied and added to 32 bits (vpmaddwd: exact, as no pair
 * reaches 2^31), lanes summed modulo 2^32. Compiled without VNNI: this is the path for x86-64-v3 alone. */
#include "../internal.h"
#include <immintrin.h>

static inline uint32_t hsum(__m256i v){
    uint32_t l[8], s = 0; _mm256_storeu_si256((__m256i *)l, v);
    for (int i = 0; i < 8; i++) s += l[i];
    return s;
}

int32_t lp_dot_i8_avx2(const int8_t *a, const int8_t *b, size_t n){
    __m256i s0 = _mm256_setzero_si256(), s1 = _mm256_setzero_si256();
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(a + i)), y = _mm256_loadu_si256((const __m256i *)(b + i));
        s0 = _mm256_add_epi32(s0, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm256_castsi256_si128(x)), _mm256_cvtepi8_epi16(_mm256_castsi256_si128(y))));
        s1 = _mm256_add_epi32(s1, _mm256_madd_epi16(_mm256_cvtepi8_epi16(_mm256_extracti128_si256(x, 1)), _mm256_cvtepi8_epi16(_mm256_extracti128_si256(y, 1))));
    }
    return (int32_t)(hsum(_mm256_add_epi32(s0, s1)) + (uint32_t)lp_dot_i8_scalar(a + i, b + i, n - i));
}

void lp_dot_i8_batch_avx2(const int8_t *q, const int8_t *rows, size_t nrows, size_t n, size_t stride, int32_t *out){
    for (size_t r = 0; r < nrows; r++) out[r] = lp_dot_i8_avx2(q, rows + r * stride, n);
}
