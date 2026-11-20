/* Fixed-point coordinates: exact integer centroids, the wall, and 4D Hilbert order. */
#include "laplace/laplace.h"
#include "internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void lp_coord_mean(const lp_coord_sum *a, lp_coord *out){ for (int d = 0; d < 4; d++) out->m[d] = a->n ? lp_div128(a->s[d], a->n) : 0; }

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
void lp_coord_trunc(const double x[4], lp_coord *out){
    for (int d = 0; d < 4; d++) { double m = x[d] * LP_FIXED_ONE; out->m[d] = m != m ? 0 : m >= LP_FIXED_ONE ? (int64_t)LP_FIXED_ONE : m <= -LP_FIXED_ONE ? -(int64_t)LP_FIXED_ONE : (int64_t)m; }
}

bool lp_coord_inside(const lp_coord *c){
    unsigned __int128 s = 0;
    for (int d = 0; d < 4; d++) {
        /* Reject an axis outside the cube before squaring. Four INT64_MIN axes otherwise wrap the
         * unsigned 128-bit sum to zero and incorrectly pass the wall check. */
        if (c->m[d] < -(INT64_C(1) << 53) || c->m[d] > (INT64_C(1) << 53)) return false;
        __int128 v = c->m[d]; s += (unsigned __int128)(v * v);
    }
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

/* floor((x + 1) * 32768), clamped to [0, 65535]. Scaling by a power of two is exact here. Split the integer
 * and fractional parts before adding the offset: x + 1 would erase small negative values and round points
 * immediately below a positive cell boundary onto it. This also leaves the result independent of FP mode. */
uint32_t lp_hilbert4_axis(double x){
    if (!(x > -1.0)) return 0;
    if (x >= 1.0) return 65535u;
    double scaled = x * 32768.0;
    int32_t whole = (int32_t)scaled;
    return (uint32_t)(32768 + whole - (scaled < (double)whole));
}

uint64_t lp_hilbert4(const lp_coord *c){
    uint32_t g[4];
    /* floor((m / 2^53 + 1) * 2^15) = floor((m + 2^53) / 2^38).
     * Keep it integral: the double addition rounds a point immediately below a
     * positive grid boundary onto the boundary, and depends on the caller's FP mode. */
    const int64_t one = INT64_C(1) << 53;
    for (int d = 0; d < 4; d++)
        g[d] = c->m[d] <= -one ? 0 : c->m[d] >= one ? 65535u
             : (uint32_t)((uint64_t)(c->m[d] + one) >> 38);
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

/* Box to ranges: the 16-ary tree of Hilbert cells. A cell at level l (side 2^(16-l)) is the Hilbert values sharing
 * one 4l-bit prefix, and its corner is the decoded first value with the low bits cleared. The exact cover is tried
 * first, depth first: cells inside the box taken whole, cells crossing its faces split, within LP_HRANGE_WORK cells
 * of work. A box too large for that is covered a level at a time, crossing cells taken whole once the next level
 * could not fit the work bound. Either way the ranges are sorted, merged where adjacent, and the narrowest gaps
 * closed until there are at most cap. */
typedef struct { uint64_t prefix; } cell;
static int range_cmp(const void *a, const void *b){
    const lp_hrange *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}
/* Where the cell with prefix p at level L (1..16) lies against the box: 0 outside, 1 inside, 2 crossing a face. */
static int cell_vs_box(uint64_t p, int L, const uint32_t lo[4], const uint32_t hi[4]){
    int shift = 4 * (16 - L); uint32_t side = 1u << (16 - L);
    uint32_t g[4]; lp_hilbert4_decode(p << shift, g);
    int inside = 1;
    for (int d = 0; d < 4; d++) {
        uint32_t o = g[d] & ~(side - 1), e = o + side - 1;
        if (e < lo[d] || o > hi[d]) return 0;
        if (o < lo[d] || e > hi[d]) inside = 0;
    }
    return inside ? 1 : 2;
}
/* The exact cover, depth first: the number of cells, written to out when out is not NULL; 0 when the walk would
 * visit more than room cells (inside or crossing), which a box with any cell never otherwise gives. */
static size_t exact_cover(const uint32_t lo[4], const uint32_t hi[4], lp_hrange *out, size_t room){
    uint64_t stack[16 * 16]; int lvl[16 * 16]; size_t sp = 0, n = 0, work = 0;
    for (uint64_t k = 0; k < 16; k++) { stack[sp] = k; lvl[sp] = 1; sp++; }
    while (sp) {
        uint64_t p = stack[--sp]; int L = lvl[sp];
        int r = cell_vs_box(p, L, lo, hi);
        if (r == 0) continue;
        if (++work > room) return 0;
        if (r == 1) {
            if (out) { int shift = 4 * (16 - L); out[n].lo = p << shift; out[n].hi = out[n].lo | ((1ull << shift) - 1); }
            n++; continue;
        }
        for (uint64_t k = 0; k < 16; k++) { stack[sp] = (p << 4) | k; lvl[sp] = L + 1; sp++; }   /* L < 16: a single cell is never crossing */
    }
    return n;
}
/* The cover a level at a time, for a box whose exact cover is beyond the work bound: at most room ranges, crossing
 * cells of the last level that fit taken whole. 0 without memory. */
static size_t level_cover(const uint32_t lo[4], const uint32_t hi[4], lp_hrange *full, size_t room){
    cell *cross = malloc(sizeof *cross * room), *next = malloc(sizeof *next * room);
    if (!cross || !next) { free(cross); free(next); return 0; }
    size_t n = 0, ncross = 1; cross[0].prefix = 0;
    int level = 0;
    for (; level < 16 && ncross; level++) {
        if (n + ncross * 16 > room) break;                               /* the next level could need more than the room */
        size_t nn = 0;
        for (size_t c = 0; c < ncross; c++)
            for (uint64_t k = 0; k < 16; k++) {
                uint64_t p = (cross[c].prefix << 4) | k;
                int r = cell_vs_box(p, level + 1, lo, hi);
                if (r == 0) continue;
                if (r == 1) { int shift = 4 * (15 - level); full[n].lo = p << shift; full[n].hi = full[n].lo | ((1ull << shift) - 1); n++; }
                else next[nn++].prefix = p;
            }
        cell *t = cross; cross = next; next = t; ncross = nn;
    }
    int shift = 4 * (16 - level);                                        /* the cells still crossing, taken whole */
    for (size_t c = 0; c < ncross; c++) {
        uint64_t first = shift == 64 ? 0 : cross[c].prefix << shift;
        full[n].lo = first; full[n].hi = shift == 64 ? UINT64_MAX : first | ((1ull << shift) - 1); n++;
    }
    free(cross); free(next);
    return n;
}
/* Sorted, disjoint ranges merged where adjacent, then the m - cap narrowest gaps closed (the earliest of equal ones
 * first). Closing one gap changes no other, so the greedy sequence is the m - cap smallest gaps at once. Without
 * memory for that, one range over all of them, which covers. */
typedef struct { uint64_t width; size_t at; } gap;
static int gap_cmp(const void *a, const void *b){
    const gap *x = a, *y = b;
    if (x->width != y->width) return x->width < y->width ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}
static size_t merge_to_cap(lp_hrange *r, size_t n, size_t cap){
    qsort(r, n, sizeof *r, range_cmp);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m && r[i].lo == r[m - 1].hi + 1) { r[m - 1].hi = r[i].hi; continue; }
        r[m++] = r[i];
    }
    if (m <= cap) return m;
    gap *g = malloc(sizeof *g * (m - 1)); unsigned char *shut = calloc(m, 1);
    if (!g || !shut) { free(g); free(shut); r[0].hi = r[m - 1].hi; return 1; }
    for (size_t i = 1; i < m; i++) { g[i - 1].width = r[i].lo - r[i - 1].hi; g[i - 1].at = i; }
    qsort(g, m - 1, sizeof *g, gap_cmp);
    for (size_t i = 0; i < m - cap; i++) shut[g[i].at] = 1;
    free(g);
    size_t w = 0;
    for (size_t i = 0; i < m; i++) { if (i && shut[i]) { r[w - 1].hi = r[i].hi; continue; } r[w++] = r[i]; }
    free(shut);
    return w;
}
size_t lp_hilbert4_ranges(const uint32_t lo[4], const uint32_t hi[4], lp_hrange *out, size_t cap){
    for (int d = 0; d < 4; d++) if (lo[d] > hi[d] || lo[d] > 65535u) return 0;
    if (cap == 0) return 0;
    uint32_t h4[4]; for (int d = 0; d < 4; d++) h4[d] = hi[d] > 65535u ? 65535u : hi[d];
    size_t room = LP_HRANGE_WORK, n = exact_cover(lo, h4, NULL, room);
    lp_hrange *full = malloc(sizeof *full * (n ? n : room));
    if (full) {
        if (n) exact_cover(lo, h4, full, room);
        else n = level_cover(lo, h4, full, room);
    }
    if (!full || !n) { free(full); out[0].lo = 0; out[0].hi = UINT64_MAX; return 1; }   /* no memory: the whole space, which covers */
    size_t m = merge_to_cap(full, n, cap);
    memcpy(out, full, sizeof *full * m);
    free(full);
    return m;
}

