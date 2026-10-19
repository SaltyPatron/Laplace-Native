#ifndef LAPLACE_GEOMETRY_H
#define LAPLACE_GEOMETRY_H

#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { LP_POINT = 1, LP_LINESTRING, LP_POLYGON, LP_MULTIPOINT,
       LP_MULTILINESTRING, LP_MULTIPOLYGON, LP_GEOMETRYCOLLECTION };
#define LP_EWKB_MAX_DEPTH 64u

typedef struct {
    uint32_t type, srid;
    bool has_srid;
    unsigned depth;
    size_t vertices, parts;
} lp_ewkb_info;

/* A point, line or polygon ring, read where it lies. Parts never bridge rings or collection members. */
typedef struct {
    const uint8_t *data;
    size_t count;
    bool little_endian;
} lp_ewkb_span;
typedef bool (*lp_ewkb_span_fn)(void *context, const lp_ewkb_span *span);

/* Validate the complete input before invoking a visitor. Accept both byte orders, optional SRIDs, EWKB Z/M
 * flags and ISO ZM type numbers. Reject truncated bodies, trailing bytes, wrong multi-member types and
 * nesting beyond LP_EWKB_MAX_DEPTH. No allocations. Empty geometries are valid and visit no vertices.
 * A visitor returning false stops traversal and makes this function return false. */
LP_API bool lp_ewkb_visit(const uint8_t *ewkb, size_t len, lp_ewkb_span_fn visitor,
                         void *context, lp_ewkb_info *info);
LP_API bool lp_ewkb_vertex(const lp_ewkb_span *span, size_t index, double xyzm[4]);

/* NULL output measures the required vertex capacity. Otherwise capacity must hold every vertex; on success
 * count is the number of distinct IDs, sorted in memcmp order. M and spare bits never enter a key. */
LP_API bool lp_ewkb_keys(const uint8_t *ewkb, size_t len, lp_id *out, size_t capacity, size_t *count);

/* Writers return required bytes without touching a short output, or zero on invalid/overflowing input.
 * New headers are little-endian EWKB ZM; collection members retain their encoded bytes and byte order.
 * NULL srid omits the SRID field. Coordinate and child inputs must not
 * overlap output. NULL coordinates for a point write POINT EMPTY. */
typedef struct { const double *xyzm; size_t count; } lp_ewkb_ring;
typedef struct { const uint8_t *data; size_t len; } lp_ewkb_slice;
LP_API size_t lp_ewkb_point_zm(const double xyzm[4], const uint32_t *srid, uint8_t *out, size_t cap);
LP_API size_t lp_ewkb_line_zm(const double *xyzm, size_t count, const uint32_t *srid, uint8_t *out, size_t cap);
LP_API size_t lp_ewkb_polygon_zm(const lp_ewkb_ring *rings, size_t count, const uint32_t *srid,
                               uint8_t *out, size_t cap);
LP_API size_t lp_ewkb_collection_zm(uint32_t type, const lp_ewkb_slice *members, size_t count,
                                  const uint32_t *srid, uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
