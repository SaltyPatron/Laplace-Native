/* The dispatched kernels against their scalar definitions, at whatever dispatch level LAPLACE_ISA selects: the
 * squared-distance rows under Fréchet, DTW, EDR and Hausdorff (bit for bit, every row length past each SIMD width and
 * tail), and the int8 dot and its batch (exact, the wrap past n = 131071 included). The dot's speed is printed. */
#include "laplace/laplace.h"
#include "check.h"
#include <math.h>
#include <time.h>

static uint64_t rng = 0x2545F4914F6CDD1Dull;
static uint64_t next(void){ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static double unit(void){ return (double)(next() >> 11) / 9007199254740992.0 * 2.0 - 1.0; }

static double d2(const double *p, const double *q){
    double dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2], dm = p[3] - q[3];
    return ((dx * dx + dy * dy) + dz * dz) + dm * dm;
}
static double minf(double a, double b){ return b < a ? b : a; }

/* The measures by their recurrences over the whole matrix. */
static void define(const double *a, size_t na, const double *b, size_t nb, double eps,
                   double *fr, double *dtw, size_t *edr, double *haus){
    double *F = malloc(sizeof(double) * na * nb), *D = malloc(sizeof(double) * na * nb);
    size_t *E = malloc(sizeof(size_t) * (na + 1) * (nb + 1));
    for (size_t i = 0; i < na; i++) for (size_t j = 0; j < nb; j++) {
        double c = d2(a + 4 * i, b + 4 * j), m = INFINITY, w = INFINITY;
        if (i) { m = minf(m, F[(i - 1) * nb + j]); w = minf(w, D[(i - 1) * nb + j]); }
        if (j) { m = minf(m, F[i * nb + j - 1]); w = minf(w, D[i * nb + j - 1]); }
        if (i && j) { m = minf(m, F[(i - 1) * nb + j - 1]); w = minf(w, D[(i - 1) * nb + j - 1]); }
        if (!i && !j) m = w = 0;
        F[i * nb + j] = c > m ? c : m;
        D[i * nb + j] = w + sqrt(c);
    }
    for (size_t i = 0; i <= na; i++) for (size_t j = 0; j <= nb; j++) {
        size_t e;
        if (!i) e = j; else if (!j) e = i;
        else {
            e = E[(i - 1) * (nb + 1) + j - 1] + (sqrt(d2(a + 4 * (i - 1), b + 4 * (j - 1))) <= eps ? 0 : 1);
            if (E[(i - 1) * (nb + 1) + j] + 1 < e) e = E[(i - 1) * (nb + 1) + j] + 1;
            if (E[i * (nb + 1) + j - 1] + 1 < e) e = E[i * (nb + 1) + j - 1] + 1;
        }
        E[i * (nb + 1) + j] = e;
    }
    double h = 0;
    for (int w = 0; w < 2; w++) {
        const double *p = w ? b : a, *q = w ? a : b; size_t np = w ? nb : na, nq = w ? na : nb;
        for (size_t i = 0; i < np; i++) { double m = INFINITY; for (size_t j = 0; j < nq; j++) m = minf(m, d2(p + 4 * i, q + 4 * j)); if (m > h) h = m; }
    }
    *fr = sqrt(F[na * nb - 1]); *dtw = D[na * nb - 1]; *edr = E[na * (nb + 1) + nb]; *haus = sqrt(h);
    free(F); free(D); free(E);
}

static int32_t dot_def(const int8_t *a, const int8_t *b, size_t n){
    int64_t s = 0; for (size_t i = 0; i < n; i++) s += (int64_t)a[i] * b[i];
    return (int32_t)(uint32_t)(uint64_t)s;
}

static double now(void){ struct timespec t; timespec_get(&t, TIME_UTC); return (double)t.tv_sec + t.tv_nsec * 1e-9; }

