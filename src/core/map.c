/* Maps: one open-addressing table, linear probing, slots holding an entry's index + 1, the table at most half full.
 * Keyed by ID (an ID is a hash: its bytes 8 to 15 are the hash, which nothing shards or partitions by), by bytes
 * (hashed with FNV-1a), or over records that already hold their IDs. Entries keep the order they were added in. */
#include "laplace/laplace.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>

static inline uint64_t id_hash(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 8, 8); return k; }

/* A map is working memory unless it lasts as long as the process (a perf-cache's keys): then it is the C library's. */
static void *mem(bool lasting, void *p, size_t n){ if (!lasting) return lp_realloc(p, n); void *q = realloc(p, n ? n : 1); if (!q) abort(); return q; }
static void mem_free(bool lasting, void *p){ if (lasting) free(p); else lp_free(p); }
static void *grow(bool lasting, void *p, size_t *cap, size_t need, size_t size){
    if (need <= *cap) return p;
    size_t c = *cap ? *cap : 16; while (c < need) c *= 2; *cap = c;
    return mem(lasting, p, c * size);
}

/* The slots: grown and rebuilt, over the n entries there are, when one more would fill more than half of them. hash(i)
 * gives entry i's hash. */
typedef struct { uint32_t *slot; size_t cap; bool lasting; } Slots;
static void slots_rebuild(Slots *s, size_t n, uint64_t (*hash)(const void *, size_t), const void *ctx){
    mem_free(s->lasting, s->slot); s->cap = s->cap ? s->cap * 2 : 1024; while (s->cap < (n + 1) * 2) s->cap *= 2;
    s->slot = mem(s->lasting, NULL, s->cap * sizeof *s->slot); memset(s->slot, 0, s->cap * sizeof *s->slot);
    for (size_t i = 0; i < n; i++) { size_t k = hash(ctx, i) & (s->cap - 1); while (s->slot[k]) k = (k + 1) & (s->cap - 1); s->slot[k] = (uint32_t)i + 1; }
}

/* ---- by ID */
struct lp_idmap { lp_id *key; uint8_t *val; size_t vsize, n, cap; Slots s; };
static uint64_t idmap_hash(const void *m, size_t i){ return id_hash(&((const lp_idmap *)m)->key[i]); }

lp_idmap *lp_idmap_sized(size_t value_bytes){ lp_idmap *m = lp_zalloc(sizeof *m); m->vsize = value_bytes; return m; }
lp_idmap *lp_idmap_new(void){ return lp_idmap_sized(sizeof(uint32_t)); }
void lp_idmap_free(lp_idmap *m){ if (!m) return; lp_free(m->key); lp_free(m->val); lp_free(m->s.slot); lp_free(m); }
void lp_idmap_clear(lp_idmap *m){ m->n = 0; if (m->s.slot) memset(m->s.slot, 0, m->s.cap * sizeof *m->s.slot); }
size_t lp_idmap_count(const lp_idmap *m){ return m ? m->n : 0; }
const lp_id *lp_idmap_key(const lp_idmap *m, size_t i){ return &m->key[i]; }
const lp_id *lp_idmap_keys(const lp_idmap *m){ return m->key; }
void *lp_idmap_at(lp_idmap *m, size_t i){ return m->val + i * m->vsize; }
uint32_t *lp_idmap_value(lp_idmap *m, size_t i){ return (uint32_t *)lp_idmap_at(m, i); }

int64_t lp_idmap_find(const lp_idmap *m, const lp_id *id){
    if (!m || !m->s.cap) return -1;
    size_t k = id_hash(id) & (m->s.cap - 1);
    for (uint32_t x; (x = m->s.slot[k]); k = (k + 1) & (m->s.cap - 1)) if (lp_id_eq(&m->key[x - 1], id)) return x - 1;
    return -1;
}
size_t lp_idmap_put(lp_idmap *m, const lp_id *id, bool *fresh){
    if ((m->n + 1) * 2 > m->s.cap) slots_rebuild(&m->s, m->n, idmap_hash, m);
    size_t k = id_hash(id) & (m->s.cap - 1);
    for (uint32_t x; (x = m->s.slot[k]); k = (k + 1) & (m->s.cap - 1)) if (lp_id_eq(&m->key[x - 1], id)) { if (fresh) *fresh = false; return x - 1; }
    if (m->n == m->cap) { size_t c = m->cap; m->key = grow(false, m->key, &c, m->n + 1, sizeof *m->key); m->val = mem(false, m->val, c * (m->vsize ? m->vsize : 1)); m->cap = c; }
    m->key[m->n] = *id; memset(m->val + m->n * m->vsize, 0, m->vsize); m->s.slot[k] = (uint32_t)++m->n;
    if (fresh) *fresh = true;
    return m->n - 1;
}

