/* Laplace-Native: the shared native library of Laplace.
 *
 * Identity, UTF-8, tier 0, fixed-point coordinates, composition, text decomposition (UAX #29), geometry packing,
 * Hilbert order, trajectory matching, shape measures, consensus (Glicko-2 with signed trust), and the pull's search
 * kernels. Laplace-Engine and Laplace-postgres call it and keep no copy of any of it. Every function here is
 * deterministic: the same inputs give the same bits on every CPU, whichever SIMD path runs. */
#ifndef LAPLACE_H
#define LAPLACE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define LP_API __declspec(dllexport)
#else
#  define LP_API __attribute__((visibility("default")))
#endif

/* ---------------------------------------------------------------- constants */
#define LP_NCP        1114112u               /* codepoints in the Unicode codespace */
#define LP_FIXED_ONE  9007199254740992.0     /* 2^53: a coordinate m has the value m / 2^53 */

/* ---------------------------------------------------------------- CPU features and dispatch */
enum {
    LP_CPU_SSE2    = 1u << 0,
    LP_CPU_SSE41   = 1u << 1,
    LP_CPU_AVX2    = 1u << 2,                /* with FMA and BMI2: x86-64-v3 */
    LP_CPU_AVX512  = 1u << 3,                /* F, BW, VL, DQ: x86-64-v4 */
    LP_CPU_VNNI512 = 1u << 4,                /* AVX-512 VNNI */
    LP_CPU_AVXVNNI = 1u << 5,                /* AVX-VNNI (256-bit) */
    LP_CPU_AMX     = 1u << 6
};
LP_API uint32_t    lp_cpu_features(void);    /* what this CPU supports */
LP_API uint32_t    lp_cpu_active(void);      /* what the dispatcher uses (LAPLACE_ISA may lower it) */
LP_API const char *lp_cpu_describe(uint32_t features);

/* ---------------------------------------------------------------- memory: one way to grow, one allocator
 * Working memory (arrays, maps, buffers) comes from lp_realloc and goes back through lp_free, which a host may point at
 * its own allocator: the PostgreSQL extension points them at its memory contexts, so an error frees what a call held.
 * What lives as long as the process (the tables built over tier 0 and the highway) is never working memory. */
typedef void *(*lp_realloc_fn)(void *p, size_t n);
typedef void  (*lp_free_fn)(void *p);
LP_API void  lp_allocator(lp_realloc_fn grow, lp_free_fn release);   /* NULL, NULL: the C library's */
LP_API void *lp_realloc(void *p, size_t n);                          /* never NULL: an allocation that fails aborts */
LP_API void  lp_free(void *p);
static inline void *lp_alloc(size_t n){ return lp_realloc(NULL, n); }
static inline void *lp_zalloc(size_t n){ void *p = lp_realloc(NULL, n); memset(p, 0, n); return p; }
/* Room for at least need elements of size bytes in *p, whose room is *cap: doubled as it fills. Returns *p. */
LP_API void *lp_reserve(void **p, size_t *cap, size_t need, size_t size);

/* A growable array of any type, the one way an array grows in Laplace:
 *   lp_vec(lp_id) ids = { 0 };  lp_push(&ids, id);  ids.v[i] for i < ids.n;  lp_vec_free(&ids); */
#define lp_vec(T)            struct { T *v; size_t n, cap; }
#define lp_vec_reserve(a, k) lp_reserve((void **)&(a)->v, &(a)->cap, (k), sizeof *(a)->v)
#define lp_push(a, ...)      (lp_vec_reserve((a), (a)->n + 1), (a)->v[(a)->n++] = (__VA_ARGS__))
#define lp_vec_add(a)        (lp_vec_reserve((a), (a)->n + 1), memset(&(a)->v[(a)->n], 0, sizeof *(a)->v), &(a)->v[(a)->n++])
#define lp_vec_free(a)       (lp_free((a)->v), (a)->v = NULL, (a)->n = (a)->cap = 0)

/* Bytes as they are written for someone else to read: a file, the wire, a COPY stream. */
typedef struct { uint8_t *b; size_t n, cap; } lp_buf;
LP_API uint8_t *lp_buf_room(lp_buf *, size_t k);                    /* k more bytes at the end: where they go */
static inline void lp_buf_put(lp_buf *b, const void *p, size_t k){ if (k) memcpy(lp_buf_room(b, k), p, k); }
static inline void lp_buf_free(lp_buf *b){ lp_free(b->b); b->b = NULL; b->n = b->cap = 0; }

