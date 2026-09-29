/* The pull's search kernel: the open set of a best-first search over rated claims. Entities are found by ID in O(1);
 * the entity of least order is taken in O(log n). Costs only fall, so an entity reached again more cheaply is queued
 * again and its older entry is passed over when it surfaces. */
#include "laplace/laplace.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double order; uint32_t idx; } Entry;
struct lp_frontier {
    lp_reached *r; size_t n, cap;
    uint32_t *slot; size_t scap;                       /* open addressing: index + 1 */
    Entry *heap; size_t hn, hcap;
};

static void *grow(void *p, size_t n){ p = realloc(p, n ? n : 1); if (!p) abort(); return p; }
static uint64_t key(const lp_id *id){ uint64_t k; memcpy(&k, id->b + 2, 8); return k; }

lp_frontier *lp_frontier_new(void){ return calloc(1, sizeof(lp_frontier)); }
void lp_frontier_free(lp_frontier *f){ if (!f) return; free(f->r); free(f->slot); free(f->heap); free(f); }
size_t lp_frontier_count(const lp_frontier *f){ return f->n; }

static void rehash(lp_frontier *f){
    free(f->slot); f->scap = f->scap ? f->scap * 2 : 1024; f->slot = calloc(f->scap, sizeof *f->slot); if (!f->slot) abort();
    for (size_t i = 0; i < f->n; i++) {
        size_t k = key(&f->r[i].id) & (f->scap - 1);
        while (f->slot[k]) k = (k + 1) & (f->scap - 1);
        f->slot[k] = (uint32_t)i + 1;
    }
}
static lp_reached *find(const lp_frontier *f, const lp_id *id, size_t *at){
    if (!f->scap) return NULL;
    size_t k = key(id) & (f->scap - 1);
    while (f->slot[k]) { lp_reached *x = &f->r[f->slot[k] - 1]; if (!memcmp(x->id.b, id->b, 16)) return x; k = (k + 1) & (f->scap - 1); }
    if (at) *at = k;
    return NULL;
}
const lp_reached *lp_frontier_find(const lp_frontier *f, const lp_id *id){ return find(f, id, NULL); }

static void heap_push(lp_frontier *f, double order, uint32_t idx){
    if (f->hn == f->hcap) { f->hcap = f->hcap ? f->hcap * 2 : 1024; f->heap = grow(f->heap, f->hcap * sizeof(Entry)); }
    size_t i = f->hn++;
    while (i && f->heap[(i - 1) / 2].order > order) { f->heap[i] = f->heap[(i - 1) / 2]; i = (i - 1) / 2; }
    f->heap[i] = (Entry){ order, idx };
}
static Entry heap_pop(lp_frontier *f){
    Entry top = f->heap[0], last = f->heap[--f->hn]; size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1; if (c >= f->hn) break;
        if (c + 1 < f->hn && f->heap[c + 1].order < f->heap[c].order) c++;
        if (f->heap[c].order >= last.order) break;
        f->heap[i] = f->heap[c]; i = c;
    }
    if (f->hn) f->heap[i] = last;
    return top;
}

bool lp_frontier_reach(lp_frontier *f, const lp_id *id, const lp_id *from, const lp_id *claim, double cost, double estimate,
                       uint32_t hops){
    if ((f->n + 1) * 2 > f->scap) rehash(f);
    size_t at = 0; lp_reached *x = find(f, id, &at);
    if (x) { if (x->closed || cost >= x->cost) return false; }
    else {
        if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 1024; f->r = grow(f->r, f->cap * sizeof(lp_reached)); }
        x = &f->r[f->n]; memset(x, 0, sizeof *x); x->id = *id; f->slot[at] = (uint32_t)++f->n;
    }
    if (from) x->from = *from; else memset(&x->from, 0, sizeof x->from);
    if (claim) x->claim = *claim; else memset(&x->claim, 0, sizeof x->claim);
    x->cost = cost; x->order = cost + estimate; x->hops = hops;
    heap_push(f, x->order, (uint32_t)(x - f->r));
    return true;
}

const lp_reached *lp_frontier_next(lp_frontier *f){
    while (f->hn) {
        Entry e = heap_pop(f); lp_reached *x = &f->r[e.idx];
        if (x->closed || e.order != x->order) continue;                  /* an older, dearer entry */
        x->closed = true; return x;
    }
    return NULL;
}

double lp_frontier_least(lp_frontier *f){
    while (f->hn) {
        Entry e = f->heap[0]; lp_reached *x = &f->r[e.idx];
        if (!x->closed && e.order == x->order) return e.order;
        heap_pop(f);                                                     /* an older, dearer entry */
    }
    return INFINITY;
}
