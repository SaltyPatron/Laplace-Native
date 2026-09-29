/* UAX #29 text: codepoint -> grapheme -> word segment -> sentence -> paragraph -> text. Nothing is dropped: whitespace
 * and punctuation are constituents like everything else, so the text recomposes byte for byte. A single line break
 * inside a paragraph is read as a space for segmentation only; the bytes recorded are the original ones.
 *
 * This is the one decomposition of text in Laplace. The engine records what it composes; the database composes without
 * recording, to compute the ID and coordinate of a text in place. */
#include "laplace/laplace.h"
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int32_t *b; size_t n, cap; } Bounds;
typedef struct { lp_ref *v; size_t n, cap; } Refs;
struct lp_text {
    const lp_tier0_record *t0;
    UBreakIterator *brk[3];                          /* sentence, word, character: opened once */
    Bounds sb, wb, gb;
    Refs cps, g, segs, sents, paras;
    uint8_t *cpy; size_t ccap;
    lp_compose_fn compose; void *sink;
    lp_ref *parts; size_t pcap, pn; lp_id parts_of; int want_parts;   /* the constituents of the last composition made */
};

static void *grow(void *p, size_t n){ p = realloc(p, n ? n : 1); if (!p) abort(); return p; }
static void push(Refs *r, lp_ref x){ if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 256; r->v = grow(r->v, r->cap * sizeof(lp_ref)); } r->v[r->n++] = x; }

lp_text *lp_text_new(const lp_tier0_record *t0){
    lp_text *c = calloc(1, sizeof *c); if (!c) return NULL;
    UErrorCode e = U_ZERO_ERROR; c->t0 = t0;
    c->brk[0] = ubrk_open(UBRK_SENTENCE, "", NULL, 0, &e);
    c->brk[1] = ubrk_open(UBRK_WORD, "", NULL, 0, &e);
    c->brk[2] = ubrk_open(UBRK_CHARACTER, "", NULL, 0, &e);
    if (U_FAILURE(e)) { lp_text_free(c); return NULL; }
    return c;
}
void lp_text_free(lp_text *c){
    if (!c) return;
    for (int i = 0; i < 3; i++) if (c->brk[i]) ubrk_close(c->brk[i]);
    free(c->sb.b); free(c->wb.b); free(c->gb.b); free(c->cps.v); free(c->g.v); free(c->segs.v); free(c->sents.v); free(c->paras.v);
    free(c->cpy); free(c->parts); free(c);
}
static void bounds_of(UBreakIterator *bi, UText *ut, Bounds *out){
    UErrorCode e = U_ZERO_ERROR; ubrk_setUText(bi, ut, &e); out->n = 0;
    for (int32_t p = ubrk_first(bi); p != UBRK_DONE; p = ubrk_next(bi)) {
        if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 1024; out->b = grow(out->b, out->cap * 4); }
        out->b[out->n++] = p;
    }
}

static lp_ref made(lp_text *c, const lp_ref *ch, size_t n, uint8_t tier){
    if (n == 1) return ch[0];
    lp_ref r = c->compose ? c->compose(c->sink, ch, (uint32_t)n, tier) : lp_ref_compose(ch, n, tier);
    if (c->want_parts) {                            /* the trunk is the last composition of more than one child */
        if (n > c->pcap) { c->pcap = n * 2; c->parts = grow(c->parts, c->pcap * sizeof(lp_ref)); }
        memcpy(c->parts, ch, n * sizeof(lp_ref)); c->pn = n; c->parts_of = r.id;
    }
    return r;
}

