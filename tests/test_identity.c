/* Identity against the prototype: every scalar's ID equals tier 0's, all 1,112,064 are distinct, and known
 * composition IDs match the prototype database. */
#include "laplace/tier0.h"
#include "check.h"
#include <string.h>

static int cmp16(const void *a, const void *b){ return memcmp(a, b, 16); }

/* Bounded exhaustive oracle for the established shortest-block definition.
 * This deliberately does not use the production run-boundary shortcut. */
static size_t factor_definition(const lp_ref *in, size_t n, lp_ref *out){
    size_t used=0;
    for (size_t at=0; at<n;) {
        size_t period=0, copies=0;
        for (size_t length=2; length<=(n-at)/2; length++) {
            size_t different=1; while (different<length && lp_id_eq(&in[at].id,&in[at+different].id)) different++;
            if (different==length) continue;
            size_t j=0; while (j<length && lp_id_eq(&in[at+j].id,&in[at+length+j].id)) j++;
            if (j<length) continue;
            period=length; copies=2;
            while (length<=(n-at)/ (copies+1)) {
                for (j=0; j<length && lp_id_eq(&in[at+j].id,&in[at+copies*length+j].id); j++);
                if (j<length) break; copies++;
            }
            break;
        }
        if (!period) { out[used++]=in[at++]; continue; }
        lp_ref inner[64]; size_t count=factor_definition(in+at,period,inner);
        lp_ref block=lp_ref_compose(inner,count,lp_tier_above(inner,count)); block.said=0;
        for (size_t j=0; j<copies; j++) out[used++]=block;
        at+=period*copies;
    }
    return used;
}

static void factors(const lp_tier0_record *t0){
    lp_ref alphabet[3]={lp_ref_atom(t0,'A'),lp_ref_atom(t0,'B'),lp_ref_atom(t0,'C')};
    lp_ref input[64], expected[64], actual[64]; size_t checked=0;
    for (unsigned radix=2; radix<=3; radix++) {
        size_t patterns=1, limit=radix==2?12:8;
        for (size_t length=0; length<=limit; length++,patterns*=radix) {
            for (size_t pattern=0; pattern<patterns; pattern++) {
                size_t digits=pattern;
                for (size_t i=0; i<length; i++) { input[i]=alphabet[digits%radix]; digits/=radix; input[i].position=(uint32_t)i; }
                size_t count=factor_definition(input,length,expected), got=lp_factor(input,length,actual,NULL,NULL);
                CHECK(count==got && !memcmp(expected,actual,count*sizeof *actual),"factor parity radix %u length %zu pattern %zu",radix,length,pattern);
                memcpy(actual,input,length*sizeof *actual); got=lp_factor(actual,length,actual,NULL,NULL);
                CHECK(count==got && !memcmp(expected,actual,count*sizeof *actual),"in-place factor parity length %zu pattern %zu",length,pattern);
                checked++;
            }
        }
    }
    CHECK(checked==18032,"all binary/ternary factor definitions checked: %zu",checked);
    const size_t length=160680;
    lp_ref *run=malloc((length+3)*sizeof *run), *out=malloc((length+3)*sizeof *out);
    CHECK(run && out,"allocate the real-size run regression");
    if (run && out) {
        for (size_t i=0; i<length; i++) run[i]=alphabet[0];
        size_t got=lp_factor(run,length,out,NULL,NULL);
        CHECK(got==length && !memcmp(run,out,length*sizeof *run),"a long homogeneous run stays exactly the original run");
        run[length]=alphabet[1]; run[length+1]=alphabet[0]; run[length+2]=alphabet[1];
        got=lp_factor(run,length+3,out,NULL,NULL);
        lp_ref pair=lp_ref_compose(alphabet,2,lp_tier_above(alphabet,2));
        CHECK(got==length+1 && !memcmp(run,out,(length-1)*sizeof *run) &&
              lp_id_eq(&out[length-1].id,&pair.id) && lp_id_eq(&out[length].id,&pair.id),
              "a long initial run retains a later ABAB factor");
    }
    free(run); free(out);
}

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "cannot map tier 0 at %s", LAPLACE_TIER0);
    if (!t0) DONE("identity");
    factors(t0);
    lp_id *all = malloc(sizeof(lp_id) * LP_TIER0_COUNT); size_t bad = 0, count = 0;
    CHECK(all != NULL, "cannot allocate scalar identity check");
    if (!all) DONE("identity");
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        if (!lp_unicode_scalar(cp)) continue;
        lp_id_codepoint(cp, &all[count]);
        const lp_tier0_record *record = lp_tier0_at(t0, cp);
        if (!record || memcmp(&all[count], &record->id, 16)) bad++;
        count++;
    }
    CHECK(bad == 0, "%zu codepoint IDs differ from tier 0", bad);
    CHECK(count == LP_TIER0_COUNT, "all scalar identities were checked");
    qsort(all, count, 16, cmp16); size_t dup = 0;
    for (size_t i = 1; i < count; i++) dup += !memcmp(&all[i], &all[i - 1], 16);
    CHECK(dup == 0, "%zu duplicate codepoint IDs", dup);

    /* Golden values from the prototype database (laplace_cp_id, laplace_seq_id). */
    char hex[33]; lp_id id;
    lp_id_codepoint('A', &id); lp_hex(id.b, 16, hex);
    CHECK(!strcmp(hex, "32684bfa28c0c84d6f210511aace0efc"), "ID of 'A' is %s", hex);
    CHECK(lp_id_codepoints_utf8("Holmes", 6, &id), "Holmes is valid UTF-8"); lp_hex(id.b, 16, hex);
    CHECK(!strcmp(hex, "5b7e40e9a23ae55eaccd45646934977d"), "ID of 'Holmes' is %s", hex);

    /* One-child collapse, and composing codepoint IDs gives the same as the UTF-8 shortcut. */
    lp_id kids[6]; const char *w = "Holmes";
    for (int i = 0; i < 6; i++) lp_id_codepoint((unsigned char)w[i], &kids[i]);
    lp_id c1, c2; lp_id_compose(kids, 6, &c1); lp_id_compose(kids, 1, &c2);
    CHECK(!memcmp(&c1, &id, 16), "composition of Holmes's codepoints differs from the UTF-8 path");
    CHECK(!memcmp(&c2, &kids[0], 16), "one-child collapse");
    /* Using a composition at another domain altitude must still address the same entity. */
    lp_ref children[6];
    for (int i = 0; i < 6; i++) children[i] = lp_ref_atom(t0, (unsigned char)w[i]);
    lp_ref word = lp_ref_compose(children, 6, 2), higher = lp_ref_compose(children, 6, 12);
    CHECK(lp_id_eq(&word.id, &higher.id) && lp_id_eq(&word.id, &c1), "tier never salts a content identity");
    CHECK(!memcmp(&word.c, &higher.c, sizeof word.c), "tier never changes a content centroid");
    lp_idmap *entities = lp_idmap_sized(sizeof(lp_ref)); bool fresh;
    *(lp_ref *)lp_idmap_get(entities, &word.id, &fresh) = word;
    CHECK(fresh, "first observation records the entity");
    lp_ref *same = lp_idmap_get(entities, &higher.id, &fresh);
    CHECK(!fresh && lp_idmap_count(entities) == 1 && same->tier == word.tier,
          "another tier finds the existing entity by ID alone");
    lp_idmap_free(entities);
    CHECK(!lp_id_codepoints_utf8("\xff", 1, &id), "invalid UTF-8 is rejected");
    free(all);
    DONE("identity");
}
