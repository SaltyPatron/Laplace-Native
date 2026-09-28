/* laplace-ingest: decompose UTF-8 text files into Laplace's Merkle DAG and stream it into PostgreSQL.
 *
 *   file      = [paragraph ...]            tier 5
 *   paragraph = [sentence ...]             tier 4 (ends at a blank line)
 *   sentence  = [word segment ...]         tier 3 (UAX #29)
 *   segment   = [grapheme | codepoint ...] tier 2 (UAX #29)
 *   grapheme  = [codepoint ...]            tier 1 (only when a grapheme has more than one codepoint)
 *
 * Hard-wrapped text: ICU sees a copy in which single line breaks inside a paragraph are spaces; boundaries apply to
 * the original bytes, so every byte stays in the content. Every file is recomposed from the DAG and compared byte for
 * byte. Rows go to the server by binary COPY over libpq; nothing is written to intermediate files.
 *
 * Observability: a live line every second (files, MB/s, entities per tier new and reused), then a report of every
 * phase with its time and rate.
 *
 * Usage: laplace-ingest [-d conninfo] [-t tier0.bin] [--no-load] file...
 */
#include "laplace/laplace.h"
#include "blake3.h"
#include "json_min.h"
#include <tree_sitter/api.h>
const TSLanguage *tree_sitter_json(void);
const TSLanguage *tree_sitter_xml(void);
#include <libpq-fe.h>
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <locale.h>
#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NT 256
typedef struct { uint64_t ref; uint32_t run; } Vtx;                 /* ref < NCP: codepoint; else NCP + composition */
typedef struct { lp_id id; int64_t m[4]; uint64_t vstart; uint32_t vcount, len; uint8_t tier; } Comp;

static const lp_tier0_record *t0;
static Comp *comps; static uint64_t ncomp, capcomp;
static Vtx *verts; static uint64_t nvert, capvert;
static uint64_t *table; static uint64_t tcap;
static uint64_t st_new[NT], st_hit[NT], st_mismatch;
static double T0; static uint64_t bytes_in;

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void *xrealloc(void *p, size_t n){ p = realloc(p, n); if (!p) { perror("realloc"); exit(1); } return p; }
static const lp_id *ref_id(uint64_t r){ return r < LP_NCP ? &t0[r].id : &comps[r - LP_NCP].id; }
static const int64_t *ref_m(uint64_t r){ return r < LP_NCP ? t0[r].m : comps[r - LP_NCP].m; }
static uint64_t hkey(const lp_id *id){ uint64_t k; memcpy(&k, id->b, 8); return k; }

static void table_grow(void){
    uint64_t ncap = tcap ? tcap * 2 : (1u << 22), *nt = calloc(ncap, 8);
    for (uint64_t i = 0; i < tcap; i++) if (table[i]) {
        uint64_t h = hkey(&comps[table[i] - 1].id) & (ncap - 1);
        while (nt[h]) h = (h + 1) & (ncap - 1);
        nt[h] = table[i];
    }
    free(table); table = nt; tcap = ncap;
}

/* The composition of children ch[0..n): found by ID if already recorded, else recorded. */
static lp_id *idbuf; static size_t idcap;
static uint64_t node(const uint64_t *ch, uint32_t n, uint8_t tier){
    if (n == 1) return ch[0];
    if (n > idcap) { idcap = n * 2; idbuf = xrealloc(idbuf, idcap * sizeof(lp_id)); }
    for (uint32_t i = 0; i < n; i++) idbuf[i] = *ref_id(ch[i]);
    lp_id id; lp_id_compose(idbuf, n, &id);
    if ((ncomp + 1) * 2 > tcap) table_grow();
    uint64_t s = hkey(&id) & (tcap - 1);
    while (table[s]) {
        Comp *c = &comps[table[s] - 1];
        if (!memcmp(&c->id, &id, 16)) {
            uint32_t k = 0, bad = (c->len != n);
            for (uint32_t v = 0; v < c->vcount && !bad; v++)
                for (uint32_t r = 0; r < verts[c->vstart + v].run && !bad; r++, k++) bad = (k >= n || verts[c->vstart + v].ref != ch[k]);
            st_mismatch += bad; st_hit[tier]++;
            return LP_NCP + (table[s] - 1);
        }
        s = (s + 1) & (tcap - 1);
    }
    if (ncomp == capcomp) { capcomp = capcomp ? capcomp * 2 : (1u << 22); comps = xrealloc(comps, capcomp * sizeof *comps); }
    Comp *c = &comps[ncomp]; c->id = id; c->tier = tier; c->len = n; c->vstart = nvert; c->vcount = 0;
    __int128 sum[4] = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n; i++) {
        const int64_t *m = ref_m(ch[i]); for (int d = 0; d < 4; d++) sum[d] += m[d];
        if (c->vcount && verts[nvert - 1].ref == ch[i]) { verts[nvert - 1].run++; continue; }
        if (nvert == capvert) { capvert = capvert ? capvert * 2 : (1u << 24); verts = xrealloc(verts, capvert * sizeof *verts); }
        verts[nvert++] = (Vtx){ ch[i], 1 }; c->vcount++;
    }
    for (int d = 0; d < 4; d++) c->m[d] = (int64_t)(sum[d] / (__int128)n);
    table[s] = ++ncomp; st_new[tier]++;
    return LP_NCP + (ncomp - 1);
}

/* ---- UTF-8 */
static int utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp);
static int utf8_valid(const uint8_t *s, size_t n){ size_t i = 0; uint32_t c; while (i < n) if (!utf8_next(s, n, &i, &c)) return 0; return 1; }
static int utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp){
    uint8_t b = s[*i];
    if (b < 0x80) { *cp = b; *i += 1; return 1; }
    int len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 0;
    if (!len || *i + len > n) return 0;
    uint32_t c = b & (0x7F >> len);
    for (int k = 1; k < len; k++) { if ((s[*i + k] & 0xC0) != 0x80) return 0; c = (c << 6) | (s[*i + k] & 0x3F); }
    *cp = c; *i += len; return c < LP_NCP;
}
static size_t utf8_put(uint32_t cp, uint8_t *o){
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 0x3F); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 0x3F); o[2] = 0x80 | (cp & 0x3F); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 0x3F); o[2] = 0x80 | (cp >> 6 & 0x3F); o[3] = 0x80 | (cp & 0x3F); return 4;
}

typedef struct { int32_t *b; size_t n, cap; } Bounds;
static UBreakIterator *brk[3];                                  /* sentence, word, character: opened once, reused */
static void bounds_of(UBreakIteratorType ty, UText *ut, Bounds *out){
    int k = ty == UBRK_SENTENCE ? 0 : ty == UBRK_WORD ? 1 : 2; UErrorCode e = U_ZERO_ERROR;
    if (!brk[k]) { brk[k] = ubrk_open(ty, "", NULL, 0, &e); if (U_FAILURE(e)) { fprintf(stderr, "ICU: %s\n", u_errorName(e)); exit(1); } }
    UBreakIterator *bi = brk[k]; ubrk_setUText(bi, ut, &e); if (U_FAILURE(e)) { fprintf(stderr, "ICU: %s\n", u_errorName(e)); exit(1); }
    out->n = 0;
    for (int32_t p = ubrk_first(bi); p != UBRK_DONE; p = ubrk_next(bi)) {
        if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 4096; out->b = xrealloc(out->b, out->cap * 4); }
        out->b[out->n++] = p;
    }
}
typedef struct { uint64_t *v; size_t n, cap; } Refs;
static void push(Refs *r, uint64_t x){ if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 256; r->v = xrealloc(r->v, r->cap * 8); } r->v[r->n++] = x; }
static int utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp);
/* Text into the DAG: paragraphs (blank lines), sentences and word segments (UAX #29), graphemes. Hard-wrapped lines are
 * seen by ICU as spaces through the copy cpy; boundaries apply to the original bytes. Returns the trunk. */
