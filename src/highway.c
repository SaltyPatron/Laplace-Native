/* The highway: the types' records memory-mapped read-only, their lists and edges read once from the layout beside them. */
#include "laplace/laplace.h"
#include "laplace/highway.h"
#include "internal.h"
#include "blake3.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *lp_highway_path(void){ static char path[4096]; return lp_tier0_sibling("LAPLACE_HIGHWAY", ".highway", path, sizeof path); }

/* The layout: "records N", "edges-count N", "list NAME SAY FIRST COUNT",
 * "bank NAME LIST GROUP CARRIER WIDTH OFFSET ENCODING" and
 * "edges A B FIRST COUNT" lines; the records are the file, and the edges follow them as 8-byte pairs. A bank names its
 * list as it is read: the layout writes the banks after the lists. */
typedef struct { lp_highway *h; size_t lc, ec; uint64_t nrec, nedges; bool bad; } Reading;
static bool number(const char *text, uint64_t limit, uint64_t *out){
    if (!text || !*text) return false;
    uint64_t n = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > limit || n > (limit - digit) / 10) return false;
        n = 10 * n + digit;
    }
    *out = n; return true;
}
static void layout_line(void *ctx, char **f, int n){
    Reading *r = ctx; lp_highway *h = r->h; const char *kind = f[0];
    if (r->bad) return;
    if (!strcmp(kind, "records")) r->bad = n != 2 || !number(f[1], SIZE_MAX / sizeof(lp_tier0_record), &r->nrec);
    else if (!strcmp(kind, "edges-count")) r->bad = n != 2 || !number(f[1], SIZE_MAX / sizeof(lp_edge), &r->nedges);
    else if (!strcmp(kind, "list")) {
        if (n != 5) { r->bad = true; return; }
        if (h->nbanks) { r->bad = true; return; } /* banks keep pointers into the completed list array */
        uint64_t first, count;
        if (strlen(f[1]) >= sizeof h->list[0].name || strlen(f[2]) >= sizeof h->list[0].say
            || !number(f[3], UINT32_MAX, &first) || !number(f[4], UINT32_MAX, &count)) { r->bad = true; return; }
        if (h->nlists == r->lc) {
            size_t cap = r->lc ? r->lc * 2 : 32; lp_list *lists = realloc(h->list, cap * sizeof *lists);
            if (!lists) { r->bad = true; return; } h->list = lists; r->lc = cap;
        }
        lp_list *x = &h->list[h->nlists++]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", f[1]); snprintf(x->say, sizeof x->say, "%s", f[2]); x->first = (uint32_t)first; x->count = (uint32_t)count;
    }
    else if (!strcmp(kind, "bank")) {
        uint64_t width, offset;
        if (n != 8 || strlen(f[1]) >= sizeof h->bank[0].name || strlen(f[3]) >= sizeof h->bank[0].group
            || strlen(f[4]) >= sizeof h->bank[0].carrier || !number(f[5], 64, &width) || !width
            || !number(f[6], 63, &offset) || offset + width > 64
            || (strcmp(f[7], "enum") && strcmp(f[7], "flags"))) { r->bad = true; return; }
        lp_bank *banks = realloc(h->bank, (h->nbanks + 1) * sizeof *banks);
        if (!banks) { r->bad = true; return; } h->bank = banks;
        lp_bank *x = &h->bank[h->nbanks]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", f[1]); snprintf(x->group, sizeof x->group, "%s", f[3]); snprintf(x->carrier, sizeof x->carrier, "%s", f[4]);
        x->width = (uint16_t)width; x->offset = (uint16_t)offset; x->flags = !strcmp(f[7], "flags");
        for (size_t i = 0; i < h->nlists; i++) if (!strcmp(h->list[i].name, f[2])) x->list = &h->list[i];
        if (!x->list || !x->list->count || (x->flags ? x->list->count > width
            : (uint64_t)x->list->count > (lp_mask_field(x) >> offset))) { r->bad = true; return; }
        for (size_t i = 0; i < h->nbanks; i++)
            if (!strcmp(x->name, h->bank[i].name) || (lp_mask_field(x) & lp_mask_field(h->bank + i))) { r->bad = true; return; }
        h->nbanks++;
    }
    else if (!strcmp(kind, "edges")) {
        if (n != 5) { r->bad = true; return; }
        uint64_t first, count;
        if (strlen(f[1]) >= sizeof h->edges[0].a || strlen(f[2]) >= sizeof h->edges[0].b
            || !number(f[3], UINT32_MAX, &first) || !number(f[4], UINT32_MAX, &count)) { r->bad = true; return; }
        if (h->nedgelists == r->ec) {
            size_t cap = r->ec ? r->ec * 2 : 32; lp_edges *edges = realloc(h->edges, cap * sizeof *edges);
            if (!edges) { r->bad = true; return; } h->edges = edges; r->ec = cap;
        }
        lp_edges *x = &h->edges[h->nedgelists++]; memset(x, 0, sizeof *x);
        snprintf(x->a, sizeof x->a, "%s", f[1]); snprintf(x->b, sizeof x->b, "%s", f[2]); x->first = (uint32_t)first; x->count = (uint32_t)count;
    }
}
static const void *highway_open(const char *path){
    size_t size; const uint8_t *m = lp_map_file(path, 0, &size); if (!m || size < sizeof(lp_tier0_record)) return NULL;
    char lp[4200]; lp_beside(path, ".layout", lp, sizeof lp);
    Reading r = { .h = calloc(1, sizeof(lp_highway)) }; lp_highway *h = r.h; if (!h) return NULL;
    bool ok = lp_lines(lp, 8, layout_line, &r) && !r.bad && r.nrec
           && r.nrec <= size / sizeof(lp_tier0_record)
           && r.nedges <= (size - r.nrec * sizeof(lp_tier0_record)) / sizeof(lp_edge);
    for (size_t i = 0; ok && i < h->nlists; i++)
        ok = h->list[i].first <= r.nrec && h->list[i].count <= r.nrec - h->list[i].first;
    for (size_t i = 0; ok && i < h->nedgelists; i++)
        ok = h->edges[i].first <= r.nedges && h->edges[i].count <= r.nedges - h->edges[i].first;
    if (!ok) { free(h->list); free(h->bank); free(h->edges); free(h); return NULL; }
    snprintf(h->path, sizeof h->path, "%s", path);
    h->rec = (const lp_tier0_record *)m; h->nrec = r.nrec; h->edge = (const lp_edge *)(m + r.nrec * sizeof(lp_tier0_record)); h->nedges = r.nedges;
    h->by_id = lp_idindex_build(h->rec, h->nrec, sizeof *h->rec);
    if (!h->by_id) { free(h->list); free(h->bank); free(h->edges); free(h); lp_unmap_file(m, size); return NULL; }
    return h;
}
const lp_highway *lp_highway_map(const char *path){ return lp_cached("highway", path && *path ? path : lp_highway_path(), highway_open); }

