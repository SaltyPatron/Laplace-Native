/* linalg: Procrustes recovers a known rotation, Gram-Schmidt is orthonormal, the ring graph's eigenmap is a circle,
 * and every output on fixed inputs hashes (NaN canonical) to a golden value. Prints the hash so a new machine can be
 * compared before the golden value moves. */
#include "laplace/linalg.h"
#include "check.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOLDEN 0x82d4605477ed644cull                  /* HART-DESKTOP, 2026-10-04; x86-64-v3 and x86-64 builds agree */

static uint64_t mix(uint64_t *s){
    uint64_t z = (*s += 0x9e3779b97f4a7c15u);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u; z = (z ^ (z >> 27)) * 0x94d049bb133111ebu; return z ^ (z >> 31);
}
static double uni(uint64_t *s){ return (double)(mix(s) >> 11) / 9007199254740992.0 - 0.5; }   /* [-0.5, 0.5) */
static void fnv(uint64_t *h, const double *x, size_t n){
    for (size_t i = 0; i < n; i++) {
        double v = x[i]; uint64_t u;
        if (v != v) u = 0x7ff8000000000000u; else memcpy(&u, &v, 8);
        for (int b = 0; b < 64; b += 8) { *h ^= (u >> b) & 0xff; *h *= 0x100000001b3u; }
    }
}
/* The largest-magnitude component (the first, on a tie) of x[0], x[stride], ... is positive. */
static int canonical(const double *x, size_t n, size_t stride){
    size_t best = 0; double m = -1;
    for (size_t i = 0; i < n; i++) if (fabs(x[i * stride]) > m) { m = fabs(x[i * stride]); best = i; }
    return x[best * stride] > 0;
}

static void procrustes(void){
    enum { n = 16, d = 4 };
    uint64_t s = 11;
    double A[n * d], B[n * d], R0[d * d], R[d * d], sc = 0, G[d * d];
    /* The known rotation: Givens rotations in the planes (0,1), (2,3), (1,2), (0,3). */
    for (int i = 0; i < d * d; i++) R0[i] = i % (d + 1) == 0;
    const int pl[4][2] = { {0, 1}, {2, 3}, {1, 2}, {0, 3} };
    const double th[4] = { 0.7, -1.3, 2.1, 0.4 };
    for (int g = 0; g < 4; g++) {
        int p = pl[g][0], q = pl[g][1]; double c = cos(th[g]), sn = sin(th[g]);
        memcpy(G, R0, sizeof G);
        for (int i = 0; i < d; i++) { R0[i * d + p] = c * G[i * d + p] - sn * G[i * d + q]; R0[i * d + q] = sn * G[i * d + p] + c * G[i * d + q]; }
    }
    for (int i = 0; i < n * d; i++) A[i] = uni(&s);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < d; j++) { double t = 0; for (int l = 0; l < d; l++) t += A[i * d + l] * R0[l * d + j]; B[i * d + j] = 1.5 * t; }
    CHECK(lp_procrustes(A, B, n, d, R, &sc) == 0, "procrustes runs");
    double e = 0; for (int i = 0; i < d * d; i++) e = fmax(e, fabs(R[i] - R0[i]));
    CHECK(e < 1e-12, "procrustes recovers the rotation (max error %g)", e);
    CHECK(fabs(sc - 1.5) < 1e-12, "procrustes recovers the scale (%.17g)", sc);
    CHECK(lp_procrustes(A, B, 0, d, R, &sc) == -2, "procrustes rejects n = 0");
    A[3] = NAN; CHECK(lp_procrustes(A, B, n, d, R, &sc) == -1, "procrustes rejects NaN");
}

static void gram_schmidt(void){
    enum { m = 5, d = 8 };
    uint64_t s = 23;
    double V[m * d];
    for (int i = 0; i < m * d; i++) V[i] = uni(&s);
    CHECK(lp_gram_schmidt(V, m, d) == 0, "gram_schmidt runs");
    double e = 0;
    for (int i = 0; i < m; i++) {
        for (int j = i; j < m; j++) { double t = 0; for (int l = 0; l < d; l++) t += V[i * d + l] * V[j * d + l]; e = fmax(e, fabs(t - (i == j))); }
        CHECK(canonical(V + i * d, d, 1), "gram_schmidt vector %d canonical", i);
    }
    CHECK(e < 1e-14, "gram_schmidt orthonormal (max error %g)", e);
    double D[9] = { 1, 0, 0, 0, 1, 0, 1, 1, 0 };
    CHECK(lp_gram_schmidt(D, 3, 3) == -4, "gram_schmidt rank deficiency");
    CHECK(lp_gram_schmidt(D, 4, 3) == -2, "gram_schmidt more vectors than dimensions");
}

