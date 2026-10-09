/* Paths as every reader of the store meets them: a vertex block, read where it lies. Each vertex is 32 bytes, X, Y, Z
 * (a constituent's ID, packed into their mantissas) and M (the run, and above it what the vertex is said to be). One
 * copy of every reading of a path, for the engine, the extension and every tool, so no reader decodes a path its own
 * way. And tuples: what a claim's parts are. */
#include "laplace/laplace.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>

lp_path lp_path_of(const uint8_t *ewkb, size_t len){ lp_path p = { NULL, 0 }; p.n = lp_ewkb_vertices(ewkb, len, &p.v); return p; }

static inline double m_of(lp_path p, size_t i){ double m; memcpy(&m, p.v + i * LP_VERTEX_BYTES + 24, 8); return m; }
lp_id lp_path_id(lp_path p, size_t i){ double xyz[3]; lp_id id; memcpy(xyz, p.v + i * LP_VERTEX_BYTES, 24); lp_xyz_to_id(xyz, &id); return id; }
uint32_t lp_path_run(lp_path p, size_t i){ return lp_m_run(m_of(p, i)); }
size_t lp_path_len(lp_path p){ size_t n = 0; for (size_t i = 0; i < p.n; i++) n += lp_path_run(p, i); return n; }

size_t lp_path_expand(lp_path p, lp_id *out, size_t cap){
    size_t n = 0;
    for (size_t i = 0; i < p.n; i++) {
        uint32_t run = lp_path_run(p, i);
        if (n < cap) { lp_id id = lp_path_id(p, i); for (uint32_t r = 0; r < run && n + r < cap; r++) out[n + r] = id; }
        n += run;
    }
    return n;
}
size_t lp_path_decode(lp_path p, lp_vertex *out, size_t cap){
    for (size_t i = 0; i < p.n && i < cap; i++) {
        double xyz[3], m = m_of(p, i); memcpy(xyz, p.v + i * LP_VERTEX_BYTES, 24);
        lp_xyz_to_id(xyz, &out[i].id); out[i].run = lp_m_run(m); out[i].said = lp_m_said(m); out[i].spare = lp_xyz_spare(xyz);
        out[i].outcome = lp_m_outcome(m); out[i].position = lp_m_position(m);
    }
    return p.n;
}
size_t lp_path_ids(const uint8_t *ewkb, size_t len, lp_id *out, size_t cap){ return lp_path_expand(lp_path_of(ewkb, len), out, cap); }
size_t lp_path_vertices(const uint8_t *ewkb, size_t len, lp_vertex *out, size_t cap){ return lp_path_decode(lp_path_of(ewkb, len), out, cap); }

size_t lp_ids_unique(lp_id *ids, size_t n){
    if (n < 2) return n;
    qsort(ids, n, sizeof *ids, lp_id_cmp);                       /* equal IDs are the same bytes: any sort gives one result */
    size_t m = 1; for (size_t i = 1; i < n; i++) if (!lp_id_eq(&ids[i], &ids[m - 1])) ids[m++] = ids[i];
    return m;
}
size_t lp_path_keys(lp_path p, lp_id *out){
    for (size_t i = 0; i < p.n; i++) out[i] = lp_path_id(p, i);
    return lp_ids_unique(out, p.n);
}

