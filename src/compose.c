/* Composition: an entity from its constituents. Its ID is the hash of their IDs in order; its real coordinate is the
 * exact integer average of theirs, truncated toward zero, so it can never leave the 4-ball they lie in. */
#include "laplace/laplace.h"
#include "internal.h"
#include "blake3.h"
#include <string.h>

lp_ref lp_ref_atom(const lp_tier0_record *t0, uint32_t cp){
    lp_ref r; memset(&r, 0, sizeof r); r.id = t0[cp].id; memcpy(r.c.m, t0[cp].m, sizeof r.c.m); return r;
}

/* The children's IDs go to the hasher 64 at a time, gathered side by side: the same bytes as lp_id_compose hashes. */
lp_ref lp_ref_compose(const lp_ref *ch, size_t n, uint8_t tier){
    if (n == 1) return ch[0];
    lp_ref r; memset(&r, 0, sizeof r); r.tier = tier;
    if (n == 0) return r;
    lp_coord_sum a = { { 0, 0, 0, 0 }, 0 }; lp_id run[64];
    if (n <= 64) {                                         /* one chunk: the IDs side by side, hashed at once (lp_hash16) */
        for (size_t i = 0; i < n; i++) { run[i] = ch[i].id; lp_coord_add(&a, ch[i].c.m); }
        lp_hash16(run, 16 * n, &r.id);
    } else {                                               /* more: the hasher, 64 IDs a call */
        blake3_hasher h; blake3_hasher_init(&h);
        for (size_t i = 0; i < n; ) {
            size_t k = 0; for (; k < 64 && i < n; k++, i++) { run[k] = ch[i].id; lp_coord_add(&a, ch[i].c.m); }
            blake3_hasher_update(&h, run, 16 * k);
        }
        blake3_hasher_finalize(&h, r.id.b, 16);
    }
    lp_coord_mean(&a, &r.c);
    return r;
}

lp_ref lp_ref_codepoints(const lp_tier0_record *t0, const uint8_t *s, size_t n, bool *ok){
    lp_ref r; memset(&r, 0, sizeof r); *ok = false;
    blake3_hasher h; blake3_hasher_init(&h); lp_coord_sum a = { { 0, 0, 0, 0 }, 0 }; size_t i = 0; uint32_t cp, first = 0;
    while (i < n) {
        if (!lp_utf8_next(s, n, &i, &cp)) return r;
        if (!a.n) first = cp;
        blake3_hasher_update(&h, t0[cp].id.b, 16); lp_coord_add(&a, t0[cp].m);
    }
    if (!a.n) return r;
    if (a.n == 1) { *ok = true; return lp_ref_atom(t0, first); }               /* one codepoint is that codepoint */
    blake3_hasher_finalize(&h, r.id.b, 16); lp_coord_mean(&a, &r.c); r.tier = 1; *ok = true;
    return r;
}
static int same(const lp_ref *a, const lp_ref *b, size_t L){ for (size_t i = 0; i < L; i++) if (memcmp(&a[i].id, &b[i].id, 16)) return 0; return 1; }
size_t lp_factor(const lp_ref *ch, size_t n, lp_ref *out, lp_compose_fn compose, void *sink){
    size_t m = 0;
    for (size_t i = 0; i < n; ) {
        size_t best = 0, k = 0;
        for (size_t L = 2; L <= (n - i) / 2 && !best; L++) {                   /* the shortest block that repeats here: the period */
            if (memcmp(&ch[i].id, &ch[i + L].id, 16) || !same(&ch[i], &ch[i + L], L)) continue;
            { size_t d = 1; while (d < L && !memcmp(&ch[i].id, &ch[i + d].id, 16)) d++; if (d == L) continue; }   /* one constituent over and over is a run, not a block */
            k = 2; while (i + (k + 1) * L <= n && same(&ch[i], &ch[i + k * L], L)) k++;
            best = L;
        }
        if (!best) { out[m++] = ch[i++]; continue; }
        lp_ref stack[64], *inner = best <= 64 ? stack : lp_alloc(best * sizeof *inner);
        size_t ni = lp_factor(&ch[i], best, inner, compose, sink);                            /* the block, factored the same way */
        lp_ref block = compose ? compose(sink, inner, (uint32_t)ni, lp_tier_above(inner, ni)) : lp_ref_compose(inner, ni, lp_tier_above(inner, ni));
        if (inner != stack) lp_free(inner);
        block.said = 0;
        for (size_t r = 0; r < k; r++) out[m++] = block;
        i += best * k;
    }
    return m;
}
