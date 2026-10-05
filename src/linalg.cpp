/* Laplacian eigenmaps (Spectra), Procrustes (JacobiSVD) and Gram-Schmidt (HouseholderQR) behind a C API; ported from
 * the monorepo's engine/dynamics. Eigen's packet math uses FMA wherever __FMA__ is defined (x86-64-v3), which rounds
 * once where the contract rounds twice, and its product blocking follows the cache sizes cpuid reports: both are off
 * here, so the results are the scalar ones on every machine. tests/ checks the bits against an x86-64 build. */
#if !defined(EIGEN_DONT_VECTORIZE) || !defined(EIGEN_NO_CPUID) || !defined(EIGEN_DONT_PARALLELIZE)
#  error "linalg.cpp: compile with EIGEN_DONT_VECTORIZE, EIGEN_NO_CPUID and EIGEN_DONT_PARALLELIZE"
#endif
#ifdef EIGEN_USE_MKL_ALL
#  error "linalg.cpp: MKL is not under the contract here"
#endif
#include "laplace/linalg.h"

#include <Eigen/Core>
#include <Eigen/QR>
#include <Eigen/SVD>
#include <Eigen/LU>
#include <Spectra/SymEigsSolver.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace {

using Mat = Eigen::MatrixXd;
using RowMat = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

/* The sign that makes the largest-magnitude component (the first, on a tie) positive. */
template <class V> void canon(V &&v) {
    Eigen::Index best = 0;
    double m = -1.0;
    for (Eigen::Index i = 0; i < v.size(); i++)
        if (std::fabs(v[i]) > m) { m = std::fabs(v[i]); best = i; }
    if (v[best] < 0.0) v = -v;
}

/* L x + 3 F F^T x: the found pairs (columns of F, orthonormal) moved from [0, 2] to [3, 5]. */
struct Deflated {
    using Scalar = double;
    const Mat &L;
    const Mat &F;
    Eigen::Index m;                                       /* columns of F found so far */
    Eigen::Index rows() const { return L.rows(); }
    Eigen::Index cols() const { return L.cols(); }
    void perform_op(const double *x, double *y) const {
        Eigen::Map<const Eigen::VectorXd> xv(x, L.rows());
        Eigen::Map<Eigen::VectorXd> yv(y, L.rows());
        yv.noalias() = L * xv;
        if (m) yv.noalias() += 3.0 * (F.leftCols(m) * (F.leftCols(m).transpose() * xv));
    }
};

bool finite(const double *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!std::isfinite(p[i])) return false;
    return true;
}

}  // namespace

extern "C" int lp_eigenmap(const double *W, size_t n, size_t k, double *Y, double *lambda) {
    if (!W || !Y) return -1;
    if (k == 0 || n < k + 2 || n > (size_t)INT_MAX || n > SIZE_MAX / n) return -2;
    if (!finite(W, n * n)) return -1;
    const Eigen::Index N = (Eigen::Index)n;
    try {
        Mat A = Mat::Zero(N, N);                          /* positive affinity, symmetrized; refutations are not affinity */
        for (Eigen::Index i = 0; i < N; i++)
            for (Eigen::Index j = i + 1; j < N; j++) {
                const double a = 0.5 * (std::max(W[i * n + j], 0.0) + std::max(W[j * n + i], 0.0));
                A(i, j) = a; A(j, i) = a;
            }
        Eigen::VectorXd s(N);
        for (Eigen::Index i = 0; i < N; i++) {
            double d = 0.0;
            for (Eigen::Index j = 0; j < N; j++) d += A(i, j);
            s[i] = d > 0.0 ? 1.0 / std::sqrt(d) : 0.0;
        }
        Mat L(N, N);
        for (Eigen::Index i = 0; i < N; i++)
            for (Eigen::Index j = 0; j < N; j++)
                L(i, j) = i == j ? (s[i] > 0.0 ? 1.0 : 0.0) : -(A(i, j) * s[i]) * s[j];

        /* One pair at a time, each the smallest of L with the pairs found so far moved above its spectrum ([0, 2]).
         * Lanczos from one start vector sees one direction of each eigenspace, so solving for k + 1 pairs at once
         * misses the second of a repeated eigenvalue (the ring's cos and sin); deflation finds it next. */
        Mat F(N, (Eigen::Index)k + 1);
        Eigen::VectorXd ev((Eigen::Index)k + 1);
        const Eigen::Index ncv = std::min<Eigen::Index>(N, 20);
        for (Eigen::Index p = 0; p <= (Eigen::Index)k; p++) {
            Deflated op{L, F, p};
            Spectra::SymEigsSolver<Deflated> eigs(op, 1, ncv);
            eigs.init();                                  /* Spectra's fixed-seed start vector */
            eigs.compute(Spectra::SortRule::SmallestAlge, 10000, 1e-12);
            if (eigs.info() != Spectra::CompInfo::Successful) return -3;
            ev[p] = eigs.eigenvalues()[0];
            F.col(p) = eigs.eigenvectors().col(0);
        }
        std::vector<Eigen::Index> idx((size_t)ev.size());
        std::iota(idx.begin(), idx.end(), Eigen::Index(0));
        std::stable_sort(idx.begin(), idx.end(), [&](Eigen::Index a, Eigen::Index b) { return ev[a] < ev[b]; });
        for (size_t c = 0; c < k; c++) {                  /* idx[0] is the trivial pair, D^1/2 1 at 0 */
            const Eigen::Index col = idx[c + 1];
            Eigen::VectorXd f = F.col(col).cwiseProduct(s);
            canon(f);
            for (size_t i = 0; i < n; i++) Y[i * k + c] = f[(Eigen::Index)i];
            if (lambda) lambda[c] = ev[col];
        }
        return 0;
    } catch (...) {
        return -3;
    }
}

