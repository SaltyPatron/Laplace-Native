/* JSON (laplace/json.h): what the reader takes and what it refuses, and the writer read back by the reader. */
#include "laplace/json.h"
#include "check.h"
#include <string.h>

static lp_json *parse(const char *s){ return lp_json_parse(s, strlen(s)); }

static void reads(void){
    const char *doc = " {\"name\":\"Holmes\",\"n\":-42,\"x\":2.5e1,\"ok\":true,\"no\":false,\"nil\":null,"
                      "\"list\":[1,[2],{\"k\":\"v\"}],\"name\":\"second\",\"empty\":{},\"none\":[]} ";
    lp_json *j = parse(doc);
    CHECK(j, "a valid document parses");
    if (!j) return;
    CHECK(lp_json_kind(j, LP_JSON_ROOT) == LP_JSON_OBJECT && lp_json_count(j, LP_JSON_ROOT) == 10, "the root object, members in order");
    size_t n; const char *s = lp_json_string(j, lp_json_get(j, LP_JSON_ROOT, "name"), &n);
    CHECK(s && n == 6 && !strcmp(s, "Holmes"), "a repeated key answers its first");
    CHECK(!strcmp(lp_json_key(j, LP_JSON_ROOT, 7), "name"), "the repeated key is kept");
    int64_t i; double d; bool b;
    CHECK(lp_json_integer(j, lp_json_get(j, LP_JSON_ROOT, "n"), &i) && i == -42, "an integer");
    CHECK(!lp_json_integer(j, lp_json_get(j, LP_JSON_ROOT, "x"), &i) && lp_json_number(j, lp_json_get(j, LP_JSON_ROOT, "x"), &d) && d == 25.0,
          "an exponent is a number, not an integer");
    CHECK(lp_json_bool(j, lp_json_get(j, LP_JSON_ROOT, "ok"), &b) && b, "true");
    CHECK(lp_json_bool(j, lp_json_get(j, LP_JSON_ROOT, "no"), &b) && !b, "false");
    CHECK(lp_json_kind(j, lp_json_get(j, LP_JSON_ROOT, "nil")) == LP_JSON_NULL, "null");
    uint32_t list = lp_json_get(j, LP_JSON_ROOT, "list");
    CHECK(lp_json_count(j, list) == 3 && lp_json_count(j, lp_json_item(j, list, 1)) == 1, "nested arrays");
    CHECK(!strcmp(lp_json_string(j, lp_json_get(j, lp_json_item(j, list, 2), "k"), NULL), "v"), "an object in an array");
    CHECK(lp_json_item(j, list, 3) == LP_JSON_NONE && lp_json_get(j, list, "k") == LP_JSON_NONE, "out of range, no object");
    CHECK(lp_json_get(j, LP_JSON_ROOT, "absent") == LP_JSON_NONE && !lp_json_string(j, LP_JSON_NONE, NULL), "absent");
    CHECK(lp_json_count(j, lp_json_get(j, LP_JSON_ROOT, "empty")) == 0 && lp_json_count(j, lp_json_get(j, LP_JSON_ROOT, "none")) == 0, "empty");
    lp_json_free(j);

    /* A member as it came: its own bytes, nothing around them. */
    j = parse("{ \"id\" : \"a\\\"b\" , \"params\":{\"x\":[1, 2]} }");
    s = j ? lp_json_text(j, lp_json_get(j, LP_JSON_ROOT, "params"), &n) : NULL;
    CHECK(s && n == 12 && !memcmp(s, "{\"x\":[1, 2]}", n), "an object's own text");
    s = j ? lp_json_text(j, lp_json_get(j, LP_JSON_ROOT, "id"), &n) : NULL;
    CHECK(s && n == 6 && !memcmp(s, "\"a\\\"b\"", n), "a string's own text, quotes and escapes");
    lp_json_free(j);

    /* A key inside a string or a nested object is no member of the root. */
    j = parse("{\"q\":\"\\\"text\\\":1\",\"inner\":{\"text\":2}}");
    CHECK(j && lp_json_get(j, LP_JSON_ROOT, "text") == LP_JSON_NONE, "only the root's own members");
    lp_json_free(j);

    j = parse("\"a\\\"b\\\\c\\/d\\n\\t\\u00e9\\ud83d\\ude00\\ud800x\"");
    s = j ? lp_json_string(j, LP_JSON_ROOT, &n) : NULL;
    CHECK(s && n == 19 && !memcmp(s, "a\"b\\c/d\n\t\xc3\xa9\xf0\x9f\x98\x80\xed\xa0\x80x", n),
          "escapes, a surrogate pair, a lone surrogate in its 3-byte form");
    lp_json_free(j);

    j = parse("9223372036854775807");
    CHECK(j && lp_json_integer(j, LP_JSON_ROOT, &i) && i == INT64_MAX, "int64's largest");
    lp_json_free(j);
    j = parse("9223372036854775808");
    CHECK(j && !lp_json_integer(j, LP_JSON_ROOT, &i) && lp_json_number(j, LP_JSON_ROOT, &d), "past int64: a number only");
    lp_json_free(j);
}

static void refuses(void){
    static const char *bad[] = { "", " ", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "{a:1}", "[1 2]", "01", "1.", "-", ".5",
                                 "1e", "+1", "\"abc", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "tru", "nul", "{} {}", "[1]x",
                                 "{\"a\":1}\x01" };
    for (size_t k = 0; k < sizeof bad / sizeof *bad; k++) {
        lp_json *j = parse(bad[k]);
        CHECK(!j, "refused: [%s]", bad[k]);
        lp_json_free(j);
    }
    /* Truncated: no read past the length given, even where the bytes after it would complete the text. */
    const char *whole = "{\"a\":[1,2,3]}";
    for (size_t len = 0; len < strlen(whole); len++) { lp_json *j = lp_json_parse(whole, len); CHECK(!j, "a prefix of %zu bytes", len); lp_json_free(j); }
    /* Nesting: LP_JSON_DEPTH levels are read, one more is refused. */
    char deep[2 * (LP_JSON_DEPTH + 1) + 1];
    for (int levels = LP_JSON_DEPTH; levels <= LP_JSON_DEPTH + 1; levels++) {
        memset(deep, '[', (size_t)levels); memset(deep + levels, ']', (size_t)levels);
        lp_json *j = lp_json_parse(deep, 2 * (size_t)levels);
        CHECK(levels == LP_JSON_DEPTH ? j != NULL : j == NULL, "%d levels", levels);
        lp_json_free(j);
    }
}

static void writes(void){
    const char raw[] = "say \"hi\"\\\n\r\t\b\f\x01\x1f end \xc3\xa9";
    lp_buf b = { 0 };
    lp_json_str(&b, raw, sizeof raw - 1);
    const char want[] = "\"say \\\"hi\\\"\\\\\\n\\r\\t\\b\\f\\u0001\\u001f end \xc3\xa9\"";
    CHECK(b.n == sizeof want - 1 && !memcmp(b.b, want, b.n), "the writer's escapes");
    lp_json *j = lp_json_parse((const char *)b.b, b.n); size_t n;
    const char *s = j ? lp_json_string(j, LP_JSON_ROOT, &n) : NULL;
    CHECK(s && n == sizeof raw - 1 && !memcmp(s, raw, n), "what the writer writes, the reader reads back");
    lp_json_free(j); lp_buf_free(&b);
}

int main(void){
    reads();
    refuses();
    writes();
    DONE("json");
}
