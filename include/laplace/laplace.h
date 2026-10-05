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

/* ---------------------------------------------------------------- identity */
typedef struct { uint8_t b[16]; } lp_id;

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
/* Inside the wall: m . m <= 2^106, exactly. */
LP_API bool lp_coord_inside(const lp_coord *c);
/* 4D Hilbert value on a 16-bit grid over [-1, 1]^4 (Skilling). */
LP_API uint64_t lp_hilbert4(const lp_coord *c);
LP_API uint64_t lp_hilbert4_grid(const uint32_t g[4]);
/* The grid coordinate lp_hilbert4 gives a real coordinate x: floor((x + 1) / 2 * 65536), clamped to [0, 65535]
 * (NaN to 0). */
LP_API uint32_t lp_hilbert4_axis(double x);
/* The grid cell of a Hilbert value: lp_hilbert4_grid's inverse. */
LP_API void lp_hilbert4_decode(uint64_t h, uint32_t g[4]);
/* The Hilbert values of the grid cells in the box lo..hi (inclusive on each axis), as ranges in ascending order with
 * adjacent ones merged, at most cap of them (cap >= 1). Exact when the box's exact cover takes at most LP_HRANGE_WORK
 * cells of work (the cells inside it plus the cells crossing its faces, over the whole 16-ary tree of Hilbert cells)
 * and the merged ranges fit cap. Otherwise the narrowest gaps between ranges are closed until they fit, and a box
 * beyond the work bound is covered a level at a time with crossing cells taken whole, so the ranges always cover the
 * box and may cover cells outside it (a query filters on the coordinates). A call decodes at most 16 LP_HRANGE_WORK
 * cells and holds about 128 KB. Returns the number written; 0 when lo > hi on an axis; without memory, the one range
 * of the whole space, which covers. */
#define LP_HRANGE_WORK 4096
typedef struct { uint64_t lo, hi; } lp_hrange;
LP_API size_t lp_hilbert4_ranges(const uint32_t lo[4], const uint32_t hi[4], lp_hrange *out, size_t cap);

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
/* The same, each vertex carrying a value in its spare bits (spare[i]; 0 is none; NULL: none at all). */
LP_API size_t lp_ewkb_runs_spare(const lp_id *ids, const uint64_t *m, const uint32_t *spare, size_t nv, uint8_t *out, size_t cap);
/* A vertex's spare bits (Identity: the 28 bits of X, Y and Z the 128-bit ID leaves; values from known lists, a small
 * tag saying which layout): bits 0-3 the layout's tag, 4-27 its payload; 0 is no value. The ID is read through its own
 * bits only (lp_xyz_id_mask), so a value never changes which entity a vertex is. */
#define LP_SPARE_BITS     28
#define LP_SPARE_TAG_BITS 4
static inline uint32_t lp_spare_of(uint32_t tag, uint32_t payload){ return (tag & 0xF) | (payload << LP_SPARE_TAG_BITS); }
static inline uint32_t lp_spare_tag(uint32_t v){ return v & 0xF; }
static inline uint32_t lp_spare_payload(uint32_t v){ return v >> LP_SPARE_TAG_BITS; }
LP_API extern const uint64_t lp_xyz_id_mask[3];
LP_API uint32_t lp_xyz_spare(const double xyz[3]);
LP_API void lp_xyz_spare_set(double xyz[3], uint32_t v);
/* A POINT ZM of real 4D coordinates, as EWKB (37 bytes). */
LP_API size_t lp_ewkb_point4(const double xyzm[4], uint8_t *out, size_t cap);
/* Parse a POINT ZM / LINESTRING ZM path (little-endian EWKB, optional SRID). Returns the vertex count and points
 * *vertices at the first vertex (32 bytes each: X, Y, Z, M), or 0 on malformed input. */
LP_API size_t lp_ewkb_vertices(const uint8_t *ewkb, size_t len, const uint8_t **vertices);

/* ---------------------------------------------------------------- trajectory matching */
/* Every place the phrase occurs as a run inside the path (vertices expanded by run length), the ID of the vertex
 * that follows it. Matching compares raw vertex bytes; only continuations are decoded. Returns the number of
 * continuations found; at most cap are written. */
