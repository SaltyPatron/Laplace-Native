/* The highway: the types' records memory-mapped read-only, their lists and edges read once from the layout beside them. */
#include "laplace/laplace.h"
#include "internal.h"
#include "blake3.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *lp_highway_path(void){ static char path[4096]; return lp_tier0_sibling("LAPLACE_HIGHWAY", ".highway", path, sizeof path); }

/* The layout: "records N", "edges-count N", "list NAME SAY FIRST COUNT", "bank NAME LIST GROUP CARRIER WIDTH" and
 * "edges A B FIRST COUNT" lines; the records are the file, and the edges follow them as 8-byte pairs. A bank names its
 * list as it is read: the layout writes the banks after the lists. */
typedef struct { lp_highway *h; size_t lc, ec; uint64_t nrec, nedges; } Reading;
static void layout_line(void *ctx, char **f, int n){
    Reading *r = ctx; lp_highway *h = r->h; const char *kind = f[0];
    if (!strcmp(kind, "records") && n > 1) r->nrec = strtoull(f[1], NULL, 10);
    else if (!strcmp(kind, "edges-count") && n > 1) r->nedges = strtoull(f[1], NULL, 10);
    else if (!strcmp(kind, "list") && n > 4) {
        if (h->nlists == r->lc) { r->lc = r->lc ? r->lc * 2 : 32; h->list = realloc(h->list, r->lc * sizeof(lp_list)); }
        lp_list *x = &h->list[h->nlists++]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", f[1]); snprintf(x->say, sizeof x->say, "%s", f[2]); x->first = (uint32_t)strtoul(f[3], NULL, 10); x->count = (uint32_t)strtoul(f[4], NULL, 10);
    }
    else if (!strcmp(kind, "bank") && n > 5) {
        h->bank = realloc(h->bank, (h->nbanks + 1) * sizeof(lp_bank)); lp_bank *x = &h->bank[h->nbanks++]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", f[1]); snprintf(x->group, sizeof x->group, "%s", f[3]); snprintf(x->carrier, sizeof x->carrier, "%s", f[4]);
        x->width = (uint16_t)atoi(f[5]); x->list = NULL;
        if (strcmp(f[2], "-")) for (size_t i = 0; i < h->nlists; i++) if (!strcmp(h->list[i].name, f[2])) x->list = &h->list[i];
    }
    else if (!strcmp(kind, "edges") && n > 4) {
        if (h->nedgelists == r->ec) { r->ec = r->ec ? r->ec * 2 : 32; h->edges = realloc(h->edges, r->ec * sizeof(lp_edges)); }
        lp_edges *x = &h->edges[h->nedgelists++]; memset(x, 0, sizeof *x);
        snprintf(x->a, sizeof x->a, "%s", f[1]); snprintf(x->b, sizeof x->b, "%s", f[2]); x->first = (uint32_t)strtoul(f[3], NULL, 10); x->count = (uint32_t)strtoul(f[4], NULL, 10);
    }
}
static const void *highway_open(const char *path){
    size_t size; const uint8_t *m = lp_map_file(path, 0, &size); if (!m || size < sizeof(lp_tier0_record)) return NULL;
    char lp[4200]; lp_beside(path, ".layout", lp, sizeof lp);
    Reading r = { calloc(1, sizeof(lp_highway)), 0, 0, 0, 0 }; lp_highway *h = r.h; if (!h) return NULL;
    if (!lp_lines(lp, 6, layout_line, &r) || r.nrec * sizeof(lp_tier0_record) + r.nedges * sizeof(lp_edge) > size) { free(h->list); free(h->bank); free(h->edges); free(h); return NULL; }
    snprintf(h->path, sizeof h->path, "%s", path);
    h->rec = (const lp_tier0_record *)m; h->nrec = r.nrec; h->edge = (const lp_edge *)(m + r.nrec * sizeof(lp_tier0_record)); h->nedges = r.nedges;
    return h;
}
const lp_highway *lp_highway_map(const char *path){ return lp_cached("highway", path && *path ? path : lp_highway_path(), highway_open); }

const lp_list *lp_highway_list(const lp_highway *h, const char *name){
    int64_t i = lp_name_find(h->list, h->nlists, sizeof(lp_list), offsetof(lp_list, name), offsetof(lp_list, say), name);
    return i < 0 ? NULL : &h->list[i];
}
const lp_tier0_record *lp_highway_at(const lp_highway *h, const lp_list *l, uint32_t slot){
    if (!l || slot >= l->count || (size_t)l->first + slot >= h->nrec) return NULL;
    return &h->rec[l->first + slot];
}

/* What is built over a highway the first time it is asked, once for the process: the index of its records by ID, and
 * its keys. A mapped highway is shared by every caller, so the building is under a lock. */
