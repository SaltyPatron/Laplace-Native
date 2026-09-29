/* Shape measures: what each does with a stray vertex and with a repeat, as measured (Research: Numerics). */
#include "laplace/laplace.h"
#include "check.h"
#include <math.h>
#include <string.h>

int main(void){
    enum { N = 60 }; double a[4 * N], b[4 * (N + 1)]; uint64_t s = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 4 * N; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; a[i] = (double)(s >> 11) / 9007199254740992.0; }
    size_t steps;
    CHECK(lp_frechet4(a, N, a, N) == 0 && lp_frechet4_outliers(a, N, a, N, 1) == 0, "identical: Frechet 0");
    CHECK(lp_dtw4(a, N, a, N, &steps) == 0 && steps == N, "identical: DTW 0 over %zu steps", steps);
    CHECK(lp_edr4(a, N, a, N, 0.1) == 0, "identical: EDR 0");

    memcpy(b, a, sizeof a); for (int d = 0; d < 4; d++) b[4 * 30 + d] = 5.0 + d;                 /* one stray vertex */
    double f = lp_frechet4(a, N, b, N), f1 = lp_frechet4_outliers(a, N, b, N, 1);
    CHECK(f > 4.0, "a stray vertex sets the Frechet distance (%g)", f);
    CHECK(f1 == 0.0, "skipping one vertex removes it (%g)", f1);
    CHECK(lp_edr4(a, N, b, N, 0.1) == 1, "a stray vertex is one edit (%zu)", lp_edr4(a, N, b, N, 0.1));
    double dt = lp_dtw4(a, N, b, N, NULL);
    CHECK(dt > 4.0 && dt < 2 * f, "a stray vertex adds its distance to DTW once (%g)", dt);

    memcpy(b, a, 4 * 31 * sizeof(double)); memcpy(b + 4 * 31, a + 4 * 30, 4 * 30 * sizeof(double));   /* vertex 30 twice */
    CHECK(lp_frechet4(a, N, b, N + 1) == 0, "a repeat is nothing to Frechet");
    CHECK(lp_dtw4(a, N, b, N + 1, &steps) == 0 && steps == N + 1, "a repeat is nothing to DTW");
    CHECK(lp_edr4(a, N, b, N + 1, 0.1) == 1, "a repeat is one edit");
    CHECK(lp_frechet4_outliers(a, N, b, N, 0) == lp_frechet4(a, N, b, N), "no outliers allowed is Frechet");
    DONE("shape");
}
