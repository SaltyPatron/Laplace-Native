/* Laplace-Native: the shared native library of Laplace.
 *
 * Identity, fixed-point coordinates, geometry packing, Hilbert order, trajectory matching, and consensus
 * (Glicko-2 with signed trust). Every function here is deterministic: the same inputs give the same bits on
 * every CPU, whichever SIMD path runs. */
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
/* Discrete Fréchet distance between 4D vertex sequences (n x 4 doubles each). */
LP_API double lp_frechet4(const double *a, size_t na, const double *b, size_t nb);
/* Exact centroid of points whose coordinates are fixed-point values m / 2^53; false if any is not. */
LP_API bool lp_centroid4_exact(const double *points, size_t n, double out[4]);

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

/* ---------------------------------------------------------------- tier 0 */
typedef struct { lp_id id; int64_t m[4]; uint64_t hilbert; uint32_t rank, pad; } lp_tier0_record;   /* 64 bytes */

/* Memory-map a tier-0 table (LP_NCP records). Returns NULL if the file is missing or the wrong size. */
LP_API const lp_tier0_record *lp_tier0_map(const char *path);

#ifdef __cplusplus
}
#endif
#endif
