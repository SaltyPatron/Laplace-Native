/* Geometry traversal hands each ring or line to the same contiguous-path kernels. No continuation crosses a
 * component boundary. Native-byte-order spans are read in place; other byte orders share one conversion buffer. */
#include "laplace/geometry.h"
#include "internal.h"
#include <errno.h>
#include <string.h>

size_t lp_scan_scalar(const uint8_t *v, size_t s, size_t n, const uint8_t key[24]){
    for (; s < n; s++) if (lp_vertex_is(v + s * LP_VERTEX_BYTES, key)) return s;
    return n;
}

typedef struct { lp_vertex *out; size_t cap, count; } VertexOutput;
static bool decode_vertices(void *context, const lp_ewkb_span *span){
    VertexOutput *v = context;
    for (size_t i = 0; i < span->count && v->count + i < v->cap; i++) {
        double xyzm[4]; lp_ewkb_vertex(span, i, xyzm);
        lp_path_decode(lp_path_block(xyzm, 1), v->out + v->count + i, 1);
    }
    v->count += span->count;
    return true;
}
size_t lp_path_vertices(const uint8_t *ewkb, size_t len, lp_vertex *out, size_t cap){
    VertexOutput v = { out, out ? cap : 0, 0 };
    return lp_ewkb_visit(ewkb, len, decode_vertices, &v, NULL) ? v.count : 0;
}

typedef struct { lp_id *out; size_t cap, count; } IdOutput;
static bool expand_vertices(void *context, const lp_ewkb_span *span){
    IdOutput *v = context;
    for (size_t i = 0; i < span->count; i++) {
        double xyzm[4]; lp_ewkb_vertex(span, i, xyzm); size_t run = lp_m_run(xyzm[3]);
        if (run > SIZE_MAX - v->count) { errno = EOVERFLOW; return false; }
        if (v->count < v->cap) {
            lp_id id; lp_xyz_to_id(xyzm, &id);
            size_t emit = run < v->cap - v->count ? run : v->cap - v->count;
            for (size_t j = 0; j < emit; j++) v->out[v->count + j] = id;
        }
        v->count += run;
    }
    return true;
}
size_t lp_path_ids(const uint8_t *ewkb, size_t len, lp_id *out, size_t cap){
    IdOutput v = { out, out ? cap : 0, 0 };
    return lp_ewkb_visit(ewkb, len, expand_vertices, &v, NULL) ? v.count : 0;
}

typedef struct {
    const lp_id *phrase;
    size_t np, cap, count;
    lp_id *out;
    lp_buf converted;
    uint8_t local[128 * LP_VERTEX_BYTES];
} GeometryFollows;
static bool follow_span(void *context, const lp_ewkb_span *span){
    GeometryFollows *f = context; const uint8_t *vertices = span->data; const uint16_t endian = 1;
    if (span->little_endian != (*(const uint8_t *)&endian != 0)) {
        size_t bytes = span->count * LP_VERTEX_BYTES; uint8_t *copy = f->local;
        if (bytes > sizeof f->local) {
            lp_reserve((void **)&f->converted.b, &f->converted.cap, bytes, 1); copy = f->converted.b;
        }
        for (size_t i = 0; i < span->count; i++) {
            double xyzm[4]; lp_ewkb_vertex(span, i, xyzm); memcpy(copy + i * LP_VERTEX_BYTES, xyzm, sizeof xyzm);
        }
        vertices = copy;
    }
    size_t room = f->count < f->cap ? f->cap - f->count : 0;
    size_t found = lp_path_follows(lp_path_block(vertices, span->count), f->phrase, f->np,
                                  room ? f->out + f->count : NULL, room);
    if (found > SIZE_MAX - f->count) { errno = EOVERFLOW; return false; }
    f->count += found; return true;
}
size_t lp_follows(const uint8_t *ewkb, size_t len, const lp_id *phrase, size_t np, lp_id *out, size_t cap){
    if (!np || !phrase) return 0;
    GeometryFollows f = { .phrase = phrase, .np = np, .out = out, .cap = out ? cap : 0 };
    bool ok = lp_ewkb_visit(ewkb, len, follow_span, &f, NULL);
    lp_buf_free(&f.converted);
    return ok ? f.count : 0;
}