bool lp_path_holds(lp_path p, const lp_id *ids, size_t n, bool all){
    lp_id stack[256], *v = p.n <= 256 ? stack : lp_alloc(p.n * sizeof *v); size_t k = lp_path_keys(p, v); bool r = all;
    for (size_t j = 0; j < n; j++) {
        bool found = bsearch(&ids[j], v, k, sizeof *v, lp_id_cmp) != NULL;
        if (all && !found) { r = false; break; }
        if (!all && found) { r = true; break; }
    }
    if (v != stack) lp_free(v);
    return r;
}
void lp_path_times(lp_path p, const lp_id *ids, size_t n, uint64_t *times){
    memset(times, 0, n * sizeof *times);
    for (size_t i = 0; i < p.n; i++) { lp_id v = lp_path_id(p, i); const lp_id *hit = bsearch(&v, ids, n, sizeof *ids, lp_id_cmp); if (hit) times[hit - ids] += lp_path_run(p, i); }
}
static bool in(const lp_id *id, const lp_id *ids, size_t n){ for (size_t z = 0; z < n; z++) if (lp_id_eq(id, &ids[z])) return true; return false; }
bool lp_path_middle_any(lp_path p, const lp_id *ids, size_t n){
    uint64_t np = lp_path_len(p), at = 0;
    if (!n || np < 3) return false;
    for (size_t i = 0; i < p.n; i++) {
        uint64_t first = at; at += lp_path_run(p, i);
        if (at - 1 < 1 || first > np - 2) continue;                  /* this vertex is only the first part, or only the last */
        lp_id id = lp_path_id(p, i); if (in(&id, ids, n)) return true;
    }
    return false;
}

/* Continuations: the phrase is encoded once into vertex bytes, the dispatched scan finds the vertices that hold its
 * first constituent, and from each the rest of the window is compared on the IDs' own bits, the path read with its runs
 * written out; only what follows is decoded. A vertex of run r holds r copies of its constituent, and the phrase may
 * start at any of them; where the phrase's leading constituent repeats L times and something else follows, only the
 * last L copies can start it (every earlier start meets that something else inside the run), so only those are tried. */
size_t lp_path_follows(lp_path p, const lp_id *phrase, size_t np, lp_id *out, size_t cap){
    if (!p.n || !np) return 0;
    lp_scan_fn scan = lp_kernels()->scan;
    uint8_t stack[24 * 64], *key = np <= 64 ? stack : lp_alloc(24 * np);
    for (size_t j = 0; j < np; j++) { double xyz[3]; lp_id_to_xyz(&phrase[j], xyz); memcpy(key + 24 * j, xyz, 24); }
    size_t lead = 1; while (lead < np && lp_id_eq(&phrase[lead], &phrase[0])) lead++;
    const uint8_t *v = p.v; size_t nv = p.n, found = 0;
    for (size_t i = scan(v, 0, nv, key); i < nv; i = scan(v, i + 1, nv, key)) {
        size_t r0 = lp_path_run(p, i), off0 = lead < np && r0 > lead ? r0 - lead : 0;
        for (size_t off = off0; off < r0; off++) {
            size_t vi = i, left = r0 - off, j = 0; bool ok = true;
            while (j < np) {
                if (!lp_vertex_is(v + vi * LP_VERTEX_BYTES, key + 24 * j)) { ok = false; break; }
                j++; left--;
                if (left == 0) { vi++; if (vi >= nv) break; left = lp_path_run(p, vi); }
            }
            if (!ok || j < np || vi >= nv) continue;                   /* no vertex follows the phrase */
            if (found < cap) out[found] = lp_path_id(p, vi);
            found++;
        }
    }
    if (key != stack) lp_free(key);
    return found;
}
size_t lp_follows(const uint8_t *ewkb, size_t len, const lp_id *phrase, size_t np, lp_id *out, size_t cap){
    return lp_path_follows(lp_path_of(ewkb, len), phrase, np, out, cap);
}

/* ---- tuples */
int lp_tuple_other(const lp_id *part, size_t n, const lp_id *at){
    if (n < 2) return -1;
    bool first = lp_id_eq(&part[0], at), last = lp_id_eq(&part[n - 1], at);
    if (first == last) return -1;                                 /* neither end, or both: it ties nothing to anything else */
    return first ? (int)n - 1 : 0;
}
bool lp_tuple_middle_any(const lp_id *part, size_t n, const lp_id *ids, size_t nids){
    for (size_t k = 1; k + 1 < n; k++) if (in(&part[k], ids, nids)) return true;
    return false;
}