/* The ring C_n: its first nontrivial eigenspace is cos and sin of 2 pi i / n, doubly degenerate, so the embedding is
 * that circle up to a rotation and a reflection: every point at one radius, consecutive points 2 pi / n apart. */
static void ring(void){
    enum { n = 24 };
    static double W[n * n];
    double Y[n * 2], lam[2];
    for (int i = 0; i < n; i++) { W[i * n + (i + 1) % n] = 1; W[i * n + (i + n - 1) % n] = 1; }
    CHECK(lp_eigenmap(W, n, 2, Y, lam) == 0, "eigenmap runs on the ring");
    const double two_pi = 6.283185307179586, l1 = 1 - cos(two_pi / n);
    CHECK(fabs(lam[0] - l1) < 1e-10 && fabs(lam[1] - l1) < 1e-10, "ring eigenvalues %.17g %.17g, expected %.17g", lam[0], lam[1], l1);
    double r2 = Y[0] * Y[0] + Y[1] * Y[1], er = 0, ea = 0, c00 = 0, c11 = 0, c01 = 0;
    for (int i = 0; i < n; i++) {
        const double *p = Y + 2 * i, *q = Y + 2 * ((i + 1) % n);
        er = fmax(er, fabs((p[0] * p[0] + p[1] * p[1]) / r2 - 1));
        ea = fmax(ea, fabs((p[0] * q[0] + p[1] * q[1]) / r2 - cos(two_pi / n)));
        c00 += p[0] * p[0]; c11 += p[1] * p[1]; c01 += p[0] * p[1];
    }
    CHECK(er < 1e-8, "ring embedding on one circle (max error %g)", er);
    CHECK(ea < 1e-8, "ring embedding steps by 2 pi / n (max error %g)", ea);
    CHECK(fabs(c00 - 0.5) < 1e-10 && fabs(c11 - 0.5) < 1e-10 && fabs(c01) < 1e-10, "ring columns D-orthonormal (%g %g %g)", c00, c11, c01);
    CHECK(canonical(Y, n, 2) && canonical(Y + 1, n, 2), "ring columns canonical");
    CHECK(lp_eigenmap(W, n, n - 1, Y, lam) == -2, "eigenmap rejects k > n - 2");
}

/* Every output on fixed inputs: a dense random affinity (distinct eigenvalues), Procrustes between random sets, a
 * random basis. */
static uint64_t fingerprint(void){
    enum { n = 40, k = 3, pn = 20, pd = 5, gm = 6, gd = 9 };
    uint64_t s = 101, h = 0xcbf29ce484222325u;
    static double W[n * n], Y[n * k];
    double lam[k], A[pn * pd], B[pn * pd], R[pd * pd], sc, V[gm * gd];
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) W[i * n + j] = uni(&s) + 0.5;
    CHECK(lp_eigenmap(W, n, k, Y, lam) == 0, "eigenmap runs");
    for (int c = 0; c < k; c++) CHECK(canonical(Y + c, n, k), "eigenmap column %d canonical", c);
    CHECK(lam[0] <= lam[1] && lam[1] <= lam[2], "eigenvalues ascending");
    fnv(&h, Y, n * k); fnv(&h, lam, k);
    for (int i = 0; i < pn * pd; i++) { A[i] = uni(&s); B[i] = uni(&s); }
    CHECK(lp_procrustes(A, B, pn, pd, R, &sc) == 0, "procrustes runs");
    fnv(&h, R, pd * pd); fnv(&h, &sc, 1);
    for (int i = 0; i < gm * gd; i++) V[i] = uni(&s);
    CHECK(lp_gram_schmidt(V, gm, gd) == 0, "gram_schmidt runs");
    fnv(&h, V, gm * gd);
    return h;
}

int main(void){
    procrustes();
    gram_schmidt();
    ring();
    uint64_t h = fingerprint();
    printf("linalg: hash %016llx\n", (unsigned long long)h);
    CHECK(h == GOLDEN, "linalg hash %016llx, golden %016llx", (unsigned long long)h, (unsigned long long)GOLDEN);
    DONE("linalg");
}