/* Unit 4-ball B^4_1 (m . m <= 2^106) + Euclidean query ball ||x - center||_2 <= radius pruning.
 * Every axis cell [o, e] at level L spans normalized coordinate interval [-1 + o/32768, -1 + (e+1)/32768]
 * (in exact dyadic multiples of 2^-15), so B^4_1 bounds are exact 64-bit integer arithmetic with UNIT_R2 = 2^30. */
#define LP_B4_UNIT_R2 (UINT64_C(1) << 30)

static void cell_bounds_b4(uint64_t p, int L, double x_lo[4], double x_hi[4], uint64_t *min_r2, uint64_t *max_r2){
    int shift = 4 * (16 - L);
    uint32_t side = 1u << (16 - L);
    uint32_t g[4];
    lp_hilbert4_decode(p << shift, g);
    uint64_t min2 = 0, max2 = 0;
    for (int d = 0; d < 4; d++) {
        uint32_t o = g[d] & ~(side - 1u), e = o + side - 1u;
        int64_t a = (int64_t)o - 32768;
        int64_t b = (int64_t)(e + 1u) - 32768;
        uint64_t min_a = (a <= 0 && b >= 0) ? 0u : (uint64_t)(a > 0 ? a : -b);
        uint64_t aa = (uint64_t)(a < 0 ? -a : a), bb = (uint64_t)(b < 0 ? -b : b);
        uint64_t max_a = aa > bb ? aa : bb;
        min2 += min_a * min_a;
        max2 += max_a * max_a;
        if (x_lo) x_lo[d] = -1.0 + (double)o / 32768.0;
        if (x_hi) x_hi[d] = -1.0 + (double)(e + 1u) / 32768.0;
    }
    if (min_r2) *min_r2 = min2;
    if (max_r2) *max_r2 = max2;
}

