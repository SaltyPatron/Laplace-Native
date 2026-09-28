/* 4D geometry on real coordinates: Euclidean distance and discrete Fréchet distance between trajectories.
 *
 * Every squared distance is summed in one fixed order, ((dx^2 + dy^2) + dz^2) + dm^2, in every code path, so the
 * scalar and SIMD kernels give the same bits. The SIMD kernel puts one vertex pair in each lane rather than one axis
 * in each lane, which keeps that order. */
#include "laplace/laplace.h"
#include "internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline double d2(const double *a, const double *b){
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2], dm = a[3] - b[3];
    return ((dx * dx + dy * dy) + dz * dz) + dm * dm;
}

double lp_distance4(const double a[4], const double b[4]){ return sqrt(d2(a, b)); }

/* Squared distances from one point p to b[j0..j1) (b in structure-of-arrays form). */
void lp_row_d2_scalar(const double p[4], const double *bx, const double *by, const double *bz, const double *bm,
                      size_t j0, size_t j1, double *out){
    for (size_t j = j0; j < j1; j++) {
        double dx = p[0] - bx[j], dy = p[1] - by[j], dz = p[2] - bz[j], dm = p[3] - bm[j];
        out[j] = ((dx * dx + dy * dy) + dz * dz) + dm * dm;
    }
}

typedef void (*row_fn)(const double *, const double *, const double *, const double *, const double *, size_t, size_t, double *);
static row_fn pick_row(void){
#if defined(LP_HAVE_AVX2)
    if (lp_cpu_active() & LP_CPU_AVX2) return lp_row_d2_avx2;
#endif
    return lp_row_d2_scalar;
}

/* Discrete Fréchet distance (Eiter and Mannila) between 4D vertex sequences a (na x 4) and b (nb x 4), row by row
 * in O(nb) memory. Returns the distance, not its square. */
double lp_frechet4(const double *a, size_t na, const double *b, size_t nb){
    if (!na || !nb) return INFINITY;
    static row_fn row; if (!row) row = pick_row();
    double *bx = malloc(sizeof(double) * nb * 6), *by = bx + nb, *bz = by + nb, *bm = bz + nb, *prev = bm + nb, *cur = prev + nb;
    for (size_t j = 0; j < nb; j++) { bx[j] = b[4 * j]; by[j] = b[4 * j + 1]; bz[j] = b[4 * j + 2]; bm[j] = b[4 * j + 3]; }
    for (size_t i = 0; i < na; i++) {
        row(a + 4 * i, bx, by, bz, bm, 0, nb, cur);                                  /* cur[j] = d2(a_i, b_j) */
        if (i == 0) { for (size_t j = 1; j < nb; j++) if (cur[j] < cur[j - 1]) cur[j] = cur[j - 1]; }
        else {
            cur[0] = cur[0] > prev[0] ? cur[0] : prev[0];
            for (size_t j = 1; j < nb; j++) {
                double m = prev[j - 1]; if (prev[j] < m) m = prev[j]; if (cur[j - 1] < m) m = cur[j - 1];
                if (cur[j] < m) cur[j] = m;
            }
        }
        double *t = prev; prev = cur; cur = t;
    }
    double r = sqrt(prev[nb - 1]); free(bx);
    return r;
}

/* The exact centroid of 4D points given as doubles that are fixed-point values m / 2^53. */
bool lp_centroid4_exact(const double *p, size_t n, double out[4]){
    if (!n) return false;
    __int128 s[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < n; i++) for (int d = 0; d < 4; d++) {
        double m = p[4 * i + d] * LP_FIXED_ONE;
        if (m != (double)(int64_t)m) return false;                                    /* not a fixed-point value */
        s[d] += (int64_t)m;
    }
    for (int d = 0; d < 4; d++) out[d] = (double)(int64_t)(s[d] / (__int128)n) / LP_FIXED_ONE;
    return true;
}
