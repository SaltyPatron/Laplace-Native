/* AVX2 half chords from one direction u to the directions of four points at a time: one point per lane (the four
 * rows transposed), each step in the scalar path's order, with divides and square roots (correctly rounded, like the
 * scalar ones) and multiplies and adds kept separate. */
#include "../internal.h"
#include <immintrin.h>

void lp_half_chord_avx2(const double u[4], const double *pts, size_t i0, size_t n, double *out){
    const __m256d ux = _mm256_set1_pd(u[0]), uy = _mm256_set1_pd(u[1]), uz = _mm256_set1_pd(u[2]), um = _mm256_set1_pd(u[3]);
    const __m256d two = _mm256_set1_pd(2.0);
    size_t i = i0;
    for (; i + 4 <= n; i += 4) {
        const double *p = pts + 4 * i;
        __m256d p0 = _mm256_loadu_pd(p), p1 = _mm256_loadu_pd(p + 4), p2 = _mm256_loadu_pd(p + 8), p3 = _mm256_loadu_pd(p + 12);
        __m256d t0 = _mm256_unpacklo_pd(p0, p1), t1 = _mm256_unpackhi_pd(p0, p1);      /* x0 x1 z0 z1 | y0 y1 m0 m1 */
        __m256d t2 = _mm256_unpacklo_pd(p2, p3), t3 = _mm256_unpackhi_pd(p2, p3);
        __m256d x = _mm256_permute2f128_pd(t0, t2, 0x20), z = _mm256_permute2f128_pd(t0, t2, 0x31);
        __m256d y = _mm256_permute2f128_pd(t1, t3, 0x20), m = _mm256_permute2f128_pd(t1, t3, 0x31);
        __m256d s = _mm256_add_pd(_mm256_mul_pd(x, x), _mm256_mul_pd(y, y));
        s = _mm256_add_pd(s, _mm256_mul_pd(z, z));
        s = _mm256_add_pd(s, _mm256_mul_pd(m, m));
        __m256d len = _mm256_sqrt_pd(s);
        __m256d dx = _mm256_sub_pd(ux, _mm256_div_pd(x, len)), dy = _mm256_sub_pd(uy, _mm256_div_pd(y, len));
        __m256d dz = _mm256_sub_pd(uz, _mm256_div_pd(z, len)), dm = _mm256_sub_pd(um, _mm256_div_pd(m, len));
        __m256d d = _mm256_add_pd(_mm256_mul_pd(dx, dx), _mm256_mul_pd(dy, dy));
        d = _mm256_add_pd(d, _mm256_mul_pd(dz, dz));
        d = _mm256_add_pd(d, _mm256_mul_pd(dm, dm));
        _mm256_storeu_pd(out + i, _mm256_div_pd(_mm256_sqrt_pd(d), two));
    }
    lp_half_chord_scalar(u, pts, i, n, out);
}
