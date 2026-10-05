/* Working memory, one way: the allocator a host may replace, the one doubling rule every array and buffer grows by,
 * hexadecimal, and the one sort whose order is the same on every platform. */
#include "laplace/laplace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lp_realloc_fn grow_fn;
static lp_free_fn free_fn;

void lp_allocator(lp_realloc_fn grow, lp_free_fn release){ grow_fn = grow; free_fn = release; }

void *lp_realloc(void *p, size_t n){
    void *q = grow_fn ? grow_fn(p, n ? n : 1) : realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "laplace: out of memory (%zu bytes)\n", n); abort(); }
    return q;
}
void lp_free(void *p){ if (!p) return; if (free_fn) free_fn(p); else free(p); }

void *lp_reserve(void **p, size_t *cap, size_t need, size_t size){
    if (need <= *cap) return *p;
    size_t c = *cap ? *cap : 16; while (c < need) c *= 2;
    *p = lp_realloc(*p, c * size); *cap = c;
    return *p;
}

uint8_t *lp_buf_room(lp_buf *b, size_t k){
    lp_reserve((void **)&b->b, &b->cap, b->n + k, 1);
    uint8_t *at = b->b + b->n; b->n += k; return at;
}

void lp_hex(const void *bytes, size_t n, char *out){
    static const char H[] = "0123456789abcdef"; const uint8_t *b = (const uint8_t *)bytes;
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
    out[2 * n] = 0;
}
static int nib(char c){ return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
bool lp_unhex(const char *s, size_t n, void *out){
    uint8_t *o = (uint8_t *)out;
    for (size_t i = 0; i < n; i++) { int h = nib(s[2 * i]), l = h < 0 ? -1 : nib(s[2 * i + 1]); if (l < 0) return false; o[i] = (uint8_t)(h << 4 | l); }
    return true;
}

/* A merge sort: stable, O(n log n), the same order everywhere. Runs of up to 16 are sorted by insertion first. */
static void msort(uint8_t *a, uint8_t *t, size_t n, size_t sz, int (*cmp)(const void *, const void *)){
    if (n <= 16) {
        for (size_t i = 1; i < n; i++) {
            size_t j = i; memcpy(t, a + i * sz, sz);
            while (j > 0 && cmp(a + (j - 1) * sz, t) > 0) { memcpy(a + j * sz, a + (j - 1) * sz, sz); j--; }
            memcpy(a + j * sz, t, sz);
        }
        return;
    }
    size_t h = n / 2; msort(a, t, h, sz, cmp); msort(a + h * sz, t, n - h, sz, cmp);
    if (cmp(a + (h - 1) * sz, a + h * sz) <= 0) return;                            /* already in order */
    memcpy(t, a, h * sz);
    size_t i = 0, j = h, k = 0;
    while (i < h && j < n) { if (cmp(a + j * sz, t + i * sz) < 0) memcpy(a + k++ * sz, a + j++ * sz, sz); else memcpy(a + k++ * sz, t + i++ * sz, sz); }
    if (i < h) memcpy(a + k * sz, t + i * sz, (h - i) * sz);
}
void lp_sort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)){
    if (n < 2) return;
    uint8_t *t = lp_alloc((n / 2 + 1) * size);
    msort((uint8_t *)base, t, n, size, cmp);
    lp_free(t);
}
