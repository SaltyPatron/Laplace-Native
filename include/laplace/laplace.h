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

/* ---------------------------------------------------------------- IDs written into geometry */
/* An ID's 128 bits go into the X, Y, Z mantissas (43 + 43 + 42 bits) with exponent -2, so the three values lie in
 * [0.25, 0.5), inside the 4-ball. */
LP_API void lp_id_to_xyz(const lp_id *id, double xyz[3]);
LP_API void lp_xyz_to_id(const double xyz[3], lp_id *out);

/* EWKB for a physicality path: a POINT ZM when there is one run, else a LINESTRING ZM, one vertex per run of
 * identical children with the run length in M. Returns the bytes written, or the bytes needed if cap is too small. */
LP_API size_t lp_ewkb_path(const lp_id *children, size_t n, uint8_t *out, size_t cap);
/* The same path from runs already collapsed: vertex i is ids[i] repeated runs[i] times. */
LP_API size_t lp_ewkb_runs(const lp_id *ids, const uint32_t *runs, size_t nv, uint8_t *out, size_t cap);
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

#define LP_GLICKO_SCALE 173.7178

/* One rating period against n opponents (Glickman's Glicko-2, steps 1-8). tau: system constant. */
LP_API void lp_glicko2(lp_rating *r, const lp_rating *opponents, const double *scores, size_t n, double tau);
/* The deviation a witness of trust |t| plays with: its weight g(phi) equals |t|. Trust 1 plays with deviation 0. */
LP_API double lp_trust_deviation(double trust);
/* One attestation as one matchup: the witness plays at rating opponent_rating with the deviation its trust gives;
 * a negative trust flips the outcome; trust 0 changes nothing. The deviation never falls below floor. */
LP_API void lp_attest(lp_rating *r, double trust, double score, double opponent_rating, double tau, double floor);

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

/* ---------------------------------------------------------------- composition */
/* An entity as it is composed: its ID, its real coordinate, and its tier. */
typedef struct { lp_id id; lp_coord c; uint8_t tier; } lp_ref;

LP_API lp_ref lp_ref_atom(const lp_tier0_record *t0, uint32_t cp);
/* The composition of n children in order: its ID from theirs, its coordinate the exact average of theirs. One child
 * is that child. */
LP_API lp_ref lp_ref_compose(const lp_ref *children, size_t n, uint8_t tier);

/* What receives every composition as it is made, and returns it (a table that records it, or nothing at all).
 * NULL composes without recording. */
typedef lp_ref (*lp_compose_fn)(void *sink, const lp_ref *children, uint32_t n, uint8_t tier);

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