/* Big-endian, as the database writes and reads every binary value. */
static inline uint64_t lp_be(const void *p, int n){ const uint8_t *b = (const uint8_t *)p; uint64_t u = 0; for (int i = 0; i < n; i++) u = u << 8 | b[i]; return u; }
static inline double   lp_be_f64(const void *p){ uint64_t u = lp_be(p, 8); double d; memcpy(&d, &u, 8); return d; }
static inline void     lp_put_be(uint8_t *p, uint64_t v, int n){ for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static inline void     lp_buf_be(lp_buf *b, uint64_t v, int n){ lp_put_be(lp_buf_room(b, (size_t)n), v, n); }
static inline void     lp_buf_be_f64(lp_buf *b, double d){ uint64_t u; memcpy(&u, &d, 8); lp_buf_be(b, u, 8); }
static inline void     lp_buf_be_f32(lp_buf *b, float f){ uint32_t u; memcpy(&u, &f, 4); lp_buf_be(b, u, 4); }

/* Hexadecimal: 2n lowercase digits and a NUL; and back, false on anything but 2n hexadecimal digits. */
LP_API void lp_hex(const void *bytes, size_t n, char *out);
LP_API bool lp_unhex(const char *s, size_t n, void *out);

/* Sorting that gives the same order on every platform: stable, so what compares equal keeps the order it came in
 * (the C library's qsort promises neither, and differs between releases). */
LP_API void lp_sort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

/* ---------------------------------------------------------------- identity */
typedef struct { uint8_t b[16]; } lp_id;
static inline bool lp_id_eq(const lp_id *a, const lp_id *b){ return !memcmp(a->b, b->b, 16); }
static inline int  lp_id_cmp(const void *a, const void *b){ return memcmp(a, b, 16); }      /* byte order: the database's */
static inline void lp_id_hex(const lp_id *id, char out[33]){ lp_hex(id->b, 16, out); }
static inline bool lp_id_unhex(const char *s, lp_id *out){ return lp_unhex(s, 16, out->b); }

/* A codepoint's ID: BLAKE3-128 of its UTF-8 bytes; surrogates are written in the generalized 3-byte form. */
LP_API void lp_id_codepoint(uint32_t cp, lp_id *out);
/* A composition's ID: BLAKE3-128 of its children's IDs in order, repeats included. One child is that child. */
LP_API void lp_id_compose(const lp_id *children, size_t n, lp_id *out);
/* The ID of a UTF-8 string taken as one composition of its codepoints. Returns false on invalid UTF-8. */
LP_API bool lp_id_codepoints_utf8(const char *s, size_t len, lp_id *out);

/* ---------------------------------------------------------------- UTF-8 */
/* A codepoint's UTF-8 bytes; surrogates are written in the generalized 3-byte form. Returns the length, 1 to 4. */
LP_API size_t lp_utf8_put(uint32_t cp, uint8_t out[4]);
/* The codepoint at s[*i], advancing *i past it. Returns false, leaving *i, on a malformed sequence or a value outside
 * the codespace. */
LP_API bool lp_utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp);

/* ---------------------------------------------------------------- fixed-point coordinates */
typedef struct { int64_t m[4]; } lp_coord;   /* value = m / 2^53 on each axis */

/* The exact integer average of n coordinates, truncated toward zero. */
LP_API void lp_coord_centroid(const lp_coord *c, size_t n, lp_coord *out);
/* The same average taken a coordinate at a time: four 128-bit sums, each divided once when the mean is asked for. Every
 * centroid in Laplace is this one. */
typedef struct { __int128 s[4]; uint64_t n; } lp_coord_sum;
static inline void lp_coord_add(lp_coord_sum *a, const int64_t m[4]){ for (int d = 0; d < 4; d++) a->s[d] += m[d]; a->n++; }
LP_API void lp_coord_mean(const lp_coord_sum *a, lp_coord *out);         /* 0 when nothing was added */
/* Inside the wall: m . m <= 2^106, exactly. */
LP_API bool lp_coord_inside(const lp_coord *c);
/* 4D Hilbert value on a 16-bit grid over [-1, 1]^4 (Skilling). */
LP_API uint64_t lp_hilbert4(const lp_coord *c);
LP_API uint64_t lp_hilbert4_grid(const uint32_t g[4]);
/* The Hilbert value as it is stored: its top bit flipped, so bigint order is Hilbert order. */
static inline int64_t lp_hilbert_key(uint64_t h){ return (int64_t)(h ^ 0x8000000000000000ull); }
/* A coordinate as the four doubles it is, m / 2^53; and back, false when a double is not on the fixed-point grid (or
 * outside [-1, 1]). */
static inline void lp_coord_xyzm(const lp_coord *c, double out[4]){ for (int d = 0; d < 4; d++) out[d] = (double)c->m[d] / LP_FIXED_ONE; }
LP_API bool lp_coord_of_xyzm(const double x[4], lp_coord *out);
/* A coordinate from doubles that need not lie on the grid: each truncated toward zero onto it, held to [-1, 1]. */
LP_API void lp_coord_trunc(const double x[4], lp_coord *out);

/* ---------------------------------------------------------------- IDs written into geometry */
/* An ID's 128 bits go into the X, Y, Z mantissas (43 + 43 + 42 bits) with exponent -2, so the three values lie in
 * [0.25, 0.5), inside the 4-ball. */
LP_API void lp_id_to_xyz(const lp_id *id, double xyz[3]);
LP_API void lp_xyz_to_id(const double xyz[3], lp_id *out);

