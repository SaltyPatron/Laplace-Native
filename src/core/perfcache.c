/* A perf-cache module (laplace/perfcache.h): one format, one writer, one strict loader and the lookups, for every
 * module above tier 0 that is not the highway. The module supplies its records; nothing here knows what they are. */
#include "laplace/perfcache.h"
#include "internal.h"
#include "blake3.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(lp_section) == 48, "a perf-cache section is 48 bytes");
_Static_assert(sizeof(lp_pair) == 48, "a perf-cache pair is 48 bytes");

#define PC_MAGIC  "LPPC0001"
#define PC_HEADER 128u

typedef struct { lp_perfcache public; const uint8_t *mapping; size_t size; uint8_t digest[32];
                 lp_lock bound_mu; const lp_tier0_record *bound_t0; bool bound; } Mapped;

static uint64_t read64(const uint8_t *p){ uint64_t n = 0; for (unsigned i = 0; i < 8; i++) n |= (uint64_t)p[i] << (8 * i); return n; }
static uint32_t read32(const uint8_t *p){ uint32_t n = 0; for (unsigned i = 0; i < 4; i++) n |= (uint32_t)p[i] << (8 * i); return n; }
static void write64(uint8_t *p, uint64_t n){ for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(n >> (8 * i)); }
static void write32(uint8_t *p, uint32_t n){ for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i)); }
static bool add(size_t *total, size_t count, size_t stride){
    if (stride && count > (SIZE_MAX - *total) / stride) return false;
    *total += count * stride; return true;
}
static bool named(const char *field, size_t cap){ return memchr(field, 0, cap) && field[0]; }
static int pair_cmp(const lp_pair *x, const lp_pair *y){
    int c = memcmp(x->a.b, y->a.b, 16); return c ? c : memcmp(x->b.b, y->b.b, 16);
}
static bool same_record(const lp_tier0_record *x, const lp_tier0_record *y){
    return lp_id_eq(&x->id, &y->id) && !memcmp(x->m, y->m, sizeof x->m) && x->hilbert == y->hilbert && x->pad == y->pad;
}

/* Every invariant of a module's sections, records and pairs, the same for the writer and the loader. */
static bool valid(const lp_section *s, size_t ns, const lp_tier0_record *rec, size_t nrec, const lp_pair *pair, size_t npairs){
    if (ns > UINT32_MAX || nrec > UINT32_MAX || npairs > UINT32_MAX) return false;
    size_t records = 0, pairs = 0; bool pairs_begun = false;
    for (size_t i = 0; i < ns; i++) {
        if (!named(s[i].name, sizeof s[i].name) || s[i].reserved) return false;
        for (size_t j = 0; j < i; j++) if (!strcmp(s[i].name, s[j].name)) return false;
        if (s[i].kind == LP_PERFCACHE_PAIRS) {
            pairs_begun = true;
            if (s[i].first != pairs || s[i].count > npairs - pairs) return false;
            for (uint32_t k = 1; k < s[i].count; k++) if (pair_cmp(pair + pairs + k - 1, pair + pairs + k) >= 0) return false;
            pairs += s[i].count; continue;
        }
        if ((s[i].kind != LP_PERFCACHE_DENSE && s[i].kind != LP_PERFCACHE_SPARSE) || pairs_begun) return false;
        if (s[i].first != records || s[i].count > nrec - records) return false;
        const lp_tier0_record *r = rec + records;
        for (uint32_t k = 0; k < s[i].count; k++) {
            if (r[k].rank != k) return false;
            if (s[i].kind == LP_PERFCACHE_SPARSE && k && memcmp(r[k - 1].id.b, r[k].id.b, 16) >= 0) return false;
        }
        records += s[i].count;
    }
    return records == nrec && pairs == npairs;
}
/* The same ID wherever it stands is the same record (overlap is verified by canonical record parity). */
static bool coherent(const lp_tier0_record *rec, size_t nrec, const lp_idindex *index){
    for (size_t i = 0; i < nrec; i++) {
        uint64_t probe = 0; int64_t first = lp_idindex_find(index, &rec[i].id, &probe);
        if (first < 0 || !same_record(rec + first, rec + i)) return false;
    }
    return true;
}