static Bounds sb, wb, gb; static Refs segs, sents, paras, g, cps;
static uint64_t node(const uint64_t *ch, uint32_t n, uint8_t tier);
static uint64_t decompose(const uint8_t *src, uint8_t *cpy, size_t n){
    memcpy(cpy, src, n);
    for (size_t i = 0; i < n; ) {
        if (src[i] != '\n' && src[i] != '\r') { i++; continue; }
        size_t s0 = i, e0 = i + ((src[i] == '\r' && i + 1 < n && src[i + 1] == '\n') ? 2 : 1), j = e0;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        int next_blank = (j < n && (src[j] == '\n' || src[j] == '\r'));
        size_t k = s0; while (k > 0 && (src[k - 1] == ' ' || src[k - 1] == '\t')) k--;
        int prev_blank = (k == 0 || src[k - 1] == '\n' || src[k - 1] == '\r');
        if (!next_blank && !prev_blank) for (size_t z = s0; z < e0; z++) cpy[z] = ' ';
        i = e0;
    }
    UErrorCode e = U_ZERO_ERROR; UText *ut = utext_openUTF8(NULL, (const char *)cpy, n, &e);
    bounds_of(UBRK_SENTENCE, ut, &sb); bounds_of(UBRK_WORD, ut, &wb); bounds_of(UBRK_CHARACTER, ut, &gb);
    paras.n = 0; sents.n = 0; size_t wi = 0, gi = 0;
    for (size_t si = 0; si + 1 < sb.n; si++) {
        int32_t s0 = sb.b[si], s1 = sb.b[si + 1], w0 = s0; segs.n = 0;
        while (w0 < s1) {
        while (wi < wb.n && wb.b[wi] <= w0) wi++;
        int32_t w1 = (wi < wb.n && wb.b[wi] < s1) ? wb.b[wi] : s1, g0 = w0; g.n = 0;
        while (g0 < w1) {
            while (gi < gb.n && gb.b[gi] <= g0) gi++;
            int32_t g1 = (gi < gb.n && gb.b[gi] < w1) ? gb.b[gi] : w1; size_t i = g0; uint32_t cp; cps.n = 0;
            while (i < (size_t)g1) { utf8_next(src, n, &i, &cp); push(&cps, cp); }
            push(&g, node(cps.v, cps.n, 1)); g0 = g1;
        }
        push(&segs, node(g.v, g.n, 2)); w0 = w1;
        }
        push(&sents, node(segs.v, segs.n, 3));
        int end_para = (si + 2 == sb.n);
        for (int32_t z = s1 - 1; z >= s0 && !end_para; z--) {
        if (src[z] != '\n' && src[z] != '\r' && src[z] != ' ' && src[z] != '\t') break;
        if ((src[z] == '\n' || src[z] == '\r') && cpy[z] != ' ') end_para = 1;
        }
        if (end_para) { push(&paras, node(sents.v, sents.n, 4)); sents.n = 0; }
    }
    if (sents.n) push(&paras, node(sents.v, sents.n, 4));
    utext_close(ut);
    return node(paras.v, paras.n, 5);
}


static uint8_t *rbuf; static size_t rlen, rcap;
static void expand(uint64_t r){
    if (r < LP_NCP) { if (rlen + 4 > rcap) { rcap = rcap ? rcap * 2 : (1u << 24); rbuf = xrealloc(rbuf, rcap); } rlen += utf8_put((uint32_t)r, rbuf + rlen); return; }
    const Comp *c = &comps[r - LP_NCP];
    for (uint32_t v = 0; v < c->vcount; v++) for (uint32_t k = 0; k < verts[c->vstart + v].run; k++) expand(verts[c->vstart + v].ref);
}

/* ---- tokenizer vocabularies: each token is text, decomposed like any other; the vocabulary is the path of its tokens in
 * index order. Byte-level BPE characters map back to bytes (GPT-2's table); SentencePiece's U+2581 is a space; a byte
 * token, or a byte-level token that is not valid UTF-8 by itself, is the notation <0xAB> of each byte. */
static uint64_t decompose(const uint8_t *src, uint8_t *cpy, size_t n);
static uint8_t gpt2_byte[0x180]; static int gpt2_ready;
static void gpt2_table(void){
    int bs[256], nb = 0, n = 0;
    for (int b = '!'; b <= '~'; b++) bs[nb++] = b;
    for (int b = 0xA1; b <= 0xAC; b++) bs[nb++] = b;
    for (int b = 0xAE; b <= 0xFF; b++) bs[nb++] = b;
    int is[256] = { 0 }; for (int i = 0; i < nb; i++) is[bs[i]] = 1;
    for (int i = 0; i < nb; i++) gpt2_byte[bs[i]] = (uint8_t)bs[i];
    for (int b = 0; b < 256; b++) if (!is[b]) gpt2_byte[256 + n++] = (uint8_t)b;
    gpt2_ready = 1;
}
static int utf8_valid(const uint8_t *s, size_t n);
static uint64_t text_ref(const uint8_t *t, size_t n){
    uint8_t *c = malloc(n + 1); uint64_t r = decompose(t, c, n); free(c); return r;
}
static uint64_t bytes_ref(const uint8_t *b, size_t n){         /* notation of each byte, composed */
    uint64_t *r = malloc(8 * n); char tmp[8];
    for (size_t i = 0; i < n; i++) { snprintf(tmp, sizeof tmp, "<0x%02X>", b[i]); r[i] = text_ref((const uint8_t *)tmp, 6); }
    uint64_t out = node(r, (uint32_t)n, 3); free(r); return out;
}
static const char *vocab_map_dir;
static uint64_t vocabulary(const char *path, size_t *ntok, size_t *nbyte){
    size_t len; FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); len = ftell(f); rewind(f); char *buf = malloc(len + 1);
    if (fread(buf, 1, len, f) != len) { fclose(f); free(buf); return 0; } fclose(f); buf[len] = 0; bytes_in += len;
    jdoc d = j_parse(buf, len);
    int64_t model = j_get(&d, 0, "model"), voc = model >= 0 ? j_get(&d, model, "vocab") : -1;
    if (voc < 0 || d.v[voc].t != J_OBJ) { free(buf); return 0; }
    int64_t mt = j_get(&d, model, "type"); const char *mtype = mt >= 0 && d.v[mt].t == J_STR ? d.v[mt].str : "";
    int wordpiece = !strcmp(mtype, "WordPiece"), bytelevel = strstr(buf, "\"ByteLevel\"") != NULL && !wordpiece;
    if (bytelevel && !gpt2_ready) gpt2_table();
    size_t maxid = 0; const jnode *vo = &d.v[voc];
    for (uint32_t i = 0; i < vo->n; i += 2) { size_t id = (size_t)d.v[d.kids[vo->first + i + 1]].num; if (id > maxid) maxid = id; }
    int64_t added = j_get(&d, 0, "added_tokens");
    if (added >= 0) for (uint32_t i = 0; i < d.v[added].n; i++) { uint32_t o = d.kids[d.v[added].first + i]; size_t id = (size_t)j_num(&d, o, "id", 0); if (id > maxid) maxid = id; }
    const char **tok = calloc(maxid + 1, sizeof(char *));
    for (uint32_t i = 0; i < vo->n; i += 2) tok[(size_t)d.v[d.kids[vo->first + i + 1]].num] = d.v[d.kids[vo->first + i]].str;
    if (added >= 0) for (uint32_t i = 0; i < d.v[added].n; i++) { uint32_t o = d.kids[d.v[added].first + i]; int64_t c = j_get(&d, o, "content");
        if (c >= 0) tok[(size_t)j_num(&d, o, "id", 0)] = d.v[c].str; }
    Refs refs = { 0 }; uint8_t *b = malloc(1024); *ntok = 0; *nbyte = 0;
    FILE *mf = NULL;                                              /* token index -> entity ID, for attaching testimony */
    if (vocab_map_dir) { char mp[4096]; const char *m0 = strstr(path, "/models/"); char nm[1024]; snprintf(nm, sizeof nm, "%s", m0 ? m0 + 8 : path);
        for (char *c = nm; *c; c++) if (*c == '/') *c = '_';
        snprintf(mp, sizeof mp, "%s/%s.vocabmap", vocab_map_dir, nm); mf = fopen(mp, "wb"); }
    for (size_t id = 0; id <= maxid; id++) {
        const char *s = tok[id]; if (!s || !*s) continue;
        size_t L = strlen(s), n = 0; uint64_t r;
        if (L == 6 && s[0] == '<' && s[1] == '0' && s[2] == 'x' && s[5] == '>') {     /* SentencePiece byte token */
            uint8_t v = (uint8_t)strtol(s + 3, NULL, 16); r = bytes_ref(&v, 1); (*nbyte)++;
        } else if (bytelevel) {
            int ok = 1; size_t i = 0;
            while (i < L && n < 1000) {                                                   /* characters back to bytes */
                size_t i0 = i; uint32_t c; if (!utf8_next((const uint8_t *)s, L, &i, &c)) { ok = 0; break; }
                if (c < 0x180 && (c >= 256 || gpt2_byte[c] == c) && (c >= 256 ? 1 : (c >= '!' && c <= '~') || (c >= 0xA1 && c <= 0xAC) || (c >= 0xAE && c <= 0xFF)))
                    b[n++] = gpt2_byte[c];
                else { memcpy(b + n, s + i0, i - i0); n += i - i0; }                       /* added tokens: literal text */
            }
            if (!ok) continue;
            if (utf8_valid(b, n)) r = text_ref(b, n); else { r = bytes_ref(b, n); (*nbyte)++; }
        } else {
            for (size_t i = 0; i < L && n < 1000; ) {
                if ((unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x96 && (unsigned char)s[i + 2] == 0x81) { b[n++] = ' '; i += 3; }
                else b[n++] = (uint8_t)s[i++];
            }
            if (wordpiece && n > 2 && b[0] == '#' && b[1] == '#') { memmove(b, b + 2, n - 2); n -= 2; }
            r = text_ref(b, n);
        }
        push(&refs, r); (*ntok)++;
        if (mf) { uint32_t i32 = (uint32_t)id; fwrite(&i32, 4, 1, mf); fwrite(ref_id(r)->b, 16, 1, mf); }
    }
    if (mf) fclose(mf);
    uint64_t trunk = node(refs.v, (uint32_t)refs.n, 5);
    free(refs.v); free(b); free(tok); free(buf); return trunk;
}

