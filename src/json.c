/* JSON (laplace/json.h): a strict reader into a flat node array, and the one string writer. Every read is bounded by
 * the text's length; nothing reads past it, and anything RFC 8259 does not allow is refused. */
#include "laplace/json.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t type, integral;
    uint32_t n, first;             /* containers: members or elements, and where their node numbers start in kids */
    size_t str, len;               /* strings: offset in the string arena and length */
    double num; int64_t i;
    size_t at, span;               /* the value's own bytes in the parsed text */
} Node;

struct lp_json { lp_vec(Node) nodes; lp_vec(uint32_t) kids; lp_buf strs; const char *text; };

typedef struct { const char *p, *end; lp_json *j; int depth; } Parser;

static void ws(Parser *ps){ while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\n' || *ps->p == '\r' || *ps->p == '\t')) ps->p++; }
static uint32_t node(Parser *ps, lp_json_type t){ Node *n = lp_vec_add(&ps->j->nodes); n->type = (uint8_t)t; return (uint32_t)(ps->j->nodes.n - 1); }

static int hex4(Parser *ps, uint32_t *out){
    if (ps->end - ps->p < 4) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint32_t)d;
    }
    ps->p += 4; *out = v; return 1;
}

/* A string at ps->p (its opening quote), decoded onto the arena. */
static int string(Parser *ps, size_t *off, size_t *len){
    lp_buf *s = &ps->j->strs; *off = s->n; ps->p++;
    for (;;) {
        if (ps->p >= ps->end) return 0;
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') break;
        if (c < 0x20) return 0;
        if (c != '\\') { *lp_buf_room(s, 1) = c; continue; }
        if (ps->p >= ps->end) return 0;
        char e = *ps->p++; uint32_t cp;
        switch (e) {
        case '"': case '\\': case '/': *lp_buf_room(s, 1) = (uint8_t)e; continue;
        case 'n': *lp_buf_room(s, 1) = '\n'; continue;
        case 'r': *lp_buf_room(s, 1) = '\r'; continue;
        case 't': *lp_buf_room(s, 1) = '\t'; continue;
        case 'b': *lp_buf_room(s, 1) = '\b'; continue;
        case 'f': *lp_buf_room(s, 1) = '\f'; continue;
        case 'u':
            if (!hex4(ps, &cp)) return 0;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                const char *back = ps->p; uint32_t lo; ps->p += 2;
                if (hex4(ps, &lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                else ps->p = back;                                     /* a lone high surrogate, then its own escape */
            }
            { uint8_t u[4]; size_t k = lp_utf8_put(cp, u); lp_buf_put(s, u, k); }
            continue;
        default: return 0;
        }
    }
    *len = s->n - *off; *lp_buf_room(s, 1) = 0;
    return 1;
}

static int number(Parser *ps, uint32_t *out){
    const char *b = ps->p; int integral = 1;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p >= ps->end) return 0;
    if (*ps->p == '0') ps->p++;
    else if (*ps->p >= '1' && *ps->p <= '9') while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    else return 0;
    if (ps->p < ps->end && *ps->p == '.') {
        integral = 0; ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return 0;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        integral = 0; ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return 0;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    size_t n = (size_t)(ps->p - b); char tmp[64]; char *text = n < sizeof tmp ? tmp : lp_alloc(n + 1);
    memcpy(text, b, n); text[n] = 0;
    uint32_t k = node(ps, LP_JSON_NUMBER); Node *v = &ps->j->nodes.v[k];
    v->num = strtod(text, NULL);
    if (integral) {
        char *end; errno = 0;
        long long i = strtoll(text, &end, 10);
        if (*end == 0 && errno != ERANGE) { v->integral = 1; v->i = i; }
    }
    if (text != tmp) lp_free(text);
    *out = k; return 1;
}

static int value(Parser *ps, uint32_t *out);

static int container(Parser *ps, int object, uint32_t *out){
    if (++ps->depth > LP_JSON_DEPTH) return 0;
    uint32_t me = node(ps, object ? LP_JSON_OBJECT : LP_JSON_ARRAY);
    lp_vec(uint32_t) mine = { 0 }; uint32_t members = 0; int ok = 0;
    ps->p++; ws(ps);
    if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) { ps->p++; ok = 1; goto done; }
    for (;;) {
        ws(ps);
        if (object) {
            size_t off, len;
            if (ps->p >= ps->end || *ps->p != '"' || !string(ps, &off, &len)) goto done;
            uint32_t k = node(ps, LP_JSON_STRING); ps->j->nodes.v[k].str = off; ps->j->nodes.v[k].len = len;
            lp_push(&mine, k);
            ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') goto done;
            ps->p++;
        }
        uint32_t v;
        if (!value(ps, &v)) goto done;
        lp_push(&mine, v); members++;
        ws(ps);
        if (ps->p >= ps->end) goto done;
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == (object ? '}' : ']')) { ps->p++; ok = 1; break; }
        goto done;
    }
done:
    if (ok) {
        lp_vec_reserve(&ps->j->kids, ps->j->kids.n + mine.n);
        Node *n = &ps->j->nodes.v[me]; n->first = (uint32_t)ps->j->kids.n; n->n = members;
        if (mine.n) memcpy(ps->j->kids.v + ps->j->kids.n, mine.v, mine.n * sizeof *mine.v);
        ps->j->kids.n += mine.n;
    }
    lp_vec_free(&mine); ps->depth--; *out = me;
    return ok;
}

static int literal(Parser *ps, const char *word, lp_json_type t, uint32_t *out){
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n)) return 0;
    ps->p += n; *out = node(ps, t); return 1;
}

