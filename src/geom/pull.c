/* The pull's search kernel: the open set of a best-first search over rated claims. Entities are found by ID in O(1)
 * (the one ID map, whose value is what was reached); the entity of least order is taken in O(log n) from a binary heap.
 * Costs only fall, so an entity reached again more cheaply is queued again and its older entry is passed over when it
 * surfaces. */
#include "laplace/laplace.h"
#include <math.h>
#include <string.h>

typedef struct { double order; uint32_t idx; } Entry;
struct lp_frontier { lp_idmap *at; lp_vec(Entry) heap; };

lp_frontier *lp_frontier_new(void){ lp_frontier *f = lp_zalloc(sizeof *f); f->at = lp_idmap_sized(sizeof(lp_reached)); return f; }
void lp_frontier_free(lp_frontier *f){ if (!f) return; lp_idmap_free(f->at); lp_vec_free(&f->heap); lp_free(f); }
size_t lp_frontier_count(const lp_frontier *f){ return lp_idmap_count(f->at); }
const lp_reached *lp_frontier_find(const lp_frontier *f, const lp_id *id){ return lp_idmap_lookup(f->at, id); }
static lp_reached *reached(lp_frontier *f, uint32_t i){ return lp_idmap_at(f->at, i); }

static void heap_push(lp_frontier *f, double order, uint32_t idx){
    lp_vec_reserve(&f->heap, f->heap.n + 1); Entry *h = f->heap.v; size_t i = f->heap.n++;
    while (i && h[(i - 1) / 2].order > order) { h[i] = h[(i - 1) / 2]; i = (i - 1) / 2; }
    h[i] = (Entry){ order, idx };
}
static Entry heap_pop(lp_frontier *f){
    Entry *h = f->heap.v, top = h[0], last = h[--f->heap.n]; size_t i = 0, n = f->heap.n;
    for (;;) {
        size_t c = 2 * i + 1; if (c >= n) break;
        if (c + 1 < n && h[c + 1].order < h[c].order) c++;
        if (h[c].order >= last.order) break;
        h[i] = h[c]; i = c;
    }
    if (n) h[i] = last;
    return top;
}

bool lp_frontier_reach(lp_frontier *f, const lp_id *id, const lp_id *from, const lp_id *claim, double cost, double estimate,
                       uint32_t hops){
    bool fresh; size_t i = lp_idmap_put(f->at, id, &fresh); lp_reached *x = reached(f, (uint32_t)i);
    if (!fresh && (x->closed || cost >= x->cost)) return false;
    if (fresh) x->id = *id;
    if (from) x->from = *from; else memset(&x->from, 0, sizeof x->from);
    if (claim) x->claim = *claim; else memset(&x->claim, 0, sizeof x->claim);
    x->cost = cost; x->order = cost + estimate; x->hops = hops;
    heap_push(f, x->order, (uint32_t)i);
    return true;
}

const lp_reached *lp_frontier_next(lp_frontier *f){
    while (f->heap.n) {
        Entry e = heap_pop(f); lp_reached *x = reached(f, e.idx);
        if (x->closed || e.order != x->order) continue;                  /* an older, dearer entry */
        x->closed = true; return x;
    }
    return NULL;
}

double lp_frontier_least(lp_frontier *f){
    while (f->heap.n) {
        Entry e = f->heap.v[0]; lp_reached *x = reached(f, e.idx);
        if (!x->closed && e.order == x->order) return e.order;
        heap_pop(f);                                                     /* an older, dearer entry */
    }
    return INFINITY;
}