/* ---- syntax-tree recipes (JSON, XML): the tree-sitter syntax tree, each node the composition of its children, with the bytes between
 * children kept as text, so the file recomposes byte for byte. Leaves are text, decomposed like any other. */
static uint64_t ast_node(TSNode nd, const uint8_t *src, uint32_t lo, uint32_t hi, int depth){
    uint32_t nc = ts_node_child_count(nd);
    if (nc == 0 || depth > 200) return text_ref(src + lo, hi - lo);
    Refs kids = { 0 }; uint32_t at = lo; uint8_t tmax = 0;
    for (uint32_t i = 0; i < nc; i++) {
        TSNode c = ts_node_child(nd, i); uint32_t cs = ts_node_start_byte(c), ce = ts_node_end_byte(c);
        if (cs > at) push(&kids, text_ref(src + at, cs - at));
        if (ce > cs) push(&kids, ast_node(c, src, cs, ce, depth + 1));
        at = ce;
    }
    if (hi > at) push(&kids, text_ref(src + at, hi - at));
    for (size_t i = 0; i < kids.n; i++) { uint8_t t = kids.v[i] < LP_NCP ? 0 : comps[kids.v[i] - LP_NCP].tier; if (t > tmax) tmax = t; }
    uint64_t r = node(kids.v, (uint32_t)kids.n, (uint8_t)(tmax + 1)); free(kids.v); return r;
}
/* A syntax-tree recipe: the grammar's tree of the file, recorded as content. */
static uint64_t ast_ref(TSParser **p, const TSLanguage *(*lang)(void), const uint8_t *src, size_t n){
    if (!*p) { *p = ts_parser_new(); ts_parser_set_language(*p, lang()); }
    TSTree *t = ts_parser_parse_string(*p, NULL, (const char *)src, (uint32_t)n);
    uint64_t r = ast_node(ts_tree_root_node(t), src, 0, (uint32_t)n, 0); ts_tree_delete(t); return r;
}
static TSParser *json_parser, *xml_parser;
static uint64_t json_ref(const uint8_t *src, size_t n){ return ast_ref(&json_parser, tree_sitter_json, src, n); }
static uint64_t xml_ref(const uint8_t *src, size_t n){ return ast_ref(&xml_parser, tree_sitter_xml, src, n); }

/* ---- attestations: a recipe names which parts of a source's syntax tree state something about which entity. Each
 * statement becomes a claim, a composition of entities hashed like any path, and plays a Glicko-2 matchup as it is
 * read, with the source as witness. Recipe lines (one file per source format):
 *   element NAME...            elements whose attributes attest
 *   subject ATTR codepoint     the attribute naming the subject, a codepoint in hex
 *   range ATTR ATTR codepoint  attributes naming a first..last range of codepoint subjects
 *   trust T                    the witness's trust, -1 .. 1 */
