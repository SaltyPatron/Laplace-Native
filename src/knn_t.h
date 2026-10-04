/* knn.c's kernel for one element type: T, S(name) its static names, GEMM, EPS, and KNN the public name. */
typedef struct { T a; uint32_t j; } S(cand);

static int S(lt)(S(cand) x, S(cand) y){ return x.a < y.a || (x.a == y.a && x.j < y.j); }

static void S(down)(S(cand) *h, size_t n, size_t i){          /* max-heap on (a, j): the root is the worst kept */
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && S(lt)(h[m], h[l])) m = l;
        if (r < n && S(lt)(h[m], h[r])) m = r;
        if (m == i) return;
        S(cand) t = h[i]; h[i] = h[m]; h[m] = t; i = m;
    }
}

static void S(up)(S(cand) *h, size_t i){
    while (i > 0) { size_t p = (i - 1) / 2; if (!S(lt)(h[p], h[i])) return; S(cand) t = h[i]; h[i] = h[p]; h[p] = t; i = p; }
}

static double S(norm)(const T *q, size_t dim){ double s = 0; for (size_t d = 0; d < dim; d++) s += (double)q[d] * q[d]; return s; }

static double S(dist)(const T *q, const T *b, size_t dim){
    double s = 0; for (size_t d = 0; d < dim; d++) { double e = (double)q[d] - (double)b[d]; s += e * e; } return s;
}

int KNN(const T *queries, size_t nq, const T *base, size_t nb, size_t dim, size_t k, uint32_t *out_idx, T *out_dist){
    if (!k || k > nb || !dim || nb > UINT32_MAX) return -1;
    if (!nq) return 0;
    if (!lp_mkl_reproducible()) { fprintf(stderr, "lp_knn_exact: MKL refused reproducible mode (CNR AVX2, strict)\n"); abort(); }
    size_t cap = k + MARGIN < nb ? k + MARGIN : nb;
    T *bn = malloc(sizeof(T) * nb); if (!bn) return -2;
    double bmax = 0;
    for (size_t j = 0; j < nb; j++) { double s = S(norm)(base + j * dim, dim); bn[j] = (T)s; if (s > bmax) bmax = s; }
    bmax = sqrt(bmax);
    int fail = 0;
    #pragma omp parallel
    {
        T *tile = malloc(sizeof(T) * QB * NBT); S(cand) *heap = malloc(sizeof(S(cand)) * QB * cap);
        hit *h = malloc(sizeof(hit) * cap); size_t cnt[QB]; T qn[QB];
        int ok = tile && heap && h;
        if (!ok) {
            #pragma omp atomic write
            fail = 1;
        }
        mkl_set_num_threads_local(1);
        #pragma omp for schedule(dynamic, 1)
        for (size_t q0 = 0; q0 < nq; q0 += QB) {
            if (!ok) continue;
            size_t rows = nq - q0 < QB ? nq - q0 : QB; const T *Q = queries + q0 * dim;
            for (size_t i = 0; i < rows; i++) { qn[i] = (T)S(norm)(Q + i * dim, dim); cnt[i] = 0; }
            for (size_t j0 = 0; j0 < nb; j0 += NBT) {
                size_t cols = nb - j0 < NBT ? nb - j0 : NBT;
                GEMM(CblasRowMajor, CblasNoTrans, CblasTrans, (MKL_INT)rows, (MKL_INT)cols, (MKL_INT)dim, (T)-2,
                     Q, (MKL_INT)dim, base + j0 * dim, (MKL_INT)dim, (T)0, tile, (MKL_INT)cols);
                for (size_t i = 0; i < rows; i++) {
                    S(cand) *hp = heap + i * cap; size_t c = cnt[i]; const T *t = tile + i * cols, *bj = bn + j0; T qi = qn[i];
                    for (size_t jj = 0; jj < cols; jj++) {
                        T a = qi + bj[jj] + t[jj];
                        if (c < cap) { hp[c] = (S(cand)){ a, (uint32_t)(j0 + jj) }; S(up)(hp, c++); }
                        else if (a < hp[0].a) { hp[0] = (S(cand)){ a, (uint32_t)(j0 + jj) }; S(down)(hp, cap, 0); }
                    }
                    cnt[i] = c;
                }
            }
            for (size_t i = 0; i < rows; i++) {
                const T *q = Q + i * dim; S(cand) *hp = heap + i * cap; size_t c = cnt[i];
                for (size_t m = 0; m < c; m++) h[m] = (hit){ S(dist)(q, base + (size_t)hp[m].j * dim, dim), hp[m].j };
                hit_sort(h, c);
                int sure = c == nb;                             /* every row was a candidate */
                if (!sure) {                                    /* a NaN estimate fails the test: scan */
                    double r = sqrt(S(norm)(q, dim)) + bmax, err = 2.0 * (double)(dim + 4) * EPS * r * r;
                    sure = (double)hp[0].a - err > h[k - 1].d;
                }
                if (!sure) { size_t n = 0; for (size_t j = 0; j < nb; j++) hit_keep(h, &n, k, S(dist)(q, base + j * dim, dim), (uint32_t)j); }
                uint32_t *oi = out_idx + (q0 + i) * k; T *od = out_dist + (q0 + i) * k;
                for (size_t m = 0; m < k; m++) { oi[m] = h[m].j; od[m] = (T)h[m].d; }
            }
        }
        free(tile); free(heap); free(h);
    }
    free(bn);
    return fail ? -2 : 0;
}

#undef T
#undef S
#undef GEMM
#undef EPS
#undef KNN