/* EWKB for a physicality path: a POINT ZM when there is one run, else a LINESTRING ZM, one vertex per run of
 * identical children with the run length in M. Returns the bytes written, or the bytes needed if cap is too small. */
LP_API size_t lp_ewkb_path(const lp_id *children, size_t n, uint8_t *out, size_t cap);
/* The same path from runs already collapsed: vertex i is ids[i] repeated runs[i] times. */
LP_API size_t lp_ewkb_runs(const lp_id *ids, const uint64_t *m, size_t nv, uint8_t *out, size_t cap);   /* m: each vertex's M bits as written (lp_m_of) */
/* A POINT ZM of real 4D coordinates, as EWKB (37 bytes). */
LP_API size_t lp_ewkb_point4(const double xyzm[4], uint8_t *out, size_t cap);
static inline size_t lp_ewkb_coord(const lp_coord *c, uint8_t *out, size_t cap){ double x[4]; lp_coord_xyzm(c, x); return lp_ewkb_point4(x, out, cap); }
/* Parse a POINT ZM / LINESTRING ZM path (little-endian EWKB, optional SRID). Returns the vertex count and points
 * *vertices at the first vertex (32 bytes each: X, Y, Z, M), or 0 on malformed input. */
LP_API size_t lp_ewkb_vertices(const uint8_t *ewkb, size_t len, const uint8_t **vertices);

/* ---------------------------------------------------------------- trajectory matching */
/* Every place the phrase occurs as a run inside the path (vertices expanded by run length), the ID of the vertex
 * that follows it. Matching compares raw vertex bytes; only continuations are decoded. Returns the number of
 * continuations found; at most cap are written. */
LP_API size_t lp_follows(const uint8_t *ewkb, size_t len, const lp_id *phrase, size_t np, lp_id *out, size_t cap);

/* ---------------------------------------------------------------- paths, as every reader meets them
 * A path is its vertex block: n vertices of 32 bytes, X, Y, Z (a constituent's ID, packed) and M (its run, and what it
 * is said to be). EWKB carries the block after a header; PostGIS's own serialization carries the same block, so a
 * reader inside the database hands it over as it lies, and nothing is copied to be read. Every operation on a path's
 * constituents is one of these, for the engine, the extension and every tool alike. */
typedef struct { const uint8_t *v; size_t n; } lp_path;
typedef struct { lp_id id; uint32_t run, said; } lp_vertex;
LP_API lp_path lp_path_of(const uint8_t *ewkb, size_t len);                  /* n = 0 when it is no path */
static inline lp_path lp_path_block(const void *xyzm, size_t n){ lp_path p = { (const uint8_t *)xyzm, n }; return p; }
LP_API lp_id    lp_path_id(lp_path, size_t i);                               /* vertex i's ID */
LP_API uint32_t lp_path_run(lp_path, size_t i);                              /* how many times vertex i stands */
LP_API size_t   lp_path_len(lp_path);                                        /* its constituents, each run written out */
/* Its constituents in order, each run written out: returns how many there are; at most cap are written. */
LP_API size_t lp_path_expand(lp_path, lp_id *out, size_t cap);
/* Its vertices as stored: ID, run and what each is said to be. Returns the vertex count; at most cap are written. */
LP_API size_t lp_path_decode(lp_path, lp_vertex *out, size_t cap);
/* The distinct IDs it holds, in byte order (the keys of the container index): out holds n; returns how many. */
LP_API size_t lp_path_keys(lp_path, lp_id *out);
/* Whether it holds every one of ids (all) or any of them. */
LP_API bool   lp_path_holds(lp_path, const lp_id *ids, size_t n, bool all);
/* How many times it holds each of ids, runs counted: ids sorted and distinct (lp_ids_unique); times[i] for ids[i]. */
LP_API void   lp_path_times(lp_path, const lp_id *ids, size_t n, uint64_t *times);
/* Whether any of ids stands between its first constituent and its last, runs counted as what they repeat: the place a
 * claim's predicate takes. */
LP_API bool   lp_path_middle_any(lp_path, const lp_id *ids, size_t n);
/* Every place the phrase occurs as a run inside it, the ID of the constituent that follows: lp_follows on a path. */
LP_API size_t lp_path_follows(lp_path, const lp_id *phrase, size_t np, lp_id *out, size_t cap);

/* The same, from EWKB as the database sends it. */
LP_API size_t lp_path_ids(const uint8_t *ewkb, size_t len, lp_id *out, size_t cap);
LP_API size_t lp_path_vertices(const uint8_t *ewkb, size_t len, lp_vertex *out, size_t cap);

/* IDs sorted in byte order and each kept once, in place: returns how many are left. */
LP_API size_t lp_ids_unique(lp_id *ids, size_t n);

/* ---------------------------------------------------------------- tuples: what a claim's parts are
 * A claim is a tuple of entities: its first part and its last are what it ties together, and what stands between them
 * names the tie. */
/* The other end of a tuple from `at`: the last part's index when at is the first, the first's when at is the last; -1
 * when at is neither end, or both ends are at. */
