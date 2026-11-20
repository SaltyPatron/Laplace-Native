/* AVX2 / F16C tensor element widening kernels (compiled under LP_ISA_V3). */
#include "../core/internal.h"
#include <immintrin.h>

void lp_bf16_to_f32_avx2(const uint16_t *in, size_t n, float *out){
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i v16 = _mm_loadu_si128((const __m128i *)(in + i));
        __m256i v32 = _mm256_slli_epi32(_mm256_cvtepu16_epi32(v16), 16);
        _mm256_storeu_ps(out + i, _mm256_castsi256_ps(v32));
    }
    if (i < n) lp_bf16_to_f32_scalar(in + i, n - i, out + i);
}

void lp_f16_to_f32_avx2(const uint16_t *in, size_t n, float *out){
#if defined(__F16C__)
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m128i v16 = _mm_loadu_si128((const __m128i *)(in + i));
        __m256 vf = _mm256_cvtph_ps(v16);
        _mm256_storeu_ps(out + i, vf);
    }
    if (i < n) lp_f16_to_f32_scalar(in + i, n - i, out + i);
#else
    lp_f16_to_f32_scalar(in, n, out);
#endif
}
