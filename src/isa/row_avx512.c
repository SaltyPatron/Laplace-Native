/* AVX-512 squared distances from one 4D point to eight vertices at a time: one vertex pair per lane, summed in the
 * same order as the scalar path, ((dx^2 + dy^2) + dz^2) + dm^2, with multiplies and adds kept separate. The tail is
 * one masked step: lanes past the row are neither loaded nor stored. */
#include "../internal.h"
#include <immintrin.h>

static inline __m512d row8(__m512d px, __m512d py, __m512d pz, __m512d pm, __m512d x, __m512d y, __m512d z, __m512d m){
    __m512d dx = _mm512_sub_pd(px, x), dy = _mm512_sub_pd(py, y), dz = _mm512_sub_pd(pz, z), dm = _mm512_sub_pd(pm, m);
    __m512d s = _mm512_add_pd(_mm512_mul_pd(dx, dx), _mm512_mul_pd(dy, dy));
    s = _mm512_add_pd(s, _mm512_mul_pd(dz, dz));
    return _mm512_add_pd(s, _mm512_mul_pd(dm, dm));
}

void lp_row_d2_avx512(const double p[4], const double *bx, const double *by, const double *bz, const double *bm,
                      size_t j0, size_t j1, double *out){
    const __m512d px = _mm512_set1_pd(p[0]), py = _mm512_set1_pd(p[1]), pz = _mm512_set1_pd(p[2]), pm = _mm512_set1_pd(p[3]);
    size_t j = j0;
    for (; j + 8 <= j1; j += 8)
        _mm512_storeu_pd(out + j, row8(px, py, pz, pm, _mm512_loadu_pd(bx + j), _mm512_loadu_pd(by + j),
                                       _mm512_loadu_pd(bz + j), _mm512_loadu_pd(bm + j)));
    if (j < j1) {
        __mmask8 k = (__mmask8)((1u << (j1 - j)) - 1);
        _mm512_mask_storeu_pd(out + j, k, row8(px, py, pz, pm, _mm512_maskz_loadu_pd(k, bx + j), _mm512_maskz_loadu_pd(k, by + j),
                                               _mm512_maskz_loadu_pd(k, bz + j), _mm512_maskz_loadu_pd(k, bm + j)));
    }
}