LP_API size_t lp_follows(const uint8_t *ewkb, size_t len, const lp_id *phrase, size_t np, lp_id *out, size_t cap);

/* ---------------------------------------------------------------- IDs, as every reader meets them */
/* A path's constituents in order, each run written out. Returns how many there are; at most cap are written. */
LP_API size_t lp_path_ids(const uint8_t *ewkb, size_t len, lp_id *out, size_t cap);
/* A path's vertices as stored: ID, run and what each is said to be. Returns the vertex count; at most cap written. */
typedef struct { lp_id id; uint32_t run, said, spare; } lp_vertex;
LP_API size_t lp_path_vertices(const uint8_t *ewkb, size_t len, lp_vertex *out, size_t cap);
/* A map keyed by ID, a 32-bit value per ID, in insertion order: put returns the entry's index (fresh: whether it was
 * added), find returns it or -1. */
typedef struct lp_idmap lp_idmap;
LP_API lp_idmap    *lp_idmap_new(void);
LP_API void         lp_idmap_free(lp_idmap *);
LP_API size_t       lp_idmap_put(lp_idmap *, const lp_id *id, bool *fresh);
LP_API int64_t      lp_idmap_find(const lp_idmap *, const lp_id *id);
LP_API size_t       lp_idmap_count(const lp_idmap *);
LP_API const lp_id *lp_idmap_key(const lp_idmap *, size_t i);
LP_API uint32_t    *lp_idmap_value(lp_idmap *, size_t i);
/* A value the database sends in binary: big-endian, as libpq hands it over. */
static inline uint64_t lp_be(const void *p, int n){ const uint8_t *b = (const uint8_t *)p; uint64_t u = 0; for (int i = 0; i < n; i++) u = u << 8 | b[i]; return u; }
static inline double   lp_be_f64(const void *p){ uint64_t u = lp_be(p, 8); double d; memcpy(&d, &u, 8); return d; }

/* ---------------------------------------------------------------- 4D geometry on real coordinates */
LP_API double lp_distance4(const double a[4], const double b[4]);

/* Shape measures between 4D vertex sequences (n x 4 doubles each), each for its purpose:
 *   Fréchet            the largest gap along the best walk of both: one stray vertex sets the distance
 *   Fréchet, k out     the same, with up to k interior vertices of each sequence skipped
 *   DTW                the sum of gaps along the best walk: a stray vertex adds its distance once; repeats are absorbed
 *   EDR                the number of edits between them, vertices within eps of each other counting as equal: a stray
 *                      vertex costs one edit, and so does a repeat
 *   Hausdorff          the farthest any vertex of either lies from the other's vertices: order is ignored, so a
 *                      reversal or a reordering is nothing, and one stray vertex sets it */
LP_API double lp_frechet4(const double *a, size_t na, const double *b, size_t nb);
LP_API double lp_frechet4_outliers(const double *a, size_t na, const double *b, size_t nb, unsigned k);
LP_API double lp_dtw4(const double *a, size_t na, const double *b, size_t nb, size_t *steps);   /* steps: the walk's length */
LP_API size_t lp_edr4(const double *a, size_t na, const double *b, size_t nb, double eps);
LP_API double lp_hausdorff4(const double *a, size_t na, const double *b, size_t nb);
/* Exact centroid of points whose coordinates are fixed-point values m / 2^53; false if any is not. */
LP_API bool lp_centroid4_exact(const double *points, size_t n, double out[4]);

/* ---------------------------------------------------------------- S^3: directions as unit 4-vectors
 * Points on S^3, not rotations: p and -p are different points (opposite), never identified, except by
 * lp_eigen_centroid4. Dot products and squared lengths are summed ((x + y) + z) + m, as lp_distance4 sums.
 *
 * The angle between the directions of a and b, in radians in [0, pi]: each divided by its length, then
 * 2 asin(min(chord / 2, 1)). Laplace-postgres's <=>, bit for bit. A zero vector has no direction: NaN. */
