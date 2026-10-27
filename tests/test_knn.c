/* lp_knn_exact against its definition: a brute-force scan that computes every distance the same way and sorts by
 * (distance, index). Random rows, then rows full of duplicates and ties (longer runs than the candidate margin, and
 * shorter), then the same calls on one thread and on all of them, then a hash of the outputs. The distances are
 * defined in double in a fixed order, so the hash is the same on every machine and thread count.
 * `test_knn bench` times 10k queries x 100k rows, dim 64, k 16. */
#include "laplace/laplace.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _OPENMP
#include <omp.h>
#endif

#define GOLDEN 0x4583c2fa77c90d95ull                  /* HART-DESKTOP, 2026-10-04 */

static uint64_t mix(uint64_t *s){
    uint64_t z = (*s += 0x9e3779b97f4a7c15u);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u; z = (z ^ (z >> 27)) * 0x94d049bb133111ebu; return z ^ (z >> 31);
}
static double unif(uint64_t *s){ return (double)(mix(s) >> 40) / 16777216.0 - 0.5; }
static void fnv(uint64_t *h, const void *p, size_t n){ const uint8_t *b = p; for (size_t i = 0; i < n; i++) { *h ^= b[i]; *h *= 0x100000001b3u; } }

typedef struct { double d; uint32_t j; } ref;
static int ref_cmp(const void *a, const void *b){
    const ref *x = a, *y = b; return x->d < y->d ? -1 : x->d > y->d ? 1 : x->j < y->j ? -1 : x->j > y->j;
}

/* The definition, by brute force; returns the number of queries whose answer differs. */
#define BRUTE(NAME, T)                                                                                                \
static size_t NAME(const T *Q, size_t nq, const T *B, size_t nb, size_t dim, size_t k, const uint32_t *idx, const T *dist){ \
    ref *r = malloc(sizeof(ref) * nb); size_t bad = 0;                                                                \
    for (size_t i = 0; i < nq; i++) {                                                                                 \
        for (size_t j = 0; j < nb; j++) { double s = 0;                                                               \
            for (size_t d = 0; d < dim; d++) { double e = (double)Q[i * dim + d] - (double)B[j * dim + d]; s += e * e; } \
            r[j] = (ref){ s, (uint32_t)j }; }                                                                         \
        qsort(r, nb, sizeof(ref), ref_cmp);                                                                           \
        for (size_t m = 0; m < k; m++) if (idx[i * k + m] != r[m].j || dist[i * k + m] != (T)r[m].d) { bad++; break; } \
    }                                                                                                                 \
    free(r); return bad;                                                                                              \
}
BRUTE(brute_f, float)
BRUTE(brute_d, double)

static void set_threads(int n){
#ifdef _OPENMP
    omp_set_num_threads(n);
#else
    (void)n;
#endif
}
static int max_threads(void){
#ifdef _OPENMP
    return omp_get_num_procs();
#else
    return 1;
#endif
}

/* One float case: checked against brute force, run on 1 thread and on all, hashed into h. */
static void run_f(const char *what, const float *Q, size_t nq, const float *B, size_t nb, size_t dim, size_t k, uint64_t *h){
    uint32_t *i1 = malloc(4 * nq * k), *iN = malloc(4 * nq * k); float *d1 = malloc(4 * nq * k), *dN = malloc(4 * nq * k);
    set_threads(1);             int r1 = lp_knn_exact(Q, nq, B, nb, dim, k, i1, d1);
    set_threads(max_threads()); int rN = lp_knn_exact(Q, nq, B, nb, dim, k, iN, dN);
    CHECK(r1 == 0 && rN == 0, "%s: returns 0 (%d, %d)", what, r1, rN);
    size_t bad = brute_f(Q, nq, B, nb, dim, k, iN, dN);
    CHECK(bad == 0, "%s: %zu of %zu queries differ from brute force", what, bad, nq);
    CHECK(!memcmp(i1, iN, 4 * nq * k) && !memcmp(d1, dN, 4 * nq * k), "%s: 1 thread and %d threads differ", what, max_threads());
    fnv(h, iN, 4 * nq * k); fnv(h, dN, 4 * nq * k);
    free(i1); free(iN); free(d1); free(dN);
}

static void run_d(const char *what, const double *Q, size_t nq, const double *B, size_t nb, size_t dim, size_t k, uint64_t *h){
    uint32_t *i1 = malloc(4 * nq * k), *iN = malloc(4 * nq * k); double *d1 = malloc(8 * nq * k), *dN = malloc(8 * nq * k);
    set_threads(1);             int r1 = lp_knn_exact_d(Q, nq, B, nb, dim, k, i1, d1);
    set_threads(max_threads()); int rN = lp_knn_exact_d(Q, nq, B, nb, dim, k, iN, dN);
    CHECK(r1 == 0 && rN == 0, "%s: returns 0 (%d, %d)", what, r1, rN);
    size_t bad = brute_d(Q, nq, B, nb, dim, k, iN, dN);
    CHECK(bad == 0, "%s: %zu of %zu queries differ from brute force", what, bad, nq);
    CHECK(!memcmp(i1, iN, 4 * nq * k) && !memcmp(d1, dN, 8 * nq * k), "%s: 1 thread and %d threads differ", what, max_threads());
    fnv(h, iN, 4 * nq * k); fnv(h, dN, 8 * nq * k);
    free(i1); free(iN); free(d1); free(dN);
}

