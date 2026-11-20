/* Realization: an entity's Merkle DAG back to bytes, one walk for every reader (the engine's reader, the extension's
 * text, labels and claim previews). The caller's reader answers what a node holds; this owns the order, the runs, what
 * is no part of text, the display law and the cycle check. Neither DAG height nor output length takes C stack. */
#include "laplace/laplace.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* What the workspace remembers of a node: whether it is on the active path, and whether its surface is text. */
enum { SURFACE_UNKNOWN = 0, SURFACE_TEXT = 1, SURFACE_NOT = 2, SURFACE_OPEN = 3 };
typedef struct { uint8_t active, surface; } Mark;
typedef struct { const lp_realize_child *child; size_t n, at, mark; uint32_t flags; } SurfaceFrame;
typedef struct { const lp_realize_child *child; size_t n, at, mark; uint32_t repeat, suffix; bool labels; } Frame;

struct lp_realizer {
    lp_idmap *marks;
    lp_vec(Frame) stack;
    lp_vec(SurfaceFrame) surface;
};

lp_realizer *lp_realizer_new(void){
    lp_realizer *w = lp_realloc(NULL, sizeof *w); memset(w, 0, sizeof *w);
    w->marks = lp_idmap_sized(sizeof(Mark)); return w;
}
void lp_realizer_free(lp_realizer *w){
    if (!w) return;
    lp_idmap_free(w->marks); lp_vec_free(&w->stack); lp_vec_free(&w->surface); lp_free(w);
}
void lp_realizer_forget(lp_realizer *w){ if (w) lp_idmap_clear(w->marks); }

typedef struct {
    lp_realizer *w; const lp_tier0_record *t0; lp_realize_reader read; lp_realize_sink sink; void *context;
    lp_realize_result *result;
} Walk;

static Mark *mark_at(Walk *k, size_t i){ return lp_idmap_at(k->w->marks, i); }
static size_t mark_of(Walk *k, const lp_id *id){ bool fresh; size_t i = lp_idmap_put(k->w->marks, id, &fresh); if (fresh) *mark_at(k, i) = (Mark){0}; return i; }
static bool not_text(uint32_t said){ return said == LP_SAID_CLAIM || said == LP_SAID_VOICE; }

static bool emit(Walk *k, const void *bytes, size_t n){
    if (k->sink(k->context, bytes, n)) { k->result->stopped = true; return false; }
    return true;
}
static bool emit_hex(Walk *k, const lp_id *id){ char hex[33]; lp_id_hex(id, hex); return emit(k, hex, 32); }
static bool emit_cp(Walk *k, int64_t cp){ uint8_t u[4]; return emit(k, u, lp_utf8_put((uint32_t)cp, u)); }
static int cycle(Walk *k, const lp_id *id){ k->result->cycle = *id; errno = ELOOP; return -1; }

/* Whether a node's whole surface is text: every node under it read as text, no opaque node and no tuple of anything
 * but codepoints on the way. 1 text, 0 not, 2 a pending node met (counted, nothing remembered), -1 a cycle. A tuple of
 * codepoints asked of itself is not text; met inside another surface, it is. */
static int surface(Walk *k, const lp_id *id){
    if (lp_tier0_codepoint(k->t0, id) >= 0) return 1;
    lp_realize_node node = {0}; int answer = k->read(k->context, id, &node);
    if (answer == LP_REALIZE_PENDING) { k->result->pending++; return 2; }
    if (answer != LP_REALIZE_KNOWN) return 0;
    size_t root = mark_of(k, id); Mark *m = mark_at(k, root);
    if (m->surface == SURFACE_TEXT || m->surface == SURFACE_NOT) return m->surface == SURFACE_TEXT;
    if ((node.flags & LP_REALIZE_OPAQUE) || ((node.flags & LP_REALIZE_TUPLE) && !(node.flags & LP_REALIZE_SCALAR))) { m->surface = SURFACE_NOT; return 0; }
    if (node.flags & LP_REALIZE_TUPLE) return 0;
    if (m->surface == SURFACE_OPEN) return cycle(k, id);
    if (!node.n) { m->surface = SURFACE_NOT; return 0; }
    lp_realizer *w = k->w; w->surface.n = 0;
    lp_push(&w->surface, ((SurfaceFrame){ node.child, node.n, 0, root, node.flags }));
    int outcome = 1;
    while (w->surface.n) {
        SurfaceFrame *f = w->surface.v + w->surface.n - 1; Mark *me = mark_at(k, f->mark);
        if (me->surface == SURFACE_TEXT || me->surface == SURFACE_NOT) { w->surface.n--; continue; }
        if ((f->flags & LP_REALIZE_OPAQUE) || ((f->flags & LP_REALIZE_TUPLE) && !(f->flags & LP_REALIZE_SCALAR))) { me->surface = SURFACE_NOT; continue; }
        if (!f->n) { me->surface = SURFACE_NOT; continue; }
        me->surface = SURFACE_OPEN;
        while (f->at < f->n && not_text(f->child[f->at].said)) f->at++;
        if (f->at == f->n) { me->surface = SURFACE_TEXT; continue; }
        const lp_id *next = &f->child[f->at].id;
        if (lp_tier0_codepoint(k->t0, next) >= 0) { f->at++; continue; }
        lp_realize_node child = {0}; int got = k->read(k->context, next, &child);
        if (got == LP_REALIZE_PENDING) { k->result->pending++; outcome = 2; break; }
        if (got != LP_REALIZE_KNOWN) { me->surface = SURFACE_NOT; continue; }
        size_t at = mark_of(k, next); Mark *them = mark_at(k, at); me = mark_at(k, f->mark);
        if (them->surface == SURFACE_OPEN) { outcome = cycle(k, next); break; }
        if (them->surface == SURFACE_NOT) { me->surface = SURFACE_NOT; continue; }
        if (them->surface == SURFACE_TEXT) { f->at++; continue; }
        lp_push(&w->surface, ((SurfaceFrame){ child.child, child.n, 0, at, child.flags }));
    }
    if (outcome != 1) {   /* nothing is remembered of a walk cut short */
        for (size_t i = 0; i < w->surface.n; i++) { Mark *open = mark_at(k, w->surface.v[i].mark); if (open->surface == SURFACE_OPEN) open->surface = SURFACE_UNKNOWN; }
        w->surface.n = 0; return outcome;
    }
    return mark_at(k, root)->surface == SURFACE_TEXT;
}

