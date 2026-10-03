/* Continuations: the SIMD scan and the run-length walk find exactly what a brute-force scan of the expanded path
 * finds, on random paths with repeats, at whatever dispatch level LAPLACE_ISA selects. */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>

static uint64_t rng = 88172645463325252ull;
static uint32_t next(void){ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

int main(void){
    printf("dispatch: %s (cpu: %s)\n", lp_cpu_describe(lp_cpu_active()), lp_cpu_describe(lp_cpu_features()));
    lp_id vocab[6]; for (int i = 0; i < 6; i++) lp_id_codepoint('a' + i, &vocab[i]);
    uint8_t *buf = malloc(1 << 20); lp_id *seq = malloc(sizeof(lp_id) * 5000), got[6000], want[6000];
    for (int trial = 0; trial < 400; trial++) {
        size_t n = 1 + next() % 3000; int k = 1 + next() % 4;
        for (size_t i = 0; i < n; i++) seq[i] = vocab[next() % k];            /* small alphabets: long runs, many hits */
        size_t np = 1 + next() % 3; lp_id phrase[3];
        for (size_t j = 0; j < np; j++) phrase[j] = vocab[next() % k];
        size_t len;
        if (trial & 1) len = lp_ewkb_path(seq, n, buf, 1 << 20);
        else {                                                                /* each run carrying a value in its spare bits: the same matches */
            static lp_id rid[5000]; static uint64_t rm[5000]; static uint32_t sp[5000]; size_t nr = 0;
            for (size_t i = 0; i < n; ) { size_t j = i; while (j < n && !memcmp(&seq[j], &seq[i], 16)) j++; rid[nr] = seq[i]; rm[nr] = j - i; sp[nr] = next() & ((1u << LP_SPARE_BITS) - 1); nr++; i = j; }
            len = lp_ewkb_runs_spare(rid, rm, sp, nr, buf, 1 << 20);
        }
        size_t nw = 0;
        for (size_t s = 0; s + np < n; s++) {
            size_t j = 0; while (j < np && !memcmp(&seq[s + j], &phrase[j], 16)) j++;
            if (j == np) want[nw++] = seq[s + np];
        }
        size_t ng = lp_follows(buf, len, phrase, np, got, 6000);
        CHECK(ng == nw, "trial %d: %zu continuations, expected %zu (n=%zu np=%zu)", trial, ng, nw, n, np);
        if (ng == nw) CHECK(!memcmp(got, want, 16 * nw), "trial %d: continuations differ", trial);
    }
    free(buf); free(seq);
    DONE("follows");
}