lp_ref lp_text_decompose(lp_text *c, const uint8_t *src, size_t n, lp_compose_fn compose, void *sink){
    c->compose = compose; c->sink = sink;
    if (n + 1 > c->ccap) { c->ccap = (n + 1) * 2; c->cpy = grow(c->cpy, c->ccap); }
    uint8_t *cpy = c->cpy; memcpy(cpy, src, n);
    for (size_t i = 0; i < n; ) {                                   /* a lone line break inside a paragraph: a space */
        if (src[i] != '\n' && src[i] != '\r') { i++; continue; }
        size_t s0 = i, e0 = i + ((src[i] == '\r' && i + 1 < n && src[i + 1] == '\n') ? 2 : 1), j = e0;
        while (j < n && (src[j] == ' ' || src[j] == '\t')) j++;
        int next_blank = (j < n && (src[j] == '\n' || src[j] == '\r'));
        size_t k = s0; while (k > 0 && (src[k - 1] == ' ' || src[k - 1] == '\t')) k--;
        int prev_blank = (k == 0 || src[k - 1] == '\n' || src[k - 1] == '\r');
        if (!next_blank && !prev_blank) for (size_t z = s0; z < e0; z++) cpy[z] = ' ';
        i = e0;
    }
    UErrorCode e = U_ZERO_ERROR; UText *ut = utext_openUTF8(NULL, (const char *)cpy, (int64_t)n, &e);
    bounds_of(c->brk[0], ut, &c->sb); bounds_of(c->brk[1], ut, &c->wb); bounds_of(c->brk[2], ut, &c->gb);
    c->paras.n = 0; c->sents.n = 0; size_t wi = 0, gi = 0;
    for (size_t si = 0; si + 1 < c->sb.n; si++) {
        int32_t s0 = c->sb.b[si], s1 = c->sb.b[si + 1], w0 = s0; c->segs.n = 0;
        while (w0 < s1) {
            while (wi < c->wb.n && c->wb.b[wi] <= w0) wi++;
            int32_t w1 = (wi < c->wb.n && c->wb.b[wi] < s1) ? c->wb.b[wi] : s1, g0 = w0; c->g.n = 0;
            while (g0 < w1) {
                while (gi < c->gb.n && c->gb.b[gi] <= g0) gi++;
                int32_t g1 = (gi < c->gb.n && c->gb.b[gi] < w1) ? c->gb.b[gi] : w1; size_t i = (size_t)g0; uint32_t cp; c->cps.n = 0;
                while (i < (size_t)g1) { if (!lp_utf8_next(src, n, &i, &cp)) { cp = 0xFFFD; i++; } push(&c->cps, lp_ref_atom(c->t0, cp)); }
                push(&c->g, made(c, c->cps.v, c->cps.n, 1)); g0 = g1;
            }
            push(&c->segs, made(c, c->g.v, c->g.n, 2)); w0 = w1;
        }
        push(&c->sents, made(c, c->segs.v, c->segs.n, 3));
        int end_para = (si + 2 == c->sb.n);
        for (int32_t z = s1 - 1; z >= s0 && !end_para; z--) {
            if (src[z] != '\n' && src[z] != '\r' && src[z] != ' ' && src[z] != '\t') break;
            if ((src[z] == '\n' || src[z] == '\r') && cpy[z] != ' ') end_para = 1;
        }
        if (end_para) { push(&c->paras, made(c, c->sents.v, c->sents.n, 4)); c->sents.n = 0; }
    }
    if (c->sents.n) push(&c->paras, made(c, c->sents.v, c->sents.n, 4));
    utext_close(ut);
    return made(c, c->paras.v, c->paras.n, 5);
}

lp_ref lp_text_parts(lp_text *c, const uint8_t *s, size_t n, lp_ref *parts, size_t cap, size_t *nparts){
    if (!c->parts) { c->pcap = 64; c->parts = grow(NULL, c->pcap * sizeof(lp_ref)); }
    c->pn = 0; c->want_parts = 1;
    lp_ref trunk = lp_text_decompose(c, s, n, NULL, NULL);
    c->want_parts = 0;
    if (!c->pn || memcmp(c->parts_of.b, trunk.id.b, 16)) { c->parts[0] = trunk; c->pn = 1; }     /* one codepoint */
    for (size_t i = 0; i < c->pn && i < cap; i++) parts[i] = c->parts[i];
    *nparts = c->pn;
    return trunk;
}
