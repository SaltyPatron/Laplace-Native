/* 4D geometry on real coordinates: Euclidean distance, and the shape measures between trajectories (discrete Fréchet,
 * Fréchet with outliers skipped, DTW, EDR).
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

/* b as structure of arrays, with room for `extra` more doubles after it: one allocation for a measure's whole state. */
static double *soa(const double *b, size_t nb, size_t extra, double **bx, double **by, double **bz, double **bm){
    double *p = lp_alloc(sizeof(double) * (4 * nb + extra));
    *bx = p; *by = p + nb; *bz = *by + nb; *bm = *bz + nb;
    for (size_t j = 0; j < nb; j++) { (*bx)[j] = b[4 * j]; (*by)[j] = b[4 * j + 1]; (*bz)[j] = b[4 * j + 2]; (*bm)[j] = b[4 * j + 3]; }
    return p;
}

/* Discrete Fréchet distance (Eiter and Mannila) between 4D vertex sequences a (na x 4) and b (nb x 4), row by row
 * in O(nb) memory. Returns the distance, not its square. */
double lp_frechet4(const double *a, size_t na, const double *b, size_t nb){
    if (!na || !nb) return INFINITY;
    lp_row_fn row = lp_kernels()->row_d2;
    double *bx, *by, *bz, *bm, *mem = soa(b, nb, 2 * nb, &bx, &by, &bz, &bm), *prev = bm + nb, *cur = prev + nb;
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
    double r = sqrt(prev[nb - 1]); lp_free(mem);
    return r;
}

/* Discrete Fréchet distance with up to k interior vertices of each sequence skipped (k-outlier; cf. arXiv:2202.12824).
 * F[i][j][u][v]: the least largest gap of a walk ending at (a_i, b_j) that skipped u vertices of a and v of b. A step
 * of two along a sequence skips the vertex between. Three rows of i are kept. */
double lp_frechet4_outliers(const double *a, size_t na, const double *b, size_t nb, unsigned k){
    if (!na || !nb) return INFINITY;
    if (k == 0) return lp_frechet4(a, na, b, nb);
    lp_row_fn row = lp_kernels()->row_d2;
    size_t K = (size_t)k + 1, cell = K * K, rl = nb * cell;
    double *bx, *by, *bz, *bm, *mem = soa(b, nb, 3 * rl + nb, &bx, &by, &bz, &bm);
    double *F[3] = { bm + nb, bm + nb + rl, bm + nb + 2 * rl }, *c = bm + nb + 3 * rl;
    for (size_t i = 0; i < na; i++) {
        row(a + 4 * i, bx, by, bz, bm, 0, nb, c);
        double *cur = F[i % 3];
        for (size_t j = 0; j < nb; j++) for (size_t u = 0; u < K; u++) for (size_t v = 0; v < K; v++) {
            double *out = &cur[j * cell + u * K + v];
            if (i == 0 && j == 0) { *out = (u == 0 && v == 0) ? c[0] : INFINITY; continue; }
            double best = INFINITY;
            for (size_t di = 0; di <= 2 && di <= i; di++) for (size_t dj = 0; dj <= 2 && dj <= j; dj++) {
                if (!di && !dj) continue;
                if ((di == 2 && u == 0) || (dj == 2 && v == 0)) continue;
                double p = F[(i - di) % 3][(j - dj) * cell + (u - (di == 2)) * K + (v - (dj == 2))];
                if (p < best) best = p;
            }
            *out = best > c[j] ? best : c[j];
        }
    }
    double r = INFINITY, *last = &F[(na - 1) % 3][(nb - 1) * cell];
    for (size_t x = 0; x < cell; x++) if (last[x] < r) r = last[x];
    lp_free(mem);
    return sqrt(r);
}

/* Dynamic time warping: the least sum of gaps over walks of both sequences, and the length of that walk. Among equal
 * sums the shorter walk is taken. */
double lp_dtw4(const double *a, size_t na, const double *b, size_t nb, size_t *steps){
    if (steps) *steps = 0;
    if (!na || !nb) return INFINITY;
    lp_row_fn row = lp_kernels()->row_d2;
    double *bx, *by, *bz, *bm, *mem = soa(b, nb, 3 * nb + 2, &bx, &by, &bz, &bm);
    double *c = bm + nb, *prev = c + nb, *cur = prev + nb + 1;
    size_t *lbase = lp_alloc(sizeof(size_t) * 2 * (nb + 1)), *lp = lbase, *lc = lp + nb + 1;
    for (size_t j = 0; j <= nb; j++) { prev[j] = j ? INFINITY : 0.0; lp[j] = 0; }
    for (size_t i = 0; i < na; i++) {
        row(a + 4 * i, bx, by, bz, bm, 0, nb, c);
        cur[0] = INFINITY; lc[0] = 0;
        for (size_t j = 1; j <= nb; j++) {
            double f = prev[j]; size_t l = lp[j];
            if (cur[j - 1] < f || (cur[j - 1] == f && lc[j - 1] < l)) { f = cur[j - 1]; l = lc[j - 1]; }
            if (prev[j - 1] < f || (prev[j - 1] == f && lp[j - 1] < l)) { f = prev[j - 1]; l = lp[j - 1]; }
            cur[j] = f + sqrt(c[j - 1]); lc[j] = l + 1;
        }
        double *t = prev; prev = cur; cur = t; size_t *tl = lp; lp = lc; lc = tl;
    }
    double r = prev[nb]; if (steps) *steps = lp[nb];
    lp_free(lbase); lp_free(mem);
    return r;
}

/* Edit distance on real sequences (Chen, Özsu, Oria): the edits that turn one sequence into the other, two vertices
 * counting as equal when they lie within eps of each other. */
size_t lp_edr4(const double *a, size_t na, const double *b, size_t nb, double eps){
    if (!na || !nb) return na + nb;
    lp_row_fn row = lp_kernels()->row_d2;
    double *bx, *by, *bz, *bm, *mem = soa(b, nb, nb, &bx, &by, &bz, &bm);
    double *c = bm + nb;
    size_t *base = lp_alloc(sizeof(size_t) * 2 * (nb + 1)), *prev = base, *cur = prev + nb + 1;
    for (size_t j = 0; j <= nb; j++) prev[j] = j;
    for (size_t i = 0; i < na; i++) {
        row(a + 4 * i, bx, by, bz, bm, 0, nb, c);
        cur[0] = i + 1;
        for (size_t j = 1; j <= nb; j++) {
            size_t e = prev[j - 1] + (sqrt(c[j - 1]) <= eps ? 0 : 1);
            if (prev[j] + 1 < e) e = prev[j] + 1;
            if (cur[j - 1] + 1 < e) e = cur[j - 1] + 1;
            cur[j] = e;
        }
        size_t *t = prev; prev = cur; cur = t;
    }
    size_t r = prev[nb];
    lp_free(base); lp_free(mem);
    return r;
}

/* The exact centroid of 4D points given as doubles that are fixed-point values m / 2^53: the one centroid. */
bool lp_centroid4_exact(const double *p, size_t n, double out[4]){
    if (!n) return false;
    lp_coord_sum a = { { 0, 0, 0, 0 }, 0 }; lp_coord c;
    for (size_t i = 0; i < n; i++) { if (!lp_coord_of_xyzm(p + 4 * i, &c)) return false; lp_coord_add(&a, c.m); }
    lp_coord_mean(&a, &c); lp_coord_xyzm(&c, out);
    return true;
}