typedef struct { char el[16][48]; int nel; char subj[48], lo[48], hi[48]; double trust; int on; } Recipe;
static Recipe recipe;
static int recipe_load(const char *path){
    FILE *f = fopen(path, "r"); if (!f) { perror(path); return 0; }
    char line[1024]; recipe.trust = 0; recipe.on = 1;
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *tok = strtok(line, " \t\r\n"); if (!tok) continue;
        if (!strcmp(tok, "element")) while ((tok = strtok(NULL, " \t\r\n")) && recipe.nel < 16) snprintf(recipe.el[recipe.nel++], 48, "%s", tok);
        else if (!strcmp(tok, "subject")) { tok = strtok(NULL, " \t\r\n"); if (tok) snprintf(recipe.subj, 48, "%s", tok); }
        else if (!strcmp(tok, "range")) { char *a = strtok(NULL, " \t\r\n"), *b = strtok(NULL, " \t\r\n"); if (a && b) { snprintf(recipe.lo, 48, "%s", a); snprintf(recipe.hi, 48, "%s", b); } }
        else if (!strcmp(tok, "trust")) { tok = strtok(NULL, " \t\r\n"); if (tok) recipe.trust = atof(tok); }
    }
    fclose(f); return 1;
}
/* Strings to their decomposed entity, each decomposed once. */
typedef struct { uint64_t h, ref; uint32_t off, len; } SEnt;
static SEnt *stab; static uint64_t scap, sn; static char *spool; static size_t spn, spcap;
static uint64_t fnv(const uint8_t *s, size_t n){ uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) { h ^= s[i]; h *= 1099511628211ull; } return h ? h : 1; }
static uint64_t string_ref(const uint8_t *s, size_t n){
    if ((sn + 1) * 2 > scap) {
        uint64_t oc = scap; SEnt *old = stab; scap = scap ? scap * 2 : (1u << 16); stab = calloc(scap, sizeof *stab);
        for (uint64_t i = 0; i < oc; i++) if (old[i].h) { uint64_t k = old[i].h & (scap - 1); while (stab[k].h) k = (k + 1) & (scap - 1); stab[k] = old[i]; }
        free(old);
    }
    uint64_t h = fnv(s, n), k = h & (scap - 1);
    while (stab[k].h) { if (stab[k].h == h && stab[k].len == n && !memcmp(spool + stab[k].off, s, n)) return stab[k].ref; k = (k + 1) & (scap - 1); }
    if (spn + n > spcap) { spcap = (spn + n) * 2; spool = xrealloc(spool, spcap); }
    memcpy(spool + spn, s, n); stab[k] = (SEnt){ h, text_ref(s, n), (uint32_t)spn, (uint32_t)n }; spn += n; sn++;
    return stab[k].ref;
}
/* XML attribute values with their references resolved (the predefined entities and numeric references). */
static size_t xml_unescape(const uint8_t *s, size_t n, uint8_t *o){
    size_t k = 0;
    for (size_t i = 0; i < n; ) {
        if (s[i] != '&') { o[k++] = s[i++]; continue; }
        size_t j = i + 1; while (j < n && s[j] != ';' && j - i < 12) j++;
        if (j >= n || s[j] != ';') { o[k++] = s[i++]; continue; }
        const char *e = (const char *)s + i + 1; size_t el = j - i - 1; uint32_t cp = 0; int ok = 1;
        if (el == 2 && !memcmp(e, "lt", 2)) cp = '<'; else if (el == 2 && !memcmp(e, "gt", 2)) cp = '>';
        else if (el == 3 && !memcmp(e, "amp", 3)) cp = '&'; else if (el == 4 && !memcmp(e, "apos", 4)) cp = '\''; else if (el == 4 && !memcmp(e, "quot", 4)) cp = '"';
        else if (el > 1 && e[0] == '#') cp = (uint32_t)(e[1] == 'x' ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10));
        else ok = 0;
        if (!ok) { o[k++] = s[i++]; continue; }
        k += utf8_put(cp, o + k); i = j + 1;
    }
    return k;
}
/* Attestation events in the order they were read: claim, witness file, outcome. */
typedef struct { uint64_t claim; int32_t file; float score; } Event;
static Event *ev; static uint64_t nev, cev;
static uint64_t *claim_roots; static uint64_t nclaim_roots, cclaim_roots;
static void attest(uint64_t claim, int file, float score){
    if (nev == cev) { cev = cev ? cev * 2 : (1u << 20); ev = xrealloc(ev, cev * sizeof *ev); }
    ev[nev++] = (Event){ claim, file, score };
    if (nclaim_roots == cclaim_roots) { cclaim_roots = cclaim_roots ? cclaim_roots * 2 : (1u << 20); claim_roots = xrealloc(claim_roots, cclaim_roots * 8); }
    claim_roots[nclaim_roots++] = claim;
}
static uint64_t claim3(uint64_t a, uint64_t b, uint64_t c){
    uint64_t ch[3] = { a, b, c }; uint8_t t = 0;
    for (int i = 0; i < 3; i++) { uint8_t x = ch[i] < LP_NCP ? 0 : comps[ch[i] - LP_NCP].tier; if (x > t) t = x; }
    return node(ch, 3, (uint8_t)(t + 1));
}
static int node_text(TSNode nd, const uint8_t *src, const uint8_t **p, size_t *n){
    if (ts_node_is_null(nd)) return 0;
    *p = src + ts_node_start_byte(nd); *n = ts_node_end_byte(nd) - ts_node_start_byte(nd); return 1;
}
static uint64_t n_claims_attr;
static void xml_attest(TSNode nd, const uint8_t *src, int file, uint8_t *ub){
    const char *ty = ts_node_type(nd);
    if (!strcmp(ty, "element")) {
        TSNode tag = ts_node_named_child(nd, 0); const char *tt = ts_node_type(tag);
        if (!strcmp(tt, "STag") || !strcmp(tt, "EmptyElemTag")) {
            const uint8_t *np; size_t nl; int hit = 0;
            TSNode name = ts_node_named_child(tag, 0);
            if (node_text(name, src, &np, &nl)) for (int i = 0; i < recipe.nel && !hit; i++) hit = strlen(recipe.el[i]) == nl && !memcmp(recipe.el[i], np, nl);
            if (hit) {
                uint32_t na = ts_node_named_child_count(tag); int64_t lo = -1, hi = -1;
                for (uint32_t i = 1; i < na; i++) {                          /* the subject */
                    TSNode at = ts_node_named_child(tag, i); const uint8_t *ap, *vp; size_t al, vl;
                    if (!node_text(ts_node_named_child(at, 0), src, &ap, &al) || !node_text(ts_node_named_child(at, 1), src, &vp, &vl) || vl < 2) continue;
                    long v = strtol((const char *)vp + 1, NULL, 16);
                    if (strlen(recipe.subj) == al && !memcmp(recipe.subj, ap, al)) lo = hi = v;
                    else if (strlen(recipe.lo) == al && !memcmp(recipe.lo, ap, al)) lo = v;
                    else if (strlen(recipe.hi) == al && !memcmp(recipe.hi, ap, al)) hi = v;
                }
                if (lo >= 0 && hi >= lo && hi < (int64_t)LP_NCP)
                    for (uint32_t i = 1; i < na; i++) {                      /* every other attribute: a claim per subject */
                        TSNode at = ts_node_named_child(tag, i); const uint8_t *ap, *vp; size_t al, vl;
                        if (!node_text(ts_node_named_child(at, 0), src, &ap, &al) || !node_text(ts_node_named_child(at, 1), src, &vp, &vl) || vl < 2) continue;
                        if ((strlen(recipe.subj) == al && !memcmp(recipe.subj, ap, al)) || (strlen(recipe.lo) == al && !memcmp(recipe.lo, ap, al)) ||
                            (strlen(recipe.hi) == al && !memcmp(recipe.hi, ap, al))) continue;
                        size_t ul = xml_unescape(vp + 1, vl - 2, ub);
                        if (!ul) continue;                                   /* an empty value is no content */
                        uint64_t pr = string_ref(ap, al), vr = string_ref(ub, ul);
                        for (int64_t cp = lo; cp <= hi; cp++) { attest(claim3((uint64_t)cp, pr, vr), file, 1.0f); n_claims_attr++; }
                    }
            }
        }
    }
    uint32_t nc = ts_node_named_child_count(nd);
    for (uint32_t i = 0; i < nc; i++) xml_attest(ts_node_named_child(nd, i), src, file, ub);
}
static void xml_attest_file(const uint8_t *src, size_t n, int file){
    if (!xml_parser) { xml_parser = ts_parser_new(); ts_parser_set_language(xml_parser, tree_sitter_xml()); }
    TSTree *t = ts_parser_parse_string(xml_parser, NULL, (const char *)src, (uint32_t)n);
    uint8_t *ub = malloc(n + 4); xml_attest(ts_tree_root_node(t), src, file, ub); free(ub); ts_tree_delete(t);
}

