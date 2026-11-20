/* Structural text identities: explicit expected Merkle trees, plus parity with the Engine's recording route. */
#include "laplace/tier0.h"
#include "check.h"

static lp_id sequence(const char *s){
    size_t n = strlen(s); lp_id parts[32], id;
    CHECK(n && n <= 32, "expected ASCII sequence fits");
    for (size_t i = 0; i < n && i < 32; i++) lp_id_codepoint((unsigned char)s[i], parts + i);
    lp_id_compose(parts, n, &id); return id;
}

typedef struct { lp_idmap *entities; size_t calls; } Recorded;
static lp_ref record(void *context, const lp_ref *children, uint32_t count, uint8_t tier){
    Recorded *r = context; lp_ref value = lp_ref_compose(children, count, tier);
    bool fresh; lp_ref *slot = lp_idmap_get(r->entities, &value.id, &fresh);
    if (fresh) *slot = value;
    CHECK(lp_id_eq(&slot->id, &value.id) && !memcmp(&slot->c, &value.c, sizeof value.c),
          "recording deduplicates the real native composition");
    r->calls++; return value;
}

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "map complete scalar cache"); if (!t0) DONE("ids");
    lp_text *text = lp_text_new(t0), *engine = lp_text_new(t0);
    CHECK(text && engine, "open UAX #29 contexts");
    if (!text || !engine) { lp_text_free(text); lp_text_free(engine); DONE("ids"); }
    lp_id abandon[3] = { sequence("abandon"), sequence("."), sequence("01") };
    lp_id new_york[3] = { sequence("New"), sequence(" "), sequence("York") };
    lp_id expected[3]; lp_id_compose(abandon, 3, expected); expected[1] = sequence("i46360");
    lp_id_compose(new_york, 3, expected + 2);
    const char *names[] = { "abandon.01", "i46360", "New York" };
    Recorded rows = { lp_idmap_sized(sizeof(lp_ref)), 0 };
    for (int i = 0; i < 3; i++) {
        lp_id actual; size_t n = strlen(names[i]);
        CHECK(lp_text_id(text, names[i], n, &actual) && lp_id_eq(&actual, expected + i),
              "%s has its explicit grapheme/word/phrase tree", names[i]);
        lp_ref recorded = lp_text_decompose(engine, (const uint8_t *)names[i], n, record, &rows);
        CHECK(lp_id_eq(&actual, &recorded.id), "%s matches the Engine's native recording route", names[i]);
        if (i != 1) {
            lp_id flat; CHECK(lp_id_codepoints_utf8(names[i], n, &flat) && !lp_id_eq(&actual, &flat),
                              "%s is not flattened into a codepoint-only composition", names[i]);
        }
    }
    CHECK(rows.calls && lp_idmap_count(rows.entities), "the recording route created actual compositions");
    lp_id marks[2], grapheme, actual;
    lp_id_codepoint('A', marks); lp_id_codepoint(0x30a, marks + 1); lp_id_compose(marks, 2, &grapheme);
    CHECK(lp_text_id(text, "A\xcc\x8a", 3, &actual) && lp_id_eq(&actual, &grapheme), "combining marks compose at the grapheme tier");
    lp_id composed; CHECK(lp_text_id(text, "\xc3\x85", 2, &composed) && !lp_id_eq(&composed, &actual),
                          "source text is not silently normalized");
    lp_id_codepoint(0x1f44d, marks); lp_id_codepoint(0x1f3fd, marks + 1); lp_id_compose(marks, 2, &grapheme);
    CHECK(lp_text_id(text, "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", 8, &actual) && lp_id_eq(&actual, &grapheme), "emoji modifiers remain in their grapheme");
    const char *invalid[] = { "\xc0\xaf", "\xf4\x90\x80\x80", "\xf0\x9f" };
    const size_t sizes[] = { 2, 4, 2 };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        memset(&actual, 0xa5, sizeof actual); lp_id before = actual;
        CHECK(!lp_text_id(text, invalid[i], sizes[i], &actual) && lp_id_eq(&actual, &before),
              "malformed/noncanonical UTF-8 %zu does not mint an entity", i);
    }
    CHECK(!lp_text_id(text, "", 0, &actual) && !lp_text_id(NULL, "a", 1, &actual), "reject missing content or context");
    {
        const int64_t ints[] = { 0, 5, 9, 10, 42, 255, 1000, 1111, 1212, 12341234, -1, -5, -42, -128, -1212, INT64_MAX, INT64_MIN };
        for (size_t i = 0; i < sizeof ints / sizeof *ints; i++) {
            char buf[32]; size_t bl = lp_decimal_i64(ints[i], buf, sizeof buf);
            lp_id dec_id, txt_id;
            CHECK(lp_id_i64(t0, ints[i], &dec_id) && lp_text_id(text, buf, bl, &txt_id) && lp_id_eq(&dec_id, &txt_id),
                  "scalar i64(%s) matches UAX #29 lp_text_id", buf);
        }
        const uint64_t uints[] = { 0u, 255u, 1212u, UINT64_MAX };
        for (size_t i = 0; i < sizeof uints / sizeof *uints; i++) {
            char buf[32]; size_t bl = lp_decimal_u64(uints[i], buf, sizeof buf);
            lp_id dec_id, txt_id;
            CHECK(lp_id_u64(t0, uints[i], &dec_id) && lp_text_id(text, buf, bl, &txt_id) && lp_id_eq(&dec_id, &txt_id),
                  "scalar u64(%s) matches UAX #29 lp_text_id", buf);
        }
    }
    lp_idmap_free(rows.entities); lp_text_free(text); lp_text_free(engine);
    DONE("ids");
}
