/* The flags: what the standard says of a few codepoints, read back from the records by the standard's names. */
#include "laplace/laplace.h"
#include "laplace/tier0.h"
#include "check.h"
#include <string.h>

static const char *said(const lp_layout *l, uint32_t cp, const char *property){
    const lp_field *f = lp_flags_field(l, property); if (!f) return "(no such property)";
    uint32_t v = lp_flags_get(l, cp, f); return f->nvalues ? l->value[f->first + v].say : v ? "Yes" : "No";
}
int main(void){
    const lp_layout *l = lp_flags_map(NULL);
    CHECK(l != NULL, "cannot map the flags at %s (generate them with: laplace flags)", lp_flags_path());
    if (!l) DONE("flags");
    CHECK(lp_flags_active_branches(l) == LP_TIER0_BRANCH_ALL, "default flags map activates all branches");
    CHECK(!strcmp(said(l, 'A', "General_Category"), "Uppercase_Letter"), "A is %s", said(l, 'A', "gc"));
    CHECK(!strcmp(said(l, 'a', "gc"), "Lowercase_Letter"), "a is %s", said(l, 'a', "gc"));
    CHECK(!strcmp(said(l, 'A', "script"), "Latin") && !strcmp(said(l, 0x3A9, "sc"), "Greek") && !strcmp(said(l, 0x72AC, "sc"), "Han"), "scripts: %s %s %s", said(l, 'A', "sc"), said(l, 0x3A9, "sc"), said(l, 0x72AC, "sc"));
    CHECK(!strcmp(said(l, 0x05D0, "Bidi_Class"), "Right_To_Left"), "alef is %s", said(l, 0x05D0, "bc"));
    CHECK(!strcmp(said(l, 'a', "Alphabetic"), "Yes") && !strcmp(said(l, '1', "Alphabetic"), "No"), "alphabetic");
    CHECK(!strcmp(said(l, ' ', "White_Space"), "Yes") && !strcmp(said(l, 0x1F600, "Emoji"), "Yes"), "white space and emoji");
    CHECK(!strcmp(said(l, 0x10FFFF, "gc"), "Unassigned") && !strcmp(said(l, 0xD800, "gc"), "Surrogate"), "the whole codespace");
    const lp_field *gc = lp_flags_field(l, "gc");
    CHECK(gc && lp_flags_value(l, gc, "Lu") == lp_flags_value(l, gc, "uppercase letter") && lp_flags_value(l, gc, "nothing") == -1, "values by name");

    CHECK(lp_flags_split_file(lp_flags_path()), "split flags into modular shards");
    const lp_layout *ascii_l = lp_flags_map_branches(lp_flags_path(), LP_TIER0_BRANCH_ASCII);
    CHECK(ascii_l && lp_flags_active_branches(ascii_l) == LP_TIER0_BRANCH_ASCII, "map ascii flags branch");
    if (ascii_l) {
        CHECK(!strcmp(said(ascii_l, 'A', "General_Category"), "Uppercase_Letter"), "ascii branch preserves A");
        CHECK(lp_flags_get(ascii_l, 0x72AC, gc) == 0, "ascii branch excludes CJK codepoint");
    }
    const lp_layout *cjk_l = lp_flags_map_branches(lp_flags_path(), LP_TIER0_BRANCH_BMP);
    CHECK(cjk_l && lp_flags_active_branches(cjk_l) == LP_TIER0_BRANCH_BMP, "map bmp (ascii+utf+cjk) flags branch");
    if (cjk_l) {
        CHECK(!strcmp(said(cjk_l, 0x72AC, "sc"), "Han") && !strcmp(said(cjk_l, 0x3A9, "sc"), "Greek"), "bmp flags branch preserves Han and Greek");
        CHECK(lp_flags_get(cjk_l, 0x1F600, lp_flags_field(cjk_l, "Emoji")) == 0, "bmp flags branch excludes supplementary emoji");
    }
    DONE("flags");
}