/* ---- binary COPY */
typedef struct { PGconn *pg; uint8_t *b; size_t n, cap; uint64_t rows, bytes; } Copy;
static void cflush(Copy *c){ if (c->n && PQputCopyData(c->pg, (const char *)c->b, (int)c->n) != 1) { fprintf(stderr, "COPY: %s", PQerrorMessage(c->pg)); exit(1); } c->bytes += c->n; c->n = 0; }
static void cput(Copy *c, const void *p, size_t n){ if (c->n + n > c->cap) cflush(c); if (n > c->cap) { c->cap = n * 2; c->b = xrealloc(c->b, c->cap); } memcpy(c->b + c->n, p, n); c->n += n; }
static void cbe16(Copy *c, uint16_t v){ uint8_t b[2] = { v >> 8, v }; cput(c, b, 2); }
static void cbe32(Copy *c, uint32_t v){ uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v }; cput(c, b, 4); }
static void cbe64(Copy *c, uint64_t v){ uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i)); cput(c, b, 8); }
static void cfield(Copy *c, const void *p, uint32_t n){ cbe32(c, n); cput(c, p, n); }
static void cf_i16(Copy *c, int16_t v){ cbe32(c, 2); cbe16(c, (uint16_t)v); }
static void cf_i64(Copy *c, int64_t v){ cbe32(c, 8); cbe64(c, (uint64_t)v); }
static void cf_f64(Copy *c, double v){ uint64_t u; memcpy(&u, &v, 8); cbe32(c, 8); cbe64(c, u); }
static void copy_begin(Copy *c, const char *sql){
    PGresult *r = PQexec(c->pg, sql);
    if (PQresultStatus(r) != PGRES_COPY_IN) { fprintf(stderr, "%s: %s", sql, PQerrorMessage(c->pg)); exit(1); }
    PQclear(r); c->rows = 0;
    static const uint8_t hdr[19] = { 'P','G','C','O','P','Y','\n',0xFF,'\r','\n',0, 0,0,0,0, 0,0,0,0 };
    cput(c, hdr, 19);
}
static void copy_end(Copy *c){
    cbe16(c, 0xFFFF); cflush(c);
    if (PQputCopyEnd(c->pg, NULL) != 1) { fprintf(stderr, "COPY end: %s", PQerrorMessage(c->pg)); exit(1); }
    PGresult *r = PQgetResult(c->pg);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) { fprintf(stderr, "COPY: %s", PQerrorMessage(c->pg)); exit(1); }
    PQclear(r);
}
static int64_t hkey_signed(uint64_t h){ return (int64_t)(h ^ 0x8000000000000000ull); }   /* bigint order = Hilbert order */

