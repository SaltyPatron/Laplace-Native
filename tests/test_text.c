/* Text: the documented decompositions, and the trunk's parts. */
#include "laplace/laplace.h"
#include "laplace/consensus.h"
#include "check.h"
#include <string.h>

static lp_id word(const char *w){ lp_id id; lp_id_codepoints_utf8(w, strlen(w), &id); return id; }
static lp_ref height_sink(void *context,const lp_ref *children,uint32_t count,uint8_t tier){
    if (context) ++*(size_t *)context;
    CHECK(count>=2 && tier==lp_tier_above(children,count),"every emitted text composition has its actual recursive height");
    return lp_ref_compose(children,count,tier);
}
typedef struct { const char *bytes; size_t offset, count; lp_text *reader; } Segments;
static void check_segment(void *context, size_t offset, size_t length, const lp_ref *value){
    Segments *s = context; lp_id independent;
    CHECK(offset == s->offset && length > 0, "segments cover original bytes in order");
    CHECK(lp_text_id(s->reader,s->bytes+offset,length,&independent)
          && lp_id_eq(&independent,&value->id), "segment has its standalone Native content ID");
    s->offset += length; s->count++;
}

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "cannot map tier 0 at %s", LAPLACE_TIER0);
    if (!t0) DONE("text");
    lp_text *tx = lp_text_new(t0); CHECK(tx != NULL, "ICU break iterators");
    const char *segment_text = "a banana cafe\xcc\x81 microsoft.com";
    Segments segments = {segment_text,0,0,lp_text_new(t0)};
    lp_ref segmented = lp_text_segments(tx,(const uint8_t *)segment_text,strlen(segment_text),check_segment,&segments);
    lp_id ordinary;
    CHECK(segments.count == 9 && segments.offset == strlen(segment_text), "actual segments include atoms, words and separators");
    CHECK(lp_text_id(tx,segment_text,strlen(segment_text),&ordinary) && lp_id_eq(&ordinary,&segmented.id), "visiting segments preserves the root and clears callback state");
    segments.offset = segments.count = 0;
    size_t composed = 0;
    lp_ref recorded = lp_text_decompose_segments(tx,(const uint8_t *)segment_text,strlen(segment_text),
        height_sink,&composed,check_segment,&segments);
    CHECK(composed > 0 && segments.count == 9 && segments.offset == strlen(segment_text)
          && lp_id_eq(&recorded.id,&segmented.id), "recording and segment visits share one canonical decomposition");
    CHECK(lp_text_id(tx,"other",5,&ordinary) && segments.count == 9,
          "recording segment callbacks do not leak into the next text");
    lp_text_free(segments.reader);
    lp_ref parts[16]; size_t n;

    lp_ref r = lp_text_parts(tx, (const uint8_t *)"Holmes", 6, parts, 16, &n);             /* a word fills every tier above it */
    lp_id h = word("Holmes");
    CHECK(!memcmp(&r.id, &h, 16) && r.tier == 1 && n == 6, "[H,o,l,m,e,s] stands directly above scalar leaves (tier %d, %zu parts)", r.tier, n);

    r = lp_text_parts(tx, (const uint8_t *)"Sherlock Holmes", 15, parts, 16, &n);          /* [[S,h,e,r,l,o,c,k], ' ', [H,o,l,m,e,s]] */
    lp_id k[3] = { word("Sherlock"), word(" "), word("Holmes") }, want; lp_id_compose(k, 3, &want);
    CHECK(!memcmp(&r.id, &want, 16) && r.tier == 2 && n == 3, "two words and a codepoint (tier %d, %zu parts)", r.tier, n);
    CHECK(n == 3 && !memcmp(&parts[0].id, &k[0], 16) && !memcmp(&parts[1].id, &k[1], 16) && !memcmp(&parts[2].id, &k[2], 16), "its parts, in order");
    CHECK(parts[1].tier == 0, "the space is a codepoint");
    lp_coord in[3] = { parts[0].c, parts[1].c, parts[2].c }, c; lp_coord_centroid(in, 3, &c);
    CHECK(!memcmp(&c, &r.c, sizeof c) && lp_coord_inside(&r.c), "its coordinate is the average of its constituents', inside the wall");

    /* a separator separates: the default rules' joiners are segments of their own */
    r = lp_text_parts(tx, (const uint8_t *)"microsoft.com", 13, parts, 16, &n);
    lp_id ms[3] = { word("microsoft"), word("."), word("com") };
    CHECK(n == 3 && !memcmp(&parts[0].id, &ms[0], 16) && !memcmp(&parts[1].id, &ms[1], 16) && !memcmp(&parts[2].id, &ms[2], 16) && r.tier == 2, "microsoft.com is [microsoft] [.] [com] (%zu parts, tier %d)", n, r.tier);
    r = lp_text_parts(tx, (const uint8_t *)"3.14 can't word_1", 17, parts, 16, &n);
    CHECK(n == 11, "3.14 can't word_1 is [3] [.] [14] [ ] [can] ['] [t] [ ] [word] [_] [1] (%zu parts)", n);
    /* Repetition changes occurrence counts, not the admitted UAX boundaries. */
    const char *repeated_words[] = {"banana", "mississippi", "aaaa", "abababab"};
    for (size_t w = 0; w < sizeof repeated_words / sizeof *repeated_words; w++) {
        const char *text = repeated_words[w]; size_t length = strlen(text);
        r = lp_text_parts(tx, (const uint8_t *)text, length, parts, 16, &n);
        lp_id direct = word(text);
        CHECK(n == length && r.tier == 1 && lp_id_eq(&r.id, &direct),
              "%s is one word composed directly from its %zu codepoints", text, length);
        for (size_t i = 0; i < n; i++)
            CHECK(parts[i].tier == 0 && lp_tier0_codepoint(t0, &parts[i].id) == text[i],
                  "%s retains scalar occurrence %zu", text, i);
    }

    r = lp_text_parts(tx, (const uint8_t *)"n", 1, parts, 16, &n);
    CHECK(r.tier == 0 && n == 1 && lp_tier0_codepoint(t0, &r.id) == 'n', "one codepoint is that codepoint");
    lp_id none; memset(&none, 0xAB, 16);
    CHECK(lp_tier0_codepoint(t0, &none) == -1, "an ID that is no codepoint");
    const char *height_cases[]={"Holmes","Sherlock Holmes","banana","mississippi","aaaa","abababab","e\xcc\x81","First sentence. Another sentence.\n\nA paragraph."};
    for (size_t i=0; i<sizeof height_cases/sizeof *height_cases; i++)
        lp_text_decompose(tx,(const uint8_t *)height_cases[i],strlen(height_cases[i]),height_sink,NULL);
    for (size_t i=0; i<lp_trust_class_count(); i++) {
        const lp_trust_class *c=lp_trust_class_at(i); lp_id id;
        CHECK(lp_text_id(tx,c->label,strlen(c->label),&id) && lp_trust_class_for_id(tx,&id)==c,
              "persisted class %s resolves from its canonical text ID",c->label);
    }
    lp_id unknown_class; lp_id_codepoint('!',&unknown_class);
    CHECK(!lp_trust_class_for_id(tx,&unknown_class),"an undeclared dependence class fails closed");
    lp_text_free(tx);
    DONE("text");
}