const char *lp_perfcache_beside_tier0(const char *env, const char *ending, char *out, size_t cap){
    return lp_tier0_sibling(env, ending, out, cap);
}

static void digest_of(const uint8_t *header, const uint8_t *body, size_t bytes, uint8_t out[32]){
    blake3_hasher h; blake3_hasher_init(&h);
    blake3_hasher_update(&h, header, 96); blake3_hasher_update(&h, body, bytes);
    blake3_hasher_finalize(&h, out, 32);
}

bool lp_perfcache_write(const char *path, const char *module, const uint8_t depends[32],
                        const lp_section *sections, size_t nsections,
                        const lp_tier0_record *rec, size_t nrec, const lp_pair *pair, size_t npairs,
                        uint8_t fingerprint[32]){
    if (!path || !module || !*module || strlen(module) >= 16 || !depends || (nsections && !sections)
        || (nrec && !rec) || (npairs && !pair) || nsections > UINT32_MAX) { errno = EINVAL; return false; }
    lp_section *s = calloc(nsections ? nsections : 1, sizeof *s); if (!s) return false;
    uint32_t records = 0, pairs = 0;
    for (size_t i = 0; i < nsections; i++) {
        s[i] = sections[i]; s[i].reserved = 0;
        if (s[i].kind == LP_PERFCACHE_PAIRS) { s[i].first = pairs; pairs += s[i].count; }
        else { s[i].first = records; records += s[i].count; }
    }
    lp_idindex *index = nrec ? lp_idindex_build(rec, nrec, sizeof *rec) : NULL;
    bool ok = valid(s, nsections, rec, nrec, pair, npairs) && (!nrec || (index && coherent(rec, nrec, index)));
    lp_idindex_free(index);
    size_t size = PC_HEADER;
    ok = ok && add(&size, nsections, sizeof *s) && add(&size, nrec, sizeof *rec) && add(&size, npairs, sizeof *pair);
    if (!ok) { free(s); errno = EINVAL; return false; }
    uint8_t *file = malloc(size); if (!file) { free(s); return false; }
    memset(file, 0, PC_HEADER); memcpy(file, PC_MAGIC, 8); memcpy(file + 8, module, strlen(module));
    write32(file + 24, (uint32_t)nsections); write64(file + 32, nrec); write64(file + 40, npairs); write64(file + 48, size);
    memcpy(file + 64, depends, 32);
    uint8_t *at = file + PC_HEADER;
    if (nsections) { memcpy(at, s, nsections * sizeof *s); at += nsections * sizeof *s; }
    if (nrec) { memcpy(at, rec, nrec * sizeof *rec); at += nrec * sizeof *rec; }
    if (npairs) memcpy(at, pair, npairs * sizeof *pair);
    digest_of(file, file + PC_HEADER, size - PC_HEADER, file + 96);
    if (fingerprint) memcpy(fingerprint, file + 96, 32);
    ok = lp_write_atomic(path, file, size);
    free(file); free(s); return ok;
}

