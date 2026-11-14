/* Composition: an entity from its constituents. Its ID is the hash of their IDs in order; its real coordinate is the
 * exact integer average of theirs, truncated toward zero, so it can never leave the 4-ball they lie in. */
#include "laplace/tier0.h"
#include "internal.h"
#include "blake3.h"
#include <string.h>

lp_ref lp_ref_atom(const lp_tier0_record *t0, uint32_t cp){
    lp_ref r; memset(&r, 0, sizeof r);
    const lp_tier0_record *atom = lp_tier0_at(t0, cp);
    if (atom) { r.id = atom->id; memcpy(r.c.m, atom->m, sizeof r.c.m); }
    return r;
}

typedef struct { const lp_ref *children; const uint64_t *runs; size_t count, at; } RefRuns;
static int ref_run(void *context, lp_id *child, uint64_t *run){
    RefRuns *input = context;
    if (input->at == input->count) return 1;
    *child = input->children[input->at].id;
    *run = input->runs ? input->runs[input->at] : 1;
    input->at++; return 0;
}

/* Counts weight the exact integer centroid. Native's common ordered-run
 * identity kernel handles hashing for both ordinary and compressed paths. */
static bool compose_refs(const lp_ref *ch, const uint64_t *runs, size_t n, uint8_t tier, lp_ref *out){
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (n && !ch) return false;
    uint64_t total = runs ? 0 : n;
    if (runs) for (size_t i = 0; i < n; i++) {
        if (!runs[i] || runs[i] > SIZE_MAX / sizeof(lp_id) - total) return false;
        total += runs[i];
    }
    if (total > SIZE_MAX / sizeof(lp_id)) return false;
    (void)tier;
    if (!total) return true;
    if (total == 1) { *out = ch[0]; return true; }
    out->tier = lp_tier_above(ch, n);
    lp_coord_sum sum = {{0, 0, 0, 0}, total};
    for (size_t i = 0; i < n; i++) {
        uint64_t copies = runs ? runs[i] : 1;
        for (unsigned d = 0; d < 4; d++) sum.s[d] += (__int128)ch[i].c.m[d] * copies;
    }
    RefRuns input = {ch, runs, n, 0};
    if (!lp_id_compose_runs((size_t)total, ref_run, &input, &out->id)) return false;
    lp_coord_mean(&sum, &out->c);
    return true;
}
lp_ref lp_ref_compose(const lp_ref *ch, size_t n, uint8_t tier){
    lp_ref out; compose_refs(ch, NULL, n, tier, &out); return out;
}
bool lp_ref_compose_runs(const lp_ref *ch, const uint64_t *runs, size_t n, uint8_t tier, lp_ref *out){
    if (!out) return false;
    if (n && !runs) { memset(out, 0, sizeof *out); return false; }
    lp_ref value;
    bool valid = compose_refs(ch, runs, n, tier, &value);
    *out = value; return valid;
}

lp_ref lp_ref_codepoints(const lp_tier0_record *t0, const uint8_t *s, size_t n, bool *ok){
    lp_ref r; memset(&r, 0, sizeof r); *ok = false;
    blake3_hasher h; blake3_hasher_init(&h); lp_coord_sum a = { { 0, 0, 0, 0 }, 0 }; size_t i = 0; uint32_t cp, first = 0;
    while (i < n) {
        if (!lp_utf8_next(s, n, &i, &cp)) return r;
        const lp_tier0_record *atom = lp_tier0_at(t0, cp);
        if (!atom) return r;
        if (!a.n) first = cp;
        blake3_hasher_update(&h, atom->id.b, 16); lp_coord_add(&a, atom->m);
    }
    if (!a.n) return r;
    if (a.n == 1) { *ok = true; return lp_ref_atom(t0, first); }               /* one codepoint is that codepoint */
    blake3_hasher_finalize(&h, r.id.b, 16); lp_coord_mean(&a, &r.c); r.tier = 1; *ok = true;
    return r;
}
static int same(const lp_ref *a, const lp_ref *b, size_t L){ for (size_t i = 0; i < L; i++) if (memcmp(&a[i].id, &b[i].id, 16)) return 0; return 1; }
/* ---- the shortest square at every position: for each i, the least p >= 2 such that ch[i, i + p) is not one constituent
 * over and over and ch[i + p, i + 2p) is the same block again (0 when there is none). That least p is a primitive period
 * (a block that is itself a repeat has a shorter square at the same place), and every square of a primitive period lies
 * in a run, a maximal repetition of that period. Every run of period p has a Lyndon root, a rotation of its block that is
 * the longest Lyndon word starting where it stands, under one of the two orders of the constituents (the runs theorem:
 * Bannai, I, Inenaga, Nakashima, Takeda, Tsuruta 2017). The longest Lyndon word at i ends at the next suffix smaller than
 * i's; the suffix comparisons and each run's extent are longest common extensions, read off two polynomial hashes of the
 * constituents' ranks modulo 2^61 - 1 (a block a square is taken from is compared constituent by constituent before it
 * is used). O(n log n) in time, where trying each later copy of ch[i] as a period was O(n^2) on any long sequence whose
 * constituents recur (a generated table of 600,000 tokens took hours). */
