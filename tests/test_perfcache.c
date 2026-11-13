/* A perf-cache module written, mapped and read back: direct slots, IDs, pairs, its fingerprint and tier-0 binding, and
 * the loader refusing what is not whole (a changed byte, a stale tier 0) and the writer refusing what is not lawful
 * (unsorted admitted records, one ID that is two different records, a key with two values). */
#include "laplace/perfcache.h"
#include "laplace/tier0.h"
#include "check.h"
#include <stdio.h>

static lp_tier0_record record_of(lp_ref r, uint32_t slot){
    lp_tier0_record x; memset(&x, 0, sizeof x);
    x.id = r.id; memcpy(x.m, r.c.m, sizeof x.m); x.hilbert = lp_hilbert4(&r.c); x.rank = slot; x.pad = r.tier;
    return x;
}
static int by_id(const void *a, const void *b){ return memcmp(((const lp_tier0_record *)a)->id.b, ((const lp_tier0_record *)b)->id.b, 16); }
static int by_key(const void *a, const void *b){ const lp_pair *x = a, *y = b; int c = memcmp(x->a.b, y->a.b, 16); return c ? c : memcmp(x->b.b, y->b.b, 16); }
static bool copy_with_flip(const char *from, const char *to, long at){
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb"); if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return false; }
    int c; long i = 0; while ((c = fgetc(in)) != EOF) { fputc(i++ == at ? c ^ 1 : c, out); }
    fclose(in); fclose(out); return true;
}

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0); CHECK(t0, "tier 0 maps"); if (!t0) DONE("perfcache");
    /* dense: the letters a..h by file; sparse: every two-letter [file, rank] composition, by ID; pairs: (file, rank) -> square */
    lp_tier0_record rec[8 + 64]; lp_pair pair[64]; lp_ref file[8], rank[8];
    for (uint32_t f = 0; f < 8; f++) { file[f] = lp_ref_atom(t0, 'a' + f); rank[f] = lp_ref_atom(t0, '1' + f); rec[f] = record_of(file[f], f); }
    for (uint32_t s = 0; s < 64; s++) {
        lp_ref two[2] = { file[s & 7], rank[s >> 3] }; lp_ref sq = lp_ref_compose(two, 2, lp_tier_above(two, 2));
        rec[8 + s] = record_of(sq, 0); pair[s] = (lp_pair){ file[s & 7].id, rank[s >> 3].id, sq.id };
    }
    qsort(rec + 8, 64, sizeof *rec, by_id); for (uint32_t s = 0; s < 64; s++) rec[8 + s].rank = s;
    qsort(pair, 64, sizeof *pair, by_key);
    lp_section sec[3] = { { "file", LP_PERFCACHE_DENSE, 0, 8, 0 }, { "square", LP_PERFCACHE_SPARSE, 0, 64, 0 }, { "at", LP_PERFCACHE_PAIRS, 0, 64, 0 } };
    uint8_t depends[32], fp[32], again[32]; lp_tier0_fingerprint(t0, depends);
    char path[4096] = "test_perfcache.bin", tampered[4096];   /* in the test's working directory: the build tree */
    snprintf(tampered, sizeof tampered, "%s.tampered", path);
    CHECK(lp_perfcache_write(path, "squares", depends, sec, 3, rec, 72, pair, 64, fp), "a lawful module is written");
    char path2[4200]; snprintf(path2, sizeof path2, "%s.again", path);
    CHECK(lp_perfcache_write(path2, "squares", depends, sec, 3, rec, 72, pair, 64, again) && !memcmp(fp, again, 32), "the same inputs give the same fingerprint");

    const lp_perfcache *pc = lp_perfcache_map(path); CHECK(pc, "the module maps");
    if (pc) {
        uint8_t mapped[32]; lp_perfcache_fingerprint(pc, mapped); CHECK(!memcmp(mapped, fp, 32), "mapped fingerprint is the written one");
        CHECK(lp_perfcache_map(path) == pc, "mapped once per path");
        CHECK(!strcmp(pc->module, "squares") && pc->nrec == 72 && pc->npairs == 64 && pc->nsections == 3, "header read back");
        CHECK(lp_perfcache_bound(pc, t0), "bound to the tier 0 it was composed from");
        const lp_section *f = lp_perfcache_section(pc, "file"), *sq = lp_perfcache_section(pc, "square"), *at = lp_perfcache_section(pc, "at");
        CHECK(f && sq && at && !lp_perfcache_section(pc, "nothing"), "sections by name");
        for (uint32_t i = 0; f && i < 8; i++) { const lp_tier0_record *r = lp_perfcache_at(pc, f, i); CHECK(r && lp_id_eq(&r->id, &file[i].id) && r->rank == i, "dense slot %u", i); }
        CHECK(!lp_perfcache_at(pc, f, 8) && !lp_perfcache_at(pc, at, 0), "no slot past a section, none in a pair table");
        for (uint32_t s = 0; s < 64; s++) {
            lp_ref two[2] = { file[s & 7], rank[s >> 3] }; lp_ref want = lp_ref_compose(two, 2, lp_tier_above(two, 2));
            const lp_section *in = NULL; const lp_tier0_record *r = lp_perfcache_find(pc, &want.id, &in);
            CHECK(r && in == sq && !memcmp(r->m, want.c.m, sizeof r->m) && r->pad == want.tier && r->hilbert == lp_hilbert4(&want.c), "square %u by ID", s);
            const lp_pair *p = lp_perfcache_pair(pc, at, &file[s & 7].id, &rank[s >> 3].id);
            CHECK(p && lp_id_eq(&p->c, &want.id), "pair %u", s);
        }
        lp_id none; memset(&none, 0x5a, sizeof none);
        CHECK(!lp_perfcache_find(pc, &none, NULL) && !lp_perfcache_pair(pc, at, &none, &file[0].id) && !lp_perfcache_pair(pc, sq, &file[0].id, &rank[0].id), "a miss is a miss");
        /* a stale tier 0: another table (a shard profile) is not the one the module depends on */
        const lp_tier0_record *ascii = lp_tier0_map_branches(LAPLACE_TIER0, LP_TIER0_BRANCH_ASCII);
        if (ascii) CHECK(!lp_perfcache_bound(pc, ascii), "a different tier 0 is stale");
    }
    /* a changed byte anywhere, header or body, is refused whole */
    long offsets[] = { 30, 64 + 3, 128 + 40, 128 + 3 * 48 + 5, 128 + 3 * 48 + 72 * 64 + 7 };
    for (size_t i = 0; i < sizeof offsets / sizeof *offsets; i++) {
        char t[4300]; snprintf(t, sizeof t, "%s.%zu", tampered, i);
        CHECK(copy_with_flip(path, t, offsets[i]) && !lp_perfcache_map(t), "a module with byte %ld changed is refused", offsets[i]);
        remove(t);
    }
    /* the writer refuses what the loader would */
    lp_tier0_record bad[72]; memcpy(bad, rec, sizeof bad);
    lp_tier0_record swap = bad[8]; bad[8] = bad[9]; bad[9] = swap; bad[8].rank = 0; bad[9].rank = 1;
    CHECK(!lp_perfcache_write(tampered, "squares", depends, sec, 3, bad, 72, pair, 64, NULL), "unsorted admitted records are refused");
    memcpy(bad, rec, sizeof bad); bad[1].id = bad[0].id;
    CHECK(!lp_perfcache_write(tampered, "squares", depends, sec, 3, bad, 72, pair, 64, NULL), "one ID that is two records is refused");
    memcpy(bad, rec, sizeof bad); bad[2].rank = 7;
    CHECK(!lp_perfcache_write(tampered, "squares", depends, sec, 3, bad, 72, pair, 64, NULL), "a dense record not at its slot is refused");
    lp_pair dup[64]; memcpy(dup, pair, sizeof dup); dup[1].a = dup[0].a; dup[1].b = dup[0].b;
    CHECK(!lp_perfcache_write(tampered, "squares", depends, sec, 3, rec, 72, dup, 64, NULL), "a key with two values is refused");
    lp_section late[3] = { sec[2], sec[0], sec[1] };
    CHECK(!lp_perfcache_write(tampered, "squares", depends, late, 3, rec, 72, pair, 64, NULL), "record sections come before pair tables");
    remove(path2); remove(tampered);
    DONE("perfcache");
}