int main(void){
    lp_isa_or_skip();

    double a[4 * 40], b[4 * 40];
    for (int trial = 0; trial < 300; trial++) {
        size_t na = 1 + next() % 40, nb = 1 + trial % 40;                 /* every row length 1..40 */
        for (size_t i = 0; i < 4 * na; i++) a[i] = unit();
        for (size_t i = 0; i < 4 * nb; i++) b[i] = (next() & 3) ? a[i % (4 * na)] + unit() * 1e-3 : unit();
        double fr, dtw, haus; size_t edr, steps;
        define(a, na, b, nb, 0.05, &fr, &dtw, &edr, &haus);
        CHECK(lp_frechet4(a, na, b, nb) == fr, "trial %d: Frechet %zu x %zu", trial, na, nb);
        CHECK(lp_frechet4_outliers(a, na, b, nb, 0) == fr, "trial %d: Frechet, 0 out", trial);
        CHECK(lp_dtw4(a, na, b, nb, &steps) == dtw, "trial %d: DTW %zu x %zu", trial, na, nb);
        CHECK(lp_edr4(a, na, b, nb, 0.05) == edr, "trial %d: EDR %zu x %zu", trial, na, nb);
        CHECK(lp_hausdorff4(a, na, b, nb) == haus, "trial %d: Hausdorff %zu x %zu", trial, na, nb);
    }

    enum { MAXN = 131073 };
    int8_t *x = malloc(MAXN), *y = malloc(MAXN);
    for (size_t n = 0; n <= 400; n++) {                                   /* every length past 32, 64 and 128 */
        for (size_t i = 0; i < n; i++) { x[i] = (int8_t)next(); y[i] = (int8_t)next(); }
        if (n % 7 == 0) for (size_t i = 0; i < n; i++) { x[i] = -128; y[i] = (n & 8) ? -128 : 127; }
        CHECK(lp_dot_i8(x, y, n) == dot_def(x, y, n), "dot n=%zu: %d, expected %d", n, lp_dot_i8(x, y, n), dot_def(x, y, n));
    }
    memset(x, -128, MAXN); memset(y, -128, MAXN);
    CHECK(lp_dot_i8(x, y, 131071) == 2147467264, "dot at the exact limit: %d", lp_dot_i8(x, y, 131071));
    CHECK(lp_dot_i8(x, y, 131072) == INT32_MIN && lp_dot_i8(x, y, 131073) == dot_def(x, y, 131073), "dot wraps modulo 2^32");
    for (size_t i = 0; i < MAXN; i++) { x[i] = (int8_t)next(); y[i] = (int8_t)next(); }
    CHECK(lp_dot_i8(x, y, MAXN) == dot_def(x, y, MAXN), "dot n=%d", MAXN);

    int32_t out[16];
    for (int trial = 0; trial < 400; trial++) {
        size_t n = next() % 300, nrows = next() % 16, stride = n + next() % 5;
        if (nrows * stride > MAXN) continue;
        for (size_t i = 0; i < nrows * stride; i++) y[i] = (int8_t)next();
        for (size_t i = 0; i < n; i++) x[i] = (trial % 5 == 0) ? -128 : (int8_t)next();
        lp_dot_i8_batch(x, y, nrows, n, stride, out);
        for (size_t r = 0; r < nrows; r++)
            CHECK(out[r] == dot_def(x, y + r * stride, n), "batch trial %d row %zu (n=%zu stride=%zu)", trial, r, n, stride);
    }

    {   /* speed: one 4096-byte dot, and 4096 rows of 4096 bytes against one query */
        enum { N = 4096, R = 4096 };
        int8_t *m = malloc((size_t)N * R); int32_t *o = malloc(sizeof(int32_t) * R); volatile int32_t sink = 0;
        for (size_t i = 0; i < (size_t)N * R; i++) m[i] = (int8_t)next();
        double t0 = now(); for (int k = 0; k < 20000; k++) sink += lp_dot_i8(m + (k & 7) * N, m + N * 8, N);
        double t1 = now(); for (int k = 0; k < 5; k++) lp_dot_i8_batch(m, m, R, N, N, o);
        double t2 = now();
        printf("dot_i8 n=%d: %.1f ns; batch %dx%d: %.2f ms (%.1f GB/s)\n", N, (t1 - t0) / 20000 * 1e9, R, N,
               (t2 - t1) / 5 * 1e3, (double)N * R * 5 / (t2 - t1) / 1e9);
        (void)sink; free(m); free(o);
    }
    free(x); free(y);
    DONE("kernels");
}
