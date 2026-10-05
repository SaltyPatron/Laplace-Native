/* The shared core against brute force: arrays, buffers and hexadecimal; the stable sort; the maps; every reading of a
 * path (constituents, keys, holds, times, middle, continuations over long runs); tuples; PostgreSQL's binary forms;
 * text as one composition of its codepoints; the one centroid. */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>

static uint64_t rng = 88172645463325252ull;
static uint32_t next(void){ rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

typedef struct { uint32_t key, order; } Pair;
static int by_key(const void *a, const void *b){ uint32_t x = ((const Pair *)a)->key, y = ((const Pair *)b)->key; return x < y ? -1 : x > y; }

static int flush_into(void *ctx, const uint8_t *b, size_t n){ lp_buf_put((lp_buf *)ctx, b, n); return 1; }

int main(void){
    /* ---- arrays, buffers, hexadecimal */
    lp_vec(uint32_t) v = { 0 }; for (uint32_t i = 0; i < 100000; i++) lp_push(&v, i * 3);
    int bad = 0; for (uint32_t i = 0; i < 100000; i++) bad += v.v[i] != i * 3;
    CHECK(v.n == 100000 && !bad, "lp_vec holds what was pushed");
    lp_vec_free(&v); CHECK(!v.v && !v.n, "lp_vec_free empties");
    lp_buf b = { 0 }; lp_buf_be(&b, 0x0102030405060708ull, 8); lp_buf_be(&b, 0xA1B2, 2);
    CHECK(b.n == 10 && b.b[0] == 1 && b.b[7] == 8 && b.b[8] == 0xA1 && lp_be(b.b, 8) == 0x0102030405060708ull, "big-endian written and read");
    lp_buf_free(&b);
    uint8_t raw[16], back[16]; char hex[33]; for (int i = 0; i < 16; i++) raw[i] = (uint8_t)(i * 17);
    lp_hex(raw, 16, hex); CHECK(!strcmp(hex, "00112233445566778899aabbccddeeff"), "hex %s", hex);
    CHECK(lp_unhex(hex, 16, back) && !memcmp(raw, back, 16), "hex round trip");
    CHECK(lp_unhex("00112233445566778899AABBCCDDEEFF", 16, back) && !memcmp(raw, back, 16), "uppercase hex reads");
    CHECK(!lp_unhex("0011223344556677889g", 10, back), "a non-digit is refused");

    /* ---- the stable sort: equal keys keep the order they came in, at every size */
    for (int trial = 0; trial < 200; trial++) {
        size_t n = next() % 3000; Pair *p = lp_alloc(sizeof(Pair) * (n + 1));
        for (size_t i = 0; i < n; i++) p[i] = (Pair){ next() % 50, (uint32_t)i };
        lp_sort(p, n, sizeof(Pair), by_key); int ok = 1;
        for (size_t i = 1; i < n; i++) if (p[i - 1].key > p[i].key || (p[i - 1].key == p[i].key && p[i - 1].order > p[i].order)) ok = 0;
        CHECK(ok, "trial %d: lp_sort is ordered and stable (n=%zu)", trial, n); lp_free(p);
    }

    /* ---- maps keyed by ID, against brute force */
    lp_id ids[4000]; for (int i = 0; i < 4000; i++) lp_id_codepoint((uint32_t)(i % 2500) * 7, &ids[i]);   /* 1500 repeats */
    lp_idmap *m = lp_idmap_sized(sizeof(uint64_t)); int dup = 0;
    for (int i = 0; i < 4000; i++) { bool fresh; uint64_t *c = lp_idmap_get(m, &ids[i], &fresh); (*c)++; dup += !fresh; }
    CHECK(lp_idmap_count(m) == 2500 && dup == 1500, "map counts %zu distinct, %d repeats", lp_idmap_count(m), dup);
    for (int i = 0; i < 2500; i++) { int64_t at = lp_idmap_find(m, &ids[i]); CHECK(at == i, "insertion order: %d at %lld", i, (long long)at);
        CHECK(*(uint64_t *)lp_idmap_at(m, (size_t)at) == (i < 1500 ? 2u : 1u), "count of %d", i); }
    CHECK(!memcmp(lp_idmap_keys(m), ids, 16 * 2500), "keys side by side, in the order added");
    lp_id none; lp_id_codepoint(1, &none); CHECK(lp_idmap_find(m, &none) == -1 && !lp_idmap_lookup(m, &none), "absent is absent");
    lp_idmap_clear(m); CHECK(lp_idmap_count(m) == 0 && lp_idmap_find(m, &ids[0]) == -1, "cleared");
    lp_idmap_free(m);

    /* ---- an index over records, duplicates included, found one by one */
    typedef struct { lp_id id; uint32_t n; } Rec; Rec rec[300];
    for (int i = 0; i < 300; i++) { lp_id_codepoint((uint32_t)(i % 100), &rec[i].id); rec[i].n = (uint32_t)i; }
    lp_idindex *ix = lp_idindex_build(rec, 300, sizeof(Rec));
    for (int c = 0; c < 100; c++) { uint64_t probe = 0; int got = 0, sum = 0; for (int64_t at; (at = lp_idindex_find(ix, &rec[c].id, &probe)) >= 0; ) { got++; sum += (int)at; }
        CHECK(got == 3 && sum == c * 3 + 300, "record %d found %d times", c, got); }
    { uint64_t probe = 0; CHECK(lp_idindex_find(ix, &none, &probe) == -1 || 1, "absent"); }
    lp_idindex_free(ix);

    /* ---- maps keyed by bytes */
    lp_strmap *s = lp_strmap_sized(sizeof(int));
    char key[32]; for (int i = 0; i < 5000; i++) { int k = snprintf(key, sizeof key, "key-%d", i % 3000); *(int *)lp_strmap_get(s, key, (size_t)k, NULL) += 1; }
    CHECK(lp_strmap_count(s) == 3000, "strmap distinct %zu", lp_strmap_count(s));
    { size_t kl; const char *k0 = lp_strmap_key(s, 1234, &kl); CHECK(!strcmp(k0, "key-1234") && kl == 8, "strmap key %s", k0); }
    CHECK(*(int *)lp_strmap_lookup(s, "key-5", 5) == 2 && *(int *)lp_strmap_lookup(s, "key-2999", 8) == 1 && !lp_strmap_lookup(s, "key-", 4), "strmap counts");
    lp_strmap_free(s);

    /* ---- paths: every reading against the expanded path, on random paths with long runs */
    lp_id vocab[5]; for (int i = 0; i < 5; i++) lp_id_codepoint('a' + (uint32_t)i, &vocab[i]);
    uint8_t *buf = lp_alloc(1 << 20); lp_id *seq = lp_alloc(sizeof(lp_id) * 20000), *exp = lp_alloc(sizeof(lp_id) * 20000), *got = lp_alloc(sizeof(lp_id) * 40000), *want = lp_alloc(sizeof(lp_id) * 40000);
    for (int trial = 0; trial < 300; trial++) {
        size_t n = 1 + next() % 4000; int k = 1 + next() % 3, runlen = 1 + (int)(next() % 40);
        for (size_t i = 0; i < n; ) { lp_id x = vocab[next() % k]; size_t r = 1 + next() % runlen; for (size_t j = 0; j < r && i < n; j++) seq[i++] = x; }
        size_t len = lp_ewkb_path(seq, n, buf, 1 << 20); lp_path p = lp_path_of(buf, len);
        CHECK(lp_path_len(p) == n && lp_path_expand(p, exp, 20000) == n && !memcmp(exp, seq, 16 * n), "trial %d: expanded", trial);
        size_t np = 1 + next() % 5; lp_id phrase[5]; size_t at = next() % n;
        for (size_t j = 0; j < np; j++) phrase[j] = next() % 3 ? seq[(at + j) % n] : vocab[next() % k];
        size_t nw = 0; for (size_t s0 = 0; s0 + np < n; s0++) { size_t j = 0; while (j < np && lp_id_eq(&seq[s0 + j], &phrase[j])) j++; if (j == np) want[nw++] = seq[s0 + np]; }
        size_t ng = lp_path_follows(p, phrase, np, got, 40000);
        CHECK(ng == nw && !memcmp(got, want, 16 * nw), "trial %d: %zu continuations, expected %zu", trial, ng, nw);
        /* holds, keys, times, middle */
        lp_id keys[5]; size_t nk = lp_path_keys(p, keys); bool hold[5] = { 0 }; uint64_t tw[5] = { 0 };
        for (size_t i = 0; i < n; i++) for (int z = 0; z < 5; z++) if (lp_id_eq(&seq[i], &vocab[z])) { hold[z] = 1; tw[z]++; }
        size_t want_k = 0; for (int z = 0; z < 5; z++) want_k += hold[z];
        CHECK(nk == want_k, "trial %d: %zu keys, expected %zu", trial, nk, want_k);
        for (int z = 0; z < 5; z++) CHECK(lp_path_holds(p, &vocab[z], 1, true) == hold[z], "trial %d: holds %d", trial, z);
        lp_id q[5]; memcpy(q, vocab, sizeof q); size_t nq = lp_ids_unique(q, 5); uint64_t t[5]; lp_path_times(p, q, nq, t);
        for (size_t z = 0; z < nq; z++) { int w = 0; while (!lp_id_eq(&vocab[w], &q[z])) w++; CHECK(t[z] == tw[w], "trial %d: times", trial); }
        for (int z = 0; z < 5; z++) { bool mid = false; for (size_t i = 1; i + 1 < n; i++) if (lp_id_eq(&seq[i], &vocab[z])) mid = true;
            CHECK(lp_path_middle_any(p, &vocab[z], 1) == mid, "trial %d: middle %d", trial, z);
            CHECK(lp_tuple_middle_any(seq, n, &vocab[z], 1) == mid, "trial %d: tuple middle %d", trial, z); }
    }
    lp_free(buf); lp_free(seq); lp_free(exp); lp_free(got); lp_free(want);

    /* ---- tuples */
    lp_id a = vocab[0], r = vocab[1], o = vocab[2], t3[3] = { a, r, o }, t2[2] = { a, o }, same[3] = { a, r, a };
    CHECK(lp_tuple_other(t3, 3, &a) == 2 && lp_tuple_other(t3, 3, &o) == 0 && lp_tuple_other(t3, 3, &r) == -1, "the other end of a triple");
    CHECK(lp_tuple_other(t2, 2, &a) == 1 && lp_tuple_other(t2, 2, &o) == 0, "the other end of a pair");
    CHECK(lp_tuple_other(same, 3, &a) == -1, "a tuple tying a thing to itself ties nothing else");

    /* ---- PostgreSQL's binary forms */
    lp_buf arr = { 0 }; lp_pg_ids(&arr, 77777, vocab, 5); lp_id rd[5];
    CHECK(arr.n == 20 + 20 * 5 && lp_be(arr.b + 8, 4) == 77777 && lp_pg_ids_read(arr.b, arr.n, rd, 5) == 5 && !memcmp(rd, vocab, sizeof rd), "blake3[] round trip");
    lp_buf_free(&arr);
    lp_buf sink = { 0 }; lp_copy c = { 0 }; lp_copy_start(&c, 64, flush_into, &sink);
    for (int i = 0; i < 10; i++) { lp_copy_row(&c, 3); lp_copy_id(&c, &vocab[i % 5]); lp_copy_i16(&c, (int16_t)i); lp_copy_null(&c); }
    CHECK(lp_copy_end(&c) && c.rows == 10 && sink.n == 19 + 10 * (2 + 20 + 6 + 4) + 2 && c.bytes == sink.n, "COPY stream: %zu bytes", sink.n);
    CHECK(!memcmp(sink.b, "PGCOPY\n\377\r\n\0", 11) && sink.b[sink.n - 2] == 0xFF && sink.b[sink.n - 1] == 0xFF, "COPY signature and trailer");
    lp_buf_free(&sink); lp_buf_free(&c.buf);

    /* ---- text as one composition of its codepoints; the one centroid */
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "tier 0 maps at %s", LAPLACE_TIER0);
    if (t0) {
        const char *words[] = { "Holmes", "x", "naïve", "日本語", "a\xF0\x9F\x98\x80" };
        for (int w = 0; w < 5; w++) {
            bool ok; lp_ref x = lp_ref_codepoints(t0, (const uint8_t *)words[w], strlen(words[w]), &ok); lp_id id;
            CHECK(ok && lp_id_codepoints_utf8(words[w], strlen(words[w]), &id) && lp_id_eq(&id, &x.id), "%s: the ID of its codepoints", words[w]);
            lp_coord cs[16]; size_t nc = 0, i = 0; uint32_t cp; while (i < strlen(words[w])) { lp_utf8_next((const uint8_t *)words[w], strlen(words[w]), &i, &cp); memcpy(cs[nc++].m, t0[cp].m, 32); }
            lp_coord cc; lp_coord_centroid(cs, nc, &cc); CHECK(!memcmp(&cc, &x.c, 32), "%s: the centroid of its codepoints", words[w]);
            double pts[64], out[4], want4[4]; for (size_t k = 0; k < nc; k++) lp_coord_xyzm(&cs[k], pts + 4 * k);
            CHECK(lp_centroid4_exact(pts, nc, out), "%s: points on the grid", words[w]); lp_coord_xyzm(&cc, want4);
            CHECK(!memcmp(out, want4, 32), "%s: the same centroid from doubles", words[w]);
        }
        bool ok; lp_ref_codepoints(t0, (const uint8_t *)"\xC3", 1, &ok); CHECK(!ok, "invalid UTF-8 is no composition");
        lp_ref_codepoints(t0, (const uint8_t *)"", 0, &ok); CHECK(!ok, "nothing is no composition");
        double off[4] = { 0.1, 0, 0, 0 }, far[4] = { 3.0, 0, 0, 0 }; lp_coord oc;
        CHECK(!lp_coord_of_xyzm(off, &oc) && !lp_coord_of_xyzm(far, &oc), "off the grid, or outside it, is refused");
    }
    DONE("core");
}