static int one_value(Parser *ps, uint32_t *out);
static int value(Parser *ps, uint32_t *out){
    ws(ps);
    const char *begin = ps->p;
    if (!one_value(ps, out)) return 0;
    Node *n = &ps->j->nodes.v[*out]; n->at = (size_t)(begin - ps->j->text); n->span = (size_t)(ps->p - begin);
    return 1;
}
static int one_value(Parser *ps, uint32_t *out){
    if (ps->p >= ps->end) return 0;
    switch (*ps->p) {
    case '{': return container(ps, 1, out);
    case '[': return container(ps, 0, out);
    case '"': {
        size_t off, len;
        if (!string(ps, &off, &len)) return 0;
        uint32_t k = node(ps, LP_JSON_STRING); ps->j->nodes.v[k].str = off; ps->j->nodes.v[k].len = len; *out = k; return 1;
    }
    case 't': return literal(ps, "true", LP_JSON_TRUE, out);
    case 'f': return literal(ps, "false", LP_JSON_FALSE, out);
    case 'n': return literal(ps, "null", LP_JSON_NULL, out);
    default:  return number(ps, out);
    }
}

LP_API lp_json *lp_json_parse(const char *text, size_t len){
    if (!text) return NULL;
    lp_json *j = lp_zalloc(sizeof *j); j->text = text;
    Parser ps = { text, text + len, j, 0 };
    uint32_t root;
    /* The root is node 0: the first node a value makes. */
    if (!value(&ps, &root) || root != LP_JSON_ROOT || (ws(&ps), ps.p != ps.end)) { lp_json_free(j); return NULL; }
    return j;
}

LP_API void lp_json_free(lp_json *j){
    if (!j) return;
    lp_vec_free(&j->nodes); lp_vec_free(&j->kids); lp_buf_free(&j->strs); lp_free(j);
}

static const Node *at(const lp_json *j, uint32_t k){ return j && k < j->nodes.n ? &j->nodes.v[k] : NULL; }

LP_API lp_json_type lp_json_kind(const lp_json *j, uint32_t k){ const Node *n = at(j, k); return n ? (lp_json_type)n->type : LP_JSON_NULL; }
LP_API uint32_t lp_json_count(const lp_json *j, uint32_t k){
    const Node *n = at(j, k); return n && (n->type == LP_JSON_ARRAY || n->type == LP_JSON_OBJECT) ? n->n : 0;
}
LP_API uint32_t lp_json_item(const lp_json *j, uint32_t k, uint32_t i){
    const Node *n = at(j, k);
    if (!n || i >= lp_json_count(j, k)) return LP_JSON_NONE;
    return n->type == LP_JSON_OBJECT ? j->kids.v[n->first + 2 * i + 1] : j->kids.v[n->first + i];
}
LP_API const char *lp_json_key(const lp_json *j, uint32_t k, uint32_t i){
    const Node *n = at(j, k);
    if (!n || n->type != LP_JSON_OBJECT || i >= n->n) return NULL;
    return lp_json_string(j, j->kids.v[n->first + 2 * i], NULL);
}
LP_API uint32_t lp_json_get(const lp_json *j, uint32_t k, const char *key){
    const Node *n = at(j, k);
    if (!n || n->type != LP_JSON_OBJECT || !key) return LP_JSON_NONE;
    size_t want = strlen(key);
    for (uint32_t i = 0; i < n->n; i++) {
        size_t len; const char *s = lp_json_string(j, j->kids.v[n->first + 2 * i], &len);
        if (len == want && !memcmp(s, key, want)) return j->kids.v[n->first + 2 * i + 1];
    }
    return LP_JSON_NONE;
}
LP_API const char *lp_json_string(const lp_json *j, uint32_t k, size_t *len){
    const Node *n = at(j, k);
    if (!n || n->type != LP_JSON_STRING) return NULL;
    if (len) *len = n->len;
    return (const char *)j->strs.b + n->str;
}
LP_API bool lp_json_number(const lp_json *j, uint32_t k, double *out){
    const Node *n = at(j, k); if (!n || n->type != LP_JSON_NUMBER) return false; *out = n->num; return true;
}
LP_API bool lp_json_integer(const lp_json *j, uint32_t k, int64_t *out){
    const Node *n = at(j, k); if (!n || n->type != LP_JSON_NUMBER || !n->integral) return false; *out = n->i; return true;
}
LP_API bool lp_json_bool(const lp_json *j, uint32_t k, bool *out){
    const Node *n = at(j, k); if (!n || (n->type != LP_JSON_TRUE && n->type != LP_JSON_FALSE)) return false;
    *out = n->type == LP_JSON_TRUE; return true;
}
LP_API const char *lp_json_text(const lp_json *j, uint32_t k, size_t *len){
    const Node *n = at(j, k); if (!n) return NULL; if (len) *len = n->span; return j->text + n->at;
}

LP_API void lp_json_str(lp_buf *b, const char *s, size_t len){
    static const char hex[] = "0123456789abcdef";
    *lp_buf_room(b, 1) = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *named = c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\r' ? "\\r"
                          : c == '\t' ? "\\t" : c == '\b' ? "\\b" : c == '\f' ? "\\f" : NULL;
        if (named) lp_buf_put(b, named, 2);
        else if (c < 0x20) { uint8_t *o = lp_buf_room(b, 6); memcpy(o, "\\u00", 4); o[4] = (uint8_t)hex[c >> 4]; o[5] = (uint8_t)hex[c & 15]; }
        else *lp_buf_room(b, 1) = c;
    }
    *lp_buf_room(b, 1) = '"';
}
