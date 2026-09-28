/* Fixed-point coordinates: exact integer centroids, the wall, and 4D Hilbert order. */
#include "laplace/laplace.h"

void lp_coord_centroid(const lp_coord *c, size_t n, lp_coord *out){
    __int128 s[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < n; i++) for (int d = 0; d < 4; d++) s[d] += c[i].m[d];
    for (int d = 0; d < 4; d++) out->m[d] = n ? (int64_t)(s[d] / (__int128)n) : 0;      /* C division truncates toward zero */
}

bool lp_coord_inside(const lp_coord *c){
    unsigned __int128 s = 0;
    for (int d = 0; d < 4; d++) { __int128 v = c->m[d]; s += (unsigned __int128)(v * v); }
    return s <= ((unsigned __int128)1 << 106);
}

/* Skilling's transpose-to-axes inverse: axes to Hilbert index, 4 dimensions of 16 bits. */
uint64_t lp_hilbert4_grid(const uint32_t g[4]){
    uint64_t X[4] = { g[0], g[1], g[2], g[3] };
    for (uint64_t Q = 1u << 15; Q > 1; Q >>= 1) {
        uint64_t P = Q - 1;
        for (int i = 0; i < 4; i++) {
            if (X[i] & Q) X[0] ^= P;
            else { uint64_t t = (X[0] ^ X[i]) & P; X[0] ^= t; X[i] ^= t; }
        }
    }
    for (int i = 1; i < 4; i++) X[i] ^= X[i - 1];
    uint64_t t = 0;
    for (uint64_t Q = 1u << 15; Q > 1; Q >>= 1) if (X[3] & Q) t ^= Q - 1;
    for (int i = 0; i < 4; i++) X[i] ^= t;
    uint64_t h = 0;
    for (int b = 15; b >= 0; b--) for (int i = 0; i < 4; i++) h = (h << 1) | ((X[i] >> b) & 1);
    return h;
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
