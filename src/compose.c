/* Composition: an entity from its constituents. Its ID is the hash of their IDs in order; its real coordinate is the
 * exact integer average of theirs, truncated toward zero, so it can never leave the 4-ball they lie in. */
#include "laplace/laplace.h"
#include "blake3.h"
#include <string.h>

lp_ref lp_ref_atom(const lp_tier0_record *t0, uint32_t cp){
    lp_ref r; r.id = t0[cp].id; memcpy(r.c.m, t0[cp].m, sizeof r.c.m); r.tier = 0; r.said = 0; return r;
}

lp_ref lp_ref_compose(const lp_ref *ch, size_t n, uint8_t tier){
    if (n == 1) return ch[0];
    lp_ref r; memset(&r, 0, sizeof r); r.tier = tier;
    if (n == 0) return r;
    blake3_hasher h; blake3_hasher_init(&h); __int128 s[4] = { 0, 0, 0, 0 };
    for (size_t i = 0; i < n; i++) {
        blake3_hasher_update(&h, ch[i].id.b, 16);
        for (int d = 0; d < 4; d++) s[d] += ch[i].c.m[d];
    }
    blake3_hasher_finalize(&h, r.id.b, 16);
    for (int d = 0; d < 4; d++) r.c.m[d] = (int64_t)(s[d] / (__int128)n);
    return r;
}

static uint8_t above(const lp_ref *r, size_t n){ uint8_t t = 0; for (size_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
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
        lp_ref inner[best]; size_t ni = lp_factor(&ch[i], best, inner, compose, sink);   /* the block, factored the same way */
        lp_ref block = compose ? compose(sink, inner, (uint32_t)ni, above(inner, ni)) : lp_ref_compose(inner, ni, above(inner, ni));
        block.said = 0;
        for (size_t r = 0; r < k; r++) out[m++] = block;
        i += best * k;
    }
    return m;
}
