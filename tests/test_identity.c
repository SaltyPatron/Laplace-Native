/* Identity against the prototype: every codepoint's ID equals tier 0's, all 1,114,112 are distinct, and known
 * composition IDs match the prototype database. */
#include "laplace/tier0.h"
#include "check.h"
#include "blake3.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int cmp16(const void *a, const void *b){ return memcmp(a, b, 16); }

typedef struct { const lp_id *ids; const uint64_t *runs; size_t count, at; int end; } IdRuns;
static int next_id_run(void *context, lp_id *id, uint64_t *run){
    IdRuns *input = context;
    if (input->at == input->count) return input->end;
    *id = input->ids[input->at]; *run = input->runs[input->at++]; return 0;
}
static void ordered_runs(void){
    lp_id ids[3], actual, expected;
    for (unsigned i=0; i<3; i++) lp_id_codepoint('A'+i, ids+i);
    const uint64_t cases[][3]={{1,1,1},{61,1,1},{62,1,1},{63,1,1},{64,1,1},{126,1,1},{127,1,1},{65537,1,1}};
    for (size_t c=0; c<sizeof cases/sizeof *cases; c++) {
        /* Independent BLAKE3 oracle over the fully expanded bytes. Exercise
         * both sides of the single-chunk and multi-chunk boundaries. */
        size_t count=(size_t)(cases[c][0]+cases[c][1]+cases[c][2]);
        lp_id *expanded=malloc(count*sizeof *expanded); CHECK(expanded,"allocate run oracle");
        if (!expanded) continue;
        size_t at=0;
        for (size_t i=0; i<3; i++) for (uint64_t r=0; r<cases[c][i]; r++) expanded[at++]=ids[i];
        blake3_hasher h; blake3_hasher_init(&h);
        blake3_hasher_update(&h,expanded,count*sizeof *expanded); blake3_hasher_finalize(&h,expected.b,16);
        IdRuns input={ids,cases[c],3,0,1};
        CHECK(lp_id_compose_runs(count,next_id_run,&input,&actual) && lp_id_eq(&actual,&expected),
              "streamed identity equals BLAKE3 of %zu ordered expanded children",count);
        free(expanded);
    }
    uint64_t one=1;
    IdRuns single={ids,&one,1,0,1}, empty={NULL,NULL,0,0,1};
    CHECK(lp_id_compose_runs(1,next_id_run,&single,&actual) && lp_id_eq(&actual,ids),"stream singleton collapses to child");
    lp_id_compose(NULL,0,&expected);
    CHECK(lp_id_compose_runs(0,next_id_run,&empty,&actual) && lp_id_eq(&actual,&expected),"empty stream retains empty-sequence hash");
    uint64_t bad[][3]={{0,1,1},{1,1,1},{1,1,3},{UINT64_MAX,1,1}};
    for (size_t i=0; i<sizeof bad/sizeof *bad; i++) {
        actual=ids[0]; IdRuns input={ids,bad[i],3,0,1};
        CHECK(!lp_id_compose_runs(4,next_id_run,&input,&actual) && lp_id_eq(&actual,ids),"invalid run %zu leaves output intact",i);
    }
    uint64_t exact[]={1,1,1};
    IdRuns extra={ids,exact,3,0,1}, failed={ids,exact,3,0,-1};
    CHECK(!lp_id_compose_runs(2,next_id_run,&extra,&actual),"a late end is refused");
    CHECK(!lp_id_compose_runs(3,next_id_run,&failed,&actual),"reader failures propagate");
    CHECK(!lp_id_compose_runs(SIZE_MAX,next_id_run,&empty,&actual) && empty.at==0,"byte-count overflow refused before reading");
    CHECK(!lp_id_compose_runs(1,NULL,NULL,&actual) && !lp_id_compose_runs(1,next_id_run,&single,NULL),"missing reader/output refused");
    IdRuns alias={ids,&one,1,0,1}; expected=ids[0];
    CHECK(lp_id_compose_runs(1,next_id_run,&alias,ids) && lp_id_eq(ids,&expected),"stream output may alias an input child");
}

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
        lp_ref stack[64], *inner=period<=64?stack:malloc(period*sizeof *inner);
        size_t count=factor_definition(in+at,period,inner);
        lp_ref block=lp_ref_compose(inner,count,lp_tier_above(inner,count)); block.said=0;
        if (inner!=stack) free(inner);
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
    /* Past 64 constituents the search walks each constituent's next copy instead of every length: the same factors. */
    lp_ref wide[4]={alphabet[0],alphabet[1],alphabet[2],lp_ref_atom(t0,'D')};
    uint64_t state=0x9e3779b97f4a7c15u; size_t long_checked=0;
    for (unsigned radix=2; radix<=4; radix++) for (size_t length=65; length<=128; length++) for (unsigned round=0; round<8; round++) {
        lp_ref seq[128], want[128], got_v[128];
        size_t period=1+(size_t)(state>>59)%24;                      /* a periodic body, perturbed, so blocks of every size occur */
        for (size_t i=0; i<length; i++) {
            state=state*6364136223846793005u+1442695040888963407u;
            seq[i]=i<period || (state>>60)<2 ? wide[(state>>33)%radix] : seq[i-period];
        }
        size_t count=factor_definition(seq,length,want), got=lp_factor(seq,length,got_v,NULL,NULL);
        CHECK(count==got && !memcmp(want,got_v,count*sizeof *want),"long factor parity radix %u length %zu round %u",radix,length,round);
        long_checked++;
    }
    CHECK(long_checked==1536,"long sequences checked against the definition: %zu",long_checked);
    /* Longer still, where squares nest and overlap: random periodic bodies, perturbed, and Fibonacci words (the most
     * runs a sequence can hold), each in place as well. */
    size_t longer_checked=0;
    for (unsigned radix=2; radix<=4; radix++) for (unsigned round=0; round<12; round++) {
        state=state*6364136223846793005u+1442695040888963407u;
        size_t length=129+(size_t)(state>>40)%1900, period=1+(size_t)(state>>20)%97;
        lp_ref *seq=malloc(length*sizeof *seq), *want=malloc(length*sizeof *want), *got_v=malloc(length*sizeof *got_v);
        for (size_t i=0; i<length; i++) {
            state=state*6364136223846793005u+1442695040888963407u;
            seq[i]=i<period || (state>>60)<1 ? wide[(state>>33)%radix] : seq[i-period];
        }
        size_t count=factor_definition(seq,length,want), got=lp_factor(seq,length,got_v,NULL,NULL);
        CHECK(count==got && !memcmp(want,got_v,count*sizeof *want),"longer factor parity radix %u length %zu period %zu",radix,length,period);
        got=lp_factor(seq,length,seq,NULL,NULL);
        CHECK(count==got && !memcmp(want,seq,count*sizeof *want),"longer in-place factor parity radix %u length %zu",radix,length);
        free(seq); free(want); free(got_v); longer_checked++;
    }
    for (size_t length=65; length<=1597; length=length*3/2) {
        lp_ref *seq=malloc(length*sizeof *seq), *want=malloc(length*sizeof *want), *got_v=malloc(length*sizeof *got_v);
        for (size_t i=0; i<length; i++)                            /* the Fibonacci word: ABAABABAABAAB... */
            seq[i]=wide[(size_t)((double)(i+2)/1.6180339887498949)-(size_t)((double)(i+1)/1.6180339887498949)];
        size_t count=factor_definition(seq,length,want), got=lp_factor(seq,length,got_v,NULL,NULL);
        CHECK(count==got && !memcmp(want,got_v,count*sizeof *want),"Fibonacci factor parity length %zu",length);
        free(seq); free(want); free(got_v); longer_checked++;
    }
    CHECK(longer_checked==44,"longer sequences checked against the definition: %zu",longer_checked);
    /* A generated table: one long sequence whose constituents recur every few places (its brackets, names and
     * spaces) while each row differs (its index). Every later copy of a constituent was a candidate period, so
     * factoring took the square of its length; here a 1,500-row table agrees with the definition and a
     * 40,000-row one factors in about as many steps as it has constituents. */
    {
        lp_ref tok[16]; for (unsigned t=0; t<16; t++) tok[t]=lp_ref_atom(t0,0x41+t);
        size_t rows=40000, per=16, length=rows*per;
        lp_ref *table=malloc(length*sizeof *table), *out=malloc(length*sizeof *out), *want=malloc(length*sizeof *want);
        lp_ref *index=malloc(rows*sizeof *index);
        for (size_t r=0; r<rows; r++) {                            /* [r] = {.lex_state = 37},\n */
            char digits[16]; int nd=snprintf(digits,sizeof digits,"%zu",r);
            lp_ref d[16]; for (int k=0; k<nd; k++) d[k]=lp_ref_atom(t0,(uint32_t)digits[k]);
            index[r]=nd==1?d[0]:lp_ref_compose(d,(size_t)nd,lp_tier_above(d,(size_t)nd));
        }
        static const unsigned row[16]={0,99,1,2,3,2,4,5,6,2,3,2,7,8,9,10};
        for (size_t r=0; r<rows; r++) for (size_t k=0; k<per; k++) table[r*per+k]=row[k]==99?index[r]:tok[row[k]];
        size_t small=1500*per, count=factor_definition(table,small,want), got=lp_factor(table,small,out,NULL,NULL);
        CHECK(count==got && !memcmp(want,out,count*sizeof *want),"a generated table of %zu constituents agrees with the definition",small);
        clock_t started=clock();
        got=lp_factor(table,length,out,NULL,NULL);
        double seconds=(double)(clock()-started)/CLOCKS_PER_SEC;
        CHECK(got>0 && seconds<20.0,"a generated table of %zu constituents factors in %.2f s",length,seconds);
        free(table); free(out); free(want); free(index);
    }
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
    ordered_runs();
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "cannot map tier 0 at %s", LAPLACE_TIER0);
    if (!t0) DONE("identity");
    factors(t0);
    lp_id *all = malloc(sizeof(lp_id) * LP_TIER0_COUNT); size_t bad = 0, count = 0;
    CHECK(all != NULL, "cannot allocate codepoint identity check");
    if (!all) DONE("identity");
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        lp_id_codepoint(cp, &all[count]);
        const lp_tier0_record *record = lp_tier0_at(t0, cp);
        if (!record || memcmp(&all[count], &record->id, 16)) bad++;
        count++;
    }
    CHECK(bad == 0, "%zu codepoint IDs differ from tier 0", bad);
    CHECK(count == LP_TIER0_COUNT, "all codepoint identities were checked");
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
