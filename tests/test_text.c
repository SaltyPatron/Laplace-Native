/* Text: the documented decompositions, and the trunk's parts. */
#include "laplace/laplace.h"
#include "laplace/consensus.h"
#include "check.h"
#include <string.h>

static lp_id word(const char *w){ lp_id id; lp_id_codepoints_utf8(w, strlen(w), &id); return id; }
static lp_ref height_sink(void *context,const lp_ref *children,uint32_t count,uint8_t tier){
    if (context) ++*(size_t *)context;
    CHECK(count>=2 && tier==lp_tier_above(children,count),"every emitted text composition has its exact recursive Merkle DAG height");
    return lp_ref_compose(children,count,tier);
}
static lp_ref atom(const lp_tier0_record *t0, char c){ return lp_ref_atom(t0, (uint32_t)(unsigned char)c); }
static lp_ref block(const lp_ref *children, size_t n){ return lp_ref_compose(children, n, lp_tier_above(children, n)); }
typedef struct { lp_id want; size_t hits; } Seen;
static lp_ref seen_sink(void *context, const lp_ref *children, uint32_t count, uint8_t tier){
    Seen *s = context; lp_ref r = lp_ref_compose(children, count, tier);
    if (lp_id_eq(&r.id, &s->want)) s->hits++;
    return r;
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
    /* Repeats, factored from the content alone at every tier (Storage: Compositions): one constituent repeated is a
     * run; a block of two or more repeated adjacently is its own composition, standing once per repeat; leftmost,
     * the shortest period, recursively. */
    {
        lp_ref a=atom(t0,'a'), b=atom(t0,'b'), c=atom(t0,'c'), h=atom(t0,'h'), i=atom(t0,'i'), m=atom(t0,'m'),
               nn=atom(t0,'n'), p=atom(t0,'p'), s=atom(t0,'s'), x=atom(t0,'x'), space=atom(t0,' ');
        lp_ref an_[]={a,nn}, ab_[]={a,b}, abc_[]={a,b,c}, iss_[]={i,s,s}, ha_[]={h,a};
        lp_ref an=block(an_,2), ab=block(ab_,2), abc=block(abc_,3), iss=block(iss_,3), ha=block(ha_,2);
        lp_ref x_[]={c,ab,ab}; lp_ref cabab=block(x_,3);
        lp_ref hs_[]={ha,space}; lp_ref hs=block(hs_,2);
        lp_ref aaaa[]={a,a,a,a}, banana[]={b,an,an,a}, abab[]={ab,ab}, abababab[]={ab,ab,ab,ab}, abcabcx[]={abc,abc,x},
               mississippi[]={m,iss,iss,i,p,p,i}, nested[]={cabab,cabab}, haha[]={hs,hs,ha},
               holmes[]={atom(t0,'H'),atom(t0,'o'),atom(t0,'l'),atom(t0,'m'),atom(t0,'e'),atom(t0,'s')},
               abcab[]={a,b,c,a,b}, abcd[]={a,b,c,atom(t0,'d')};
        struct { const char *text; const lp_ref *parts; size_t n; uint8_t tier; } vectors[]={
            {"aaaa",aaaa,4,1},                 /* a run, not a block: [a,a,a,a], one vertex of run 4 */
            {"banana",banana,4,2},             /* b [an]x2 a */
            {"abab",abab,2,2},                 /* [ab]x2 */
            {"abababab",abababab,4,2},         /* the shortest period: [ab]x4, not [abab]x2 */
            {"abcabcx",abcabcx,3,2},           /* [abc]x2 x */
            {"mississippi",mississippi,7,2},   /* m [iss]x2 i p p i: leftmost */
            {"cababcabab",nested,2,3},         /* [c [ab]x2]x2: the block factored the same way */
            {"ha ha ha",haha,3,3},             /* [[ha] ' ']x2 [ha]: segments factor like codepoints */
            {"Holmes",holmes,6,1},             /* no period: unchanged */
            {"abcab",abcab,5,1},               /* a copy too short to repeat whole: unchanged */
            {"abcd",abcd,4,1},
        };
        for (size_t v=0; v<sizeof vectors/sizeof *vectors; v++) {
            const char *text=vectors[v].text; size_t length=strlen(text);
            lp_ref expect=block(vectors[v].parts,vectors[v].n);
            r = lp_text_parts(tx, (const uint8_t *)text, length, parts, 16, &n);
            CHECK(n==vectors[v].n && r.tier==vectors[v].tier && lp_id_eq(&r.id,&expect.id) && !memcmp(&r.c,&expect.c,sizeof r.c),
                  "%s factors to its %zu parts at tier %u (got %zu parts, tier %u)", text, vectors[v].n, vectors[v].tier, n, r.tier);
            for (size_t k=0; k<n && k<vectors[v].n; k++)
                CHECK(lp_id_eq(&parts[k].id,&vectors[v].parts[k].id) && parts[k].tier==vectors[v].parts[k].tier,"%s part %zu", text, k);
            lp_id flat=word(text);
            CHECK(lp_id_eq(&flat,&r.id)==(vectors[v].n==length),"%s differs from its flat codepoint ID exactly when it factors", text);
        }
        Seen seen={an.id,0}; lp_ref whole=block(banana,4);
        r = lp_text_decompose(tx,(const uint8_t *)"banana",6,seen_sink,&seen);
        CHECK(seen.hits==1 && lp_id_eq(&r.id,&whole.id),"the block [a,n] is composed through the recording sink once");
    }

    r = lp_text_parts(tx, (const uint8_t *)"n", 1, parts, 16, &n);
    CHECK(r.tier == 0 && n == 1 && lp_tier0_codepoint(t0, &r.id) == 'n', "one codepoint is that codepoint");
    lp_text *surrogate_reader = lp_text_new(t0);
    CHECK(surrogate_reader != NULL, "independent generalized UTF-8 reader");
    for (uint32_t cp = 0xd800; cp <= 0xdfff; cp++) {
        uint8_t bytes[4]; size_t length = lp_utf8_put(cp, bytes);
        r = lp_text_parts(tx, bytes, length, parts, 16, &n);
        lp_ref atom = lp_ref_atom(t0, cp);
        CHECK(r.tier == 0 && n == 1 && lp_id_eq(&r.id, &atom.id)
              && !memcmp(&r.c, &atom.c, sizeof r.c) && lp_id_eq(&parts[0].id, &atom.id),
              "U+%04X remains its one canonical codepoint without replacement atoms", cp);
        Segments original = {(const char *)bytes, 0, 0, surrogate_reader};
        segmented = lp_text_segments(tx, bytes, length, check_segment, &original);
        CHECK(original.count == 1 && original.offset == length && lp_id_eq(&segmented.id, &atom.id),
              "U+%04X segmentation preserves its complete original bytes", cp);
    }
    lp_text_free(surrogate_reader);
    const uint8_t halves[] = {0xed,0xa0,0x80,0xed,0xb0,0x80};
    r = lp_text_parts(tx, halves, sizeof halves, parts, 16, &n);
    lp_id half_ids[2], joined; lp_id_codepoint(0xd800, &half_ids[0]); lp_id_codepoint(0xdc00, &half_ids[1]);
    lp_id_compose(half_ids, 2, &joined);
    CHECK(n == 2 && lp_id_eq(&r.id, &joined) && lp_id_eq(&parts[0].id, &half_ids[0])
          && lp_id_eq(&parts[1].id, &half_ids[1]), "adjacent surrogate codepoints retain their identities and order");
    r = lp_text_parts(tx,(const uint8_t *)"e\xcc\x81",3,parts,16,&n);
    CHECK(r.tier==1 && n==2,"one multi-codepoint grapheme retains tier 1 when higher singleton levels reuse it");
    r = lp_text_parts(tx,(const uint8_t *)"e\xcc\x81x",4,parts,16,&n);
    CHECK(r.tier==2 && n==2 && parts[0].tier==1 && parts[1].tier==0,"a word with a multi-codepoint grapheme stands at DAG height 2");
    r = lp_text_parts(tx,(const uint8_t *)"Hi!",3,parts,16,&n);
    CHECK(r.tier==2 && n==2 && parts[0].tier==1,"punctuation creates a sentence composition at DAG height 1 + max(1, 0) = 2");
    lp_id none; memset(&none, 0xAB, 16);
    CHECK(lp_tier0_codepoint(t0, &none) == -1, "an ID that is no codepoint");
    const char *height_cases[]={"Holmes","Sherlock Holmes","banana","mississippi","aaaa","abababab","cababcabab","ha ha ha","e\xcc\x81","First sentence. Another sentence.\n\nA paragraph."};
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
