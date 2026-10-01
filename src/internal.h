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

/* Hilbert bit interleave: bit b of axis i goes to bit 4b + (3 - i). */
uint64_t lp_hilbert4_interleave_scalar(const uint64_t X[4]);
uint64_t lp_hilbert4_interleave_bmi2(const uint64_t X[4]);

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

#endif
