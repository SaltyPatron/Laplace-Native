/* The highway: the types as the resources list them, read back from the records by slot and by ID, and the edges. */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    const lp_highway *h = lp_highway_map(NULL);
    CHECK(h != NULL, "cannot map the highway at %s (generate it with: laplace highway)", lp_highway_path());
    if (!h || !t0) DONE("highway");
    const lp_list *upos = lp_highway_list(h, "upos"), *deprel = lp_highway_list(h, "Universal dependency relation"), *ili = lp_highway_list(h, "ili");
    CHECK(upos && upos->count == 17, "UD's parts of speech: %u", upos ? upos->count : 0);
    CHECK(deprel && deprel->count >= 37, "UD's relations by the list's name as it is said");
    lp_id noun; lp_id_codepoints_utf8("NOUN", 4, &noun);
    int64_t s = lp_highway_slot(h, upos, &noun);
    CHECK(s == 7, "NOUN is UD's 8th part of speech: slot %lld", (long long)s);
    const lp_tier0_record *r = lp_highway_at(h, upos, 7);
    CHECK(r && !memcmp(&r->id, &noun, 16) && r->rank == 7, "the record of slot 7 is NOUN's content");
    CHECK(lp_highway_at(h, upos, 17) == NULL, "past the list's end");
    CHECK(lp_highway_slot(h, deprel, &noun) == -1, "NOUN is not a relation");
    CHECK(ili && ili->count > 100000, "CILI's concepts: %u", ili ? ili->count : 0);
    const lp_edge *e; size_t n = lp_highway_edges(h, "pbroleset", 0, "vnclass", &e);
    const lp_list *pb = lp_highway_list(h, "pbroleset"), *vn = lp_highway_list(h, "vnclass");
    CHECK(pb && vn, "PropBank's rolesets and VerbNet's classes are listed");
    size_t any = 0; for (uint32_t i = 0; pb && i < pb->count && !any; i++) any = lp_highway_edges(h, "pbroleset", i, "vnclass", &e);
    CHECK(any > 0, "some roleset maps to a class");
    if (any) { CHECK(e[0].to < vn->count, "an edge lands inside the list it names"); }
    (void)n;
    uint8_t fp[32]; lp_highway_fingerprint(h, fp); int zero = 1; for (int i = 0; i < 32; i++) zero &= fp[i] == 0;
    CHECK(!zero, "a fingerprint");
    DONE("highway");
}
