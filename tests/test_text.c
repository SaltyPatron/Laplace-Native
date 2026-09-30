/* Text: the documented decompositions, and the trunk's parts. */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>

static lp_id word(const char *w){ lp_id id; lp_id_codepoints_utf8(w, strlen(w), &id); return id; }

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "cannot map tier 0 at %s", LAPLACE_TIER0);
    if (!t0) DONE("text");
    lp_text *tx = lp_text_new(t0); CHECK(tx != NULL, "ICU break iterators");
    lp_ref parts[16]; size_t n;

    lp_ref r = lp_text_parts(tx, (const uint8_t *)"Holmes", 6, parts, 16, &n);             /* a word fills every tier above it */
    lp_id h = word("Holmes");
    CHECK(!memcmp(&r.id, &h, 16) && r.tier == 2 && n == 6, "[H,o,l,m,e,s] is a tier 2 word (tier %d, %zu parts)", r.tier, n);

    r = lp_text_parts(tx, (const uint8_t *)"Sherlock Holmes", 15, parts, 16, &n);          /* [[S,h,e,r,l,o,c,k], ' ', [H,o,l,m,e,s]] */
    lp_id k[3] = { word("Sherlock"), word(" "), word("Holmes") }, want; lp_id_compose(k, 3, &want);
    CHECK(!memcmp(&r.id, &want, 16) && r.tier == 3 && n == 3, "two words and a codepoint (tier %d, %zu parts)", r.tier, n);
    CHECK(n == 3 && !memcmp(&parts[0].id, &k[0], 16) && !memcmp(&parts[1].id, &k[1], 16) && !memcmp(&parts[2].id, &k[2], 16), "its parts, in order");
    CHECK(parts[1].tier == 0, "the space is a codepoint");
    lp_coord in[3] = { parts[0].c, parts[1].c, parts[2].c }, c; lp_coord_centroid(in, 3, &c);
    CHECK(!memcmp(&c, &r.c, sizeof c) && lp_coord_inside(&r.c), "its coordinate is the average of its constituents', inside the wall");

    /* a separator separates: the default rules' joiners are segments of their own */
    r = lp_text_parts(tx, (const uint8_t *)"microsoft.com", 13, parts, 16, &n);
    lp_id ms[3] = { word("microsoft"), word("."), word("com") };
    CHECK(n == 3 && !memcmp(&parts[0].id, &ms[0], 16) && !memcmp(&parts[1].id, &ms[1], 16) && !memcmp(&parts[2].id, &ms[2], 16) && r.tier == 3, "microsoft.com is [microsoft] [.] [com] (%zu parts, tier %d)", n, r.tier);
    r = lp_text_parts(tx, (const uint8_t *)"3.14 can't word_1", 17, parts, 16, &n);
    CHECK(n == 11, "3.14 can't word_1 is [3] [.] [14] [ ] [can] ['] [t] [ ] [word] [_] [1] (%zu parts)", n);
    /* repeats: a block repeated adjacently is one entity, a run of a single constituent is a run */
    r = lp_text_parts(tx, (const uint8_t *)"banana", 6, parts, 16, &n);
    lp_id an = word("an");
    CHECK(n == 4 && !memcmp(&parts[1].id, &an, 16) && !memcmp(&parts[2].id, &an, 16) && parts[1].tier == 1 && r.tier == 2, "banana is b [an]x2 a (%zu parts)", n);
    r = lp_text_parts(tx, (const uint8_t *)"mississippi", 11, parts, 16, &n);
    lp_id iss = word("iss");
    CHECK(n == 7 && !memcmp(&parts[1].id, &iss, 16) && !memcmp(&parts[2].id, &iss, 16), "mississippi is m [iss]x2 i p p i (%zu parts)", n);
    r = lp_text_parts(tx, (const uint8_t *)"aaaa", 4, parts, 16, &n);
    CHECK(n == 4 && !memcmp(&parts[0].id, &parts[3].id, 16) && parts[0].tier == 0, "aaaa stays a run of the codepoint");
    r = lp_text_parts(tx, (const uint8_t *)"abababab", 8, parts, 16, &n);
    lp_id ab = word("ab");
    CHECK(n == 4 && !memcmp(&parts[0].id, &ab, 16) && !memcmp(&parts[3].id, &ab, 16), "abababab is [ab]x4 (%zu parts)", n);

    r = lp_text_parts(tx, (const uint8_t *)"n", 1, parts, 16, &n);
    CHECK(r.tier == 0 && n == 1 && lp_tier0_codepoint(t0, &r.id) == 'n', "one codepoint is that codepoint");
    lp_id none; memset(&none, 0xAB, 16);
    CHECK(lp_tier0_codepoint(t0, &none) == -1, "an ID that is no codepoint");
    lp_text_free(tx);
    DONE("text");
}
