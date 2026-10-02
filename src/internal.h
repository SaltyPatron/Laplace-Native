/* Internal declarations shared between Laplace-Native's translation units. */
#ifndef LAPLACE_INTERNAL_H
#define LAPLACE_INTERNAL_H

#include "laplace/laplace.h"

/* Byte offset of vertex i's X in an EWKB path's vertex block (32 bytes per vertex: X, Y, Z, M). */
#define LP_VERTEX_BYTES 32

/* Match kernels: the index of the first vertex s >= start with vertex bytes X, Y, Z equal to key (24 bytes), or n. */
size_t lp_scan_scalar(const uint8_t *v, size_t start, size_t n, const uint8_t key[24]);
size_t lp_scan_avx2(const uint8_t *v, size_t start, size_t n, const uint8_t key[24]);
size_t lp_scan_avx512(const uint8_t *v, size_t start, size_t n, const uint8_t key[24]);

/* 4D row kernels: squared distances from p to b[j0..j1) given in structure-of-arrays form. */
void lp_row_d2_scalar(const double p[4], const double *bx, const double *by, const double *bz, const double *bm,
                      size_t j0, size_t j1, double *out);
void lp_row_d2_avx2(const double p[4], const double *bx, const double *by, const double *bz, const double *bm,
                    size_t j0, size_t j1, double *out);

/* The standard BLAKE3 hash of an input, its first 16 bytes (identity.c). */
void lp_hash16(const void *in, size_t len, lp_id *out);

/* Hilbert bit interleave: bit b of axis i goes to bit 4b + (3 - i). */
uint64_t lp_hilbert4_interleave_scalar(const uint64_t X[4]);
uint64_t lp_hilbert4_interleave_bmi2(const uint64_t X[4]);

/* The kernels this process runs: every variant is compiled in, and the set is chosen once, from what the CPU has and
 * what LAPLACE_ISA allows (cpu.c). Every caller takes its kernel from here. */
typedef size_t   (*lp_scan_fn)(const uint8_t *, size_t, size_t, const uint8_t[24]);
typedef void     (*lp_row_fn)(const double *, const double *, const double *, const double *, const double *, size_t, size_t, double *);
typedef uint64_t (*lp_interleave_fn)(const uint64_t[4]);
typedef struct { lp_scan_fn scan; lp_row_fn row_d2; lp_interleave_fn interleave; } lp_kernel_set;
const lp_kernel_set *lp_kernels(void);

/* s / n truncated toward zero, where the quotient is known to fit in 64 bits (a sum of n coordinates over n). One
 * hardware 128-by-64 division instead of the compiler's software routine for __int128; the same value, bit for bit. */
static inline int64_t lp_div128(__int128 s, uint64_t n){
    unsigned __int128 mag = s < 0 ? -(unsigned __int128)s : (unsigned __int128)s;
    uint64_t hi = (uint64_t)(mag >> 64), lo = (uint64_t)mag, q;
#if defined(__x86_64__)
    if (hi < n) { uint64_t rem; __asm__("divq %4" : "=a"(q), "=d"(rem) : "a"(lo), "d"(hi), "r"(n) : "cc"); }   /* divq faults only when the quotient overflows, which hi < n rules out */
    else q = (uint64_t)(mag / n);
#else
    q = (uint64_t)(mag / n);
#endif
    return s < 0 ? -(int64_t)q : (int64_t)q;
}
/* The exact integer average: four 128-bit sums, each divided once and truncated toward zero. Every centroid in Laplace
 * is this one (coordinates, compositions, points given as doubles). */
typedef struct { __int128 s[4]; uint64_t n; } lp_coord_sum;
static inline void lp_coord_add(lp_coord_sum *a, const int64_t m[4]){ for (int d = 0; d < 4; d++) a->s[d] += m[d]; a->n++; }
static inline void lp_coord_mean(const lp_coord_sum *a, lp_coord *out){ for (int d = 0; d < 4; d++) out->m[d] = a->n ? lp_div128(a->s[d], a->n) : 0; }

/* A perf-cache file, memory-mapped read-only and shared, for the life of the process. NULL if it is missing or, when
 * want is not 0, not exactly want bytes; *size, when given, its size. */
const void *lp_map_file(const char *path, size_t want, size_t *size);
/* A string map that lasts as long as the process (a perf-cache's keys): the C library's memory, never working memory. */
lp_strmap *lp_strmap_lasting(size_t value_bytes);
/* A perf-cache opened once per path for the life of the process: open runs the first time a path is asked for, and
 * every later caller shares what it made; a failure is not kept, so a cache generated later is found. */
const void *lp_cached(const char *kind, const char *path, const void *(*open)(const char *path));
/* A file beside another (a perf-cache's .layout or .keys): the path with ending appended, in out. */
void lp_beside(const char *path, const char *ending, char *out, size_t cap);
/* Where a perf-cache that goes with tier 0 is: the environment's variable when it is set, else tier 0's path with
 * ending in place of its own (buf holds it). */
const char *lp_tier0_sibling(const char *env, const char *ending, char *buf, size_t cap);
/* A layout file read line by line: each line that is not a comment split at tabs into at most max fields, and handed
 * to each. False if the file cannot be read. */
bool lp_lines(const char *path, int max, void (*each)(void *ctx, char **field, int n), void *ctx);
/* A record by its name, short or as it is said, by the standard's rule (UAX #44, LM3): its index, or -1. The names are
 * NUL-terminated fields at name_off and say_off of each record of stride bytes. */
int64_t lp_name_find(const void *records, size_t n, size_t stride, size_t name_off, size_t say_off, const char *name);

#endif
