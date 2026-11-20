/* Universal compositional scalars: decimal ASCII formatting and Merkle identity construction
 * matching UAX #29 prose decomposition bit-for-bit without ICU. */
#include "laplace/laplace.h"
#include "laplace/tier0.h"
#include "internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static size_t format_u64(uint64_t v, char *out, size_t cap){
    char rev[24];
    size_t len = 0;
    do {
        rev[len++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0);
    if (out && cap > 0) {
        size_t copy = len < cap - 1 ? len : cap - 1;
        for (size_t i = 0; i < copy; i++) out[i] = rev[len - 1 - i];
        out[copy] = 0;
    }
    return len;
}

size_t lp_decimal_u64(uint64_t v, char *out, size_t cap){
    return format_u64(v, out, cap);
}

size_t lp_decimal_u32(uint32_t v, char *out, size_t cap){
    return format_u64((uint64_t)v, out, cap);
}

size_t lp_decimal_i64(int64_t v, char *out, size_t cap){
    if (v >= 0) return format_u64((uint64_t)v, out, cap);
    uint64_t mag = (uint64_t)0 - (uint64_t)v;
    char digits[24];
    size_t dlen = format_u64(mag, digits, sizeof digits);
    size_t total = dlen + 1;
    if (out && cap > 0) {
        out[0] = '-';
        if (cap > 1) {
            size_t copy = dlen < cap - 2 ? dlen : cap - 2;
            memcpy(out + 1, digits, copy);
            out[1 + copy] = 0;
        } else {
            out[0] = 0;
        }
    }
    return total;
}

size_t lp_decimal_i32(int32_t v, char *out, size_t cap){
    return lp_decimal_i64((int64_t)v, out, cap);
}

lp_ref lp_ref_u64(const lp_tier0_record *t0, uint64_t v, lp_compose_fn compose, void *sink){
    lp_ref zero; memset(&zero, 0, sizeof zero);
    if (!t0) return zero;
    char digits[24];
    size_t n = format_u64(v, digits, sizeof digits);
    if (n == 1) return lp_ref_atom(t0, (uint32_t)(uint8_t)digits[0]);
    lp_ref ch[24], fac[24];
    for (size_t i = 0; i < n; i++) ch[i] = lp_ref_atom(t0, (uint32_t)(uint8_t)digits[i]);
    const lp_ref *src = ch;
    size_t m = n;
    if (n >= 4) {
        m = lp_factor(ch, n, fac, compose, sink);
        if (m != n) src = fac;
    }
    if (m == 1) return src[0];
    uint8_t tier = lp_tier_above(src, m);
    return compose ? compose(sink, src, (uint32_t)m, tier) : lp_ref_compose(src, m, tier);
}

lp_ref lp_ref_i64(const lp_tier0_record *t0, int64_t v, lp_compose_fn compose, void *sink){
    lp_ref zero; memset(&zero, 0, sizeof zero);
    if (!t0) return zero;
    if (v >= 0) return lp_ref_u64(t0, (uint64_t)v, compose, sink);
    uint64_t mag = (uint64_t)0 - (uint64_t)v;
    lp_ref pair[2];
    pair[0] = lp_ref_atom(t0, (uint32_t)'-');
    pair[1] = lp_ref_u64(t0, mag, compose, sink);
    uint8_t tier = lp_tier_above(pair, 2);
    return compose ? compose(sink, pair, 2u, tier) : lp_ref_compose(pair, 2, tier);
}

bool lp_id_u64(const lp_tier0_record *t0, uint64_t v, lp_id *out){
    if (!t0 || !out) { errno = EINVAL; return false; }
    *out = lp_ref_u64(t0, v, NULL, NULL).id;
    return true;
}

bool lp_id_i64(const lp_tier0_record *t0, int64_t v, lp_id *out){
    if (!t0 || !out) { errno = EINVAL; return false; }
    *out = lp_ref_i64(t0, v, NULL, NULL).id;
    return true;
}

struct lp_scalars {
    const lp_tier0_record *t0;
    lp_compose_fn compose;
    void *sink;
    lp_ref u8[256];
    lp_ref *u16_hi;   /* 256..65535 -> 65280 slots, lazily initialized */
    lp_ref *i16_neg;  /* -32768..-1 -> 32768 slots, lazily initialized */
    lp_lock mu;
};

lp_scalars *lp_scalars_of(const lp_tier0_record *t0, lp_compose_fn compose, void *sink){
    if (!t0) { errno = EINVAL; return NULL; }
    lp_scalars *s = lp_zalloc(sizeof *s);
    s->t0 = t0;
    s->compose = compose;
    s->sink = sink;
    s->mu = (lp_lock)LP_LOCK_INIT;
    for (uint64_t i = 0; i < 256u; i++) s->u8[i] = lp_ref_u64(t0, i, compose, sink);
    return s;
}

void lp_scalars_free(lp_scalars *s){
    if (!s) return;
    lp_free(s->u16_hi);
    lp_free(s->i16_neg);
    lp_free(s);
}

lp_ref lp_scalar_u8(const lp_scalars *s, uint8_t v){
    if (!s) { lp_ref zero; memset(&zero, 0, sizeof zero); return zero; }
    return s->u8[v];
}

lp_ref lp_scalar_u16(const lp_scalars *s_const, uint16_t v){
    if (!s_const) { lp_ref zero; memset(&zero, 0, sizeof zero); return zero; }
    if (v < 256u) return s_const->u8[v];
    lp_scalars *s = (lp_scalars *)s_const;
    size_t idx = (size_t)(v - 256u);
    static const lp_id zero_id = {{0}};
    lp_lock_take(&s->mu);
    if (!s->u16_hi) s->u16_hi = lp_zalloc(65280u * sizeof(lp_ref));
    lp_ref r = s->u16_hi[idx];
    if (lp_id_eq(&r.id, &zero_id)) {
        r = lp_ref_u64(s->t0, (uint64_t)v, s->compose, s->sink);
        s->u16_hi[idx] = r;
    }
    lp_lock_give(&s->mu);
    return r;
}

lp_ref lp_scalar_i16(const lp_scalars *s_const, int16_t v){
    if (!s_const) { lp_ref zero; memset(&zero, 0, sizeof zero); return zero; }
    if (v >= 0) return lp_scalar_u16(s_const, (uint16_t)v);
    lp_scalars *s = (lp_scalars *)s_const;
    size_t idx = (size_t)((int32_t)v + 32768);
    static const lp_id zero_id = {{0}};
    lp_lock_take(&s->mu);
    if (!s->i16_neg) s->i16_neg = lp_zalloc(32768u * sizeof(lp_ref));
    lp_ref r = s->i16_neg[idx];
    if (lp_id_eq(&r.id, &zero_id)) {
        r = lp_ref_i64(s->t0, (int64_t)v, s->compose, s->sink);
        s->i16_neg[idx] = r;
    }
    lp_lock_give(&s->mu);
    return r;
}