static int cell_vs_b4(uint64_t p, int L){
    uint64_t min2 = 0, max2 = 0;
    cell_bounds_b4(p, L, NULL, NULL, &min2, &max2);
    if (min2 > LP_B4_UNIT_R2) return 0;
    if (max2 <= LP_B4_UNIT_R2) return 1;
    return 2;
}

/* 0 = outside B^4_1 or outside query ball; 1 = wholly inside B^4_1 and query ball; 2 = crossing boundary. */
static int cell_vs_ball_glome(uint64_t p, int L, const double cx[4], double r2_hi, double r2_lo){
    double x_lo[4], x_hi[4];
    uint64_t min2 = 0, max2 = 0;
    cell_bounds_b4(p, L, x_lo, x_hi, &min2, &max2);
    if (min2 > LP_B4_UNIT_R2) return 0;
    double min_d2 = 0.0, max_d2 = 0.0;
    for (int d = 0; d < 4; d++) {
        double gd = cx[d] < x_lo[d] ? x_lo[d] - cx[d] : cx[d] > x_hi[d] ? cx[d] - x_hi[d] : 0.0;
        double fd1 = cx[d] - x_lo[d]; if (fd1 < 0.0) fd1 = -fd1;
        double fd2 = cx[d] - x_hi[d]; if (fd2 < 0.0) fd2 = -fd2;
        double fd = fd1 > fd2 ? fd1 : fd2;
        min_d2 += gd * gd;
        max_d2 += fd * fd;
    }
    if (min_d2 > r2_hi) return 0;
    if (max2 <= LP_B4_UNIT_R2 && max_d2 <= r2_lo) return 1;
    return 2;
}

