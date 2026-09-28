/* Coordinates against the prototype: every tier-0 point is inside the wall, its Hilbert value equals tier 0's,
 * and centroids are exact integer averages truncated toward zero. */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0 != NULL, "cannot map tier 0");
    if (!t0) DONE("coord");
    size_t outside = 0, hbad = 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        lp_coord c; memcpy(c.m, t0[cp].m, 32);
        outside += !lp_coord_inside(&c);
        hbad += lp_hilbert4(&c) != t0[cp].hilbert;
    }
    CHECK(outside == 0, "%zu tier-0 points outside the wall", outside);
    CHECK(hbad == 0, "%zu Hilbert values differ from tier 0", hbad);

    lp_coord p[3] = { { { 7, -7, 1, -1 } }, { { 0, 0, 0, 0 } }, { { 0, 0, 0, 0 } } }, c;
    lp_coord_centroid(p, 3, &c);
    CHECK(c.m[0] == 2 && c.m[1] == -2 && c.m[2] == 0 && c.m[3] == 0, "truncation toward zero: %lld %lld", (long long)c.m[0], (long long)c.m[1]);
    lp_coord big[2]; for (int d = 0; d < 4; d++) { big[0].m[d] = INT64_MAX / 2; big[1].m[d] = INT64_MAX / 2; }
    lp_coord_centroid(big, 2, &c); CHECK(c.m[0] == INT64_MAX / 2, "no overflow in the sum");
    lp_coord edge = { { 1ll << 53, 0, 0, 0 } }, over = { { (1ll << 53) + 1, 0, 0, 0 } };
    CHECK(lp_coord_inside(&edge) && !lp_coord_inside(&over), "the wall is exact");
    DONE("coord");
}