static const void *perfcache_open(const char *path){
    size_t size = 0; const uint8_t *m = lp_map_file(path, 0, &size); if (!m) return NULL;
    bool ok = size >= PC_HEADER && !memcmp(m, PC_MAGIC, 8) && named((const char *)m + 8, 16) && !read32(m + 28) && !read64(m + 56);
    uint64_t ns = 0, nr = 0, np = 0; size_t expected = PC_HEADER;
    if (ok) {
        ns = read32(m + 24); nr = read64(m + 32); np = read64(m + 40);
        ok = nr <= UINT32_MAX && np <= UINT32_MAX && add(&expected, (size_t)ns, sizeof(lp_section))
            && add(&expected, (size_t)nr, sizeof(lp_tier0_record)) && add(&expected, (size_t)np, sizeof(lp_pair))
            && expected == size && read64(m + 48) == size;
    }
    uint8_t digest[32];
    if (ok) { digest_of(m, m + PC_HEADER, size - PC_HEADER, digest); ok = !memcmp(digest, m + 96, 32); }
    Mapped *mapped = ok ? calloc(1, sizeof *mapped) : NULL; ok = ok && mapped;
    if (ok) {
        lp_perfcache *p = &mapped->public; mapped->mapping = m; mapped->size = size; memcpy(mapped->digest, digest, 32);
        memcpy(p->module, m + 8, 16); memcpy(p->depends, m + 64, 32);
        p->section = (const lp_section *)(m + PC_HEADER); p->nsections = (size_t)ns;
        p->rec = (const lp_tier0_record *)(p->section + ns); p->nrec = (size_t)nr;
        p->pair = (const lp_pair *)(p->rec + nr); p->npairs = (size_t)np;
        ok = valid(p->section, p->nsections, p->rec, p->nrec, p->pair, p->npairs);
        if (ok && p->nrec) { p->by_id = lp_idindex_build(p->rec, p->nrec, sizeof *p->rec); ok = p->by_id && coherent(p->rec, p->nrec, p->by_id); }
        snprintf(p->path, sizeof p->path, "%s", path);
        mapped->bound_mu = (lp_lock)LP_LOCK_INIT;
    }
    if (!ok) { if (mapped) lp_idindex_free(mapped->public.by_id); free(mapped); lp_unmap_file(m, size); return NULL; }
    return mapped;
}
const lp_perfcache *lp_perfcache_map(const char *path){
    if (!path || !*path) return NULL;
    return lp_cached("perfcache", path, perfcache_open);
}
void lp_perfcache_fingerprint(const lp_perfcache *p, uint8_t out[32]){
    if (!p) { memset(out, 0, 32); return; }
    memcpy(out, ((const Mapped *)p)->digest, 32);
}
bool lp_perfcache_bound(const lp_perfcache *p, const lp_tier0_record *t0){
    if (!p || !t0) return false;
    Mapped *mapped = (Mapped *)p; bool bound;
    lp_lock_take(&mapped->bound_mu);
    if (mapped->bound_t0 != t0) {
        uint8_t fp[32]; lp_tier0_fingerprint(t0, fp);
        mapped->bound = !memcmp(fp, p->depends, 32); mapped->bound_t0 = t0;
    }
    bound = mapped->bound;
    lp_lock_give(&mapped->bound_mu);
    return bound;
}
const lp_section *lp_perfcache_section(const lp_perfcache *p, const char *name){
    if (!p || !name) return NULL;
    for (size_t i = 0; i < p->nsections; i++) if (!strcmp(p->section[i].name, name)) return p->section + i;
    return NULL;
}
static bool owned(const lp_perfcache *p, const lp_section *s){ return p && s && s >= p->section && s < p->section + p->nsections; }
const lp_tier0_record *lp_perfcache_at(const lp_perfcache *p, const lp_section *s, uint32_t slot){
    if (!owned(p, s) || s->kind == LP_PERFCACHE_PAIRS || slot >= s->count) return NULL;
    return p->rec + s->first + slot;
}
const lp_tier0_record *lp_perfcache_find(const lp_perfcache *p, const lp_id *id, const lp_section **section){
    if (section) *section = NULL;
    if (!p || !id || !p->nrec) return NULL;
    uint64_t probe = 0; int64_t at = lp_idindex_find(p->by_id, id, &probe);
    if (at < 0) return NULL;
    if (section) for (size_t i = 0; i < p->nsections; i++) {
        const lp_section *s = p->section + i;
        if (s->kind != LP_PERFCACHE_PAIRS && (uint64_t)at >= s->first && (uint64_t)at < (uint64_t)s->first + s->count) { *section = s; break; }
    }
    return p->rec + at;
}
const lp_pair *lp_perfcache_pair(const lp_perfcache *p, const lp_section *s, const lp_id *a, const lp_id *b){
    if (!owned(p, s) || s->kind != LP_PERFCACHE_PAIRS || !a || !b) return NULL;
    lp_pair key; key.a = *a; key.b = *b;
    const lp_pair *base = p->pair + s->first; size_t lo = 0, hi = s->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2; int c = pair_cmp(base + mid, &key);
        if (!c) return base + mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
