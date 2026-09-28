/* Identity against the prototype: every codepoint's ID equals tier 0's, all 1,114,112 are distinct, and known
 * composition IDs match the prototype database. */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>

static int cmp16(const void *a, const void *b){ return memcmp(a, b, 16); }

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "cannot map tier 0 at %s", LAPLACE_TIER0);
    if (!t0) DONE("identity");
    lp_id *all = malloc(sizeof(lp_id) * LP_NCP); size_t bad = 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        lp_id_codepoint(cp, &all[cp]);
        if (memcmp(&all[cp], &t0[cp].id, 16)) bad++;
    }
    CHECK(bad == 0, "%zu codepoint IDs differ from tier 0", bad);
    qsort(all, LP_NCP, 16, cmp16); size_t dup = 0;
    for (size_t i = 1; i < LP_NCP; i++) dup += !memcmp(&all[i], &all[i - 1], 16);
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
    CHECK(!lp_id_codepoints_utf8("\xff", 1, &id), "invalid UTF-8 is rejected");
    free(all);
    DONE("identity");
}