LP_API int  lp_tuple_other(const lp_id *part, size_t n, const lp_id *at);
/* Whether any of ids stands between the first part and the last. */
LP_API bool lp_tuple_middle_any(const lp_id *part, size_t n, const lp_id *ids, size_t nids);

/* ---------------------------------------------------------------- maps keyed by ID
 * One open-addressing table for every set or map of IDs: an ID is a BLAKE3 hash, so its own bits are the hash (bytes 8
 * to 15, which nothing shards or partitions by). Entries keep the order they were added in, keys side by side, each
 * with a value of the size the map was made for, zeroed when it is added. */
typedef struct lp_idmap lp_idmap;
LP_API lp_idmap    *lp_idmap_new(void);                                      /* a 32-bit value per ID */
LP_API lp_idmap    *lp_idmap_sized(size_t value_bytes);                      /* any value: a count, a sum, a struct */
LP_API void         lp_idmap_free(lp_idmap *);
LP_API void         lp_idmap_clear(lp_idmap *);                              /* empty, its room kept */
LP_API size_t       lp_idmap_put(lp_idmap *, const lp_id *id, bool *fresh);  /* the entry's index; fresh: it was added */
LP_API int64_t      lp_idmap_find(const lp_idmap *, const lp_id *id);        /* the entry's index, or -1 */
LP_API size_t       lp_idmap_count(const lp_idmap *);
LP_API const lp_id *lp_idmap_key(const lp_idmap *, size_t i);
LP_API const lp_id *lp_idmap_keys(const lp_idmap *);                         /* every key, in the order added */
LP_API void        *lp_idmap_at(lp_idmap *, size_t i);                       /* entry i's value */
LP_API uint32_t    *lp_idmap_value(lp_idmap *, size_t i);                    /* entry i's value, in a map of 32-bit values */
/* An ID's value, added (zeroed) when it is new: *fresh, when given, says which. */
static inline void *lp_idmap_get(lp_idmap *m, const lp_id *id, bool *fresh){ return lp_idmap_at(m, lp_idmap_put(m, id, fresh)); }
static inline void *lp_idmap_lookup(lp_idmap *m, const lp_id *id){ int64_t i = lp_idmap_find(m, id); return i < 0 ? NULL : lp_idmap_at(m, (size_t)i); }

/* An index over records that hold their own IDs (tier 0, the highway): only their places are kept, never a copy. The
 * ID is at the start of each record of stride bytes. find gives the records with this ID one by one: *probe starts at
 * 0 and is advanced; -1 when there are no more. */
typedef struct lp_idindex lp_idindex;
LP_API lp_idindex *lp_idindex_build(const void *records, size_t n, size_t stride);
LP_API void        lp_idindex_free(lp_idindex *);
LP_API int64_t     lp_idindex_find(const lp_idindex *, const lp_id *id, uint64_t *probe);

/* A map keyed by bytes (a name, a source's key), each key kept once: the same table as lp_idmap, the key hashed. */
typedef struct lp_strmap lp_strmap;
LP_API lp_strmap   *lp_strmap_sized(size_t value_bytes);
LP_API void         lp_strmap_free(lp_strmap *);
LP_API size_t       lp_strmap_put(lp_strmap *, const void *key, size_t len, bool *fresh);
LP_API int64_t      lp_strmap_find(const lp_strmap *, const void *key, size_t len);
LP_API size_t       lp_strmap_count(const lp_strmap *);
LP_API const char  *lp_strmap_key(const lp_strmap *, size_t i, size_t *len);  /* NUL-terminated as well */
LP_API void        *lp_strmap_at(lp_strmap *, size_t i);
static inline void *lp_strmap_get(lp_strmap *m, const void *k, size_t len, bool *fresh){ return lp_strmap_at(m, lp_strmap_put(m, k, len, fresh)); }
static inline void *lp_strmap_lookup(lp_strmap *m, const void *k, size_t len){ int64_t i = lp_strmap_find(m, k, len); return i < 0 ? NULL : lp_strmap_at(m, (size_t)i); }
LP_API uint64_t     lp_hash_bytes(const void *p, size_t n);                  /* FNV-1a, 64 bits */

/* ---------------------------------------------------------------- PostgreSQL's binary forms, as bytes
 * What a client sends the database and reads back in binary: arrays of IDs, and COPY streams. Pure bytes, no libpq, so
 * every client writes them one way. */
/* An array of IDs (blake3[]) of the element type elem_oid. */
LP_API void   lp_pg_ids(lp_buf *, uint32_t elem_oid, const lp_id *ids, size_t n);
/* An array's header; its n elements follow, each lp_pg_elem (or a NULL, length -1). */
LP_API void   lp_pg_array(lp_buf *, uint32_t elem_oid, size_t n);
static inline void lp_pg_elem(lp_buf *b, const void *p, uint32_t len){ lp_buf_be(b, len, 4); lp_buf_put(b, p, len); }
/* The IDs of a blake3[] as the database sends it: returns how many it holds; at most cap are written. */
LP_API size_t lp_pg_ids_read(const uint8_t *a, size_t len, lp_id *out, size_t cap);

