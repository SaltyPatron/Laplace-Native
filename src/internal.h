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

#endif
