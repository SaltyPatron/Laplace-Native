/* IDs written into geometry, and EWKB physicality paths. */
#include "laplace/geometry.h"
#include "internal.h"
#include <errno.h>
#include <string.h>

#define EWKB_Z     0x80000000u
#define EWKB_M     0x40000000u
#define EWKB_SRID  0x20000000u
#define EXP_BITS   (1021ull << 52)                      /* exponent -2: [0.25, 0.5) */
#define MASK43     ((1ull << 43) - 1)
#define MASK42     ((1ull << 42) - 1)
/* The 128-bit ID is 43 + 43 + 42 bits of the X, Y and Z mantissas; the 9 + 9 + 10 bits above them are the vertex's
 * spare bits (Identity: values from known lists, a small tag saying which layout). An ID is read through its own bits
 * only, so a value in the spare bits never changes which entity a vertex is. */
const uint64_t lp_xyz_id_mask[3] = { EXP_BITS | MASK43, EXP_BITS | MASK43, EXP_BITS | MASK42 };

void lp_id_to_xyz(const lp_id *id, double xyz[3]){
    unsigned __int128 v = 0;
    for (int i = 15; i >= 0; i--) v = (v << 8) | id->b[i];
    uint64_t part[3] = { (uint64_t)(v & MASK43), (uint64_t)((v >> 43) & MASK43), (uint64_t)(v >> 86) };
    for (int k = 0; k < 3; k++) { uint64_t bits = EXP_BITS | part[k]; memcpy(&xyz[k], &bits, 8); }
}