/* A binary COPY stream: rows go into the buffer and out through flush (PQputCopyData, or a file) whenever it holds
 * flush_at bytes. lp_copy_start writes the header, lp_copy_end the trailer and the last flush; flush returns 0 on
 * failure, and lp_copy_end returns whether every flush succeeded. */
typedef int (*lp_copy_flush_fn)(void *ctx, const uint8_t *bytes, size_t n);
typedef struct { lp_buf buf; size_t flush_at; lp_copy_flush_fn flush; void *ctx; uint64_t rows, bytes; bool failed; } lp_copy;
LP_API void lp_copy_start(lp_copy *, size_t flush_at, lp_copy_flush_fn, void *ctx);
LP_API void lp_copy_row(lp_copy *, uint16_t fields);                         /* a row of this many fields begins */
LP_API void lp_copy_field(lp_copy *, const void *p, uint32_t len);
LP_API bool lp_copy_end(lp_copy *);
static inline void lp_copy_null(lp_copy *c){ lp_buf_be(&c->buf, 0xFFFFFFFFu, 4); }
static inline void lp_copy_id(lp_copy *c, const lp_id *id){ lp_copy_field(c, id->b, 16); }
static inline void lp_copy_i16(lp_copy *c, int16_t v){ lp_buf_be(&c->buf, 2, 4); lp_buf_be(&c->buf, (uint16_t)v, 2); }
static inline void lp_copy_i32(lp_copy *c, int32_t v){ lp_buf_be(&c->buf, 4, 4); lp_buf_be(&c->buf, (uint32_t)v, 4); }
static inline void lp_copy_i64(lp_copy *c, int64_t v){ lp_buf_be(&c->buf, 8, 4); lp_buf_be(&c->buf, (uint64_t)v, 8); }
static inline void lp_copy_f32(lp_copy *c, float v){ lp_buf_be(&c->buf, 4, 4); lp_buf_be_f32(&c->buf, v); }
static inline void lp_copy_f64(lp_copy *c, double v){ lp_buf_be(&c->buf, 8, 4); lp_buf_be_f64(&c->buf, v); }

/* ---------------------------------------------------------------- 4D geometry on real coordinates */
LP_API double lp_distance4(const double a[4], const double b[4]);

/* Shape measures between 4D vertex sequences (n x 4 doubles each), each for its purpose:
 *   Fréchet            the largest gap along the best walk of both: one stray vertex sets the distance
 *   Fréchet, k out     the same, with up to k interior vertices of each sequence skipped
 *   DTW                the sum of gaps along the best walk: a stray vertex adds its distance once; repeats are absorbed
 *   EDR                the number of edits between them, vertices within eps of each other counting as equal: a stray
 *                      vertex costs one edit, and so does a repeat */
LP_API double lp_frechet4(const double *a, size_t na, const double *b, size_t nb);
LP_API double lp_frechet4_outliers(const double *a, size_t na, const double *b, size_t nb, unsigned k);
LP_API double lp_dtw4(const double *a, size_t na, const double *b, size_t nb, size_t *steps);   /* steps: the walk's length */
LP_API size_t lp_edr4(const double *a, size_t na, const double *b, size_t nb, double eps);
/* Exact centroid of points whose coordinates are fixed-point values m / 2^53; false if any is not. */
LP_API bool lp_centroid4_exact(const double *points, size_t n, double out[4]);

/* ---------------------------------------------------------------- b beats c given a */
typedef struct { uint32_t row, col; float score, z; } lp_rowsig_hit;
typedef struct { uint64_t rows, candidates, above[3], rows_without; double flops; } lp_rowsig_stats;   /* above z = zmin, 4, 5 */
/* For each row a of A (m x r): scores against every row of B (n x r), scaled; the row's mean and spread over all n; and
 * the candidates above mean + zmin * sd (at most cap per row, highest first). Returns the hits written. Needs MKL. */
LP_API size_t lp_rowsig(const float *A, size_t m, const float *B, size_t n, size_t r, float scale, float zmin, uint32_t cap,
                        lp_rowsig_hit *out, size_t out_cap, lp_rowsig_stats *stats);

/* ---------------------------------------------------------------- consensus: Glicko-2 */
typedef struct { double rating, deviation, volatility; } lp_rating;

#define LP_GLICKO_SCALE      173.7178
#define LP_GLICKO_RATING     1500.0       /* the anchor, and where the unrated enter */
#define LP_GLICKO_DEVIATION  350.0        /* the unrated's deviation */
#define LP_GLICKO_VOLATILITY 0.06         /* the unrated's volatility, and every witness's */
#define LP_ATTEST_TAU        0.5          /* the system constant every attestation is played with */
#define LP_ATTEST_FLOOR      30.0         /* the least deviation a standing keeps, and a witness enters with */
static inline lp_rating lp_rating_stock(void){ lp_rating r = { LP_GLICKO_RATING, LP_GLICKO_DEVIATION, LP_GLICKO_VOLATILITY }; return r; }
/* The deviation a claim enters with when a witness of trust t brings it: the deviation that trust plays with, never
 * below the floor; the unrated's when the witness says nothing (trust 0). */
