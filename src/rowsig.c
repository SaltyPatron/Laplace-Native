/* "b beats c given a": for every row a of A (m x r) against every candidate row b of B (n x r), the score a.b; each
 * row's own noise (mean and spread over all n candidates); and the few candidates that stand out from it.
 *
 * Rows are taken in blocks whose full score tile (block x n) stays in cache. One GEMM fills the tile; while it is hot,
 * a first pass accumulates each row's sum and sum of squares, and a second keeps only candidates above
 * mean + zmin * sd, in a small per-row buffer ordered by score. Pairs are never enumerated: once a row's scores are on
 * one scale, every pairwise outcome among them is implied. Blocks run in parallel, each with a single-threaded GEMM. */
#include "laplace/laplace.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <mkl.h>
#ifdef _OPENMP
#include <omp.h>
#endif

typedef struct { float s; uint32_t j; } cand;

static void keep(cand *buf, uint32_t *cnt, uint32_t cap, float s, uint32_t j){
    uint32_t c = *cnt;
    if (c < cap) {                                              /* insertion into a small sorted buffer, largest first */
        uint32_t i = c++; while (i > 0 && buf[i - 1].s < s) { buf[i] = buf[i - 1]; i--; } buf[i] = (cand){ s, j }; *cnt = c; return;
    }
    if (s <= buf[cap - 1].s) return;
    uint32_t i = cap - 1; while (i > 0 && buf[i - 1].s < s) { buf[i] = buf[i - 1]; i--; } buf[i] = (cand){ s, j };
}

size_t lp_rowsig(const float *A, size_t m, const float *B, size_t n, size_t r, float scale, float zmin, uint32_t cap,
                 lp_rowsig_hit *out, size_t out_cap, lp_rowsig_stats *st){
    const size_t blk = 16;                                      /* 16 rows x n candidates: 2 MB of scores for n = 32k */
    size_t total = 0; uint64_t above[3] = { 0, 0, 0 }; uint64_t rows_empty = 0;
    #pragma omp parallel
    {
        float *tile = malloc(sizeof(float) * blk * n); cand *buf = malloc(sizeof(cand) * cap);
        lp_rowsig_hit *loc = malloc(sizeof(lp_rowsig_hit) * blk * cap); uint64_t la[3] = { 0, 0, 0 }, le = 0;
        #ifdef _OPENMP
        mkl_set_num_threads_local(1);
        #endif
        #pragma omp for schedule(dynamic, 1)
        for (size_t b0 = 0; b0 < m; b0 += blk) {
            size_t rows = m - b0 < blk ? m - b0 : blk, nl = 0;
            cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)rows, (MKL_INT)n, (MKL_INT)r, scale,
                        A + b0 * r, (MKL_INT)r, B, (MKL_INT)r, 0.0f, tile, (MKL_INT)n);
            for (size_t i = 0; i < rows; i++) {
                const float *t = tile + i * n; double s1 = 0, s2 = 0;
                for (size_t j = 0; j < n; j++) { s1 += t[j]; s2 += (double)t[j] * t[j]; }
                double mean = s1 / n, sd = sqrt(s2 / n - mean * mean); if (sd <= 0) sd = 1e-12;
                float floor3 = (float)(mean + zmin * sd), f4 = (float)(mean + 4 * sd), f5 = (float)(mean + 5 * sd);
                uint32_t c = 0; uint64_t a3 = 0, a4 = 0, a5 = 0;
                for (size_t j = 0; j < n; j++) {
                    float v = t[j];
                    if (v > floor3) { a3++; a4 += v > f4; a5 += v > f5; keep(buf, &c, cap, v, (uint32_t)j); }
                }
                la[0] += a3; la[1] += a4; la[2] += a5; le += a3 == 0;
                for (uint32_t k = 0; k < c; k++)
                    loc[nl++] = (lp_rowsig_hit){ (uint32_t)(b0 + i), buf[k].j, buf[k].s, (float)((buf[k].s - mean) / sd) };
            }
            #pragma omp critical
            {
                for (size_t k = 0; k < nl && total < out_cap; k++) out[total++] = loc[k];
            }
        }
        #pragma omp critical
        { for (int k = 0; k < 3; k++) above[k] += la[k]; rows_empty += le; }
        free(tile); free(buf); free(loc);
    }
    if (st) { st->rows = m; st->candidates = n; for (int k = 0; k < 3; k++) st->above[k] = above[k]; st->rows_without = rows_empty;
              st->flops = 2.0 * m * n * r; }
    return total;
}
