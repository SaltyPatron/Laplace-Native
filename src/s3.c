/* S^3 math: the angle between directions, the log and exp maps, slerp, the Karcher mean and Markley's eigen-centroid.
 * Ported from the monorepo's engine/core math4d.c (log/exp/Karcher) and the old laplace_pg s3_ops.c (Markley).
 *
 * Every sum of four products is taken ((x + y) + z) + m, as lp_distance4 takes it, and every reduction over points
 * visits them in one canonical order, so a result does not depend on the order the points arrive in. */
#include "laplace/laplace.h"
#include "internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline double dot4(const double a[4], const double b[4]){ return ((a[0] * b[0] + a[1] * b[1]) + a[2] * b[2]) + a[3] * b[3]; }

/* The direction: p divided (not multiplied by a reciprocal) by its length, as <=> does. The origin gives NaN. */
static inline void dir4(const double p[4], double u[4]){
    double n = sqrt(((p[0] * p[0] + p[1] * p[1]) + p[2] * p[2]) + p[3] * p[3]);
    for (int d = 0; d < 4; d++) u[d] = p[d] / n;
}

/* 2 asin(h) with h held to 1: unit vectors are unit within rounding, so opposite ones can be a chord of 2 + ulp apart.
 * Written h > 1 ? 1 : h so that NaN passes through (min written h < 1 ? h : 1 would turn the origin's NaN into pi). */
static inline double angle_of_half_chord(double h){ return 2.0 * asin(h > 1.0 ? 1.0 : h); }

double lp_angle4(const double a[4], const double b[4]){
    double u[4], v[4];
    dir4(a, u); dir4(b, v);
    return angle_of_half_chord(lp_distance4(u, v) / 2.0);
}

void lp_half_chord_scalar(const double u[4], const double *pts, size_t i0, size_t n, double *out){
    for (size_t i = i0; i < n; i++) { double v[4]; dir4(pts + 4 * i, v); out[i] = lp_distance4(u, v) / 2.0; }
}

void lp_angle4_batch(const double q[4], const double *pts, size_t n, double *out){
    double u[4]; dir4(q, u);
    lp_kernels()->half_chord(u, pts, 0, n, out);
    for (size_t i = 0; i < n; i++) out[i] = angle_of_half_chord(out[i]);   /* one libm, whichever kernel ran */
}

/* ---------------------------------------------------------------- log, exp, slerp */
void lp_s3_log(const double base[4], const double p[4], double out[4]){
    double c = dot4(base, p), v[4];
    for (int d = 0; d < 4; d++) v[d] = p[d] - c * base[d];
    double vn = sqrt(dot4(v, v));
    if (vn == 0.0) { out[0] = out[1] = out[2] = out[3] = 0.0; return; }
    double s = atan2(vn, c) / vn;                      /* atan2, not acos(c): accurate near 0 and near pi alike */
    for (int d = 0; d < 4; d++) out[d] = v[d] * s;
}

void lp_s3_exp(const double base[4], const double v[4], double out[4]){
    double t = sqrt(dot4(v, v));
    if (t == 0.0) { memmove(out, base, 4 * sizeof(double)); return; }
    double ct = cos(t), st = sin(t) / t, r[4];
    for (int d = 0; d < 4; d++) r[d] = ct * base[d] + st * v[d];
    double n = sqrt(dot4(r, r));
    for (int d = 0; d < 4; d++) out[d] = r[d] / n;
}

void lp_slerp4(const double a[4], const double b[4], double t, double out[4]){
    double c = dot4(a, b), v[4];
    for (int d = 0; d < 4; d++) v[d] = b[d] - c * a[d];
    double vn = sqrt(dot4(v, v));
    if (vn == 0.0) { memmove(out, a, 4 * sizeof(double)); return; }
    /* sin((1 - t) th) / sin th and sin(t th) / sin th are exactly 1 and 0 at t = 0, and 0 and 1 at t = 1 (the same
     * sin th divides itself), so both endpoints come back to the bit. */
    double th = atan2(vn, c), s = sin(th), wa = sin((1.0 - t) * th) / s, wb = sin(t * th) / s, r[4];
    for (int d = 0; d < 4; d++) r[d] = wa * a[d] + wb * b[d];
    memcpy(out, r, sizeof r);
}

/* ---------------------------------------------------------------- canonical order */
/* Numeric order, component by component (never memcmp, which would depend on byte order); NaN last and equal to
 * itself, so the order stays total. Equal rows are interchangeable in any sum, so heapsort's instability is harmless. */
static int dcmp(double a, double b){
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == b) return 0;
    int an = isnan(a), bn = isnan(b);
    return an == bn ? 0 : an ? 1 : -1;
}
static int rcmp(const double *x, const double *y){
    for (int d = 0; d < 4; d++) { int c = dcmp(x[d], y[d]); if (c) return c; }
    return 0;
}
static void rswap(double *x, double *y){ double t[4]; memcpy(t, x, sizeof t); memcpy(x, y, sizeof t); memcpy(y, t, sizeof t); }
static void sift(double *p, size_t root, size_t n){
    while (root < n / 2) {
        size_t ch = 2 * root + 1;
        if (ch + 1 < n && rcmp(p + 4 * ch, p + 4 * (ch + 1)) < 0) ch++;
        if (rcmp(p + 4 * root, p + 4 * ch) >= 0) return;
        rswap(p + 4 * root, p + 4 * ch); root = ch;
    }
}
/* A sorted copy of the points (heapsort: fixed stack, no hidden workspace), or NULL when out of memory. */
static double *canonical(const double *pts, size_t n){
    double *p = malloc(sizeof(double) * 4 * n); if (!p) return NULL;
    memcpy(p, pts, sizeof(double) * 4 * n);
    for (size_t r = n / 2; r > 0; r--) sift(p, r - 1, n);
    for (size_t e = n; e > 1; e--) { rswap(p, p + 4 * (e - 1)); sift(p, 0, e - 1); }
    return p;
}

