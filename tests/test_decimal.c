#include "laplace/laplace.h"
#include "laplace/tier0.h"
#include "check.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct { lp_id id; size_t calls; } SpyCtx;
static lp_ref spy_compose(void *sink, const lp_ref *children, uint32_t n, uint8_t tier){
    SpyCtx *s = (SpyCtx *)sink;
    s->calls++;
    return lp_ref_compose(children, n, tier);
}

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "cannot map tier 0");
    if (!t0) DONE("decimal");

    char buf[32];
    CHECK(lp_decimal_u64(0, buf, sizeof buf) == 1 && !strcmp(buf, "0"), "decimal 0");
    CHECK(lp_decimal_u64(255, buf, sizeof buf) == 3 && !strcmp(buf, "255"), "decimal 255");
    CHECK(lp_decimal_i64(-128, buf, sizeof buf) == 4 && !strcmp(buf, "-128"), "decimal -128");
    CHECK(lp_decimal_i64(INT64_MIN, buf, sizeof buf) == 20 && !strcmp(buf, "-9223372036854775808"), "decimal INT64_MIN");
    CHECK(lp_decimal_u32(4294967295u, buf, sizeof buf) == 10 && !strcmp(buf, "4294967295"), "decimal u32 max");
    CHECK(lp_decimal_i32(-42, buf, sizeof buf) == 3 && !strcmp(buf, "-42"), "decimal i32");

    /* Single digit is tier 0 atom directly; multi-digit composes; >=4 digits runs lp_factor; negatives compose ['-','|n|']. */
    lp_ref r5 = lp_ref_u64(t0, 5, NULL, NULL);
    lp_ref atom5 = lp_ref_atom(t0, '5');
    CHECK(r5.tier == 0 && lp_id_eq(&r5.id, &atom5.id), "single digit is Tier-0 atom");

    lp_ref r255 = lp_ref_u64(t0, 255, NULL, NULL);
    lp_id id255; CHECK(lp_id_u64(t0, 255, &id255) && lp_id_eq(&r255.id, &id255) && r255.tier == 1, "255 tier 1");

    lp_ref rm5 = lp_ref_i64(t0, -5, NULL, NULL);
    lp_ref neg_pair[2] = { lp_ref_atom(t0, '-'), atom5 };
    lp_ref want_m5 = lp_ref_compose(neg_pair, 2, 1);
    CHECK(lp_id_eq(&rm5.id, &want_m5.id) && rm5.tier == 1, "-5 matches ['-','5'] at tier 1");

    lp_ref r1212 = lp_ref_u64(t0, 1212, NULL, NULL);
    CHECK(r1212.tier == 2, "1212 factored by lp_factor to tier 2 (got %u)", r1212.tier);

    lp_ref rm128 = lp_ref_i64(t0, -128, NULL, NULL);
    CHECK(rm128.tier == 2, "-128 has tier 1 + max(0,1) = 2");

    SpyCtx spy = {0};
    lp_scalars *sc = lp_scalars_of(t0, spy_compose, &spy);
    CHECK(sc != NULL && spy.calls == 246u, "scalars precomputes 246 multi-digit compositions in 0..255 (got %zu)", spy.calls);
    if (sc) {
        lp_ref s255 = lp_scalar_u8(sc, 255);
        CHECK(lp_id_eq(&s255.id, &id255), "u8 scalar table matches lp_id_u64(255)");
        lp_ref s1212 = lp_scalar_u16(sc, 1212);
        CHECK(lp_id_eq(&s1212.id, &r1212.id), "u16 scalar table matches lp_ref_u64(1212)");
        lp_ref sm128 = lp_scalar_i16(sc, -128);
        CHECK(lp_id_eq(&sm128.id, &rm128.id), "i16 scalar table matches lp_ref_i64(-128)");
        lp_ref sm5 = lp_scalar_i16(sc, -5);
        CHECK(lp_id_eq(&sm5.id, &rm5.id), "i16 scalar table matches lp_ref_i64(-5)");
        lp_scalars_free(sc);
    }

    DONE("decimal");
}
