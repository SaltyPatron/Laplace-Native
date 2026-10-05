/* AVX-512 (BW) int8 dot: vpmaddwd on 32 widened bytes per step, the tail one masked step. Compiled without VNNI: the
 * path for x86-64-v4 alone. */
#include "../internal.h"
#include <immintrin.h>

static inline __m512i step(__m512i s, __m256i x, __m256i y){
    return _mm512_add_epi32(s, _mm512_madd_epi16(_mm512_cvtepi8_epi16(x), _mm512_cvtepi8_epi16(y)));
}

int32_t lp_dot_i8_avx512(const int8_t *a, const int8_t *b, size_t n){
    __m512i s0 = _mm512_setzero_si512(), s1 = s0;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        s0 = step(s0, _mm256_loadu_si256((const __m256i *)(a + i)), _mm256_loadu_si256((const __m256i *)(b + i)));
        s1 = step(s1, _mm256_loadu_si256((const __m256i *)(a + i + 32)), _mm256_loadu_si256((const __m256i *)(b + i + 32)));
    }
    for (; i < n; i += 32) {
        __mmask32 k = n - i >= 32 ? 0xFFFFFFFFu : (__mmask32)((1u << (n - i)) - 1);
        s0 = step(s0, _mm256_maskz_loadu_epi8(k, a + i), _mm256_maskz_loadu_epi8(k, b + i));
    }
    return (int32_t)(uint32_t)_mm512_reduce_add_epi32(_mm512_add_epi32(s0, s1));
}

void lp_dot_i8_batch_avx512(const int8_t *q, const int8_t *rows, size_t nrows, size_t n, size_t stride, int32_t *out){
    for (size_t r = 0; r < nrows; r++) out[r] = lp_dot_i8_avx512(q, rows + r * stride, n);
}
