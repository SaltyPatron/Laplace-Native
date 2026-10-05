/* A minimal test harness: CHECK counts failures, and the test exits nonzero if any failed. */
#ifndef LP_CHECK_H
#define LP_CHECK_H
#include "laplace/laplace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int lp_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { lp_failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                                                fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define DONE(name) do { printf("%s: %s\n", name, lp_failures ? "FAILED" : "ok"); return lp_failures ? 1 : 0; } while (0)


/* A test run at a dispatch level (LAPLACE_ISA) this CPU does not have is skipped (77), not run at a lower level. */
#define LP_SKIP 77
static inline void lp_isa_or_skip(void){
    const char *w = getenv("LAPLACE_ISA"); uint32_t f = lp_cpu_features();
    uint32_t need = !w ? 0 : !strcmp(w, "sse2") ? LP_CPU_SSE2 : !strcmp(w, "avx2") ? LP_CPU_AVX2
                  : !strcmp(w, "avxvnni") ? (LP_CPU_AVX2 | LP_CPU_AVXVNNI) : !strcmp(w, "avx512") ? LP_CPU_AVX512
                  : !strcmp(w, "avx512vnni") ? (LP_CPU_AVX512 | LP_CPU_VNNI512) : 0;
    printf("dispatch: %s", lp_cpu_describe(lp_cpu_active())); printf(" (cpu: %s)\n", lp_cpu_describe(f));   /* one buffer */
    if ((f & need) != need) { printf("skipped: no %s on this CPU\n", w); exit(LP_SKIP); }
}
#endif
