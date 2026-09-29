/* Composition: an entity from its constituents. Its ID is the hash of their IDs in order; its real coordinate is the
 * exact integer average of theirs, truncated toward zero, so it can never leave the 4-ball they lie in. */
#include "laplace/laplace.h"
#include "blake3.h"
#include <string.h>

lp_ref lp_ref_atom(const lp_tier0_record *t0, uint32_t cp){
    lp_ref r; r.id = t0[cp].id; memcpy(r.c.m, t0[cp].m, sizeof r.c.m); r.tier = 0; return r;
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
