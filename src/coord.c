/* Fixed-point coordinates: exact integer centroids, the wall, and 4D Hilbert order. */
#include "laplace/laplace.h"
#include "internal.h"

void lp_coord_centroid(const lp_coord *c, size_t n, lp_coord *out){
    lp_coord_sum a = { { 0, 0, 0, 0 }, 0 };
    for (size_t i = 0; i < n; i++) lp_coord_add(&a, c[i].m);
    lp_coord_mean(&a, out);                                                  /* truncated toward zero, as C division is */
}

/* A double is on the grid when it is exactly m / 2^53 for an integer m with |m| <= 2^53: the range is checked before
 * the conversion, so nothing outside it is ever converted. */
bool lp_coord_of_xyzm(const double x[4], lp_coord *out){
    for (int d = 0; d < 4; d++) {
        double m = x[d] * LP_FIXED_ONE;
        if (!(m >= -LP_FIXED_ONE && m <= LP_FIXED_ONE) || m != (double)(int64_t)m) return false;
        out->m[d] = (int64_t)m;
    }
    return true;
}

bool lp_coord_inside(const lp_coord *c){
    unsigned __int128 s = 0;
    for (int d = 0; d < 4; d++) { __int128 v = c->m[d]; s += (unsigned __int128)(v * v); }
    return s <= ((unsigned __int128)1 << 106);
}

/* Skilling's axes-to-transpose, 4 dimensions of 16 bits, without branches: each conditional exchange is a mask. */
static inline void hilbert_transpose(uint64_t X[4]){
    for (uint64_t Q = 1u << 15; Q > 1; Q >>= 1) {
        uint64_t P = Q - 1;
        for (int i = 0; i < 4; i++) {
            uint64_t bit = 0 - ((X[i] & Q) != 0);                         /* all ones when the bit is set */
            uint64_t t = (X[0] ^ X[i]) & P & ~bit;                        /* exchange low bits when it is clear */
            X[0] ^= (P & bit) | t; X[i] ^= t & (uint64_t)(i != 0 ? ~0ull : 0);
        }
    }
    for (int i = 1; i < 4; i++) X[i] ^= X[i - 1];
    uint64_t t = 0;
    for (uint64_t Q = 1u << 15; Q > 1; Q >>= 1) t ^= (Q - 1) & (0 - ((X[3] & Q) != 0));
    for (int i = 0; i < 4; i++) X[i] ^= t;
}

uint64_t lp_hilbert4_interleave_scalar(const uint64_t X[4]){
    uint64_t h = 0;
    for (int b = 15; b >= 0; b--) for (int i = 0; i < 4; i++) h = (h << 1) | ((X[i] >> b) & 1);
    return h;
}

uint64_t lp_hilbert4_grid(const uint32_t g[4]){
    uint64_t X[4] = { g[0], g[1], g[2], g[3] };
    hilbert_transpose(X);
    return lp_kernels()->interleave(X);
}

/* The grid is taken in IEEE double exactly as tier 0 was generated: floor((x + 1) / 2 * 65536), clamped to
 * [0, 65535], with x = m / 2^53. Built without FP contraction, so these bits are the same on every CPU. */
uint64_t lp_hilbert4(const lp_coord *c){
    uint32_t g[4];
    for (int d = 0; d < 4; d++) {
        double x = (double)c->m[d] / LP_FIXED_ONE, v = (x + 1.0) / 2.0 * 65536.0;
        g[d] = v < 0 ? 0 : v > 65535.0 ? 65535u : (uint32_t)v;
    }
    return lp_hilbert4_grid(g);
}
