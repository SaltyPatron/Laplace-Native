/* The flags: what the standard says of a few codepoints, read back from the records by the standard's names. */
#include "laplace/laplace.h"
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
    CHECK(!strcmp(said(l, 'A', "General_Category"), "Uppercase_Letter"), "A is %s", said(l, 'A', "gc"));
    CHECK(!strcmp(said(l, 'a', "gc"), "Lowercase_Letter"), "a is %s", said(l, 'a', "gc"));
    CHECK(!strcmp(said(l, 'A', "script"), "Latin") && !strcmp(said(l, 0x3A9, "sc"), "Greek") && !strcmp(said(l, 0x72AC, "sc"), "Han"), "scripts: %s %s %s", said(l, 'A', "sc"), said(l, 0x3A9, "sc"), said(l, 0x72AC, "sc"));
    CHECK(!strcmp(said(l, 0x05D0, "Bidi_Class"), "Right_To_Left"), "alef is %s", said(l, 0x05D0, "bc"));
    CHECK(!strcmp(said(l, 'a', "Alphabetic"), "Yes") && !strcmp(said(l, '1', "Alphabetic"), "No"), "alphabetic");
    CHECK(!strcmp(said(l, ' ', "White_Space"), "Yes") && !strcmp(said(l, 0x1F600, "Emoji"), "Yes"), "white space and emoji");
    CHECK(!strcmp(said(l, 0x10FFFF, "gc"), "Unassigned") && !strcmp(said(l, 0xD800, "gc"), "Surrogate"), "the whole codespace");
    const lp_field *gc = lp_flags_field(l, "gc");
    CHECK(gc && lp_flags_value(l, gc, "Lu") == lp_flags_value(l, gc, "uppercase letter") && lp_flags_value(l, gc, "nothing") == -1, "values by name");
    DONE("flags");
}