static int bench(void){
    const size_t nq = 10000, nb = 100000, dim = 64, k = 16; uint64_t s = 11;
    float *Q = malloc(4 * nq * dim), *B = malloc(4 * nb * dim); uint32_t *idx = malloc(4 * nq * k); float *dist = malloc(4 * nq * k);
    for (size_t i = 0; i < nq * dim; i++) Q[i] = (float)unif(&s);
    for (size_t i = 0; i < nb * dim; i++) B[i] = (float)unif(&s);
    set_threads(max_threads());
    lp_knn_exact(Q, 64, B, nb, dim, k, idx, dist);              /* warm: MKL's first call */
    struct timespec t0, t1; timespec_get(&t0, TIME_UTC);
    int rc = lp_knn_exact(Q, nq, B, nb, dim, k, idx, dist);
    timespec_get(&t1, TIME_UTC);
    double sec = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    printf("knn bench: %zu x %zu, dim %zu, k %zu, %d threads: %.3f s (%.1f GFLOP/s GEMM), rc %d\n", nq, nb, dim, k, max_threads(),
           sec, 2.0 * nq * nb * dim / sec * 1e-9, rc);
    free(Q); free(B); free(idx); free(dist); return rc;
}

int main(int argc, char **argv){
    if (argc > 1 && !strcmp(argv[1], "bench")) return bench();
    CHECK(lp_mkl_reproducible(), "MKL accepts reproducible mode (CNR AVX2, strict)");
    uint64_t h = 0xcbf29ce484222325u, s = 5;

    {   /* random rows; dim not a multiple of any vector width, nb not a multiple of the tile */
        const size_t nq = 300, nb = 5000, dim = 37;
        float *Q = malloc(4 * nq * dim), *B = malloc(4 * nb * dim);
        for (size_t i = 0; i < nq * dim; i++) Q[i] = (float)unif(&s);
        for (size_t i = 0; i < nb * dim; i++) B[i] = (float)unif(&s);
        run_f("random", Q, nq, B, nb, dim, 10, &h);
        run_f("random k = 1", Q, nq, B, nb, dim, 1, &h);
        run_f("k = nb", Q, 7, B, 40, dim, 40, &h);
        double *Qd = malloc(8 * nq * dim), *Bd = malloc(8 * nb * dim);
        for (size_t i = 0; i < nq * dim; i++) Qd[i] = unif(&s);
        for (size_t i = 0; i < nb * dim; i++) Bd[i] = unif(&s);
        run_d("random double", Qd, nq, Bd, nb, dim, 10, &h);
        free(Q); free(B); free(Qd); free(Bd);
    }
    {   /* ties: 60 distinct rows on a small integer grid, each repeated 50 times (runs longer than the margin), and
         * queries on the grid too, so whole runs sit at equal distance and only the index orders them */
        const size_t nq = 200, nb = 3000, dim = 8, nd = 60;
        float *P = malloc(4 * nd * dim), *Q = malloc(4 * nq * dim), *B = malloc(4 * nb * dim);
        for (size_t i = 0; i < nd * dim; i++) P[i] = (float)(mix(&s) % 5);
        for (size_t j = 0; j < nb; j++) memcpy(B + j * dim, P + (mix(&s) % nd) * dim, 4 * dim);
        for (size_t i = 0; i < nq * dim; i++) Q[i] = (float)(mix(&s) % 5);
        run_f("duplicates", Q, nq, B, nb, dim, 20, &h);
        /* short runs: each random row three times, so near and exact ties fall inside the margin */
        const size_t nb3 = 6000, dim3 = 24;
        float *Q3 = malloc(4 * nq * dim3), *B3 = malloc(4 * nb3 * dim3);
        for (size_t j = 0; j < nb3; j += 3) for (size_t d = 0; d < dim3; d++) B3[j * dim3 + d] = B3[(j + 1) * dim3 + d] = B3[(j + 2) * dim3 + d] = (float)unif(&s);
        for (size_t i = 0; i < nq; i++) for (size_t d = 0; d < dim3; d++) Q3[i * dim3 + d] = B3[(i * 29 % nb3) * dim3 + d] + (float)(unif(&s) * 1e-3);
        run_f("triplicates", Q3, nq, B3, nb3, dim3, 8, &h);
        free(P); free(Q); free(B); free(Q3); free(B3);
    }
    uint32_t i0; float d0; float one = 0;
    CHECK(lp_knn_exact(&one, 1, &one, 1, 1, 2, &i0, &d0) == -1, "k > nb is refused");
    CHECK(lp_knn_exact(&one, 1, &one, 1, 1, 0, &i0, &d0) == -1, "k = 0 is refused");

    printf("knn: hash %016llx\n", (unsigned long long)h);
    if (GOLDEN) CHECK(h == GOLDEN, "knn hash %016llx, golden %016llx", (unsigned long long)h, (unsigned long long)GOLDEN);
    DONE("knn");
}
