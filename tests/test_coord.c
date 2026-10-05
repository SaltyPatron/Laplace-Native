/* Coordinates against the prototype: every tier-0 point is inside the wall, its Hilbert value equals tier 0's,
 * and centroids are exact integer averages truncated toward zero. */
#include "laplace/laplace.h"
#include "check.h"
#include "../src/internal.h"
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
    /* the hardware division equals the compiler's __int128 division on sums of up to 4096 coordinates of either sign */
    { uint64_t x = 0x9E3779B97F4A7C15ull; size_t wrong = 0;
      for (int i = 0; i < 1000000; i++) {
          x ^= x << 13; x ^= x >> 7; x ^= x << 17; uint64_t n = (x % 4096) + 1; x ^= x << 13; x ^= x >> 7; x ^= x << 17;
          __int128 s = 0; int64_t m = (int64_t)(x >> (1 + x % 11)); if (x & 1) m = -m;       /* a coordinate, any magnitude below 2^63 */
          for (uint64_t k = 0; k < n; k++) s += m; s += (int64_t)(x >> 40) * (n & 1 ? -1 : 1);   /* a sum that does not divide evenly */
          wrong += lp_div128(s, n) != (int64_t)(s / (__int128)n);
      }
      CHECK(wrong == 0, "%zu quotients differ from __int128 division", wrong); }

    /* Hilbert decode is the encode's inverse: on every tier-0 point and on a million grid cells */
    { size_t bad = 0;
      for (uint32_t cp = 0; cp < LP_NCP; cp++) {
          uint32_t g[4], e[4]; lp_hilbert4_decode(t0[cp].hilbert, g);
          for (int d = 0; d < 4; d++) e[d] = lp_hilbert4_axis((double)t0[cp].m[d] / LP_FIXED_ONE);
          bad += memcmp(g, e, sizeof g) != 0;
      }
      CHECK(bad == 0, "%zu tier-0 Hilbert values decode to another cell", bad);
      uint64_t x = 0x2545F4914F6CDD1Dull; bad = 0;
      for (int i = 0; i < 1000000; i++) {
          x ^= x << 13; x ^= x >> 7; x ^= x << 17;
          uint32_t g[4] = { (uint32_t)(x & 0xffff), (uint32_t)(x >> 16 & 0xffff), (uint32_t)(x >> 32 & 0xffff), (uint32_t)(x >> 48) }, r[4];
          uint64_t h = lp_hilbert4_grid(g); lp_hilbert4_decode(h, r);
          bad += memcmp(g, r, sizeof g) != 0;
          lp_hilbert4_decode(x, r); bad += lp_hilbert4_grid(r) != x;           /* and the other way round */
      }
      CHECK(bad == 0, "%zu grid cells or values do not round-trip", bad); }

    /* Box to ranges: ascending, disjoint, within the budget, covering every cell of the box; exact when the budget
     * allows (small boxes, every cell counted) */
    { uint64_t x = 0x9E3779B97F4A7C15ull; size_t bad_order = 0, over = 0, missed = 0, inexact = 0;
      static lp_hrange r[1024];
      for (int t = 0; t < 2000; t++) {
          uint32_t lo[4], hi[4]; uint64_t cells = 1;
          int small = t % 2;
          for (int d = 0; d < 4; d++) {
              x ^= x << 13; x ^= x >> 7; x ^= x << 17;
              uint32_t w = small ? (uint32_t)(x % 6) : (uint32_t)(x % 20000);
              lo[d] = (uint32_t)((x >> 20) % (65536 - w)); hi[d] = lo[d] + w; cells *= (uint64_t)w + 1;
          }
          size_t cap = small ? 1024 : (size_t[]){ 1, 8, 64, 512 }[t / 2 % 4];
          size_t n = lp_hilbert4_ranges(lo, hi, r, cap);
          over += n == 0 || n > cap;
          uint64_t total = 0;
          for (size_t i = 0; i < n; i++) { bad_order += r[i].lo > r[i].hi || (i && r[i].lo <= r[i - 1].hi + 1 && r[i - 1].hi != UINT64_MAX); total += r[i].hi - r[i].lo + 1; }
          if (small && total != cells) inexact++;
          for (int s = 0; s < 200; s++) {                                     /* cells of the box fall in a range */
              uint32_t g[4];
              for (int d = 0; d < 4; d++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; g[d] = lo[d] + (uint32_t)(x % ((uint64_t)hi[d] - lo[d] + 1)); }
              uint64_t h = lp_hilbert4_grid(g); int in = 0;
              for (size_t i = 0; i < n && !in; i++) in = h >= r[i].lo && h <= r[i].hi;
              missed += !in;
          }
      }
      CHECK(bad_order == 0, "%zu ranges out of order, overlapping or touching", bad_order);
      CHECK(over == 0, "%zu boxes gave no ranges or more than the budget", over);
      CHECK(missed == 0, "%zu cells of a box outside its ranges", missed);
      CHECK(inexact == 0, "%zu small boxes not covered exactly", inexact);
      uint32_t a[4] = { 5, 5, 5, 5 }, b[4] = { 4, 9, 9, 9 };
      CHECK(lp_hilbert4_ranges(b, a, r, 8) == 0, "an empty box gives no ranges");
      uint32_t z[4] = { 0, 0, 0, 0 }, f[4] = { 65535, 65535, 65535, 65535 };
      CHECK(lp_hilbert4_ranges(z, f, r, 8) == 1 && r[0].lo == 0 && r[0].hi == UINT64_MAX, "the whole grid is one range");
      /* Exact at the smallest budgets: the budget is not what limits refinement */
      CHECK(lp_hilbert4_ranges(z, z, r, 1) == 1 && r[0].lo == lp_hilbert4_grid(z) && r[0].hi == r[0].lo, "one cell, budget 1: that cell");
      uint32_t p[4] = { 32767, 0, 0, 0 }, q[4] = { 32768, 0, 0, 0 };          /* two cells either side of the coarsest split */
      uint64_t hp = lp_hilbert4_grid(p), hq = lp_hilbert4_grid(q);
      size_t n2 = lp_hilbert4_ranges(p, q, r, 2);
      CHECK(n2 == 2 && r[0].lo == r[0].hi && r[1].lo == r[1].hi && r[0].lo == (hp < hq ? hp : hq) && r[1].lo == (hp < hq ? hq : hp),
            "two cells across the top split, budget 2: those two cells");
      CHECK(lp_hilbert4_ranges(p, q, r, 1) == 1 && r[0].lo == (hp < hq ? hp : hq) && r[0].hi == (hp < hq ? hq : hp),
            "the same with budget 1: the one gap closed, nothing wider"); }
    DONE("coord");
}