static size_t exact_cover_ball(const double cx[4], double r2_hi, double r2_lo, lp_hrange *out, size_t room, bool *overflow){
    uint64_t stack[16 * 16]; int lvl[16 * 16]; size_t sp = 0, n = 0, work = 0;
    *overflow = false;
    for (int k = 15; k >= 0; k--) { stack[sp] = (uint64_t)k; lvl[sp] = 1; sp++; }
    while (sp) {
        uint64_t p = stack[--sp]; int L = lvl[sp];
        int r = cell_vs_ball_glome(p, L, cx, r2_hi, r2_lo);
        if (r == 0) continue;
        if (++work > room) { *overflow = true; return 0; }
        if (r == 1 || L == 16) {
            if (out) {
                int shift = 4 * (16 - L);
                uint64_t lo = p << shift, hi = lo | ((UINT64_C(1) << shift) - 1u);
                if (n && lo == out[n - 1].hi + 1u) out[n - 1].hi = hi;
                else { out[n].lo = lo; out[n].hi = hi; n++; }
            } else {
                n++;
            }
            continue;
        }
        for (int k = 15; k >= 0; k--) { stack[sp] = (p << 4) | (uint64_t)k; lvl[sp] = L + 1; sp++; }
    }
    return n;
}

static size_t level_cover_ball(const double cx[4], double r2_hi, double r2_lo, lp_hrange *full, size_t room){
    cell *cross = malloc(sizeof *cross * room), *next = malloc(sizeof *next * room);
    if (!cross || !next) { free(cross); free(next); return 0; }
    size_t n = 0, ncross = 1; cross[0].prefix = 0;
    int level = 0;
    for (; level < 16 && ncross; level++) {
        if (n + ncross * 16 > room) break;
        size_t nn = 0;
        for (size_t c = 0; c < ncross; c++) {
            for (uint64_t k = 0; k < 16; k++) {
                uint64_t p = (cross[c].prefix << 4) | k;
                int r = cell_vs_ball_glome(p, level + 1, cx, r2_hi, r2_lo);
                if (r == 0) continue;
                if (r == 1) {
                    int shift = 4 * (15 - level);
                    full[n].lo = p << shift;
                    full[n].hi = full[n].lo | ((UINT64_C(1) << shift) - 1u);
                    n++;
                } else {
                    next[nn++].prefix = p;
                }
            }
        }
        cell *t = cross; cross = next; next = t; ncross = nn;
    }
    int shift = 4 * (16 - level);
    for (size_t c = 0; c < ncross && n < room; c++) {
        uint64_t first = shift == 64 ? 0 : cross[c].prefix << shift;
        full[n].lo = first;
        full[n].hi = shift == 64 ? UINT64_MAX : first | ((UINT64_C(1) << shift) - 1u);
        n++;
    }
    free(cross); free(next);
    return n;
}

static uint64_t b4_estimate_cell(uint64_t p, int L, int depth_left){
    int s = cell_vs_b4(p, L);
    if (s == 0) return 0;
    uint64_t vol = UINT64_C(1) << (4 * (16 - L));
    if (s == 1 || L == 16) return vol;
    if (depth_left <= 0) return vol / 2u;
    uint64_t sum = 0;
    for (uint64_t k = 0; k < 16; k++) sum += b4_estimate_cell((p << 4) | k, L + 1, depth_left - 1);
    return sum;
}