LP_API double lp_entry_deviation(double trust);

/* One rating period against n opponents (Glickman's Glicko-2, steps 1-8). tau: system constant. */
LP_API void lp_glicko2(lp_rating *r, const lp_rating *opponents, const double *scores, size_t n, double tau);
/* The trust classes (manifest/trust_classes.toml): a witness's class is the only statement of its trust, and the
 * class's prior is the trust its claims are played at. Append-only, in the manifest's order; a label the manifest
 * does not declare is no class (NULL). */
typedef struct { const char *label; double prior; } lp_trust_class;
LP_API size_t lp_trust_class_count(void);
LP_API const lp_trust_class *lp_trust_class_at(size_t i);
LP_API const lp_trust_class *lp_trust_class_named(const char *label);
/* The deviation a witness of trust |t| plays with: its weight g(phi) equals |t|. Trust 1 plays with deviation 0. */
LP_API double lp_trust_deviation(double trust);
/* One attestation as one matchup: the witness plays at rating opponent_rating with the deviation its trust gives;
 * a negative trust flips the outcome; trust 0 changes nothing. The deviation never falls below floor. */
LP_API void lp_attest(lp_rating *r, double trust, double score, double opponent_rating, double tau, double floor);
/* One matchup, played as it arrives. There are no rating periods: no time passes between matchups, so a standing's
 * deviation is never widened for a period gone by; it changes only by what the matchup tells. */
LP_API void lp_matchup(lp_rating *r, const lp_rating *opponent, double score, double tau);

/* How hard a strand tugs back: the chance its claim beats the anchor (rating 1500), taken k deviations below its
 * rating, so a claim few have witnessed counts for less than its rating alone says. */
LP_API double lp_confidence(const lp_rating *r, double k);
/* The cost of crossing a claim: -ln(confidence) + per_hop. Costs add, so the cheapest chain is the one whose
 * confidences multiply to the most; per_hop prefers short chains. */
LP_API double lp_cost(const lp_rating *r, double k, double per_hop);

/* ---------------------------------------------------------------- tier 0 */
typedef struct { lp_id id; int64_t m[4]; uint64_t hilbert; uint32_t rank, pad; } lp_tier0_record;   /* 64 bytes */

/* Where tier 0 is: $LAPLACE_TIER0, or the path the library was built with. */
LP_API const char *lp_tier0_path(void);
/* Memory-map a tier-0 table (LP_NCP records); NULL or "" maps lp_tier0_path(). Returns NULL if the file is missing or
 * the wrong size. */
LP_API const lp_tier0_record *lp_tier0_map(const char *path);
/* The codepoint with this ID, or -1: O(1), from a table built on first use. */
LP_API int64_t lp_tier0_codepoint(const lp_tier0_record *t0, const lp_id *id);
/* The fingerprint of a tier 0: BLAKE3-256 of the table. Two installs with one fingerprint give the same coordinates
 * to the same content. */
LP_API void lp_tier0_fingerprint(const lp_tier0_record *t0, uint8_t out[32]);

/* ---------------------------------------------------------------- the flags that go with tier 0
 * One 256-bit record per codepoint: a bit for each binary property and a field for each enumerated property of the
 * Unicode Standard, in the standard's own order, a field holding its value's place in the standard's own list. The
 * layout is a file beside the records; laplace flags (Laplace-Engine) generates both. */
typedef struct { uint8_t b[32]; } lp_flags;
typedef struct { char name[32], say[64]; uint16_t bit, width, nvalues; uint32_t first; } lp_field;   /* first: its first value in the layout's values */
typedef struct { char name[48], say[64]; } lp_value;
typedef struct { const lp_flags *flags; lp_field *field; size_t nfields; lp_value *value; size_t nvalues; } lp_layout;

/* Where the flags are: $LAPLACE_FLAGS, or tier 0's path with .flags in place of its ending. */
LP_API const char *lp_flags_path(void);
/* Memory-map the flags and read their layout (path.layout); NULL or "" maps lp_flags_path(). NULL if either is missing. */
LP_API const lp_layout *lp_flags_map(const char *path);
/* A field by the property's name, short or as it is said, by the standard's matching rule; NULL if there is none. */
LP_API const lp_field *lp_flags_field(const lp_layout *, const char *property);
/* What a codepoint's field holds: 0 or 1 for a binary property, a value's place in its list for an enumerated one. */
LP_API uint32_t lp_flags_get(const lp_layout *, uint32_t cp, const lp_field *);
/* A value's place in a field's list by its name, short or as it is said; -1 if the list does not hold it. */
LP_API int32_t lp_flags_value(const lp_layout *, const lp_field *, const char *value);
/* The standard's rule for matching names (UAX #44, LM3): case, spaces, underscores and hyphens do not count. */
LP_API bool lp_name_same(const char *a, size_t al, const char *b, size_t bl);

