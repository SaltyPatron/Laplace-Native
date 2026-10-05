/* AVX-512 VNNI int8 dot (vpdpbusd, EVEX encoding), 64 bytes per step, the tail one masked step: a is taken as
 * a ^ 0x80 = a + 128, unsigned, and 128 * sum(b) taken off again, as in the AVX-VNNI kernel. A masked-off byte of b
 * is 0, so it adds nothing to either sum. */
#include "../internal.h"
#include <immintrin.h>

static inline __mmask64 tail(size_t left){ return left >= 64 ? ~0ull : (1ull << left) - 1; }

int32_t lp_dot_i8_avx512vnni(const int8_t *a, const int8_t *b, size_t n){
    const __m512i h = _mm512_set1_epi8((char)0x80);
    __m512i s0 = _mm512_setzero_si512(), s1 = s0, c0 = s0, c1 = s0;
    size_t i = 0;
    for (; i + 128 <= n; i += 128) {
        __m512i y0 = _mm512_loadu_si512((const void *)(b + i)), y1 = _mm512_loadu_si512((const void *)(b + i + 64));
        s0 = _mm512_dpbusd_epi32(s0, _mm512_xor_si512(_mm512_loadu_si512((const void *)(a + i)), h), y0);
        s1 = _mm512_dpbusd_epi32(s1, _mm512_xor_si512(_mm512_loadu_si512((const void *)(a + i + 64)), h), y1);
        c0 = _mm512_dpbusd_epi32(c0, h, y0);
        c1 = _mm512_dpbusd_epi32(c1, h, y1);
    }
    for (; i < n; i += 64) {
        __mmask64 k = tail(n - i);
        __m512i y = _mm512_maskz_loadu_epi8(k, b + i);
        s0 = _mm512_dpbusd_epi32(s0, _mm512_xor_si512(_mm512_maskz_loadu_epi8(k, a + i), h), y);
        c0 = _mm512_dpbusd_epi32(c0, h, y);
    }
    return (int32_t)((uint32_t)_mm512_reduce_add_epi32(_mm512_add_epi32(s0, s1))
                   - (uint32_t)_mm512_reduce_add_epi32(_mm512_add_epi32(c0, c1)));
}

/* Four rows at a time against one query: the query's bytes are the signed side, each row's the unsigned. */
void lp_dot_i8_batch_avx512vnni(const int8_t *q, const int8_t *rows, size_t nrows, size_t n, size_t stride, int32_t *out){
    const __m512i h = _mm512_set1_epi8((char)0x80);
    uint32_t c = 0;                                                       /* 128 * sum(q), modulo 2^32 */
    for (size_t i = 0; i < n; i++) c += (uint32_t)(128 * (int32_t)q[i]);
    size_t r = 0;
    for (; r + 4 <= nrows; r += 4) {
        const int8_t *p = rows + r * stride;
        __m512i s[4] = { _mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512(), _mm512_setzero_si512() };
        for (size_t i = 0; i < n; i += 64) {
            __mmask64 k = tail(n - i);                                    /* masked-off bytes of q are 0 */
            __m512i y = _mm512_maskz_loadu_epi8(k, q + i);
            for (int j = 0; j < 4; j++)
                s[j] = _mm512_dpbusd_epi32(s[j], _mm512_xor_si512(_mm512_maskz_loadu_epi8(k, p + j * stride + i), h), y);
        }
        for (int j = 0; j < 4; j++) out[r + j] = (int32_t)((uint32_t)_mm512_reduce_add_epi32(s[j]) - c);
    }
    for (; r < nrows; r++) out[r] = lp_dot_i8_avx512vnni(q, rows + r * stride, n);
}
