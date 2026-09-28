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

#define NT 6
typedef struct { uint64_t ref; uint32_t run; } Vtx;                 /* ref < NCP: codepoint; else NCP + composition */
typedef struct { lp_id id; int64_t m[4]; uint64_t vstart; uint32_t vcount, len; uint8_t tier; } Comp;

static const lp_tier0_record *t0;
static Comp *comps; static uint64_t ncomp, capcomp;
static Vtx *verts; static uint64_t nvert, capvert;
static uint64_t *table; static uint64_t tcap;
static uint64_t st_new[NT], st_hit[NT], st_mismatch;
static double T0;

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
static void bounds_of(UBreakIteratorType ty, UText *ut, Bounds *out){
    UErrorCode e = U_ZERO_ERROR; UBreakIterator *bi = ubrk_open(ty, "", NULL, 0, &e);
    ubrk_setUText(bi, ut, &e); if (U_FAILURE(e)) { fprintf(stderr, "ICU: %s\n", u_errorName(e)); exit(1); }
    out->n = 0;
    for (int32_t p = ubrk_first(bi); p != UBRK_DONE; p = ubrk_next(bi)) {
        if (out->n == out->cap) { out->cap = out->cap ? out->cap * 2 : 4096; out->b = xrealloc(out->b, out->cap * 4); }
        out->b[out->n++] = p;
    }
    ubrk_close(bi);
}
typedef struct { uint64_t *v; size_t n, cap; } Refs;
static void push(Refs *r, uint64_t x){ if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 256; r->v = xrealloc(r->v, r->cap * 8); } r->v[r->n++] = x; }