/* ---------------------------------------------------------------- the highway: the types, as a perf-cache
 * What a curated resource says in its enumerations is a type, not content (Semantics: Claims). Each list of types is
 * the resource's own, in the order it writes it: UD's parts of speech and relations, WordNet's lexicographer files,
 * CILI's concepts, VerbNet's classes, FrameNet's frames, PropBank's rolesets, and so on. A type's record is the ID
 * and real coordinate of its content (the text NOUN as UD writes it; an ILI's definition as CILI gives it), in the
 * form of a tier-0 record with its slot as the rank; and the mappings the highway sources draw between lists are
 * edges, pairs of slots. laplace highway (Laplace-Engine) generates highway.bin and its layout beside it. */
typedef struct { char name[32], say[64]; uint32_t first, count; } lp_list;          /* first: its first record; count: how many */
typedef struct { uint32_t from, to; } lp_edge;
typedef struct { char a[32], b[32]; uint32_t first, count; } lp_edges;              /* edges from list a to list b, sorted by from */
typedef struct { char name[32]; uint16_t bit, width; const lp_list *list; } lp_mask;   /* a list small enough to be a field of the 256-bit mask: its first bit, one per slot */
typedef struct { const lp_tier0_record *rec; size_t nrec; lp_list *list; size_t nlists; const lp_edge *edge; size_t nedges; lp_edges *edges; size_t nedgelists;
                 lp_mask *mask; size_t nmasks; lp_idindex *by_id; char path[4096]; lp_strmap *keys; } lp_highway;
/* The mask's fields (Semantics: Claims, Masks): "kind" (bits 0 to 7: what a row is) and the lists small enough. */
#define LP_KIND_CLAIM    0
#define LP_KIND_RECORD   1
#define LP_KIND_TUPLE    2
#define LP_KIND_FILE     3
#define LP_MASK_BITS     256
/* Where the highway is: $LAPLACE_HIGHWAY, or tier 0's path with .highway in place of its ending. */
LP_API const char *lp_highway_path(void);
/* Memory-map the highway and read its layout (path.layout); NULL or "" maps lp_highway_path(). NULL if either is missing. */
LP_API const lp_highway *lp_highway_map(const char *path);
/* A list by its name, short or as it is said; NULL if there is none. */
LP_API const lp_list *lp_highway_list(const lp_highway *, const char *name);
/* The record of a slot of a list: its content's ID, coordinate and Hilbert value; NULL past the list's end. */
LP_API const lp_tier0_record *lp_highway_at(const lp_highway *, const lp_list *, uint32_t slot);
/* The slot in a list whose content has this ID, or -1: O(1), from a table built on first use. */
LP_API int64_t lp_highway_slot(const lp_highway *, const lp_list *, const lp_id *id);
/* The slots of list b that a slot of list a maps to: a pointer into the edges and how many; 0 when none. */
LP_API size_t lp_highway_edges(const lp_highway *, const char *a, uint32_t slot, const char *b, const lp_edge **out);
/* The fingerprint of a highway: BLAKE3-256 of its records and edges. */
LP_API void lp_highway_fingerprint(const lp_highway *, uint8_t out[32]);
/* The mask bit of a type, by its content's ID: the field of the list it is in, plus its slot; -1 when the ID is no
 * type, or its list is no mask field. O(1). */
LP_API int32_t lp_highway_mask_bit(const lp_highway *, const lp_id *id);
/* A mask field by its name (kind, or a list's name); NULL if there is none. */
LP_API const lp_mask *lp_highway_mask(const lp_highway *, const char *name);
/* The slot of a list a source's own key points at (CILI's i46360, PropBank's abandon.01, a VerbNet class number):
 * from the keys beside the highway (path.keys: list, key, slot), read once; -1 when none. Keys are how a resource
 * points at its types, resolved here and recorded nowhere. */
LP_API int64_t lp_highway_key(const lp_highway *, const lp_list *, const char *key);

/* ---------------------------------------------------------------- composition */
/* An entity as it is composed: its ID, its real coordinate, and its tier. */
typedef struct { lp_id id; lp_coord c; uint8_t tier; uint8_t said; } lp_ref;       /* said: what it is within the path it is put in (LP_SAID_*); never part of its ID */

/* M of a path's vertex is that vertex's metadata, as bits: a double holds 53 of them exactly (Storage: Physicality).
 * The low 30 are how many times the vertex is repeated. The 3 above them say what the vertex is within the path it is
 * put in: a claim, witnessed in what the path belongs to; a record, holding claims witnessed in it; a tuple, things
 * that together name one thing; or the metadata of what the path is (a file's, beside its content). */
