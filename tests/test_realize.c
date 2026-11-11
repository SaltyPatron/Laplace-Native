/* lp_realize: one walk from an entity back to its text, exact and as display labels, over a caller's reader. */
#include "check.h"
#include <errno.h>

typedef struct { lp_id id; lp_realize_child *child; size_t n; uint32_t flags; int answer; } Node;
typedef struct { Node *node; size_t n; char out[4096]; size_t len, limit; size_t reads; } World;

static int reader(void *context, const lp_id *id, lp_realize_node *out){
    World *w = context; w->reads++;
    for (size_t i = 0; i < w->n; i++) if (lp_id_eq(&w->node[i].id, id)) {
        if (w->node[i].answer != LP_REALIZE_KNOWN) return w->node[i].answer;
        out->child = w->node[i].child; out->n = w->node[i].n; out->flags = w->node[i].flags; return LP_REALIZE_KNOWN;
    }
    return LP_REALIZE_ABSENT;
}
static int sink(void *context, const uint8_t *bytes, size_t n){
    World *w = context;
    if (w->len + n >= sizeof w->out) return 1;
    memcpy(w->out + w->len, bytes, n); w->len += n; w->out[w->len] = 0;
    return w->limit && w->len >= w->limit;
}
typedef struct { size_t depth; lp_id leaf; char out[8]; size_t len; lp_realize_child *link; } Deep;
static lp_id deep_id(size_t i){ lp_id id; memset(id.b, 0, sizeof id.b); id.b[0] = 0xDD; memcpy(id.b + 8, &i, sizeof i); return id; }
static int deep_reader(void *context, const lp_id *id, lp_realize_node *out){
    Deep *d = context; size_t i; memcpy(&i, id->b + 8, sizeof i);
    if (id->b[0] != 0xDD || i >= d->depth) return LP_REALIZE_ABSENT;
    d->link[i] = (lp_realize_child){ i + 1 < d->depth ? deep_id(i + 1) : d->leaf, 1, 0 };   /* stays put while walked */
    out->child = d->link + i; out->n = 1; out->flags = 0; return LP_REALIZE_KNOWN;
}
static int deep_sink(void *context, const uint8_t *bytes, size_t n){
    Deep *d = context; if (d->len + n > sizeof d->out) return 1; memcpy(d->out + d->len, bytes, n); d->len += n; return 0;
}
static lp_id named(uint8_t tag){ lp_id id; memset(id.b, 0, sizeof id.b); id.b[0] = 0xEE; id.b[8] = tag; return id; }
static const char *run(lp_realizer *r, const lp_tier0_record *t0, World *w, const lp_id *root, uint32_t mode, lp_realize_result *res){
    w->len = 0; w->out[0] = 0;
    CHECK(lp_realize(r, t0, root, mode, reader, sink, w, res), "realize %02x", root->b[1]);
    return w->out;
}

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0, "tier 0 mapped");
    if (!t0) DONE("realize");
    lp_id cp[128]; for (uint32_t c = 0; c < 128; c++) cp[c] = lp_ref_atom(t0, c).id;
    lp_id eacute = lp_ref_atom(t0, 0xE9).id;

    /* hello = [h, e, l x2, o]; word = [a, b]; twice = [word x3, eacute]; record = [hello, voice, claim x2] */
    lp_realize_child hello[] = {{cp['h'],1,0},{cp['e'],1,0},{cp['l'],2,0},{cp['o'],1,0}};
    lp_realize_child word[] = {{cp['a'],1,0},{cp['b'],1,0}};
    lp_id W = named(1), H = named(2), T = named(3), R = named(4), C = named(5), V = named(6), P = named(7), Q = named(8), X = named(9);
    lp_realize_child twice[] = {{W,3,0},{eacute,1,0}};
    lp_realize_child record[] = {{H,1,0},{V,1,LP_SAID_VOICE},{C,2,LP_SAID_CLAIM}};
    lp_realize_child claim[] = {{W,1,0},{H,1,0},{T,1,0}};
    lp_realize_child pair[] = {{cp['x'],1,0},{cp['y'],1,0}};                  /* a tuple of codepoints */
    lp_realize_child holds_pair[] = {{P,1,0},{cp['!'],1,0}};                  /* text around that tuple */
    lp_realize_child structure[] = {{H,1,0},{X,2,0},{P,1,0}};                 /* a tuple of a word, an opaque leaf, a tuple */
    Node nodes[] = {
        {H, hello, 4, 0, LP_REALIZE_KNOWN}, {W, word, 2, 0, LP_REALIZE_KNOWN}, {T, twice, 2, 0, LP_REALIZE_KNOWN},
        {R, record, 3, LP_REALIZE_OPAQUE, LP_REALIZE_KNOWN}, {C, claim, 3, LP_REALIZE_TUPLE, LP_REALIZE_KNOWN},
        {V, word, 2, 0, LP_REALIZE_KNOWN}, {P, pair, 2, LP_REALIZE_TUPLE | LP_REALIZE_SCALAR, LP_REALIZE_KNOWN},
        {Q, holds_pair, 2, 0, LP_REALIZE_KNOWN}, {X, NULL, 0, LP_REALIZE_OPAQUE, LP_REALIZE_KNOWN},
        {named(10), structure, 3, LP_REALIZE_TUPLE, LP_REALIZE_KNOWN},
    };
    World w = { nodes, sizeof nodes / sizeof *nodes, {0}, 0, 0, 0 };
    lp_realizer *r = lp_realizer_new(); lp_realize_result res;

    CHECK(!strcmp(run(r, t0, &w, &H, LP_REALIZE_TEXT, &res), "hello"), "runs written out: %s", w.out);
    CHECK(!strcmp(run(r, t0, &w, &T, LP_REALIZE_TEXT, &res), "ababab\xC3\xA9"), "a run of a composition, UTF-8: %s", w.out);
    CHECK(!strcmp(run(r, t0, &w, &cp['z'], LP_REALIZE_TEXT, &res), "z"), "a codepoint is itself");
    CHECK(!strcmp(run(r, t0, &w, &R, LP_REALIZE_TEXT, &res), "hello"), "a record reads as its thing, claims and voices left out: %s", w.out);
    CHECK(!strcmp(run(r, t0, &w, &C, LP_REALIZE_TEXT, &res), "abhelloababab\xC3\xA9"), "exact text of a tuple is its parts: %s", w.out);
    lp_id nothing = named(99);
    CHECK(!strcmp(run(r, t0, &w, &nothing, LP_REALIZE_TEXT, &res), "") && !res.pending, "absent writes nothing");

    /* labels */
    CHECK(!strcmp(run(r, t0, &w, &H, LP_REALIZE_LABELS, &res), "hello"), "a text surface is its text: %s", w.out);
    CHECK(!strcmp(run(r, t0, &w, &C, LP_REALIZE_LABELS, &res), "[ab \xC2\xB7 hello \xC2\xB7 ababab\xC3\xA9]"), "a tuple is its parts: %s", w.out);
    char hex[33]; lp_id_hex(&X, hex); char want[256];
    snprintf(want, sizeof want, "[hello \xC2\xB7 %s \xC3\x97 2 \xC2\xB7 [x \xC2\xB7 y]]", hex);
    CHECK(!strcmp(run(r, t0, &w, &nodes[9].id, LP_REALIZE_LABELS, &res), want), "an opaque leaf is its ID, a run shown once: %s", w.out);
    CHECK(!strcmp(run(r, t0, &w, &Q, LP_REALIZE_LABELS, &res), "xy!"), "a tuple of codepoints inside text is text: %s", w.out);
    CHECK(!strcmp(run(r, t0, &w, &P, LP_REALIZE_LABELS, &res), "xy"), "...and remembered as text after: %s", w.out);
    lp_realizer_forget(r);
    CHECK(!strcmp(run(r, t0, &w, &P, LP_REALIZE_LABELS, &res), "[x \xC2\xB7 y]"), "a tuple of codepoints asked of itself is a structure: %s", w.out);
    snprintf(want, sizeof want, "[[ab \xC2\xB7 hello \xC2\xB7 ababab\xC3\xA9] \xC3\x97 2]");
    lp_id_hex(&V, hex);
    char rec[256]; snprintf(rec, sizeof rec, "[hello \xC2\xB7 ab \xC2\xB7 [ab \xC2\xB7 hello \xC2\xB7 ababab\xC3\xA9] \xC3\x97 2]");
    CHECK(!strcmp(run(r, t0, &w, &R, LP_REALIZE_LABELS, &res), rec), "a record's structure shows what it says: %s", w.out);
    lp_id_hex(&nothing, hex);
    CHECK(!strcmp(run(r, t0, &w, &nothing, LP_REALIZE_LABELS, &res), hex), "absent is its ID");

    /* pending: skipped and counted once a run, then walked again when known */
    nodes[1].answer = LP_REALIZE_PENDING;
    CHECK(!strcmp(run(r, t0, &w, &T, LP_REALIZE_TEXT, &res), "\xC3\xA9") && res.pending == 1, "pending once a run: %zu", res.pending);
    nodes[1].answer = LP_REALIZE_KNOWN;
    CHECK(!strcmp(run(r, t0, &w, &T, LP_REALIZE_TEXT, &res), "ababab\xC3\xA9") && !res.pending, "fetched, walked again");

    /* a byte limit stops the walk, and the active path is cleared for the next */
    w.limit = 3;
    CHECK(!strcmp(run(r, t0, &w, &T, LP_REALIZE_TEXT, &res), "aba") && res.stopped, "the sink stops: %s", w.out);
    w.limit = 0;
    CHECK(!strcmp(run(r, t0, &w, &T, LP_REALIZE_TEXT, &res), "ababab\xC3\xA9") && !res.stopped, "nothing left active");

    /* a cycle is ELOOP, named, and leaves the workspace reusable */
    lp_realize_child loop[] = {{cp['a'],1,0},{named(20),1,0}};
    Node ring[] = {{named(20), loop, 2, 0, LP_REALIZE_KNOWN}};
    World cyc = { ring, 1, {0}, 0, 0, 0 };
    lp_id start = named(20);
    errno = 0;
    CHECK(!lp_realize(r, t0, &start, LP_REALIZE_TEXT, reader, sink, &cyc, &res) && errno == ELOOP && lp_id_eq(&res.cycle, &start), "cycle");
    errno = 0;
    lp_realizer_forget(r);
    CHECK(!lp_realize(r, t0, &start, LP_REALIZE_LABELS, reader, sink, &cyc, &res) && errno == ELOOP, "cycle in labels");
    CHECK(!strcmp(run(r, t0, &w, &T, LP_REALIZE_TEXT, &res), "ababab\xC3\xA9"), "reusable after a cycle");
    errno = 0;
    CHECK(!lp_realize(r, t0, &H, 7, reader, sink, &w, &res) && errno == EINVAL, "bad mode");

    /* height takes no C stack: a chain 200,000 deep, each node holding the next, the last a codepoint */
    size_t depth = 200000;
    CHECK(!strcmp(run(r, t0, &(World){0}, &cp['q'], LP_REALIZE_TEXT, &res), "q"), "a leaf");
    Deep deep = { depth, cp['q'], {0}, 0, calloc(depth, sizeof(lp_realize_child)) };
    CHECK(deep.link, "allocate chain");
    if (!deep.link) DONE("realize");
    lp_id top = deep_id(0);
    CHECK(lp_realize(r, t0, &top, LP_REALIZE_TEXT, deep_reader, deep_sink, &deep, &res) && deep.len == 1 && deep.out[0] == 'q', "deep chain, text");
    deep.len = 0;
    CHECK(lp_realize(r, t0, &top, LP_REALIZE_LABELS, deep_reader, deep_sink, &deep, &res) && deep.len == 1 && deep.out[0] == 'q', "deep chain, labels");
    lp_realizer_free(r); free(deep.link);
    DONE("realize");
}
