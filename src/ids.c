/* IDs as every reader of the store meets them: a path's constituents in order, and a set or map keyed by ID. One copy
 * for the engine, the extension and every tool, so no reader decodes a path or hashes an ID its own way. */
#include "laplace/laplace.h"
#include <stdlib.h>
#include <string.h>

/* A path's constituents in order, each run written out: vertex i's ID repeated its run length. Returns how many there
 * are; at most cap are written, so a caller with too little room learns how much it needs. */
size_t lp_path_ids(const uint8_t *ewkb, size_t len, lp_id *out, size_t cap){
    const uint8_t *vx; size_t nv = lp_ewkb_vertices(ewkb, len, &vx), n = 0;
    for (size_t i = 0; i < nv; i++) {
        double xyz[3], m; memcpy(xyz, vx + 32 * i, 24); memcpy(&m, vx + 32 * i + 24, 8);
        uint32_t run = lp_m_run(m); lp_id id; if (n < cap) lp_xyz_to_id(xyz, &id);
        for (uint32_t r = 0; r < run; r++, n++) if (n < cap) out[n] = id;
    }
    return n;
}

/* An ID map: open addressing on the ID's own bits (an ID is a hash, so they are already spread), a value per ID. */
struct lp_idmap { lp_id *key; uint32_t *val; uint32_t *slot; size_t n, cap, scap; };
static uint64_t idkey(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 4, 8); return k; }
static void *grow(void *p, size_t n){ p = realloc(p, n ? n : 1); if (!p) abort(); return p; }
static void rehash(lp_idmap *m){
    free(m->slot); m->scap = m->scap ? m->scap * 2 : 1024; m->slot = calloc(m->scap, sizeof *m->slot); if (!m->slot) abort();
    for (size_t i = 0; i < m->n; i++) { size_t k = idkey(&m->key[i]) & (m->scap - 1); while (m->slot[k]) k = (k + 1) & (m->scap - 1); m->slot[k] = (uint32_t)i + 1; }
}
lp_idmap *lp_idmap_new(void){ return calloc(1, sizeof(lp_idmap)); }
void lp_idmap_free(lp_idmap *m){ if (!m) return; free(m->key); free(m->val); free(m->slot); free(m); }
size_t lp_idmap_count(const lp_idmap *m){ return m ? m->n : 0; }
const lp_id *lp_idmap_key(const lp_idmap *m, size_t i){ return &m->key[i]; }
uint32_t *lp_idmap_value(lp_idmap *m, size_t i){ return &m->val[i]; }
int64_t lp_idmap_find(const lp_idmap *m, const lp_id *id){
    if (!m || !m->scap) return -1;
    size_t k = idkey(id) & (m->scap - 1);
    while (m->slot[k]) { uint32_t i = m->slot[k] - 1; if (!memcmp(&m->key[i], id, 16)) return i; k = (k + 1) & (m->scap - 1); }
    return -1;
}
size_t lp_idmap_put(lp_idmap *m, const lp_id *id, bool *fresh){
    if ((m->n + 1) * 2 > m->scap) rehash(m);
    size_t k = idkey(id) & (m->scap - 1);
    while (m->slot[k]) { uint32_t i = m->slot[k] - 1; if (!memcmp(&m->key[i], id, 16)) { if (fresh) *fresh = false; return i; } k = (k + 1) & (m->scap - 1); }
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 1024; m->key = grow(m->key, m->cap * sizeof *m->key); m->val = grow(m->val, m->cap * sizeof *m->val); }
    m->key[m->n] = *id; m->val[m->n] = 0; m->slot[k] = (uint32_t)++m->n; if (fresh) *fresh = true;
    return m->n - 1;
}

/* A path's vertices as they are stored: each one's ID, its run, and what it is said to be (the M bits). Returns how
 * many vertices there are; at most cap are written. */
size_t lp_path_vertices(const uint8_t *ewkb, size_t len, lp_vertex *out, size_t cap){
    const uint8_t *vx; size_t nv = lp_ewkb_vertices(ewkb, len, &vx);
    for (size_t i = 0; i < nv && i < cap; i++) {
        double xyz[3], m; memcpy(xyz, vx + 32 * i, 24); memcpy(&m, vx + 32 * i + 24, 8);
        lp_xyz_to_id(xyz, &out[i].id); out[i].run = lp_m_run(m); out[i].said = lp_m_said(m);
    }
    return nv;
}