void lp_xyz_to_id(const double xyz[3], lp_id *out){
    uint64_t part[3];
    static const uint64_t own[3] = { MASK43, MASK43, MASK42 };
    for (int k = 0; k < 3; k++) { uint64_t bits; memcpy(&bits, &xyz[k], 8); part[k] = bits & own[k]; }
    unsigned __int128 v = (unsigned __int128)part[0] | ((unsigned __int128)part[1] << 43) | ((unsigned __int128)part[2] << 86);
    for (int i = 0; i < 16; i++) { out->b[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

/* The 28 spare bits: bits 0-8 above X's 43, 9-17 above Y's 43, 18-27 above Z's 42. */
uint32_t lp_xyz_spare(const double xyz[3]){
    uint64_t b[3]; memcpy(b, xyz, 24);
    return (uint32_t)(((b[0] >> 43) & 0x1FF) | (((b[1] >> 43) & 0x1FF) << 9) | (((b[2] >> 42) & 0x3FF) << 18));
}
void lp_xyz_spare_set(double xyz[3], uint32_t v){
    uint64_t b[3]; memcpy(b, xyz, 24);
    b[0] = (b[0] & ~(0x1FFull << 43)) | ((uint64_t)(v & 0x1FF) << 43);
    b[1] = (b[1] & ~(0x1FFull << 43)) | ((uint64_t)((v >> 9) & 0x1FF) << 43);
    b[2] = (b[2] & ~(0x3FFull << 42)) | ((uint64_t)((v >> 18) & 0x3FF) << 42);
    memcpy(xyz, b, 24);
}
static uint8_t *put32(uint8_t *p, uint32_t v){
    for (int i = 0; i < 4; i++) { *p++ = (uint8_t)v; v >>= 8; }
    return p;
}
static uint8_t *putd(uint8_t *p, double v){
    uint64_t bits; memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; i++) { *p++ = (uint8_t)bits; bits >>= 8; }
    return p;
}

typedef struct {
    const uint8_t *data;
    size_t len, at, vertices, parts;
    unsigned depth;
    lp_ewkb_span_fn visit;
    void *context;
} GeometryReader;

static bool read32(GeometryReader *r, bool little, uint32_t *out){
    if (r->len - r->at < 4) return false;
    const uint8_t *p = r->data + r->at; r->at += 4;
    *out = little ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24
                  : (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
    return true;
}

static uint64_t read64(const uint8_t *p, bool little){
    uint64_t bits = 0;
    if (little) for (int i = 7; i >= 0; i--) bits = (bits << 8) | p[i];
    else for (int i = 0; i < 8; i++) bits = (bits << 8) | p[i];
    return bits;
}

bool lp_ewkb_vertex(const lp_ewkb_span *span, size_t index, double xyzm[4]){
    if (!span || !span->data || !xyzm || index >= span->count || index > SIZE_MAX / LP_VERTEX_BYTES) return false;
    const uint8_t *p = span->data + index * LP_VERTEX_BYTES;
    for (int d = 0; d < 4; d++) { uint64_t bits = read64(p + 8 * d, span->little_endian); memcpy(xyzm + d, &bits, 8); }
    return true;
}

static bool read_header(GeometryReader *r, lp_ewkb_info *info, bool *little){
    if (r->at == r->len || r->data[r->at] > 1) return false;
    *little = r->data[r->at++] != 0;
    uint32_t raw;
    if (!read32(r, *little, &raw) || (raw & UINT32_C(0x10000000))) return false;
    uint32_t type = raw & UINT32_C(0x0fffffff), dimension = type / 1000;
    if (dimension > 3) return false;
    bool z = (raw & EWKB_Z) || dimension == 1 || dimension == 3;
    bool m = (raw & EWKB_M) || dimension == 2 || dimension == 3;
    type %= 1000;
    if (!z || !m || type < LP_POINT || type > LP_GEOMETRYCOLLECTION) return false;
    *info = (lp_ewkb_info){ .type = type, .has_srid = (raw & EWKB_SRID) != 0 };
    return !info->has_srid || read32(r, *little, &info->srid);
}

static bool read_span(GeometryReader *r, uint32_t count, bool little, bool point){
    if (count > (r->len - r->at) / LP_VERTEX_BYTES || count > SIZE_MAX - r->vertices) return false;
    lp_ewkb_span span = { r->data + r->at, count, little };
    r->at += (size_t)count * LP_VERTEX_BYTES;
    if (point) {
        bool empty = true;
        for (int d = 0; d < 4; d++) {
            uint64_t bits = read64(span.data + 8 * d, little);
            empty = empty && (bits & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)
                          && (bits & UINT64_C(0x000fffffffffffff)) != 0;
        }
        if (empty) return true;
    }
    if (!count) return true;
    r->vertices += count; r->parts++;
    return !r->visit || r->visit(r->context, &span);
}

static bool read_geometry(GeometryReader *r, unsigned depth, uint32_t expected, lp_ewkb_info *root){
    lp_ewkb_info info; bool little;
    if (depth >= LP_EWKB_MAX_DEPTH || !read_header(r, &info, &little) || (expected && info.type != expected)) return false;
    if (r->depth < depth + 1) r->depth = depth + 1;
    if (root) *root = info;
    if (info.type == LP_POINT) return read_span(r, 1, little, true);
    uint32_t count;
    if (!read32(r, little, &count)) return false;
    if (info.type == LP_LINESTRING) return read_span(r, count, little, false);
    if (info.type == LP_POLYGON) {
        if (count > (r->len - r->at) / 4) return false;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t vertices;
            if (!read32(r, little, &vertices) || !read_span(r, vertices, little, false)) return false;
        }
        return true;
    }
    if (count > (r->len - r->at) / 5) return false;
    uint32_t child = info.type == LP_GEOMETRYCOLLECTION ? 0 : info.type - 3;
    for (uint32_t i = 0; i < count; i++) if (!read_geometry(r, depth + 1, child, NULL)) return false;
    return true;
}

bool lp_ewkb_visit(const uint8_t *ewkb, size_t len, lp_ewkb_span_fn visitor, void *context, lp_ewkb_info *info){
    if (info) memset(info, 0, sizeof *info);
    if (!ewkb || !len) { errno = EINVAL; return false; }
    GeometryReader r = { .data = ewkb, .len = len }; lp_ewkb_info root;
    if (!read_geometry(&r, 0, 0, &root) || r.at != len) { errno = EINVAL; return false; }
    root.vertices = r.vertices; root.parts = r.parts; root.depth = r.depth;
    if (info) *info = root;
    if (visitor) {
        r.at = r.vertices = r.parts = 0; r.visit = visitor; r.context = context;
        if (!read_geometry(&r, 0, 0, NULL)) return false;
    }
    return true;
}

typedef struct { lp_id *out; size_t count; } GeometryKeys;
static bool collect_keys(void *context, const lp_ewkb_span *span){
    GeometryKeys *keys = context;
    for (size_t i = 0; i < span->count; i++) {
        double xyzm[4]; lp_ewkb_vertex(span, i, xyzm); lp_xyz_to_id(xyzm, keys->out + keys->count++);
    }
    return true;
}

bool lp_ewkb_keys(const uint8_t *ewkb, size_t len, lp_id *out, size_t capacity, size_t *count){
    if (!count) { errno = EINVAL; return false; }
    *count = 0; lp_ewkb_info info;
    if (!lp_ewkb_visit(ewkb, len, NULL, NULL, &info)) return false;
    *count = info.vertices;
    if (!out) return true;
    if (capacity < info.vertices) { errno = ENOSPC; return false; }
    GeometryKeys keys = { out, 0 };
    if (!lp_ewkb_visit(ewkb, len, collect_keys, &keys, NULL)) return false;
    *count = lp_ids_unique(out, keys.count);
    return true;
}

static uint8_t *write_header(uint8_t *p, uint32_t type, const uint32_t *srid){
    *p++ = 1; p = put32(p, type | EWKB_Z | EWKB_M | (srid ? EWKB_SRID : 0));
    return srid ? put32(p, *srid) : p;
}
static uint8_t *write_points(uint8_t *p, const double *xyzm, size_t n){
    for (size_t i = 0; i < n; i++) for (int d = 0; d < 4; d++) p = putd(p, xyzm[4 * i + d]);
    return p;
}

size_t lp_ewkb_point_zm(const double xyzm[4], const uint32_t *srid, uint8_t *out, size_t cap){
    size_t need = (srid ? 9u : 5u) + LP_VERTEX_BYTES;
    if (!out || cap < need) return need;
    uint8_t *p = write_header(out, LP_POINT, srid);
    double empty[4];
    if (!xyzm) { uint64_t nan = UINT64_C(0x7ff8000000000000); for (int d = 0; d < 4; d++) memcpy(empty + d, &nan, 8); xyzm = empty; }
    write_points(p, xyzm, 1); return need;
}

size_t lp_ewkb_line_zm(const double *xyzm, size_t count, const uint32_t *srid, uint8_t *out, size_t cap){
    size_t header = (srid ? 9u : 5u) + 4u;
    if ((count && !xyzm) || count > UINT32_MAX || count > (SIZE_MAX - header) / LP_VERTEX_BYTES) return 0;
    size_t need = header + count * LP_VERTEX_BYTES;
    if (!out || cap < need) return need;
    uint8_t *p = put32(write_header(out, LP_LINESTRING, srid), (uint32_t)count);
    write_points(p, xyzm, count); return need;
}

size_t lp_ewkb_polygon_zm(const lp_ewkb_ring *rings, size_t count, const uint32_t *srid, uint8_t *out, size_t cap){
    if ((count && !rings) || count > UINT32_MAX) return 0;
    size_t need = (srid ? 9u : 5u) + 4u;
    for (size_t i = 0; i < count; i++) {
        if ((rings[i].count && !rings[i].xyzm) || rings[i].count > UINT32_MAX || need > SIZE_MAX - 4
            || rings[i].count > (SIZE_MAX - need - 4) / LP_VERTEX_BYTES) return 0;
        need += 4 + rings[i].count * LP_VERTEX_BYTES;
    }
    if (!out || cap < need) return need;
    uint8_t *p = put32(write_header(out, LP_POLYGON, srid), (uint32_t)count);
    for (size_t i = 0; i < count; i++) p = write_points(put32(p, (uint32_t)rings[i].count), rings[i].xyzm, rings[i].count);
    return need;
}

size_t lp_ewkb_collection_zm(uint32_t type, const lp_ewkb_slice *members, size_t count,
                            const uint32_t *srid, uint8_t *out, size_t cap){
    if (type < LP_MULTIPOINT || type > LP_GEOMETRYCOLLECTION || (count && !members) || count > UINT32_MAX) return 0;
    size_t need = (srid ? 9u : 5u) + 4u;
    for (size_t i = 0; i < count; i++) {
        lp_ewkb_info child;
        if (!lp_ewkb_visit(members[i].data, members[i].len, NULL, NULL, &child)
            || child.depth >= LP_EWKB_MAX_DEPTH || (type != LP_GEOMETRYCOLLECTION && child.type != type - 3)
            || members[i].len > SIZE_MAX - need) return 0;
        need += members[i].len;
    }
    if (!out || cap < need) return need;
    uint8_t *p = put32(write_header(out, type, srid), (uint32_t)count);
    for (size_t i = 0; i < count; i++) { memcpy(p, members[i].data, members[i].len); p += members[i].len; }
    return need;
}

size_t lp_ewkb_path(const lp_id *ch, size_t n, uint8_t *out, size_t cap){
    if (!n || !ch || n > SIZE_MAX / sizeof *ch) return 0;
    const size_t max_run = (size_t)((UINT64_C(1) << LP_M_RUN_BITS) - 1);
    size_t runs = 0;
    for (size_t i = 0; i < n; ) {
        size_t j = i + 1;
        while (j < n && j - i < max_run && !memcmp(&ch[j], &ch[i], 16)) j++;
        if (++runs > UINT32_MAX || runs > (SIZE_MAX - 9) / LP_VERTEX_BYTES) return 0;
        i = j;
    }
    size_t need = runs == 1 ? 5 + 32 : 9 + 32 * runs;
    if (!out || cap < need || n == 0) return n == 0 ? 0 : need;
    uint8_t *p = out; *p++ = 1;                                                 /* little-endian */
    p = put32(p, (runs == 1 ? 1u : 2u) | EWKB_Z | EWKB_M);
    if (runs != 1) p = put32(p, (uint32_t)runs);
    for (size_t i = 0; i < n; ) {
        size_t j = i + 1; while (j < n && j - i < max_run && !memcmp(&ch[j], &ch[i], 16)) j++;
        double xyz[3]; lp_id_to_xyz(&ch[i], xyz);
        p = putd(p, xyz[0]); p = putd(p, xyz[1]); p = putd(p, xyz[2]); p = putd(p, (double)(j - i));
        i = j;
    }
    return need;
}

size_t lp_ewkb_vertices(const uint8_t *e, size_t len, const uint8_t **vertices){
    if (!vertices) return 0;
    *vertices = NULL; lp_ewkb_info info;
    const uint16_t endian = 1;
    if (!lp_ewkb_visit(e, len, NULL, NULL, &info) || e[0] != *(const uint8_t *)&endian
        || (info.type != LP_POINT && info.type != LP_LINESTRING) || !info.vertices) return 0;
    *vertices = e + (info.has_srid ? 9 : 5) + (info.type == LP_LINESTRING ? 4 : 0);
    return info.vertices;
}

size_t lp_ewkb_runs(const lp_id *ids, const uint64_t *m, size_t nv, uint8_t *out, size_t cap){ return lp_ewkb_runs_spare(ids, m, NULL, nv, out, cap); }
size_t lp_ewkb_runs_spare(const lp_id *ids, const uint64_t *m, const uint32_t *spare, size_t nv, uint8_t *out, size_t cap){
    if (!nv || !ids || !m || nv > UINT32_MAX || nv > (SIZE_MAX - 9) / LP_VERTEX_BYTES) return 0;
    for (size_t i = 0; i < nv; i++)
        if (m[i] > (UINT64_C(1) << 53) - 1 || (spare && spare[i] >> LP_SPARE_BITS)) return 0;
    size_t need = nv == 1 ? 5 + 32 : 9 + 32 * nv;
    if (!out || cap < need || nv == 0) return nv == 0 ? 0 : need;
    uint8_t *p = out; *p++ = 1;
    p = put32(p, (nv == 1 ? 1u : 2u) | EWKB_Z | EWKB_M);
    if (nv != 1) p = put32(p, (uint32_t)nv);
    for (size_t i = 0; i < nv; i++) {
        double xyz[3]; lp_id_to_xyz(&ids[i], xyz); if (spare && spare[i]) lp_xyz_spare_set(xyz, spare[i]);
        p = putd(p, xyz[0]); p = putd(p, xyz[1]); p = putd(p, xyz[2]); p = putd(p, (double)m[i]);
    }
    return need;
}

static bool enrich_vertex(lp_path path, size_t at, lp_vertex *vertex){
    lp_path_decode(lp_path_block(path.v + at * LP_VERTEX_BYTES, 1), vertex, 1);
    double packed; memcpy(&packed, path.v + at * LP_VERTEX_BYTES + 24, 8);
    double xyz[3]; lp_id_to_xyz(&vertex->id, xyz); lp_xyz_spare_set(xyz, vertex->spare);
    return !memcmp(xyz,path.v + at * LP_VERTEX_BYTES,sizeof xyz)
        && packed == (double)lp_m_full(vertex->run, vertex->said, vertex->outcome, vertex->position);
}
static uint8_t *enrich_put(uint8_t *out, const lp_vertex *vertex){
    double xyz[3]; lp_id_to_xyz(&vertex->id, xyz); lp_xyz_spare_set(xyz, vertex->spare);
    for (int axis = 0; axis < 3; axis++) out = putd(out, xyz[axis]);
    return putd(out, (double)lp_m_full(vertex->run, vertex->said, vertex->outcome, vertex->position));
}
/* Walk counted sequences together, including differently split equal runs.
 * Only an absent said annotation is enriched. A different outcome is never
 * treated as absent: zero is an affirmation, not an unknown outcome. */
static size_t enrich_vertices(lp_path a, lp_path b, uint8_t *out){
    size_t ai = 0, bi = 0, count = 0; lp_vertex av = {0}, bv = {0}, pending = {0};
    while (ai < a.n || av.run || bi < b.n || bv.run) {
        if (!av.run && ai < a.n && !enrich_vertex(a, ai++, &av)) return 0;
        if (!bv.run && bi < b.n && !enrich_vertex(b, bi++, &bv)) return 0;
        if (!av.run || !bv.run || !lp_id_eq(&av.id, &bv.id)
            || (av.said && bv.said && av.said != bv.said)
            || av.spare != bv.spare || av.outcome != bv.outcome || av.position != bv.position) return 0;
        lp_vertex next = av; next.run = av.run < bv.run ? av.run : bv.run;
        if (!next.said) next.said = bv.said;
        av.run -= next.run; bv.run -= next.run;
        if (pending.run && lp_id_eq(&pending.id, &next.id) && pending.said == next.said
            && pending.spare == next.spare && pending.outcome == next.outcome && pending.position == next.position
            && (uint64_t)pending.run + next.run < (UINT64_C(1) << LP_M_RUN_BITS)) pending.run += next.run;
        else {
            if (pending.run) { count++; if (out) out = enrich_put(out, &pending); }
            pending = next;
        }
    }
    if (pending.run) { count++; if (out) enrich_put(out, &pending); }
    return count;
}
size_t lp_ewkb_enrich_path(lp_path a, lp_path b, uint8_t *out, size_t cap){
    if (!a.n || !b.n) return 0;
    size_t count = enrich_vertices(a, b, NULL);
    if (!count || count > UINT32_MAX || count > (SIZE_MAX - 9) / LP_VERTEX_BYTES) return 0;
    size_t need = count == 1 ? 5 + LP_VERTEX_BYTES : 9 + count * LP_VERTEX_BYTES;
    if (!out || cap < need) return need;
    uint8_t *p = out; *p++ = 1;
    p = put32(p, (count == 1 ? LP_POINT : LP_LINESTRING) | EWKB_Z | EWKB_M);
    if (count != 1) p = put32(p, (uint32_t)count);
    enrich_vertices(a, b, p);
    return need;
}

size_t lp_ewkb_point4(const double c[4], uint8_t *out, size_t cap){
    return lp_ewkb_point_zm(c, NULL, out, cap);
}