enum { FACTOR_DIRECT = 64 };
#define SQ_PRIME ((UINT64_C(1) << 61) - 1)
static inline uint64_t sq_mul(uint64_t a, uint64_t b){
    unsigned __int128 p = (unsigned __int128)a * b;
    uint64_t r = (uint64_t)(p & SQ_PRIME) + (uint64_t)(p >> 61);
    r = (r & SQ_PRIME) + (r >> 61);
    return r >= SQ_PRIME ? r - SQ_PRIME : r;
}
typedef struct { const uint32_t *rank; const uint64_t *h[2], *pw[2]; size_t n; } Squares;
static inline uint64_t sq_hash(const Squares *s, int k, size_t at, size_t len){
    uint64_t x = s->h[k][at + len] + SQ_PRIME - sq_mul(s->h[k][at], s->pw[k][len]);
    return x >= SQ_PRIME ? x - SQ_PRIME : x;
}
static inline bool sq_equal(const Squares *s, size_t a, size_t b, size_t len){
    return sq_hash(s, 0, a, len) == sq_hash(s, 0, b, len) && sq_hash(s, 1, a, len) == sq_hash(s, 1, b, len);
}
/* the longest l <= cap with [a, a + l) the same as [b, b + l) (forward), or [a - l, a) as [b - l, b) (backward) */
static size_t sq_extend(const Squares *s, size_t a, size_t b, size_t cap, bool backward){
    size_t l = 0;
    while (l < cap && l < 8 && s->rank[backward ? a - 1 - l : a + l] == s->rank[backward ? b - 1 - l : b + l]) l++;
    if (l < 8 || l == cap) return l;
#define SQ_SAME(len) (backward ? sq_equal(s, a - (len), b - (len), (len)) : sq_equal(s, a, b, (len)))
    size_t step = 8;
    while (step <= cap - l && SQ_SAME(l + step)) { l += step; step <<= 1; }
    size_t hi = step <= cap - l ? l + step - 1 : cap;
    while (l < hi) { size_t mid = l + (hi - l + 1) / 2; if (SQ_SAME(mid)) l = mid; else hi = mid - 1; }
#undef SQ_SAME
    return l;
}
/* the suffix at j (j > i) is smaller than the suffix at i under order o (o = 1: the constituents' order reversed); a
 * suffix that is a prefix of the other is the smaller under both */