bool lp_highway_contains(const lp_highway *h, const lp_id *id){
    if (!h || !id) return false;
    uint64_t probe = 0;
    return lp_idindex_find(h->by_id, id, &probe) >= 0;
}

const lp_list *lp_highway_list(const lp_highway *h, const char *name){
    int64_t i = lp_name_find(h->list, h->nlists, sizeof(lp_list), offsetof(lp_list, name), offsetof(lp_list, say), name);
    return i < 0 ? NULL : &h->list[i];
}
const lp_tier0_record *lp_highway_at(const lp_highway *h, const lp_list *l, uint32_t slot){
    if (!l || slot >= l->count || (size_t)l->first + slot >= h->nrec) return NULL;
    return &h->rec[l->first + slot];
}

/* Published mappings already own their immutable ID index. Queries never
 * allocate or take an initialization lock while record workers are running. */
static const lp_idindex *by_id(const lp_highway *h){ return h->by_id; }
static lp_lock build_mu = LP_LOCK_INIT; /* the separate, lazy textual-key index */
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
    if (!h || !name) return NULL;
    for (size_t i = 0; i < h->nbanks; i++) if (!strcmp(h->bank[i].name, name)) return &h->bank[i];
    return NULL;
}
const lp_bank *lp_highway_bank_of(const lp_highway *h, const lp_id *id, int32_t *bit){
    if (bit) *bit = -1;
    if (!h || !id) return NULL;
    const lp_idindex *index = by_id(h); uint64_t probe = 0;
    for (int64_t i; (i = lp_idindex_find(index, id, &probe)) >= 0; ) {
        for (size_t k = 0; k < h->nbanks; k++) { const lp_list *l = h->bank[k].list; if (!l) continue;
            if ((size_t)i >= l->first && (size_t)i < (size_t)l->first + l->count) { uint32_t slot = (uint32_t)i - l->first;
                const lp_bank *bank = h->bank + k;
                if (bank->flags ? slot >= bank->width : (uint64_t)slot + 1 > (lp_mask_field(bank) >> bank->offset)) continue;
                if (bit) *bit = (int32_t)slot; return bank; } }
    }
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