/* ---------------------------------------------------------------- Karcher mean */
#define KARCHER_ITERS 128
#define KARCHER_TOL   1e-12

bool lp_karcher_mean4(const double *pts, size_t n, double out[4]){
    if (!n) return false;
    double *p = canonical(pts, n); if (!p) return false;
    double est[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < n; i++) for (int d = 0; d < 4; d++) est[d] += p[4 * i + d];
    double en = sqrt(dot4(est, est));
    if (en == 0.0 || !isfinite(en)) { memcpy(est, p, sizeof est); en = sqrt(dot4(est, est)); }   /* balanced: start at the first */
    for (int d = 0; d < 4; d++) est[d] /= en;
    bool ok = false;
    for (int it = 0; it < KARCHER_ITERS && !ok; it++) {
        double g[4] = { 0, 0, 0, 0 };
        for (size_t i = 0; i < n; i++) { double t[4]; lp_s3_log(est, p + 4 * i, t); for (int d = 0; d < 4; d++) g[d] += t[d]; }
        for (int d = 0; d < 4; d++) g[d] /= (double)n;
        double step = sqrt(dot4(g, g));
        if (!isfinite(step)) break;
        lp_s3_exp(est, g, est);
        ok = step < KARCHER_TOL;
    }
    free(p);
    memcpy(out, est, sizeof est);
    return ok;
}

/* ---------------------------------------------------------------- Markley's eigen-centroid */
/* Cyclic Jacobi on a symmetric 4x4 (Numerical Recipes' jacobi): the same rotations in the same order every time.
 * A rotation whose off-diagonal entry no longer changes either diagonal entry sets it to zero instead, so the
 * off-diagonal reaches exact zero and the loop ends; 50 sweeps bound it regardless. V gets the eigenvectors as columns. */
static void jacobi4(double A[4][4], double V[4][4]){
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) V[i][j] = i == j;
    for (int sweep = 0; sweep < 50; sweep++) {
        double off = 0;
        for (int p = 0; p < 3; p++) for (int q = p + 1; q < 4; q++) off += fabs(A[p][q]);
        if (off == 0.0) return;
        for (int p = 0; p < 3; p++) for (int q = p + 1; q < 4; q++) {
            double apq = A[p][q], g = 100.0 * fabs(apq);
            if (apq == 0.0) continue;
            if (sweep > 3 && fabs(A[p][p]) + g == fabs(A[p][p]) && fabs(A[q][q]) + g == fabs(A[q][q])) { A[p][q] = A[q][p] = 0.0; continue; }
            double h = A[q][q] - A[p][p], t;
            if (fabs(h) + g == fabs(h)) t = apq / h;                                   /* theta^2 would overflow */
            else { double th = 0.5 * h / apq; t = 1.0 / (fabs(th) + sqrt(1.0 + th * th)); if (th < 0.0) t = -t; }
            double c = 1.0 / sqrt(t * t + 1.0), s = t * c;
            for (int k = 0; k < 4; k++) { double x = A[k][p], y = A[k][q]; A[k][p] = c * x - s * y; A[k][q] = s * x + c * y; }
            for (int k = 0; k < 4; k++) { double x = A[p][k], y = A[q][k]; A[p][k] = c * x - s * y; A[q][k] = s * x + c * y; }
            A[p][q] = A[q][p] = 0.0;                                                   /* what the rotation was chosen to give */
            for (int k = 0; k < 4; k++) { double x = V[k][p], y = V[k][q]; V[k][p] = c * x - s * y; V[k][q] = s * x + c * y; }
        }
    }
}

void lp_eigen_centroid4(const double *pts, size_t n, double out[4]){
    double *p = n ? canonical(pts, n) : NULL;
    if (!p) { out[0] = out[1] = out[2] = out[3] = NAN; return; }
    double A[4][4] = { { 0 } }, V[4][4];
    for (size_t i = 0; i < n; i++) {
        const double *x = p + 4 * i;
        for (int r = 0; r < 4; r++) for (int c = r; c < 4; c++) A[r][c] += x[r] * x[c];
    }
    free(p);
    for (int r = 1; r < 4; r++) for (int c = 0; c < r; c++) A[r][c] = A[c][r];   /* summed once: exactly symmetric */
    jacobi4(A, V);
    int best = 0;
    for (int k = 1; k < 4; k++) if (A[k][k] > A[best][best]) best = k;            /* ties: the lowest column */
    double v[4] = { V[0][best], V[1][best], V[2][best], V[3][best] }, nv = sqrt(dot4(v, v));
    double sign = 1.0;
    for (int d = 0; d < 4; d++) if (v[d] != 0.0) { sign = v[d] < 0.0 ? -1.0 : 1.0; break; }
    for (int d = 0; d < 4; d++) out[d] = sign * v[d] / nv + 0.0;              /* + 0: no -0 from the flip */
}