static bool sq_smaller(const Squares *s, size_t j, size_t i, int o){
    size_t l = sq_extend(s, i, j, s->n - j, false);
    if (j + l == s->n) return true;
    uint32_t x = s->rank[j + l], y = s->rank[i + l];
    return o ? x > y : x < y;
}
static uint32_t *shortest_squares(const lp_ref *ch, size_t n){
    uint32_t *rank = lp_alloc(n * sizeof *rank), *next = lp_alloc(n * sizeof *next), *sq = lp_alloc(n * sizeof *sq);
    uint64_t *h[2], *pw[2];
    static const uint64_t base[2] = { UINT64_C(0x1f3d5b79a2c4e6f1) % SQ_PRIME, UINT64_C(0x0b5ad4eceda1ce2b) % SQ_PRIME };
    lp_idmap *ids = lp_idmap_new();
    for (size_t i = 0; i < n; i++) { bool fresh; rank[i] = (uint32_t)lp_idmap_put(ids, &ch[i].id, &fresh); }   /* one rank per distinct ID */
    lp_idmap_free(ids);
    for (int k = 0; k < 2; k++) {
        h[k] = lp_alloc((n + 1) * sizeof *h[k]); pw[k] = lp_alloc((n + 1) * sizeof *pw[k]);
        h[k][0] = 0; pw[k][0] = 1;
        for (size_t i = 0; i < n; i++) {
            uint64_t x = sq_mul(h[k][i], base[k]) + rank[i] + 1;
            h[k][i + 1] = x >= SQ_PRIME ? x - SQ_PRIME : x;
            pw[k][i + 1] = sq_mul(pw[k][i], base[k]);
        }
    }
    Squares s = { rank, { h[0], h[1] }, { pw[0], pw[1] }, n };
    memset(sq, 0, n * sizeof *sq);
    for (int o = 0; o < 2; o++) {
        for (size_t i = n; i-- > 0; ) {                                 /* the next smaller suffix, by its own chain */
            size_t j = i + 1;
            while (j < n && !sq_smaller(&s, j, i, o)) j = next[j];
            next[i] = (uint32_t)j;
        }
        for (size_t i = 0; i < n; i++) {
            size_t p = next[i] - i, j = i + p;
            if (p < 2 || j >= n) continue;                              /* a Lyndon word of one constituent, or no copy after it */
            size_t r = sq_extend(&s, i, j, n - j, false);
            size_t l = i ? sq_extend(&s, i, j, (2 * p - 1 < i ? 2 * p - 1 : i), true) : 0;
            if (l + r < p) continue;                                    /* no square of this period through i */
            /* one root a run: the first whose word is p long (a later root, l >= p, is taken only when the one before it
             * was not a longest Lyndon word, which the runs theorem allows only at the run's start) */
            if (l >= p && (l >= 2 * p - 1 || next[i - p] - (i - p) == p)) continue;
            for (size_t x = i - l; x + 2 * p <= j + r; x++) if (!sq[x] || sq[x] > p) sq[x] = (uint32_t)p;
        }
    }
    for (int k = 0; k < 2; k++) { lp_free(h[k]); lp_free(pw[k]); }
    lp_free(rank); lp_free(next);
    return sq;
}
/* the shortest non-uniform block that repeats at i, by trying each length; 0 when none */
static size_t square_at(const lp_ref *ch, size_t n, size_t i){
    size_t run = 1; while (i + run < n && !memcmp(&ch[i].id, &ch[i + run].id, 16)) run++;
    for (size_t L = run < 2 ? 2 : run; L <= (n - i) / 2; L++) {
        if (memcmp(&ch[i].id, &ch[i + L].id, 16) || !same(&ch[i], &ch[i + L], L)) continue;
        size_t d = 1; while (d < L && !memcmp(&ch[i].id, &ch[i + d].id, 16)) d++;
        if (d < L) return L;                                            /* one constituent over and over is a run, not a block */
    }
    return 0;
}
size_t lp_factor(const lp_ref *ch, size_t n, lp_ref *out, lp_compose_fn compose, void *sink){
    /* every square is found before anything is written: out may be ch itself */
    uint32_t *sq = n > FACTOR_DIRECT && n < UINT32_MAX ? shortest_squares(ch, n) : NULL;
    size_t m = 0;
    for (size_t i = 0; i < n; ) {
        size_t best = sq ? sq[i] : square_at(ch, n, i), k = 0;
        if (best && sq && !same(&ch[i], &ch[i + best], best)) best = square_at(ch, n, i);  /* a hash that agreed falsely */
        if (best) { k = 2; while (i + (k + 1) * best <= n && same(&ch[i], &ch[i + k * best], best)) k++; }
        if (!best) { out[m++] = ch[i++]; continue; }
        lp_ref stack[64], *inner = best <= 64 ? stack : lp_alloc(best * sizeof *inner);
        size_t ni = lp_factor(&ch[i], best, inner, compose, sink);                            /* the block, factored the same way */
        lp_ref block = compose ? compose(sink, inner, (uint32_t)ni, lp_tier_above(inner, ni)) : lp_ref_compose(inner, ni, lp_tier_above(inner, ni));
        if (inner != stack) lp_free(inner);
        block.said = 0;
        for (size_t r = 0; r < k; r++) out[m++] = block;
        i += best * k;
    }
    lp_free(sq);
    return m;
}
