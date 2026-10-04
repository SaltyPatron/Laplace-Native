/* Exact k nearest neighbours by squared Euclidean distance: for every query, the k base rows nearest it, ordered by
 * (distance, index). The distance is the one definition the result is exact to: the sum over d of (q_d - b_d)^2,
 * accumulated in double in index order.
 *
 * Candidates come from the GEMM: |q|^2 + |b|^2 - 2 q.b, the cross term from cblas_?gemm over a tile of queries x base
 * rows under MKL's reproducible mode. Each query keeps its k + margin best candidates in a heap ordered by
 * (estimate, index), then their true distances are computed directly and sorted. An estimate is off from the true
 * distance by at most err = 2 (dim + 4) eps (|q| + max|b|)^2, and every row left out has an estimate at or above the
 * worst one kept: when that estimate - err is above the k-th true distance, no row left out can enter the k. When it
 * is not (a run of near-ties or duplicates longer than the margin), the query is answered by a direct scan of every
 * row. Either way the answer is the definition's, so near-ties come out exact and in index order.
 *
 * Query blocks run in parallel, each with a single-threaded GEMM; each query writes only its own output slots and
 * nothing is reduced across threads, so the result does not depend on the thread count. */
#include "laplace/laplace.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <mkl.h>

#define QB 64                                                   /* queries per block */
#define NBT 2048                                                /* base rows per tile: 64 x 2048 estimates, 512 KB float */
#define MARGIN 16

typedef struct { double d; uint32_t j; } hit;

static int hit_lt(hit a, hit b){ return a.d < b.d || (a.d == b.d && a.j < b.j); }

static void hit_sort(hit *h, size_t n){                         /* insertion: n is k + margin */
    for (size_t i = 1; i < n; i++) { hit x = h[i]; size_t p = i; while (p > 0 && hit_lt(x, h[p - 1])) { h[p] = h[p - 1]; p--; } h[p] = x; }
}

/* Keep the k smallest by (d, j) in a sorted buffer; j arrives in increasing order, so an equal d never displaces. */
static void hit_keep(hit *h, size_t *cnt, size_t k, double d, uint32_t j){
    size_t c = *cnt;
    if (c == k) { if (!(d < h[k - 1].d)) return; c--; }
    size_t p = c; while (p > 0 && d < h[p - 1].d) { h[p] = h[p - 1]; p--; } h[p] = (hit){ d, j }; *cnt = c + 1;
}

#define T float
#define S(x) x##_f
#define GEMM cblas_sgemm
#define EPS FLT_EPSILON
#define KNN lp_knn_exact
#include "knn_t.h"

#define T double
#define S(x) x##_d
#define GEMM cblas_dgemm
#define EPS DBL_EPSILON
#define KNN lp_knn_exact_d
#include "knn_t.h"
