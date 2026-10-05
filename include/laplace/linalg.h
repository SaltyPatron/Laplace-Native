/* Laplace-Native linear algebra: Laplacian eigenmaps, Procrustes and Gram-Schmidt, over Eigen and Spectra.
 *
 * In the library laplace_linalg, built when LAPLACE_EIGEN_DIR and LAPLACE_SPECTRA_DIR are set. Matrices are row-major
 * doubles. Every output is canonical: each vector's largest-magnitude component (the first, on a tie) is positive and
 * eigenvalues are in ascending order, so the same inputs give the same bits on every machine. Eigen is compiled
 * scalar (EIGEN_DONT_VECTORIZE: its packet math fuses with FMA under x86-64-v3) with fixed blocking (EIGEN_NO_CPUID).
 * Returns: 0, or -1 a null or non-finite argument, -2 a size out of range, -3 no convergence, -4 rank-deficient. */
#ifndef LAPLACE_LINALG_H
#define LAPLACE_LINALG_H

#include "laplace/laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Laplacian eigenmap of a dense affinity W (n x n): W's positive part, symmetrized ((W + W^T) / 2, diagonal
 * ignored), its normalized Laplacian I - D^-1/2 W D^-1/2, and its k smallest eigenpairs after the trivial one, found
 * one at a time by Spectra's symmetric Lanczos with the pairs before deflated (a repeated eigenvalue gives all of its
 * eigenspace). Y (n x k) holds f = D^-1/2 v, the solutions of (D - W) f = lambda D f with f^T D f = 1; lambda (k, or
 * NULL) their eigenvalues. 1 <= k <= n - 2. */
LP_API int lp_eigenmap(const double *W, size_t n, size_t k, double *Y, double *lambda);

/* Orthogonal Procrustes about the origin: the rotation R (d x d, det +1) and scale s that minimize |s A R - B| for
 * A, B (n x d), from the SVD (JacobiSVD) of A^T B. Center A and B first for a rigid fit. scale may be NULL. */
LP_API int lp_procrustes(const double *A, const double *B, size_t n, size_t d, double *R, double *scale);

/* Orthonormalize m vectors of dimension d (rows of V, m <= d) in place, by Householder QR: rows 0..i of the result
 * span what rows 0..i of V spanned. */
LP_API int lp_gram_schmidt(double *V, size_t m, size_t d);

#ifdef __cplusplus
}
#endif
#endif