extern "C" int lp_procrustes(const double *A, const double *B, size_t n, size_t d, double *R, double *scale) {
    if (!A || !B || !R) return -1;
    if (n == 0 || d == 0 || n > (size_t)INT_MAX || d > (size_t)INT_MAX || n > SIZE_MAX / d) return -2;
    if (!finite(A, n * d) || !finite(B, n * d)) return -1;
    try {
        Eigen::Map<const RowMat> a(A, (Eigen::Index)n, (Eigen::Index)d), b(B, (Eigen::Index)n, (Eigen::Index)d);
        const double aa = a.squaredNorm();
        if (!(aa > 0.0)) return -2;
        const Mat H = a.transpose() * b;
        Eigen::JacobiSVD<Mat> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
        const Mat &U = svd.matrixU(), &V = svd.matrixV();
        Eigen::VectorXd D = Eigen::VectorXd::Ones((Eigen::Index)d);
        if ((U * V.transpose()).determinant() < 0.0) D[(Eigen::Index)d - 1] = -1.0;   /* a rotation, not a reflection */
        const Mat Rm = U * D.asDiagonal() * V.transpose();
        for (size_t i = 0; i < d; i++)
            for (size_t j = 0; j < d; j++) R[i * d + j] = Rm((Eigen::Index)i, (Eigen::Index)j);
        if (scale) *scale = svd.singularValues().cwiseProduct(D).sum() / aa;
        return 0;
    } catch (...) {
        return -3;
    }
}

extern "C" int lp_gram_schmidt(double *V, size_t m, size_t d) {
    if (!V) return -1;
    if (m == 0 || d == 0) return 0;
    if (m > d || d > (size_t)INT_MAX || m > SIZE_MAX / d) return -2;
    if (!finite(V, m * d)) return -1;
    try {
        const Eigen::Index M = (Eigen::Index)m, D = (Eigen::Index)d;
        Mat X(D, M);                                      /* the vectors as columns */
        for (Eigen::Index i = 0; i < M; i++)
            for (Eigen::Index j = 0; j < D; j++) X(j, i) = V[i * d + j];
        Eigen::HouseholderQR<Mat> qr(X);
        const Mat Rm = qr.matrixQR().triangularView<Eigen::Upper>();
        const double tol = (double)std::max(m, d) * Rm.diagonal().cwiseAbs().maxCoeff() * std::numeric_limits<double>::epsilon();
        for (Eigen::Index i = 0; i < M; i++)
            if (!(std::fabs(Rm(i, i)) > tol)) return -4;
        Mat Q = qr.householderQ() * Mat::Identity(D, M);
        for (Eigen::Index i = 0; i < M; i++) {
            canon(Q.col(i));
            for (Eigen::Index j = 0; j < D; j++) V[i * d + j] = Q(j, i);
        }
        return 0;
    } catch (...) {
        return -3;
    }
}