#define LP_SAID_CLAIM  1u
#define LP_SAID_RECORD 2u
#define LP_SAID_TUPLE  3u      /* the vertex is a path of things that together name one thing: not text, and not a claim */
#define LP_SAID_METADATA 4u    /* the vertex is the metadata of what the path is: a file's, beside its content */
#define LP_M_RUN_BITS  30
#define LP_M_SAID_BITS 3
#define LP_M_SAID_MASK ((1ull << LP_M_SAID_BITS) - 1)
static inline uint64_t lp_m_bits(double m){ return m < 1 ? 1ull : (uint64_t)m; }
static inline uint32_t lp_m_run(double m){ uint32_t r = (uint32_t)(lp_m_bits(m) & ((1ull << LP_M_RUN_BITS) - 1)); return r ? r : 1u; }
static inline uint32_t lp_m_said(double m){ return (uint32_t)((lp_m_bits(m) >> LP_M_RUN_BITS) & LP_M_SAID_MASK); }
/* M as it is written for a vertex repeated run times that is said to be what said names. */
static inline double lp_m_of(uint32_t run, uint32_t said){ return (double)(((uint64_t)(said & LP_M_SAID_MASK) << LP_M_RUN_BITS) | (run ? run : 1u)); }

LP_API lp_ref lp_ref_atom(const lp_tier0_record *t0, uint32_t cp);
/* The composition of n children in order: its ID from theirs, its coordinate the exact average of theirs. One child
 * is that child. */
LP_API lp_ref lp_ref_compose(const lp_ref *children, size_t n, uint8_t tier);
/* The tier a composition of these children takes at the least: one above the highest of them. */
static inline uint8_t lp_tier_above(const lp_ref *r, size_t n){ uint8_t t = 0; for (size_t i = 0; i < n; i++) if (r[i].tier > t) t = r[i].tier; return (uint8_t)(t < 255 ? t + 1 : 255); }
/* UTF-8 taken as one composition of its codepoints, with no tier between (lp_id_codepoints_utf8's ID): its ID and
 * coordinate from tier 0, its tier one above them; one codepoint is that codepoint. ok is false on invalid UTF-8 or an
 * empty string. */
LP_API lp_ref lp_ref_codepoints(const lp_tier0_record *t0, const uint8_t *s, size_t n, bool *ok);

/* What receives every composition as it is made, and returns it (a table that records it, or nothing at all).
 * NULL composes without recording. */
typedef lp_ref (*lp_compose_fn)(void *sink, const lp_ref *children, uint32_t n, uint8_t tier);

/* Repeats within a composition, factored from the content alone (Storage: Compositions, Repeats). A block of two or
 * more constituents repeated adjacently becomes one entity, composed through compose/sink at one tier above its
 * highest constituent, and stands in the sequence once per repeat (the path writer makes the run); leftmost, the
 * shortest period, recursive, so the same content always factors the same way. A repeated single constituent is a run already and is
 * left as it is. out holds at most n; the count written is returned. */
LP_API size_t lp_factor(const lp_ref *children, size_t n, lp_ref *out, lp_compose_fn compose, void *sink);

/* ---------------------------------------------------------------- text (liblaplace_text; needs ICU)
 * UAX #29: codepoint -> grapheme -> word segment -> sentence -> paragraph -> text, tiers 0 to 5. Nothing is dropped:
 * whitespace and punctuation are constituents like everything else, so the text recomposes byte for byte. A single line
 * break inside a paragraph is read as a space for segmentation only; what is recorded is the original. One lp_text
 * per thread. */
typedef struct lp_text lp_text;
LP_API lp_text *lp_text_new(const lp_tier0_record *t0);
LP_API void     lp_text_free(lp_text *);
LP_API lp_ref   lp_text_decompose(lp_text *, const uint8_t *s, size_t n, lp_compose_fn compose, void *sink);
/* The same, also giving the trunk's own constituents in order, repeats included (at most cap are written; the count
 * is returned in *nparts). A trunk that is one codepoint has itself as its only part. */
LP_API lp_ref   lp_text_parts(lp_text *, const uint8_t *s, size_t n, lp_ref *parts, size_t cap, size_t *nparts);

/* ---------------------------------------------------------------- the pull: search over rated claims
 * The open set of a best-first search (Dijkstra, A*): entities by ID, each with the cost of the cheapest chain found
 * to it, the estimate that orders it, and the entity and claim it was reached through. */
typedef struct lp_frontier lp_frontier;
typedef struct { lp_id id, from, claim; double cost, order; uint32_t hops; bool closed; } lp_reached;

LP_API lp_frontier *lp_frontier_new(void);
LP_API void         lp_frontier_free(lp_frontier *);
/* Reach id at this cost through claim from `from`; estimate is the remaining cost's lower bound (0 for Dijkstra).
 * Returns true if this is the cheapest chain to it so far. */
LP_API bool lp_frontier_reach(lp_frontier *, const lp_id *id, const lp_id *from, const lp_id *claim, double cost,
                              double estimate, uint32_t hops);
/* Close and return the open entity of least order, or NULL when none is open. */
LP_API const lp_reached *lp_frontier_next(lp_frontier *);
/* The least order among open entities, or infinity when none is open. */
LP_API double lp_frontier_least(lp_frontier *);
LP_API const lp_reached *lp_frontier_find(const lp_frontier *, const lp_id *id);
LP_API size_t lp_frontier_count(const lp_frontier *);

#ifdef __cplusplus
}
#endif
#endif
