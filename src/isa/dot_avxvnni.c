/* AVX-VNNI int8 dot (vpdpbusd, VEX encoding). vpdpbusd multiplies unsigned bytes by signed ones, so a is taken as
 * a ^ 0x80 = a + 128, unsigned, and 128 * sum(b) is taken off again: a vpdpbusd of 0x80 bytes with b, or for a batch
 * 128 * sum(q) once. Every sum wraps modulo 2^32 alike, so the difference is the scalar path's value. */
#include "../internal.h"
#include <immintrin.h>

static inline uint32_t hsum(__m256i v){
    uint32_t l[8], s = 0; _mm256_storeu_si256((__m256i *)l, v);
    for (int i = 0; i < 8; i++) s += l[i];
    return s;
}

int32_t lp_dot_i8_avxvnni(const int8_t *a, const int8_t *b, size_t n){
    const __m256i h = _mm256_set1_epi8((char)0x80);
    __m256i s0 = _mm256_setzero_si256(), s1 = s0, c0 = s0, c1 = s0;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        __m256i y0 = _mm256_loadu_si256((const __m256i *)(b + i)), y1 = _mm256_loadu_si256((const __m256i *)(b + i + 32));
        s0 = _mm256_dpbusd_avx_epi32(s0, _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)(a + i)), h), y0);
        s1 = _mm256_dpbusd_avx_epi32(s1, _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)(a + i + 32)), h), y1);
        c0 = _mm256_dpbusd_avx_epi32(c0, h, y0);
        c1 = _mm256_dpbusd_avx_epi32(c1, h, y1);
    }
    for (; i + 32 <= n; i += 32) {
        __m256i y = _mm256_loadu_si256((const __m256i *)(b + i));
        s0 = _mm256_dpbusd_avx_epi32(s0, _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)(a + i)), h), y);
        c0 = _mm256_dpbusd_avx_epi32(c0, h, y);
    }
    uint32_t s = hsum(_mm256_add_epi32(s0, s1)) - hsum(_mm256_add_epi32(c0, c1));
    return (int32_t)(s + (uint32_t)lp_dot_i8_scalar(a + i, b + i, n - i));
}

/* Four rows at a time against one query: the query's bytes are the signed side, each row's the unsigned. */
void lp_dot_i8_batch_avxvnni(const int8_t *q, const int8_t *rows, size_t nrows, size_t n, size_t stride, int32_t *out){
    const __m256i h = _mm256_set1_epi8((char)0x80);
    size_t nv = n & ~(size_t)31;
    uint32_t c = 0;                                                       /* 128 * sum(q[0..nv)), modulo 2^32 */
    for (size_t i = 0; i < nv; i++) c += (uint32_t)(128 * (int32_t)q[i]);
    size_t r = 0;
    for (; r + 4 <= nrows; r += 4) {
        const int8_t *p = rows + r * stride;
        __m256i s[4] = { _mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256() };
        for (size_t i = 0; i < nv; i += 32) {
            __m256i y = _mm256_loadu_si256((const __m256i *)(q + i));
            for (int k = 0; k < 4; k++)
                s[k] = _mm256_dpbusd_avx_epi32(s[k], _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)(p + k * stride + i)), h), y);
        }
        for (int k = 0; k < 4; k++)
            out[r + k] = (int32_t)(hsum(s[k]) - c + (uint32_t)lp_dot_i8_scalar(q + nv, p + k * stride + nv, n - nv));
    }
    for (; r < nrows; r++) out[r] = lp_dot_i8_avxvnni(q, rows + r * stride, n);
}
