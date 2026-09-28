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

#endif