/* ---- over records that hold their IDs: built once, for the life of the process (plain malloc, never working memory) */
struct lp_idindex { const uint8_t *rec; size_t n, stride, cap; uint32_t *slot; };
lp_idindex *lp_idindex_build(const void *records, size_t n, size_t stride){
    lp_idindex *x = calloc(1, sizeof *x); if (!x) return NULL;
    x->rec = records; x->n = n; x->stride = stride; x->cap = 1024; while (x->cap < n * 2 + 2) x->cap *= 2;
    x->slot = calloc(x->cap, sizeof *x->slot); if (!x->slot) { free(x); return NULL; }
    for (size_t i = 0; i < n; i++) {
        size_t k = id_hash((const lp_id *)(x->rec + i * stride)) & (x->cap - 1);
        while (x->slot[k]) k = (k + 1) & (x->cap - 1);
        x->slot[k] = (uint32_t)i + 1;
    }
    return x;
}
void lp_idindex_free(lp_idindex *x){ if (!x) return; free(x->slot); free(x); }
int64_t lp_idindex_find(const lp_idindex *x, const lp_id *id, uint64_t *probe){
    if (!x) return -1;
    size_t k = (id_hash(id) + *probe) & (x->cap - 1);
    for (uint32_t s; (s = x->slot[k]); k = (k + 1) & (x->cap - 1), (*probe)++)
        if (lp_id_eq((const lp_id *)(x->rec + (size_t)(s - 1) * x->stride), id)) { (*probe)++; return s - 1; }
    return -1;
}

/* ---- by bytes */
uint64_t lp_hash_bytes(const void *p, size_t n){
    const uint8_t *s = (const uint8_t *)p; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ s[i]) * 1099511628211ull;
    return h;
}
typedef struct { size_t off, len; uint64_t h; } Key;
struct lp_strmap { Key *key; uint8_t *val; size_t vsize, n, cap; uint8_t *pool; size_t pn, pcap; Slots s; };
static uint64_t strmap_hash(const void *m, size_t i){ return ((const lp_strmap *)m)->key[i].h; }

lp_strmap *lp_strmap_sized(size_t value_bytes){ lp_strmap *m = lp_zalloc(sizeof *m); m->vsize = value_bytes; return m; }
lp_strmap *lp_strmap_lasting(size_t value_bytes){ lp_strmap *m = calloc(1, sizeof *m); if (!m) abort(); m->vsize = value_bytes; m->s.lasting = true; return m; }
void lp_strmap_free(lp_strmap *m){ if (!m) return; bool l = m->s.lasting; mem_free(l, m->key); mem_free(l, m->val); mem_free(l, m->pool); mem_free(l, m->s.slot); mem_free(l, m); }
size_t lp_strmap_count(const lp_strmap *m){ return m ? m->n : 0; }
const char *lp_strmap_key(const lp_strmap *m, size_t i, size_t *len){ if (len) *len = m->key[i].len; return (const char *)m->pool + m->key[i].off; }
void *lp_strmap_at(lp_strmap *m, size_t i){ return m->val + i * m->vsize; }

static int64_t strmap_probe(const lp_strmap *m, const void *k, size_t len, uint64_t h, size_t *at){
    size_t s = h & (m->s.cap - 1);
    for (uint32_t x; (x = m->s.slot[s]); s = (s + 1) & (m->s.cap - 1)) {
        const Key *e = &m->key[x - 1];
        if (e->h == h && e->len == len && !memcmp(m->pool + e->off, k, len)) return x - 1;
    }
    if (at) *at = s;
    return -1;
}
int64_t lp_strmap_find(const lp_strmap *m, const void *k, size_t len){
    if (!m || !m->s.cap) return -1;
    return strmap_probe(m, k, len, lp_hash_bytes(k, len), NULL);
}
size_t lp_strmap_put(lp_strmap *m, const void *k, size_t len, bool *fresh){
    if ((m->n + 1) * 2 > m->s.cap) slots_rebuild(&m->s, m->n, strmap_hash, m);
    uint64_t h = lp_hash_bytes(k, len); size_t at = 0; int64_t i = strmap_probe(m, k, len, h, &at);
    if (i >= 0) { if (fresh) *fresh = false; return (size_t)i; }
    bool l = m->s.lasting;
    if (m->n == m->cap) { size_t c = m->cap; m->key = grow(l, m->key, &c, m->n + 1, sizeof *m->key); m->val = mem(l, m->val, c * (m->vsize ? m->vsize : 1)); m->cap = c; }
    size_t off = m->pn; m->pool = grow(l, m->pool, &m->pcap, m->pn + len + 1, 1); memcpy(m->pool + off, k, len); m->pool[off + len] = 0; m->pn += len + 1;
    m->key[m->n] = (Key){ off, len, h }; memset(m->val + m->n * m->vsize, 0, m->vsize); m->s.slot[at] = (uint32_t)++m->n;
    if (fresh) *fresh = true;
    return m->n - 1;
}