/* Into one occurrence of id: written at once (a codepoint, a hex label), skipped (pending, absent, nothing to write),
 * or a frame to walk. 1 a frame was pushed, 0 nothing to walk, 2 pending, 3 absent, 4 the sink stopped, -1 a cycle. */
static int enter(Walk *k, const lp_id *id, bool labels){
    int64_t cp = lp_tier0_codepoint(k->t0, id);
    if (cp >= 0) return emit_cp(k, cp) ? 0 : 4;
    lp_realize_node node = {0}; int answer = k->read(k->context, id, &node);
    if (answer == LP_REALIZE_PENDING) { k->result->pending++; return 2; }
    if (answer != LP_REALIZE_KNOWN) return labels && !emit_hex(k, id) ? 4 : 3;
    if (labels) {
        int text = surface(k, id);
        if (text < 0) return -1;
        if (text == 2) return 2;
        bool structured = ((node.flags & LP_REALIZE_TUPLE) || ((node.flags & LP_REALIZE_OPAQUE) && node.n)) && !text;
        if (!structured) {
            if (!text) return emit_hex(k, id) ? 0 : 4;
            labels = false;
        }
    }
    if (!labels && !node.n) return 0;
    size_t at = mark_of(k, id); Mark *m = mark_at(k, at);
    if (m->active) return cycle(k, id);
    m->active = 1;
    lp_push(&k->w->stack, ((Frame){ node.child, node.n, 0, at, 0, 0, labels }));
    if (labels && !emit(k, "[", 1)) return 4;
    return 1;
}

bool lp_realize(lp_realizer *w, const lp_tier0_record *t0, const lp_id *root, uint32_t mode,
                lp_realize_reader reader, lp_realize_sink sink, void *context, lp_realize_result *result){
    lp_realize_result none;
    if (!result) result = &none;
    memset(result, 0, sizeof *result);
    if (!w || !t0 || !root || !reader || !sink || mode > LP_REALIZE_LABELS) { errno = EINVAL; return false; }
    Walk k = { w, t0, reader, sink, context, result };
    w->stack.n = 0;
    int got = enter(&k, root, mode == LP_REALIZE_LABELS);
    while (got >= 0 && got != 4 && w->stack.n) {
        Frame *f = w->stack.v + w->stack.n - 1;
        if (f->labels) {
            if (f->suffix > 1) {
                char times[24]; int n = snprintf(times, sizeof times, " \xC3\x97 %u", (unsigned)f->suffix);
                f->suffix = 0;
                if (!emit(&k, times, (size_t)n)) { got = 4; break; }
            }
            if (f->at < f->n) {
                if (f->at && !emit(&k, " \xC2\xB7 ", 4)) { got = 4; break; }
                lp_realize_child child = f->child[f->at++]; f->suffix = child.run;
                got = enter(&k, &child.id, true);
                continue;
            }
            if (!emit(&k, "]", 1)) { got = 4; break; }
        } else {
            while (!f->repeat && f->at < f->n) { lp_realize_child child = f->child[f->at++]; if (!not_text(child.said)) f->repeat = child.run; }
            if (f->repeat) {
                f->repeat--;
                got = enter(&k, &f->child[f->at - 1].id, false);
                /* a node pending or absent stands for nothing in any occurrence of its run */
                if (got == 2 || got == 3) { f->repeat = 0; got = 0; }   /* nothing was pushed: f stands */
                continue;
            }
        }
        mark_at(&k, f->mark)->active = 0; w->stack.n--;
        got = 0;
    }
    /* A stop or a cycle can leave a walk inside a shared branch: the active path is cleared so another root, or this
     * one walked again, can reuse the same DAG. */
    for (size_t i = 0; i < w->stack.n; i++) mark_at(&k, w->stack.v[i].mark)->active = 0;
    w->stack.n = 0;
    if (got < 0) return false;
    return true;
}
