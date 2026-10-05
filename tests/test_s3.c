/* S^3 math: known angles, <=>'s definition, the batch against one point at a time, log/exp, slerp, the two means. */
#include "laplace/laplace.h"
#include "check.h"
#include <math.h>
#include <string.h>

static const double PI = 3.141592653589793;
static uint64_t s = 0x9E3779B97F4A7C15ull;
static double rnd(void){ s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0 * 2.0 - 1.0; }
static void unit(double p[4]){ double n = sqrt(((p[0] * p[0] + p[1] * p[1]) + p[2] * p[2]) + p[3] * p[3]); for (int d = 0; d < 4; d++) p[d] /= n; }
static double gap(const double a[4], const double b[4]){ double m = 0; for (int d = 0; d < 4; d++) if (fabs(a[d] - b[d]) > m) m = fabs(a[d] - b[d]); return m; }

/* Laplace-postgres's <=> as written there (gist4d.c), for a direction that exists. */
static double pg_angle(const double a[4], const double b[4]){
    double u[4], v[4], na = sqrt(((a[0] * a[0] + a[1] * a[1]) + a[2] * a[2]) + a[3] * a[3]), nb = sqrt(((b[0] * b[0] + b[1] * b[1]) + b[2] * b[2]) + b[3] * b[3]);
    for (int d = 0; d < 4; d++) { u[d] = a[d] / na; v[d] = b[d] / nb; }
    double h = lp_distance4(u, v) / 2.0; return 2.0 * asin(h < 1.0 ? h : 1.0);
}