LP_API double lp_angle4(const double a[4], const double b[4]);
/* out[i] = lp_angle4(q, pts + 4i) for i < n, bit for bit on every ISA. */
LP_API void lp_angle4_batch(const double q[4], const double *pts, size_t n, double *out);
/* For unit base and p. The log map: the tangent at base pointing along the geodesic to p, its length the angle
 * atan2(|p - (base.p) base|, base.p). Zero when p is base, and zero when p is -base (no single geodesic). */
LP_API void lp_s3_log(const double base[4], const double p[4], double out[4]);
/* The exp map, its inverse: walk |v| radians from unit base along tangent v (v orthogonal to base). The result is
 * divided by its length so repeated steps do not drift off the sphere. v = 0 gives base exactly. */
LP_API void lp_s3_exp(const double base[4], const double v[4], double out[4]);
/* Spherical interpolation along the geodesic from unit a to unit b (no short-way flip): t = 0 gives a and t = 1 gives
 * b, exactly. When b is a or -a there is no single geodesic, and the result is a. */
LP_API void lp_slerp4(const double a[4], const double b[4], double t, double out[4]);
/* The Karcher (Fréchet) mean of n unit points: the point that minimises the summed squared angles, by gradient steps
 * through lp_s3_log/lp_s3_exp from the normalised Euclidean mean. Points are visited in one canonical order
 * (numeric, component by component), so any permutation of them gives the same bits. Converged when a step is under
 * 1e-12 radians, within 128 steps; false otherwise, or for n = 0 or no memory (out then holds the last estimate). */
LP_API bool lp_karcher_mean4(const double *pts, size_t n, double out[4]);
/* Markley's average: the dominant eigenvector of sum p p^T (summed in the canonical order above), by cyclic Jacobi;
 * unit length, its first nonzero component positive. p and -p count alike. NaN for n = 0 or no memory. */
LP_API void lp_eigen_centroid4(const double *pts, size_t n, double out[4]);

/* ---------------------------------------------------------------- b beats c given a */
typedef struct { uint32_t row, col; float score, z; } lp_rowsig_hit;
typedef struct { uint64_t rows, candidates, above[3], rows_without; double flops; } lp_rowsig_stats;   /* above z = zmin, 4, 5 */
/* For each row a of A (m x r): scores against every row of B (n x r), scaled; the row's mean and spread over all n; and
 * the candidates above mean + zmin * sd (at most cap per row, highest first). Returns the hits written. Needs MKL. */
LP_API size_t lp_rowsig(const float *A, size_t m, const float *B, size_t n, size_t r, float scale, float zmin, uint32_t cap,
                        lp_rowsig_hit *out, size_t out_cap, lp_rowsig_stats *stats);
/* Pins MKL to one reproducible code path (CNR: AVX2, strict), the same bits on every machine; lp_rowsig calls it.
 * Anything else that calls MKL calls it first, before any MKL function. Returns false if MKL refused. Needs MKL. */
LP_API int lp_mkl_reproducible(void);

/* ---------------------------------------------------------------- consensus: Glicko-2 */
typedef struct { double rating, deviation, volatility; } lp_rating;

#define LP_GLICKO_SCALE 173.7178

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
typedef struct { char name[32], group[16], carrier[16]; uint16_t width; const lp_list *list; } lp_bank;   /* a mask of its own: a value's bit is its frozen slot in the list (none for kind) */
typedef struct { const lp_tier0_record *rec; size_t nrec; lp_list *list; size_t nlists; const lp_edge *edge; size_t nedges; lp_edges *edges; size_t nedgelists;
                 lp_bank *bank; size_t nbanks; uint32_t *by_id; size_t nby; char path[4096]; void *keys; } lp_highway;
/* The banks (manifest/banks.tsv): one mask per semantic group, on the row the group describes (carrier: entity, occurrence,
 * row). "kind" is the row's own bank: bits 0 to 7 say what a row is. */
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
/* A bank by its name; NULL if there is none. */
LP_API const lp_bank *lp_highway_bank(const lp_highway *, const char *name);
/* The bank a type is a value of, by its content's ID, and its bit there (its frozen slot); NULL when the ID is no type
 * or its list is no bank. O(1). */
LP_API const lp_bank *lp_highway_bank_of(const lp_highway *, const lp_id *id, int32_t *bit);
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
