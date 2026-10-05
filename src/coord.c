/* Fixed-point coordinates: exact integer centroids, the wall, and 4D Hilbert order. */
#include "laplace/laplace.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>

void lp_coord_centroid(const lp_coord *c, size_t n, lp_coord *out){
    __int128 s[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < n; i++) for (int d = 0; d < 4; d++) s[d] += c[i].m[d];
    for (int d = 0; d < 4; d++) out->m[d] = n ? lp_div128(s[d], n) : 0;      /* truncated toward zero, as C division is */
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

typedef uint64_t (*interleave_fn)(const uint64_t[4]);
static interleave_fn pick_interleave(void){
#if defined(LP_HAVE_AVX2)
    if (lp_cpu_active() & LP_CPU_AVX2) return lp_hilbert4_interleave_bmi2;            /* x86-64-v3 includes BMI2 */
#endif
    return lp_hilbert4_interleave_scalar;
}

uint64_t lp_hilbert4_grid(const uint32_t g[4]){
    static interleave_fn inter; if (!inter) inter = pick_interleave();
    uint64_t X[4] = { g[0], g[1], g[2], g[3] };
    hilbert_transpose(X);
    return inter(X);
}

/* The grid is taken in IEEE double exactly as tier 0 was generated: floor((x + 1) / 2 * 65536), clamped to
 * [0, 65535], with x = m / 2^53. Built without FP contraction, so these bits are the same on every CPU. */
uint32_t lp_hilbert4_axis(double x){
    double v = (x + 1.0) / 2.0 * 65536.0;
    return !(v >= 0) ? 0 : v > 65535.0 ? 65535u : (uint32_t)v;
}

uint64_t lp_hilbert4(const lp_coord *c){
    uint32_t g[4];
    for (int d = 0; d < 4; d++) g[d] = lp_hilbert4_axis((double)c->m[d] / LP_FIXED_ONE);
    return lp_hilbert4_grid(g);
}

/* Skilling's transpose-to-axes: hilbert_transpose's inverse. */
static inline void hilbert_axes(uint64_t X[4]){
    uint64_t t = X[3] >> 1;
    for (int i = 3; i > 0; i--) X[i] ^= X[i - 1];
    X[0] ^= t;
    for (uint64_t Q = 2; Q != (1u << 16); Q <<= 1) {
        uint64_t P = Q - 1;
        for (int i = 3; i >= 0; i--) {
            if (X[i] & Q) X[0] ^= P;
            else { t = (X[0] ^ X[i]) & P; X[0] ^= t; X[i] ^= t; }
        }
    }
}

void lp_hilbert4_decode(uint64_t h, uint32_t g[4]){
    uint64_t X[4] = { 0, 0, 0, 0 };
    for (int b = 15; b >= 0; b--) for (int i = 0; i < 4; i++) X[i] |= ((h >> (4 * b + 3 - i)) & 1) << b;
    hilbert_axes(X);
    for (int i = 0; i < 4; i++) g[i] = (uint32_t)X[i];
}

/* Box to ranges: the 16-ary tree of Hilbert cells, a level at a time. A cell at level l (side 2^(16-l)) is the
 * Hilbert values sharing one 4l-bit prefix, and its corner is the decoded first value with the low bits cleared.
 * Cells inside the box are emitted whole; cells crossing its faces are split at the next level while the ranges
 * could still fit in cap, and emitted whole once they could not. */
typedef struct { uint64_t prefix; } cell;
static int range_cmp(const void *a, const void *b){
    const lp_hrange *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}
size_t lp_hilbert4_ranges(const uint32_t lo[4], const uint32_t hi[4], lp_hrange *out, size_t cap){
    for (int d = 0; d < 4; d++) if (lo[d] > hi[d] || lo[d] > 65535u) return 0;
    if (cap == 0) return 0;
    uint32_t h4[4]; for (int d = 0; d < 4; d++) h4[d] = hi[d] > 65535u ? 65535u : hi[d];
    /* The level loop keeps full ranges plus crossing cells within room, so these never grow. The room is the work
     * bound, not the budget: a box whose exact cover fits it is refined to the exact cover whatever cap is, and the
     * budget is met afterwards by closing gaps, so a small box is exact whenever its cell count fits cap. */
    size_t n = 0, ncross = 1, room = cap * 4 + 16;
    if (room < LP_HRANGE_WORK) room = LP_HRANGE_WORK;
    lp_hrange *full = malloc(sizeof *full * room);
    cell *cross = malloc(sizeof *cross * room), *next = malloc(sizeof *next * room);
    if (!full || !cross || !next) {                     /* no memory: the whole space, which covers; never an empty answer */
        free(full); free(cross); free(next);
        out[0].lo = 0; out[0].hi = UINT64_MAX; return 1;
    }
    cross[0].prefix = 0;
    int level = 0;
    for (; level < 16 && ncross; level++) {
        if (n + ncross * 16 > room) break;      /* the next level could need more cells than there is room for */
        int shift = 4 * (15 - level);                              /* low bits of a child's first value */
        uint32_t side = 1u << (15 - level);
        size_t nn = 0;
        for (size_t c = 0; c < ncross; c++) {
            for (uint64_t k = 0; k < 16; k++) {
                uint64_t p = (cross[c].prefix << 4) | k, first = p << shift;
                uint32_t g[4]; lp_hilbert4_decode(first, g);
                int inside = 1, out_ = 0;
                for (int d = 0; d < 4; d++) {
                    uint32_t o = g[d] & ~(side - 1), e = o + side - 1;
                    if (e < lo[d] || o > h4[d]) { out_ = 1; break; }
                    if (o < lo[d] || e > h4[d]) inside = 0;
                }
                if (out_) continue;
                if (inside) {
                    full[n].lo = first; full[n].hi = first | ((1ull << shift) - 1); n++;
                } else next[nn++].prefix = p;
            }
        }
        cell *t = cross; cross = next; next = t; ncross = nn;
    }
    /* The cells still crossing the box are taken whole. */
    int shift = 4 * (16 - level);
    for (size_t c = 0; c < ncross; c++) {
        uint64_t first = shift == 64 ? 0 : cross[c].prefix << shift;
        full[n].lo = first; full[n].hi = shift == 64 ? UINT64_MAX : first | ((1ull << shift) - 1); n++;
    }
    free(cross); free(next);
    qsort(full, n, sizeof *full, range_cmp);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m && full[i].lo == full[m - 1].hi + 1) { full[m - 1].hi = full[i].hi; continue; }
        full[m++] = full[i];
    }
    /* Over budget: close the narrowest gaps (the earliest of equal ones), each merging two neighbours. */
    while (m > cap) {
        size_t at = 1; uint64_t best = full[1].lo - full[0].hi;
        for (size_t i = 2; i < m; i++) if (full[i].lo - full[i - 1].hi < best) { best = full[i].lo - full[i - 1].hi; at = i; }
        full[at - 1].hi = full[at].hi;
        memmove(full + at, full + at + 1, sizeof *full * (m - at - 1)); m--;
    }
    memcpy(out, full, sizeof *full * m);
    free(full);
    return m;
}
