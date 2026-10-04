/* rowsig under MKL's reproducible mode: fixed rows in, every hit and statistic hashed. The hash is the same on every
 * machine (hart-server's i7-6850K and HART-DESKTOP's i9-14900KS agreed on it); a different CPU path or MKL mode that
 * changes one rounding changes it. Prints the hash so a new machine can be compared before the golden value moves. */
#include "laplace/laplace.h"
#include "check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOLDEN 0x57a15d803a0123a4ull                  /* hart-server and HART-DESKTOP, 2026-10-04 */

static uint64_t mix(uint64_t *s){
    uint64_t z = (*s += 0x9e3779b97f4a7c15u);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u; z = (z ^ (z >> 27)) * 0x94d049bb133111ebu; return z ^ (z >> 31);
}
static void fnv(uint64_t *h, const void *p, size_t n){ const uint8_t *b = p; for (size_t i = 0; i < n; i++) { *h ^= b[i]; *h *= 0x100000001b3u; } }

int main(void){
    CHECK(lp_mkl_reproducible(), "MKL accepts reproducible mode (CNR AVX2, strict)");
    const size_t m = 96, n = 3000, r = 160;               /* r not a multiple of any vector width: tails are exercised */
    float *A = malloc(sizeof(float) * m * r), *B = malloc(sizeof(float) * n * r);
    uint64_t s = 7;
    for (size_t i = 0; i < m * r; i++) A[i] = (float)((double)(mix(&s) >> 40) / 16777216.0 - 0.5);
    for (size_t i = 0; i < n * r; i++) B[i] = (float)((double)(mix(&s) >> 40) / 16777216.0 - 0.5);
    for (size_t i = 0; i < m; i++) for (size_t k = 0; k < r; k++) B[(i * 31 % n) * r + k] += A[i * r + k];   /* planted hits */

    size_t cap = m * 8; lp_rowsig_hit *hits = malloc(sizeof *hits * cap); lp_rowsig_stats st; memset(&st, 0, sizeof st);
    size_t k = lp_rowsig(A, m, B, n, r, 1.0f, 3.0f, 8, hits, cap, &st);
    CHECK(k > 0, "rowsig finds the planted rows");

    uint64_t h = 0xcbf29ce484222325u;
    for (size_t i = 0; i < k; i++) { fnv(&h, &hits[i].row, 4); fnv(&h, &hits[i].col, 4); fnv(&h, &hits[i].score, 4); fnv(&h, &hits[i].z, 4); }
    fnv(&h, &st.rows, sizeof st.rows); fnv(&h, &st.candidates, sizeof st.candidates); fnv(&h, st.above, sizeof st.above);
    fnv(&h, &st.rows_without, sizeof st.rows_without);
    printf("rowsig: %zu hits, hash %016llx\n", k, (unsigned long long)h);
    if (GOLDEN) CHECK(h == GOLDEN, "rowsig hash %016llx, golden %016llx", (unsigned long long)h, (unsigned long long)GOLDEN);
    free(A); free(B); free(hits);
    DONE("rowsig");
}