static uint64_t gap_live_b4_weight(uint64_t lo, uint64_t hi){
    if (lo > hi) return 0;
    uint64_t weight = 0, cur = lo;
    for (;;) {
        int best_L = 16;
        uint64_t rem = hi - cur;
        for (int L = 1; L < 16; L++) {
            int shift = 4 * (16 - L);
            uint64_t mask = (UINT64_C(1) << shift) - 1u;
            if ((cur & mask) == 0 && rem >= mask) { best_L = L; break; }
        }
        int shift = 4 * (16 - best_L);
        uint64_t mask = shift == 0 ? 0u : ((UINT64_C(1) << shift) - 1u);
        weight += b4_estimate_cell(cur >> shift, best_L, 1);
        if (rem == mask) break;
        cur += mask + 1u;
    }
    return weight;
}

typedef struct { uint64_t live; uint64_t width; size_t at; } ball_gap;
static int ball_gap_cmp(const void *a, const void *b){
    const ball_gap *x = a, *y = b;
    if (x->live != y->live) return x->live < y->live ? -1 : 1;
    if (x->width != y->width) return x->width < y->width ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;
}

static size_t merge_ball_to_cap(lp_hrange *r, size_t n, size_t cap){
    qsort(r, n, sizeof *r, range_cmp);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m && r[i].lo <= r[m - 1].hi + (r[m - 1].hi == UINT64_MAX ? 0u : 1u)) {
            if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi;
            continue;
        }
        r[m++] = r[i];
    }
    if (m <= cap) return m;
    ball_gap *g = malloc(sizeof *g * (m - 1));
    unsigned char *shut = calloc(m, 1);
    if (!g || !shut) { free(g); free(shut); r[0].hi = r[m - 1].hi; return 1; }
    for (size_t i = 1; i < m; i++) {
        uint64_t glo = r[i - 1].hi + 1u;
        uint64_t ghi = r[i].lo - 1u;
        g[i - 1].live = gap_live_b4_weight(glo, ghi);
        g[i - 1].width = r[i].lo - r[i - 1].hi;
        g[i - 1].at = i;
    }
    qsort(g, m - 1, sizeof *g, ball_gap_cmp);
    for (size_t i = 0; i < m - cap; i++) shut[g[i].at] = 1;
    free(g);
    size_t w = 0;
    for (size_t i = 0; i < m; i++) {
        if (i && shut[i]) { r[w - 1].hi = r[i].hi; continue; }
        r[w++] = r[i];
    }
    free(shut);
    return w;
}

size_t lp_hilbert4_ball_ranges(const lp_coord *center, double radius, lp_hrange *out, size_t cap){
    if (!center || !out || cap == 0 || isnan(radius) || radius < 0.0) return 0;
    if (radius == 0.0) {
        if (!lp_coord_inside(center)) return 0;
        uint64_t h = lp_hilbert4(center);
        out[0].lo = h;
        out[0].hi = h;
        return 1;
    }
    double cx[4];
    lp_coord_xyzm(center, cx);
    double eps = (1.0 + radius) * 1e-13;
    double r_hi = radius + eps;
    double r_lo = radius > eps ? radius - eps : 0.0;
    double r2_hi = r_hi * r_hi;
    double r2_lo = r_lo * r_lo;

    size_t room = LP_HRANGE_WORK;
    bool overflow = false;
    size_t n = exact_cover_ball(cx, r2_hi, r2_lo, NULL, room, &overflow);
    if (!overflow && n == 0) return 0;
    lp_hrange *full = malloc(sizeof *full * (overflow ? room : n));
    if (full) {
        if (!overflow) n = exact_cover_ball(cx, r2_hi, r2_lo, full, room, &overflow);
        else n = level_cover_ball(cx, r2_hi, r2_lo, full, room);
    }
    if (!full || n == 0) { free(full); out[0].lo = 0; out[0].hi = UINT64_MAX; return 1; }
    size_t m = merge_ball_to_cap(full, n, cap);
    memcpy(out, full, sizeof *full * m);
    free(full);
    return m;
}
