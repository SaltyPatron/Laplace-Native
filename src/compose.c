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
    out->tier = tier;
    if (!total) return true;
    if (total == 1) { *out = ch[0]; return true; }
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
size_t lp_factor(const lp_ref *ch, size_t n, lp_ref *out, lp_compose_fn compose, void *sink){
    size_t m = 0, run_end = 0;
    for (size_t i = 0; i < n; ) {
        /* A non-uniform repeated block cannot be shorter than the initial
         * run: it would either be homogeneous or cross the run boundary in
         * only its second copy. Keep that boundary while consuming the run. */
        if (run_end <= i) {
            run_end = i + 1;
            while (run_end < n && !memcmp(&ch[i].id, &ch[run_end].id, 16)) run_end++;
        }
        size_t first = run_end - i; if (first < 2) first = 2;
        size_t best = 0, k = 0;
        for (size_t L = first; L <= (n - i) / 2 && !best; L++) {               /* the shortest non-uniform block that repeats here */
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
