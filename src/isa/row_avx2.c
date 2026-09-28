/* AVX2 squared distances from one 4D point to four vertices at a time: one vertex pair per lane, summed in the
 * same order as the scalar path, ((dx^2 + dy^2) + dz^2) + dm^2, with multiplies and adds kept separate. */
#include "../internal.h"
#include <immintrin.h>

void lp_row_d2_avx2(const double p[4], const double *bx, const double *by, const double *bz, const double *bm,
                    size_t j0, size_t j1, double *out){
    const __m256d px = _mm256_set1_pd(p[0]), py = _mm256_set1_pd(p[1]), pz = _mm256_set1_pd(p[2]), pm = _mm256_set1_pd(p[3]);
    size_t j = j0;
    for (; j + 4 <= j1; j += 4) {
        __m256d dx = _mm256_sub_pd(px, _mm256_loadu_pd(bx + j)), dy = _mm256_sub_pd(py, _mm256_loadu_pd(by + j));
        __m256d dz = _mm256_sub_pd(pz, _mm256_loadu_pd(bz + j)), dm = _mm256_sub_pd(pm, _mm256_loadu_pd(bm + j));
        __m256d s = _mm256_add_pd(_mm256_mul_pd(dx, dx), _mm256_mul_pd(dy, dy));
        s = _mm256_add_pd(s, _mm256_mul_pd(dz, dz));
        s = _mm256_add_pd(s, _mm256_mul_pd(dm, dm));
        _mm256_storeu_pd(out + j, s);
    }
    lp_row_d2_scalar(p, bx, by, bz, bm, j, j1, out);
}