/* ---- observability */
static double last_tick;
static void tick(int fi, int nfiles, int force){
    double t = now(); if (!force && t - last_tick < 1.0) return; last_tick = t;
    double el = t - T0; uint64_t nw = 0, hw = 0; for (int k = 1; k < NT; k++) { nw += st_new[k]; hw += st_hit[k]; }
    fprintf(stderr, "\r[%6.1fs] files %d/%d  %7.1f MB  %6.1f MB/s  entities %'10llu new, %'11llu reused  (words %'llu new / %'llu reused)   ",
            el, fi, nfiles, bytes_in / 1e6, bytes_in / 1e6 / el, (unsigned long long)nw, (unsigned long long)hw,
            (unsigned long long)st_new[2], (unsigned long long)st_hit[2]);
}
static void phase(const char *name, double secs, double items, const char *unit){
    printf("  %-44s %8.2f s", name, secs);
    if (items > 0) printf("   %'14.0f %s/s", items / secs, unit);
    putchar('\n');
}
int main(int argc, char **argv){
    const char *conninfo = "host=localhost port=5432 user=laplace dbname=laplace";
    const char *t0path = "/repos/src/Laplace-Prototype/tier0/tier0.bin"; int load = 1, a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-d") && a + 1 < argc) conninfo = argv[++a];
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) t0path = argv[++a];
        else if (!strcmp(argv[a], "--no-load")) load = 0;
        else if (!strcmp(argv[a], "--vocab-map") && a + 1 < argc) vocab_map_dir = argv[++a];
        else if (!strcmp(argv[a], "--recipe") && a + 1 < argc) { if (!recipe_load(argv[++a])) return 2; }
        else { fprintf(stderr, "usage: laplace-ingest [-d conninfo] [-t tier0.bin] [--no-load] file...\n"); return 2; }
    }
    int nfiles = argc - a; if (nfiles <= 0) { fprintf(stderr, "no files\n"); return 2; }
    setlocale(LC_NUMERIC, "en_US.UTF-8");
    T0 = now();
    printf("laplace-ingest   cpu: %s   dispatch: %s\n", lp_cpu_describe(lp_cpu_features()), lp_cpu_describe(lp_cpu_active()));
    double t = now(); t0 = lp_tier0_map(t0path);
    if (!t0) { fprintf(stderr, "cannot map tier 0 at %s\n", t0path); return 1; }
    double t_map = now() - t;

    uint64_t *trunks = calloc(nfiles, 8); size_t *fbytes = calloc(nfiles, sizeof(size_t)); uint8_t (*fsha)[32] = calloc(nfiles, 32);
    uint8_t *known = calloc(nfiles, 1); int nknown = 0; PGconn *pg = NULL; double t_known = 0;
    if (load) {
        pg = PQconnectdb(conninfo);
        if (PQstatus(pg) != CONNECTION_OK) { fprintf(stderr, "%s", PQerrorMessage(pg)); return 1; }
        /* Files whose exact bytes are already recorded are skipped before decomposition: one set-based query over the
         * BLAKE3-256 of every file. */
        t = now(); size_t cap = 20 + (size_t)nfiles * 36; uint8_t *ab = malloc(cap), *q = ab + 20; uint8_t *buf = NULL; size_t bcap = 0;
        for (int fi = 0; fi < nfiles; fi++) {
            FILE *f = fopen(argv[a + fi], "rb"); if (!f) continue;
            fseek(f, 0, SEEK_END); size_t n = ftell(f); rewind(f);
            if (n > bcap) { bcap = n; buf = xrealloc(buf, bcap ? bcap : 1); }
            if (fread(buf, 1, n, f) != n) { fclose(f); continue; } fclose(f);
            blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, buf, n); blake3_hasher_finalize(&h, fsha[fi], 32);
            uint32_t l = htonl(32); memcpy(q, &l, 4); memcpy(q + 4, fsha[fi], 32); q += 36;
        }
        free(buf);
        uint32_t hdr[5] = { htonl(1), htonl(0), htonl(17), htonl((uint32_t)nfiles), htonl(1) }; memcpy(ab, hdr, 20);
        const char *vals[1] = { (const char *)ab }; int lens[1] = { (int)(q - ab) }, fmts[1] = { 1 };
        PGresult *qr = PQexecParams(pg, "SELECT DISTINCT content FROM source WHERE content = ANY($1::bytea[])", 1, NULL, vals, lens, fmts, 1);
        if (PQresultStatus(qr) != PGRES_TUPLES_OK) { fprintf(stderr, "source check: %s", PQerrorMessage(pg)); return 1; }
        for (int rr = 0; rr < PQntuples(qr); rr++)
            for (int fi = 0; fi < nfiles; fi++) if (!known[fi] && !memcmp(fsha[fi], PQgetvalue(qr, rr, 0), 32)) { known[fi] = 1; nknown++; }
        PQclear(qr); free(ab); t_known = now() - t;
        printf("files already recorded byte for byte: %d of %d (checked in %.1f ms)\n", nknown, nfiles, t_known * 1000);
    }

    /* ---- decompose */
    t = now();
    int exact = 0, mism = 0, skipped = 0, vocabs = 0, jsons = 0; size_t vtokens = 0, vbytes = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *fn = argv[a + fi];
        if (known[fi]) { tick(fi + 1, nfiles, 0); continue; }
        size_t fl = strlen(fn);
        if (fl >= 14 && !strcmp(fn + fl - 14, "tokenizer.json")) {                  /* a vocabulary, not a document */
            size_t nt, nb; uint64_t before = bytes_in; trunks[fi] = vocabulary(fn, &nt, &nb); fbytes[fi] = bytes_in - before;
            if (trunks[fi]) { exact++; vocabs++; vtokens += nt; vbytes += nb; } else skipped++;
            if (!load) { FILE *hf = fopen(fn, "rb"); size_t hn; uint8_t *hb; fseek(hf, 0, SEEK_END); hn = ftell(hf); rewind(hf); hb = malloc(hn);
                         if (fread(hb, 1, hn, hf) == hn) { blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, hb, hn); blake3_hasher_finalize(&h, fsha[fi], 32); }
                         free(hb); fclose(hf); }
            tick(fi + 1, nfiles, 0); continue;
        }
        FILE *f = fopen(fn, "rb");
        if (!f) { perror(fn); skipped++; continue; }
        fseek(f, 0, SEEK_END); size_t n = ftell(f); rewind(f);
        uint8_t *src = malloc(n + 1), *cpy = malloc(n + 1);
        if (fread(src, 1, n, f) != n) { perror(fn); return 1; }
        fclose(f); bytes_in += n; fbytes[fi] = n;
        int ok = 1; for (size_t i = 0; i < n && ok; ) { uint32_t cp; ok = utf8_next(src, n, &i, &cp); }
        if (!ok) { fprintf(stderr, "\n  %s: invalid UTF-8, skipped\n", fn); skipped++; free(src); free(cpy); continue; }
        if (fl >= 5 && !strcmp(fn + fl - 5, ".json")) { trunks[fi] = json_ref(src, n); jsons++; }   /* recipe: the syntax tree */
        else if (fl >= 4 && !strcmp(fn + fl - 4, ".xml")) { trunks[fi] = xml_ref(src, n); jsons++; if (recipe.on) xml_attest_file(src, n, fi); }
        else trunks[fi] = decompose(src, cpy, n);
        rlen = 0; expand(trunks[fi]);
        if (rlen == n && !memcmp(rbuf, src, n)) exact++; else mism++;
        if (!load) { blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, src, n); blake3_hasher_finalize(&h, fsha[fi], 32); }
        free(src); free(cpy);
        tick(fi + 1, nfiles, 0);
    }
    tick(nfiles, nfiles, 1); fputc('\n', stderr);
    double t_dec = now() - t;

    /* ---- occurrences and distinct parents, parents before children */
    t = now();
    double *occ_c = calloc(ncomp, 8), *occ_a = calloc(LP_NCP, 8); uint64_t *par_c = calloc(ncomp, 8), *par_a = calloc(LP_NCP, 8);
    for (int fi = 0; fi < nfiles; fi++) if (trunks[fi] >= LP_NCP) occ_c[trunks[fi] - LP_NCP] += 1;
    uint64_t *stamp = calloc(LP_NCP + ncomp, 8);                /* the last parent that counted each child: O(vertices) */
    for (uint64_t i = ncomp; i-- > 0; ) {
        const Comp *c = &comps[i];
        for (uint32_t v = 0; v < c->vcount; v++) {
            uint64_t r = verts[c->vstart + v].ref; double add = occ_c[i] * verts[c->vstart + v].run;
            int first = stamp[r] != i + 1; stamp[r] = i + 1;
            if (r < LP_NCP) { occ_a[r] += add; par_a[r] += first; } else { occ_c[r - LP_NCP] += add; par_c[r - LP_NCP] += first; }
        }
    }
    free(stamp);
    double t_occ = now() - t;

    if (jsons) printf("\n== files decomposed by their syntax tree (JSON, XML): %d", jsons);
    if (vocabs) printf("\n== vocabularies: %d, %'zu tokens (%'zu written as byte notation)", vocabs, vtokens, vbytes);
    printf("\n== decomposition: %d files, %.1f MB, recomposed byte for byte %d, mismatched %d, skipped %d, ID collisions %llu\n",
           nfiles, bytes_in / 1e6, exact, mism, skipped, (unsigned long long)st_mismatch);
    static const char *tn[NT] = { "codepoint", "grapheme", "word segment", "sentence", "paragraph", "file" };
    printf("   %-13s %14s %16s\n", "tier", "distinct", "reused");
    for (int k = 1; k < NT; k++) {
        if (!st_new[k] && !st_hit[k]) continue;
        char nm[16]; if (!tn[k]) snprintf(nm, sizeof nm, "tier %d", k);
        printf("   %-13s %'14llu %'16llu\n", tn[k] ? tn[k] : nm, (unsigned long long)st_new[k], (unsigned long long)st_hit[k]);
    }
    printf("   compositions %'llu, path vertices %'llu after run-length encoding\n", (unsigned long long)ncomp, (unsigned long long)nvert);

    printf("\n== phases\n");
    phase("map tier 0", t_map, 0, "");
    if (load) phase("check file hashes against the database", t_known, (double)nfiles, "files");
    phase("decompose, hash, deduplicate, recompose", t_dec, bytes_in / 1e6, "MB");
    phase("occurrences and parents", t_occ, (double)ncomp, "compositions");
    if (!load) return mism ? 1 : 0;

    /* ---- load */
    PGresult *r = PQexec(pg, "SET synchronous_commit = off"); PQclear(r);
    Copy c = { pg, malloc(1 << 22), 0, 1 << 22, 0, 0 };
    uint8_t geo[64 * 1024]; lp_id *ids = NULL; uint32_t *runs = NULL; size_t idc = 0;

    /* ---- deduplication against what is recorded: trunk to leaf, by ID alone.
     * Tier is not content. A node stored when tiers were capped has the same ID at the tier this run assigns, and a
     * lookup that also demands the new tier misses it and writes it again. A node found means its whole subtree is
     * recorded, so nothing below it is checked or sent. */
    t = now();
    uint8_t *keep = calloc(LP_NCP + ncomp, 1);                       /* 1: new, to be written */
    uint8_t *seen = calloc(LP_NCP + ncomp, 1);
    uint64_t *front = malloc(8 * (ncomp + 1)), *nextf = malloc(8 * (ncomp + 1)), nf = 0, nn;
    uint64_t checked = 0, found = 0; int rounds = 0;
    for (int fi = 0; fi < nfiles; fi++) { if (known[fi]) continue; uint64_t tr = trunks[fi]; if (tr >= LP_NCP && !seen[tr]) { seen[tr] = 1; front[nf++] = tr; } }
    for (uint64_t i = 0; i < nclaim_roots; i++) { uint64_t cr = claim_roots[i]; if (cr >= LP_NCP && !seen[cr]) { seen[cr] = 1; front[nf++] = cr; } }   /* claims are roots too */
    size_t abcap = 0; uint8_t *ab = NULL;
    printf("\n== deduplication, trunk to leaf\n");
    while (nf) {
        rounds++; nn = 0;
        double tq = now(); uint64_t hit = 0, cnt = nf;
        for (uint64_t i0 = 0; i0 < nf; ) {                           /* chunks of up to 200k IDs as one binary uuid[] */
            size_t need = 20 + 20 * 200000; if (need > abcap) { abcap = need; ab = xrealloc(ab, abcap); }
            uint8_t *q = ab + 20; uint32_t n = 0; uint64_t i = i0;
            for (; i < nf && n < 200000; i++, n++) {
                const Comp *cm = &comps[front[i] - LP_NCP];
                uint32_t l = htonl(16); memcpy(q, &l, 4); memcpy(q + 4, cm->id.b, 16); q += 20;
            }
            i0 = i;
            uint32_t hdr[5] = { htonl(1), htonl(0), htonl(2950), htonl(n), htonl(1) }; memcpy(ab, hdr, 20);
            const char *vals[1] = { (const char *)ab }; int lens[1] = { (int)(q - ab) }, fmts[1] = { 1 };
            PGresult *qr = PQexecParams(pg, "SELECT id FROM entity WHERE id = ANY($1::uuid[])", 1, NULL, vals, lens, fmts, 1);
            if (PQresultStatus(qr) != PGRES_TUPLES_OK) { fprintf(stderr, "dedup: %s", PQerrorMessage(pg)); return 1; }
            for (int rr = 0; rr < PQntuples(qr); rr++) {               /* found: mark by looking the ID up in the table */
                lp_id fid; memcpy(fid.b, PQgetvalue(qr, rr, 0), 16);
                uint64_t sidx = hkey(&fid) & (tcap - 1);
                while (table[sidx] && memcmp(&comps[table[sidx] - 1].id, &fid, 16)) sidx = (sidx + 1) & (tcap - 1);
                if (table[sidx]) { keep[LP_NCP + table[sidx] - 1] = 2; hit++; }
            }
            PQclear(qr); checked += n;
        }
        found += hit;
        printf("  round %d  %'10llu candidates  %'10llu recorded  %'10llu new   %8.1f ms\n", rounds,
               (unsigned long long)cnt, (unsigned long long)hit, (unsigned long long)(cnt - hit), (now() - tq) * 1000);
        for (uint64_t i = 0; i < nf; i++) {                          /* new nodes: descend into their children */
            uint64_t r = front[i];
            if (keep[r] == 2) { keep[r] = 0; continue; }
            keep[r] = 1; const Comp *cm = &comps[r - LP_NCP];
            for (uint32_t v = 0; v < cm->vcount; v++) {
                uint64_t ch = verts[cm->vstart + v].ref;
                if (ch >= LP_NCP && !seen[ch]) { seen[ch] = 1; nextf[nn++] = ch; }
            }
        }
        uint64_t *tmp = front; front = nextf; nextf = tmp; nf = nn;
    }
    r = PQexec(pg, "SELECT count(*) FROM entity WHERE tier = 0");
    int atoms_recorded = PQresultStatus(r) == PGRES_TUPLES_OK && atoll(PQgetvalue(r, 0, 0)) == (long long)LP_NCP; PQclear(r);
    if (!atoms_recorded) for (uint64_t k = 0; k < LP_NCP; k++) keep[k] = 1;
    uint64_t nkeep = 0; for (uint64_t k = LP_NCP; k < LP_NCP + ncomp; k++) nkeep += keep[k] == 1;
    printf("  %d rounds, %'llu IDs checked, %'llu subtrees already recorded; %'llu compositions new; tier 0 %s\n", rounds,
           (unsigned long long)checked, (unsigned long long)found, (unsigned long long)nkeep, atoms_recorded ? "already recorded" : "written now");
    double t_dedup = now() - t;

    t = now(); copy_begin(&c, "COPY entity (id, tier, coord, hilbert) FROM STDIN (FORMAT binary)");
    for (uint64_t k = 0; k < LP_NCP + ncomp; k++) {
        if (keep[k] != 1) continue;
        const int64_t *m = ref_m(k); lp_coord co; memcpy(co.m, m, 32);
        double xyzm[4]; for (int d = 0; d < 4; d++) xyzm[d] = (double)m[d] / LP_FIXED_ONE;
        uint64_t hv = k < LP_NCP ? t0[k].hilbert : lp_hilbert4(&co);
        cbe16(&c, 4); cfield(&c, ref_id(k)->b, 16); cf_i16(&c, k < LP_NCP ? 0 : comps[k - LP_NCP].tier);
        size_t gl = lp_ewkb_point4(xyzm, geo, sizeof geo); cfield(&c, geo, (uint32_t)gl); cf_i64(&c, hkey_signed(hv));
        c.rows++;
    }
    copy_end(&c); double t_ent = now() - t; uint64_t ent_rows = c.rows, ent_bytes = c.bytes; c.bytes = 0;

    t = now(); copy_begin(&c, "COPY physicality (entity, tier, hilbert, path) FROM STDIN (FORMAT binary)");
    for (uint64_t k = 0; k < LP_NCP + ncomp; k++) {
        if (keep[k] != 1) continue;
        size_t gl; uint8_t *gp = geo; uint64_t hv;
        if (k < LP_NCP) { uint32_t one = 0; gl = lp_ewkb_runs(&t0[k].id, &one, 1, geo, sizeof geo); hv = t0[k].hilbert; }
        else {
            const Comp *cm = &comps[k - LP_NCP];
            if (cm->vcount > idc) { idc = cm->vcount * 2; ids = xrealloc(ids, idc * sizeof(lp_id)); runs = xrealloc(runs, idc * 4); }
            for (uint32_t v = 0; v < cm->vcount; v++) { ids[v] = *ref_id(verts[cm->vstart + v].ref); runs[v] = verts[cm->vstart + v].run; }
            gl = lp_ewkb_runs(ids, runs, cm->vcount, NULL, 0);
            if (gl > sizeof geo) gp = malloc(gl);
            lp_ewkb_runs(ids, runs, cm->vcount, gp, gl);
            lp_coord co; memcpy(co.m, cm->m, 32); hv = lp_hilbert4(&co);
        }
        cbe16(&c, 4); cfield(&c, ref_id(k)->b, 16); cf_i16(&c, k < LP_NCP ? 0 : comps[k - LP_NCP].tier); cf_i64(&c, hkey_signed(hv));
        cfield(&c, gp, (uint32_t)gl); if (gp != geo) free(gp);
        c.rows++;
    }
    copy_end(&c); double t_phy = now() - t; uint64_t phy_rows = c.rows, phy_bytes = c.bytes; c.bytes = 0;

    t = now(); copy_begin(&c, "COPY entity_stats (id, tier, parents, occurrences) FROM STDIN (FORMAT binary)");
    for (uint64_t k = 0; k < LP_NCP + ncomp; k++) {
        if (keep[k] != 1 && !(k < LP_NCP)) continue;              /* stats of recorded entities: set-based update, next */
        double o = k < LP_NCP ? occ_a[k] : occ_c[k - LP_NCP]; uint64_t p = k < LP_NCP ? par_a[k] : par_c[k - LP_NCP];
        if (k < LP_NCP && (o == 0 || atoms_recorded)) continue;
        cbe16(&c, 4); cfield(&c, ref_id(k)->b, 16); cf_i16(&c, k < LP_NCP ? 0 : comps[k - LP_NCP].tier); cf_i64(&c, (int64_t)p); cf_f64(&c, o);
        c.rows++;
    }
    copy_end(&c); double t_sta = now() - t; uint64_t sta_rows = c.rows;

    t = now(); copy_begin(&c, "COPY source (trunk, origin, format, bytes, content) FROM STDIN (FORMAT binary)");
    for (int fi = 0; fi < nfiles; fi++) {
        if (known[fi] || (!fbytes[fi] && !trunks[fi])) continue;
        const char *fn = argv[a + fi], *fmt = strlen(fn) >= 14 && !strcmp(fn + strlen(fn) - 14, "tokenizer.json") ? "tokenizer vocabulary" : "text/plain; charset=utf-8";
        cbe16(&c, 5); cfield(&c, ref_id(trunks[fi])->b, 16); cfield(&c, fn, (uint32_t)strlen(fn)); cfield(&c, fmt, (uint32_t)strlen(fmt));
        cf_i64(&c, (int64_t)fbytes[fi]); cfield(&c, fsha[fi], 32);
    }
    copy_end(&c); double t_src = now() - t;

    /* ---- semantics: the witness, the ledger in reading order, and each claim's standing after its matchups */
    double t_sem = now(); uint64_t n_led = 0, n_std_new = 0, n_std_upd = 0;
    if (nev) {
        uint32_t *slot = calloc(ncomp, 4); uint64_t nst = 0;
        for (uint64_t i = 0; i < nev; i++) { uint64_t k = ev[i].claim - LP_NCP; if (!slot[k]) slot[k] = (uint32_t)++nst; }
        lp_rating *st = malloc(sizeof(lp_rating) * (nst + 1)); uint32_t *matches = calloc(nst + 1, 4); uint8_t *had = calloc(nst + 1, 1);
        for (uint64_t k = 1; k <= nst; k++) st[k] = (lp_rating){ 1500.0, 350.0, 0.06 };
        /* claims already recorded start from their recorded standing: one set-based query per chunk */
        uint64_t *old = malloc(8 * (nst + 1)), nold = 0;
        for (uint64_t k = 0; k < ncomp; k++) if (slot[k] && keep[LP_NCP + k] != 1) old[nold++] = k;
        for (uint64_t i0 = 0; i0 < nold; i0 += 200000) {
            uint32_t n = (uint32_t)(nold - i0 < 200000 ? nold - i0 : 200000); size_t need = 20 + 20 * (size_t)n;
            if (need > abcap) { abcap = need; ab = xrealloc(ab, abcap); } uint8_t *q = ab + 20;
            for (uint32_t j = 0; j < n; j++) { uint32_t l = htonl(16); memcpy(q, &l, 4); memcpy(q + 4, comps[old[i0 + j]].id.b, 16); q += 20; }
            uint32_t hdr[5] = { htonl(1), htonl(0), htonl(2950), htonl(n), htonl(1) }; memcpy(ab, hdr, 20);
            const char *vals[1] = { (const char *)ab }; int lens[1] = { (int)(q - ab) }, fmts[1] = { 1 };
            PGresult *qr = PQexecParams(pg, "SELECT claim, rating, deviation, volatility, matches FROM standing WHERE claim = ANY($1::uuid[])", 1, NULL, vals, lens, fmts, 0);
            if (PQresultStatus(qr) != PGRES_TUPLES_OK) { fprintf(stderr, "standing: %s", PQerrorMessage(pg)); return 1; }
            for (int rr = 0; rr < PQntuples(qr); rr++) {
                lp_id fid; const char *u = PQgetvalue(qr, rr, 0); for (int b = 0, h = 0; b < 16; h++) { if (u[h] == '-') continue; unsigned x; sscanf(u + h, "%2x", &x); fid.b[b++] = (uint8_t)x; h++; }
                uint64_t sidx = hkey(&fid) & (tcap - 1);
                while (table[sidx] && memcmp(&comps[table[sidx] - 1].id, &fid, 16)) sidx = (sidx + 1) & (tcap - 1);
                if (!table[sidx]) continue;
                uint32_t sl = slot[table[sidx] - 1]; st[sl] = (lp_rating){ atof(PQgetvalue(qr, rr, 1)), atof(PQgetvalue(qr, rr, 2)), atof(PQgetvalue(qr, rr, 3)) };
                matches[sl] = (uint32_t)atoi(PQgetvalue(qr, rr, 4)); had[sl] = 1;
            }
            PQclear(qr);
        }
        /* the matchups, first in, first out */
        for (uint64_t i = 0; i < nev; i++) { uint32_t sl = slot[ev[i].claim - LP_NCP]; lp_attest(&st[sl], recipe.trust, ev[i].score, 1500.0, 0.5, 0.0); matches[sl]++; }

        copy_begin(&c, "COPY witness (id, lineage, trust) FROM STDIN (FORMAT binary)");
        for (int fi = 0; fi < nfiles; fi++) {
            if (known[fi] || !trunks[fi]) continue; int used = 0; for (uint64_t i = 0; i < nev && !used; i++) used = ev[i].file == fi;
            if (!used) continue;
            cbe16(&c, 3); cfield(&c, ref_id(trunks[fi])->b, 16); cbe32(&c, 0xFFFFFFFFu); cf_f64(&c, recipe.trust);
        }
        copy_end(&c);
        copy_begin(&c, "COPY attestation (claim, witness, score) FROM STDIN (FORMAT binary)");
        for (uint64_t i = 0; i < nev; i++) {
            float sc = ev[i].score; uint32_t u; memcpy(&u, &sc, 4);
            cbe16(&c, 3); cfield(&c, ref_id(ev[i].claim)->b, 16); cfield(&c, ref_id(trunks[ev[i].file])->b, 16); cbe32(&c, 4); cbe32(&c, u); n_led++;
        }
        copy_end(&c);
        copy_begin(&c, "COPY standing (claim, rating, deviation, volatility, matches) FROM STDIN (FORMAT binary)");
        for (uint64_t k = 0; k < ncomp; k++) {
            uint32_t sl = slot[k]; if (!sl || had[sl]) continue;
            cbe16(&c, 5); cfield(&c, comps[k].id.b, 16); cf_f64(&c, st[sl].rating); cf_f64(&c, st[sl].deviation); cf_f64(&c, st[sl].volatility);
            cbe32(&c, 4); cbe32(&c, matches[sl]); n_std_new++;
        }
        copy_end(&c);
        /* recorded standings: updated in place, one set-based statement per chunk of binary arrays */
        uint64_t *upd = malloc(8 * (nst + 1)), nupd = 0;
        for (uint64_t k = 0; k < ncomp; k++) if (slot[k] && had[slot[k]]) upd[nupd++] = k;
        for (uint64_t i0 = 0; i0 < nupd; i0 += 100000) {
            uint32_t n = (uint32_t)(nupd - i0 < 100000 ? nupd - i0 : 100000);
            static const uint32_t oid[5] = { 2950, 701, 701, 701, 23 }; static const int w[5] = { 16, 8, 8, 8, 4 };
            uint8_t *arr[5]; int alen[5];
            for (int f = 0; f < 5; f++) {
                arr[f] = malloc(20 + (size_t)n * (4 + w[f])); uint8_t *q = arr[f] + 20;
                uint32_t hdr[5] = { htonl(1), htonl(0), htonl(oid[f]), htonl(n), htonl(1) }; memcpy(arr[f], hdr, 20);
                for (uint32_t j = 0; j < n; j++) {
                    uint64_t k = upd[i0 + j]; uint32_t sl = slot[k], l = htonl((uint32_t)w[f]); memcpy(q, &l, 4); q += 4;
                    if (f == 0) memcpy(q, comps[k].id.b, 16);
                    else if (f < 4) { double d = f == 1 ? st[sl].rating : f == 2 ? st[sl].deviation : st[sl].volatility; uint64_t u; memcpy(&u, &d, 8); for (int y = 0; y < 8; y++) q[y] = (uint8_t)(u >> (56 - 8 * y)); }
                    else { uint32_t m = htonl(matches[sl]); memcpy(q, &m, 4); }
                    q += w[f];
                }
                alen[f] = (int)(q - arr[f]);
            }
            const char *vals[5] = { (char *)arr[0], (char *)arr[1], (char *)arr[2], (char *)arr[3], (char *)arr[4] }; int fmts[5] = { 1, 1, 1, 1, 1 };
            PGresult *ur = PQexecParams(pg, "UPDATE standing s SET rating = u.r, deviation = u.d, volatility = u.v, matches = u.m "
                "FROM unnest($1::uuid[], $2::float8[], $3::float8[], $4::float8[], $5::int[]) AS u(c, r, d, v, m) WHERE s.claim = u.c", 5, NULL, vals, alen, fmts, 0);
            if (PQresultStatus(ur) != PGRES_COMMAND_OK) { fprintf(stderr, "standing update: %s", PQerrorMessage(pg)); return 1; }
            PQclear(ur); for (int f = 0; f < 5; f++) free(arr[f]); n_std_upd += n;
        }
        free(upd);
        free(slot); free(st); free(matches); free(had); free(old);
    }
    t_sem = now() - t_sem;

    printf("\n== load (binary COPY into partitioned tables)\n");
    phase("deduplication against the database", t_dedup, (double)checked, "IDs");
    phase("entity", t_ent, (double)ent_rows, "rows"); printf("  %-44s %'8.1f MB sent\n", "", ent_bytes / 1e6);
    phase("physicality", t_phy, (double)phy_rows, "rows"); printf("  %-44s %'8.1f MB sent\n", "", phy_bytes / 1e6);
    phase("entity_stats", t_sta, (double)sta_rows, "rows");
    phase("source", t_src, 0, "");
    if (nev) { phase("attestations, matchups, standings", t_sem, (double)n_led, "attestations");
               printf("  %'llu claims attested from %'llu statements; standings %'llu new, %'llu updated\n", (unsigned long long)(n_std_new + n_std_upd),
                      (unsigned long long)n_claims_attr, (unsigned long long)n_std_new, (unsigned long long)n_std_upd); }
    PQfinish(pg);
    printf("\n== total %.1f s\n", now() - T0);
    return mism ? 1 : 0;
}