static lp_lock build_mu = LP_LOCK_INIT;
static const lp_idindex *by_id(const lp_highway *hc){
    lp_highway *h = (lp_highway *)hc; lp_idindex *x = __atomic_load_n(&h->by_id, __ATOMIC_ACQUIRE);
    if (x) return x;
    lp_lock_take(&build_mu);
    if (!(x = h->by_id)) __atomic_store_n(&h->by_id, x = lp_idindex_build(h->rec, h->nrec, sizeof *h->rec), __ATOMIC_RELEASE);
    lp_lock_give(&build_mu);
    return x;
}
/* The record's place among all the highway's records, in list l (any list when l is NULL), or -1. */
static int64_t record_of(const lp_highway *h, const lp_list *l, const lp_id *id){
    const lp_idindex *x = by_id(h); uint64_t probe = 0;
    for (int64_t i; (i = lp_idindex_find(x, id, &probe)) >= 0; )
        if (!l || ((size_t)i >= l->first && (size_t)i < (size_t)l->first + l->count)) return i;
    return -1;
}
int64_t lp_highway_slot(const lp_highway *h, const lp_list *l, const lp_id *id){
    int64_t i = record_of(h, l, id); return i < 0 ? -1 : i - (l ? (int64_t)l->first : 0);
}
size_t lp_highway_edges(const lp_highway *h, const char *a, uint32_t slot, const char *b, const lp_edge **out){
    for (size_t i = 0; i < h->nedgelists; i++) {
        const lp_edges *e = &h->edges[i]; if (strcmp(e->a, a) || strcmp(e->b, b)) continue;
        const lp_edge *p = h->edge + e->first; size_t lo = 0, hi = e->count;              /* sorted by from: binary search to the first */
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (p[mid].from < slot) lo = mid + 1; else hi = mid; }
        size_t n = 0; while (lo + n < e->count && p[lo + n].from == slot) n++;
        *out = p + lo; return n;
    }
    *out = NULL; return 0;
}
void lp_highway_fingerprint(const lp_highway *h, uint8_t out[32]){
    blake3_hasher hs; blake3_hasher_init(&hs);
    blake3_hasher_update(&hs, h->rec, h->nrec * sizeof(lp_tier0_record));
    blake3_hasher_update(&hs, h->edge, h->nedges * sizeof(lp_edge));
    blake3_hasher_finalize(&hs, out, 32);
}

const lp_bank *lp_highway_bank(const lp_highway *h, const char *name){
    for (size_t i = 0; i < h->nbanks; i++) if (!strcmp(h->bank[i].name, name)) return &h->bank[i];
    return NULL;
}
const lp_bank *lp_highway_bank_of(const lp_highway *h, const lp_id *id, int32_t *bit){
    int64_t i = record_of(h, NULL, id); if (bit) *bit = -1; if (i < 0) return NULL;
    for (size_t k = 0; k < h->nbanks; k++) { const lp_list *l = h->bank[k].list; if (!l) continue;
        if ((size_t)i >= l->first && (size_t)i < (size_t)l->first + l->count) { uint32_t slot = (uint32_t)i - l->first;
            if (slot >= h->bank[k].width) return NULL; if (bit) *bit = (int32_t)slot; return &h->bank[k]; } }
    return NULL;
}

/* The keys: "list\tkey\tslot" lines beside the highway, read once. A key is looked up with its list: the map's key is
 * the list's place (4 bytes) and the key's bytes. */
typedef struct { lp_highway *h; lp_strmap *m; } KeyReading;
static void key_line(void *ctx, char **f, int n){
    KeyReading *r = ctx; if (n < 3) return;
    uint32_t li = 0; while (li < r->h->nlists && strcmp(r->h->list[li].name, f[0])) li++; if (li == r->h->nlists) return;
    size_t kl = strlen(f[1]); char k[4 + 512]; if (kl > 512) return; memcpy(k, &li, 4); memcpy(k + 4, f[1], kl);
    bool fresh; uint32_t *slot = lp_strmap_get(r->m, k, 4 + kl, &fresh); if (fresh) *slot = (uint32_t)strtoul(f[2], NULL, 10);
}
static lp_strmap *keys(const lp_highway *hc){
    lp_highway *h = (lp_highway *)hc; lp_strmap *m = __atomic_load_n(&h->keys, __ATOMIC_ACQUIRE);
    if (m) return m;
    lp_lock_take(&build_mu);
    if (!(m = h->keys)) { KeyReading r = { h, lp_strmap_lasting(sizeof(uint32_t)) }; char kp[4200]; lp_beside(h->path, ".keys", kp, sizeof kp);
        lp_lines(kp, 3, key_line, &r); __atomic_store_n(&h->keys, m = r.m, __ATOMIC_RELEASE); }
    lp_lock_give(&build_mu);
    return m;
}
int64_t lp_highway_key(const lp_highway *h, const lp_list *l, const char *key){
    if (!l) return -1;
    size_t kl = strlen(key); char k[4 + 512]; if (kl > 512) return -1;
    uint32_t li = (uint32_t)(l - h->list); memcpy(k, &li, 4); memcpy(k + 4, key, kl);
    int64_t i = lp_strmap_find(keys(h), k, 4 + kl);
    return i < 0 ? -1 : (int64_t)*(uint32_t *)lp_strmap_at(keys(h), (size_t)i);
}