int main(void){
    const double e0[4] = { 1, 0, 0, 0 }, e1[4] = { 0, 1, 0, 0 }, n3[4] = { 0, 0, 0, -5 }, o[4] = { 0, 0, 0, 0 };
    const double m0[4] = { -2, 0, 0, 0 }, r60[4] = { 0.5, 0.8660254037844386, 0, 0 };
    CHECK(lp_angle4(e0, e0) == 0.0, "a direction is 0 from itself");
    CHECK(fabs(lp_angle4(e0, e1) - PI / 2) <= 4.5e-16, "orthogonal: pi/2 (%.17g)", lp_angle4(e0, e1));
    CHECK(lp_angle4(e0, n3) == lp_angle4(e0, e1), "length is ignored");
    CHECK(lp_angle4(e0, m0) == PI, "opposite: pi exactly (%.17g)", lp_angle4(e0, m0));
    CHECK(fabs(lp_angle4(e0, r60) - PI / 3) <= 4.5e-16, "pi/3 (%.17g)", lp_angle4(e0, r60));
    CHECK(isnan(lp_angle4(o, e0)) && isnan(lp_angle4(e1, o)), "the origin has no direction: NaN");
    CHECK(lp_angle4(e0, e1) == lp_angle4(e1, e0), "symmetric");

    enum { N = 103 }; double q[4] = { 0.3, -0.7, 0.2, 0.5 }, pts[4 * N], one[N], batch[N];
    for (int i = 0; i < N; i++) { double sc = i % 3 == 0 ? 1e-3 : i % 3 == 1 ? 1.0 : 1e3; for (int d = 0; d < 4; d++) pts[4 * i + d] = rnd() * sc; }
    for (int d = 0; d < 4; d++) { pts[4 * 5 + d] = q[d]; pts[4 * 6 + d] = -3.0 * q[d]; pts[4 * 7 + d] = 0.0; }   /* itself, opposite, origin */
    int pg_ok = 1;
    for (int i = 0; i < N; i++) { one[i] = lp_angle4(q, pts + 4 * i); if (i != 7 && memcmp(&one[i], &(double){ pg_angle(q, pts + 4 * i) }, 8)) pg_ok = 0; }
    CHECK(pg_ok, "lp_angle4 is <=> bit for bit");
    lp_angle4_batch(q, pts, N, batch);
    CHECK(!memcmp(one, batch, sizeof one), "batch (%s) equals lp_angle4 bit for bit", lp_cpu_describe(lp_cpu_active()));
    CHECK(batch[5] == 0.0 && batch[6] == PI && isnan(batch[7]), "batch: itself 0, opposite pi, origin NaN");
    lp_angle4_batch(q, pts + 4, 3, batch);                                     /* shorter than one vector */
    CHECK(!memcmp(one + 1, batch, 3 * sizeof(double)), "a batch of 3 too");

    double worst_rt = 0, worst_len = 0, worst_orth = 0;                          /* exp(log(p)) = p */
    for (int k = 0; k < 1000; k++) {
        double b[4], p[4], v[4], back[4];
        for (int d = 0; d < 4; d++) { b[d] = rnd(); p[d] = rnd(); } unit(b); unit(p);
        if (lp_angle4(b, p) > 3.0) continue;                                     /* near -b the map is ill-conditioned */
        lp_s3_log(b, p, v); lp_s3_exp(b, v, back);
        double g = gap(back, p), l = fabs(sqrt(((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]) + v[3] * v[3]) - lp_angle4(b, p));
        double orth = fabs(((b[0] * v[0] + b[1] * v[1]) + b[2] * v[2]) + b[3] * v[3]);
        if (g > worst_rt) worst_rt = g; if (l > worst_len) worst_len = l; if (orth > worst_orth) worst_orth = orth;
    }
    CHECK(worst_rt <= 1e-14, "exp(log(p)) is p within 1e-14 (%.3g)", worst_rt);
    CHECK(worst_len <= 1e-14, "|log(p)| is the angle within 1e-14 (%.3g)", worst_len);
    CHECK(worst_orth <= 1e-14, "log(p) is tangent at base within 1e-14 (%.3g)", worst_orth);
    { double v[4], z[4] = { 0, 0, 0, 0 }, back[4];
      lp_s3_log(e0, e0, v); CHECK(v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0, "log of base is 0");
      lp_s3_exp(r60, z, back); CHECK(!memcmp(back, r60, sizeof back), "exp of 0 is base exactly"); }

    { double a[4], b[4], r[4];                                                  /* slerp */
      for (int d = 0; d < 4; d++) { a[d] = rnd(); b[d] = rnd(); } unit(a); unit(b);
      lp_slerp4(a, b, 0.0, r); CHECK(!memcmp(r, a, sizeof r), "slerp at 0 is a exactly");
      lp_slerp4(a, b, 1.0, r); CHECK(!memcmp(r, b, sizeof r), "slerp at 1 is b exactly");
      lp_slerp4(a, b, 0.25, r);
      CHECK(fabs(lp_angle4(a, r) - 0.25 * lp_angle4(a, b)) <= 1e-14 && fabs(lp_angle4(r, b) - 0.75 * lp_angle4(a, b)) <= 1e-14, "slerp at 1/4 is a quarter along");
      lp_slerp4(e0, e1, 0.5, r); const double mid[4] = { 0.7071067811865476, 0.7071067811865476, 0, 0 };
      CHECK(gap(r, mid) <= 2.3e-16, "slerp(e0, e1, 1/2) is the midpoint"); }

    /* Karcher: symmetric sets have their centre as the mean. */
    { const double pair[8] = { 1, 0, 0, 0, 0, 1, 0, 0 }, mid[4] = { 0.7071067811865476, 0.7071067811865476, 0, 0 }; double r[4];
      CHECK(lp_karcher_mean4(pair, 2, r) && gap(r, mid) <= 1e-15, "Karcher of e0, e1 is their midpoint");
      CHECK(!lp_karcher_mean4(pair, 0, r), "no points: no mean"); }
    const double ax[4] = { 0.5, -0.5, 0.5, 0.5 }, t1[4] = { 0.5, 0.5, 0.5, -0.5 }, t2[4] = { 0.5, 0.5, -0.5, 0.5 };
    double ring[4 * 6], rev[4 * 6], r[4], rr[4];
    for (int k = 0; k < 6; k++) {                                               /* a ring of six at 0.6 rad about ax */
        double phi = k * PI / 3, c = cos(0.6), sn = sin(0.6);
        for (int d = 0; d < 4; d++) ring[4 * k + d] = c * ax[d] + sn * (cos(phi) * t1[d] + sin(phi) * t2[d]);
    }
    for (int k = 0; k < 6; k++) memcpy(rev + 4 * k, ring + 4 * ((k * 5 + 2) % 6), 4 * sizeof(double));
    CHECK(lp_karcher_mean4(ring, 6, r) && gap(r, ax) <= 1e-12, "Karcher of a symmetric ring is its axis (%.3g)", gap(r, ax));
    CHECK(lp_karcher_mean4(rev, 6, rr) && !memcmp(r, rr, sizeof r), "Karcher: any order, the same bits");

    /* Markley: the same ring, with half its points negated (p and -p alike), and the axis's sign canonical. */
    for (int k = 0; k < 6; k += 2) for (int d = 0; d < 4; d++) ring[4 * k + d] = -ring[4 * k + d];
    for (int k = 0; k < 6; k++) memcpy(rev + 4 * k, ring + 4 * ((k * 5 + 2) % 6), 4 * sizeof(double));
    lp_eigen_centroid4(ring, 6, r); lp_eigen_centroid4(rev, 6, rr);
    CHECK(gap(r, ax) <= 1e-14, "eigen-centroid of the ring is its axis (%.3g)", gap(r, ax));
    CHECK(!memcmp(r, rr, sizeof r), "eigen-centroid: any order, the same bits");
    { const double neg[4] = { -0.5, 0.5, -0.5, -0.5 }; lp_eigen_centroid4(neg, 1, r);
      CHECK(gap(r, ax) <= 1e-15 && r[0] > 0, "eigen-centroid: first nonzero component positive"); }
    { const double e3[4] = { 0, 0, 0, -1 }; lp_eigen_centroid4(e3, 1, r);
      CHECK(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 1, "eigen-centroid of -e3 is e3"); }
    lp_eigen_centroid4(ring, 0, r); CHECK(isnan(r[0]), "no points: NaN");
    DONE("s3");
}