static uint8_t *rbuf; static size_t rlen, rcap;
static void expand(uint64_t r){
    if (r < LP_NCP) { if (rlen + 4 > rcap) { rcap = rcap ? rcap * 2 : (1u << 24); rbuf = xrealloc(rbuf, rcap); } rlen += utf8_put((uint32_t)r, rbuf + rlen); return; }
    const Comp *c = &comps[r - LP_NCP];
    for (uint32_t v = 0; v < c->vcount; v++) for (uint32_t k = 0; k < verts[c->vstart + v].run; k++) expand(verts[c->vstart + v].ref);
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
static uint64_t bytes_in; static double last_tick;
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
    int exact = 0, mism = 0, skipped = 0;
    Bounds sb = {0}, wb = {0}, gb = {0}; Refs segs = {0}, sents = {0}, paras = {0}, g = {0}, cps = {0};
    for (int fi = 0; fi < nfiles; fi++) {
        const char *fn = argv[a + fi];
        if (known[fi]) { tick(fi + 1, nfiles, 0); continue; }
        FILE *f = fopen(fn, "rb");
        if (!f) { perror(fn); skipped++; continue; }
        fseek(f, 0, SEEK_END); size_t n = ftell(f); rewind(f);
        uint8_t *src = malloc(n + 1), *cpy = malloc(n + 1);
        if (fread(src, 1, n, f) != n) { perror(fn); return 1; }
        fclose(f); bytes_in += n; fbytes[fi] = n;
        int ok = 1; for (size_t i = 0; i < n && ok; ) { uint32_t cp; ok = utf8_next(src, n, &i, &cp); }
        if (!ok) { fprintf(stderr, "\n  %s: invalid UTF-8, skipped\n", fn); skipped++; free(src); free(cpy); continue; }
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
        trunks[fi] = node(paras.v, paras.n, 5);
        rlen = 0; expand(trunks[fi]);
        if (rlen == n && !memcmp(rbuf, src, n)) exact++; else mism++;
        if (!load) { blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, src, n); blake3_hasher_finalize(&h, fsha[fi], 32); }
        utext_close(ut); free(src); free(cpy);
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

    printf("\n== decomposition: %d files, %.1f MB, recomposed byte for byte %d, mismatched %d, skipped %d, ID collisions %llu\n",
           nfiles, bytes_in / 1e6, exact, mism, skipped, (unsigned long long)st_mismatch);
    static const char *tn[NT] = { "codepoint", "grapheme", "word segment", "sentence", "paragraph", "file" };
    printf("   %-13s %14s %16s\n", "tier", "distinct", "reused");
    for (int k = 1; k < NT; k++) printf("   %-13s %'14llu %'16llu\n", tn[k], (unsigned long long)st_new[k], (unsigned long long)st_hit[k]);
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

    /* ---- deduplication against what is recorded: trunk to leaf, one set-based query per tier per round.
     * A node found means its whole subtree is recorded, so nothing below it is checked or sent. */
    t = now();
    uint8_t *keep = calloc(LP_NCP + ncomp, 1);                       /* 1: new, to be written */
    uint8_t *seen = calloc(LP_NCP + ncomp, 1);
    uint64_t *front = malloc(8 * (ncomp + 1)), *nextf = malloc(8 * (ncomp + 1)), nf = 0, nn;
    uint64_t checked = 0, found = 0; int rounds = 0;
    for (int fi = 0; fi < nfiles; fi++) { if (known[fi]) continue; uint64_t tr = trunks[fi]; if (tr >= LP_NCP && !seen[tr]) { seen[tr] = 1; front[nf++] = tr; } }
    size_t abcap = 0; uint8_t *ab = NULL;
    printf("\n== deduplication, trunk to leaf\n");
    while (nf) {
        rounds++; nn = 0;
        for (int tier = NT - 1; tier >= 1; tier--) {
            uint64_t cnt = 0; for (uint64_t i = 0; i < nf; i++) cnt += comps[front[i] - LP_NCP].tier == tier;
            if (!cnt) continue;
            double tq = now(); uint64_t hit = 0;
            for (uint64_t i0 = 0; i0 < nf; ) {                       /* chunks of up to 200k IDs as one binary uuid[] */
                size_t need = 20 + 20 * 200000; if (need > abcap) { abcap = need; ab = xrealloc(ab, abcap); }
                uint8_t *q = ab + 20; uint32_t n = 0; uint64_t i = i0;
                for (; i < nf && n < 200000; i++) {
                    const Comp *cm = &comps[front[i] - LP_NCP]; if (cm->tier != tier) continue;
                    uint32_t l = htonl(16); memcpy(q, &l, 4); memcpy(q + 4, cm->id.b, 16); q += 20; n++;
                }
                i0 = i; if (!n) continue;
                uint32_t hdr[5] = { htonl(1), htonl(0), htonl(2950), htonl(n), htonl(1) }; memcpy(ab, hdr, 20);
                char tb[8]; snprintf(tb, sizeof tb, "%d", tier);
                const char *vals[2] = { (const char *)ab, tb }; int lens[2] = { (int)(q - ab), 0 }, fmts[2] = { 1, 0 };
                PGresult *qr = PQexecParams(pg, "SELECT id FROM entity WHERE tier = $2::smallint AND id = ANY($1::uuid[])", 2, NULL, vals, lens, fmts, 1);
                if (PQresultStatus(qr) != PGRES_TUPLES_OK) { fprintf(stderr, "dedup: %s", PQerrorMessage(pg)); return 1; }
                for (int rr = 0; rr < PQntuples(qr); rr++) {           /* found: mark by looking the ID up in the table */
                    lp_id fid; memcpy(fid.b, PQgetvalue(qr, rr, 0), 16);
                    uint64_t sidx = hkey(&fid) & (tcap - 1);
                    while (table[sidx] && memcmp(&comps[table[sidx] - 1].id, &fid, 16)) sidx = (sidx + 1) & (tcap - 1);
                    if (table[sidx]) { keep[LP_NCP + table[sidx] - 1] = 2; hit++; }
                }
                PQclear(qr); checked += n;
            }
            found += hit;
            printf("  round %d  tier %d  %'10llu candidates  %'10llu recorded  %'10llu new   %8.1f ms\n", rounds, tier,
                   (unsigned long long)cnt, (unsigned long long)hit, (unsigned long long)(cnt - hit), (now() - tq) * 1000);
        }
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
        const char *fn = argv[a + fi], *fmt = "text/plain; charset=utf-8";
        cbe16(&c, 5); cfield(&c, ref_id(trunks[fi])->b, 16); cfield(&c, fn, (uint32_t)strlen(fn)); cfield(&c, fmt, (uint32_t)strlen(fmt));
        cf_i64(&c, (int64_t)fbytes[fi]); cfield(&c, fsha[fi], 32);
    }
    copy_end(&c); double t_src = now() - t;

    printf("\n== load (binary COPY into partitioned tables)\n");
    phase("deduplication against the database", t_dedup, (double)checked, "IDs");
    phase("entity", t_ent, (double)ent_rows, "rows"); printf("  %-44s %'8.1f MB sent\n", "", ent_bytes / 1e6);
    phase("physicality", t_phy, (double)phy_rows, "rows"); printf("  %-44s %'8.1f MB sent\n", "", phy_bytes / 1e6);
    phase("entity_stats", t_sta, (double)sta_rows, "rows");
    phase("source", t_src, 0, "");
    PQfinish(pg);
    printf("\n== total %.1f s\n", now() - T0);
    return mism ? 1 : 0;
}
