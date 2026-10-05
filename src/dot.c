/* int8 dot products for quantized rows. Integer arithmetic, so every path gives the same value: the sum modulo 2^32,
 * which is the exact sum while n <= 131071 (|a[i] * b[i]| <= 2^14). */
#include "laplace/laplace.h"
#include "internal.h"

int32_t lp_dot_i8_scalar(const int8_t *a, const int8_t *b, size_t n){
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += (uint32_t)((int32_t)a[i] * b[i]);
    return (int32_t)s;
}
void lp_dot_i8_batch_scalar(const int8_t *q, const int8_t *rows, size_t nrows, size_t n, size_t stride, int32_t *out){
    for (size_t r = 0; r < nrows; r++) out[r] = lp_dot_i8_scalar(q, rows + r * stride, n);
}

typedef int32_t (*dot_fn)(const int8_t *, const int8_t *, size_t);
typedef void (*batch_fn)(const int8_t *, const int8_t *, size_t, size_t, size_t, int32_t *);
static void pick(dot_fn *d, batch_fn *b){
    uint32_t f = lp_cpu_active();
#if defined(LP_HAVE_AVX512VNNI)
    if (f & LP_CPU_VNNI512) { *d = lp_dot_i8_avx512vnni; *b = lp_dot_i8_batch_avx512vnni; return; }
#endif
#if defined(LP_HAVE_AVXVNNI)
    if (f & LP_CPU_AVXVNNI) { *d = lp_dot_i8_avxvnni; *b = lp_dot_i8_batch_avxvnni; return; }
#endif
#if defined(LP_HAVE_AVX512)
    if (f & LP_CPU_AVX512) { *d = lp_dot_i8_avx512; *b = lp_dot_i8_batch_avx512; return; }
#endif
#if defined(LP_HAVE_AVX2)
    if (f & LP_CPU_AVX2) { *d = lp_dot_i8_avx2; *b = lp_dot_i8_batch_avx2; return; }
#endif
    (void)f; *d = lp_dot_i8_scalar; *b = lp_dot_i8_batch_scalar;
}
static dot_fn dot; static batch_fn batch;

int32_t lp_dot_i8(const int8_t *a, const int8_t *b, size_t n){
    if (!dot) pick(&dot, &batch);
    return dot(a, b, n);
}
void lp_dot_i8_batch(const int8_t *q, const int8_t *rows, size_t nrows, size_t n, size_t stride, int32_t *out){
    if (!batch) pick(&dot, &batch);
    batch(q, rows, nrows, n, stride, out);
}
