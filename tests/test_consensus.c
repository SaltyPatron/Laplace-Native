/* Consensus: Glickman's worked Glicko-2 example, the trust-to-deviation mapping, signed trust, and a witness's series
 * solved as one update. */
#include "laplace/laplace.h"
#include "check.h"
#include <math.h>
#include <string.h>

/* A stock claim (1500/350) after one witness's series. */
static lp_rating series(double t, uint32_t n, double s){ lp_rating r = lp_rating_stock(); lp_attest_series(&r, t, n, s, LP_ATTEST_FLOOR); return r; }

int main(void){
    /* Glickman, "Example of the Glicko-2 system": 1500/200/0.06 plays 1400/30 (win), 1550/100 (loss),
     * 1700/300 (loss) with tau 0.5, and ends at 1464.06 / 151.52 / 0.05999. */
    lp_rating r = { 1500, 200, 0.06 };
    lp_rating opp[3] = { { 1400, 30, 0.06 }, { 1550, 100, 0.06 }, { 1700, 300, 0.06 } };
    double s[3] = { 1, 0, 0 };
    lp_glicko2(&r, opp, s, 3, 0.5);
    CHECK(fabs(r.rating - 1464.06) < 0.01, "rating %.4f", r.rating);
    CHECK(fabs(r.deviation - 151.52) < 0.01, "deviation %.4f", r.deviation);
    CHECK(fabs(r.volatility - 0.05999) < 0.00001, "volatility %.6f", r.volatility);

    /* Trust t plays with the deviation whose weight g equals |t|; 0.669 is Glicko's unrated 350. */
    CHECK(lp_trust_deviation(1.0) == 0.0, "trust 1 plays with deviation 0");
    CHECK(fabs(lp_trust_deviation(0.9) - 152.6) < 0.5, "trust 0.9 -> %.1f", lp_trust_deviation(0.9));
    CHECK(fabs(lp_trust_deviation(0.669) - 350) < 1, "trust 0.669 -> %.1f", lp_trust_deviation(0.669));
    CHECK(isinf(lp_trust_deviation(0.0)), "trust 0 carries no information");

    /* Signed trust: a loss from a witness at -t equals a win from one at +t; trust 0 changes nothing. */
    lp_rating a = { 1500, 350, 0.06 }, b = a, c = a;
    lp_attest(&a, 0.8, 1.0, 1500, 0.5, 30); lp_attest(&b, -0.8, 0.0, 1500, 0.5, 30); lp_attest(&c, 0.0, 1.0, 1500, 0.5, 30);
    CHECK(a.rating == b.rating && a.deviation == b.deviation, "flip: %.6f vs %.6f", a.rating, b.rating);
    CHECK(c.rating == 1500 && c.deviation == 350, "trust 0 changes nothing");
    CHECK(a.rating > 1500, "a win raises the standing");
    lp_rating lo = { 1500, 350, 0.06 }; lp_attest(&lo, 0.3, 1.0, 1500, 0.5, 30);
    CHECK(lo.rating - 1500 < a.rating - 1500, "low trust moves the standing less");
    /* A witness's series, solved as one update from a stock claim (1500/350), against Python's computation of the same
     * rule: s_eff = 0.5 + t (s - 0.5) against the anchor, the posterior mode by bisection, the deviation from the
     * information, floored by the witness's own trust deviation. */
    lp_rating s1 = series(0.9, 1, 1.0), s30 = series(0.9, 30, 1.0), s1k = series(0.9, 1000, 1.0), s1m = series(0.9, 1000000, 1.0);
    CHECK(fabs(s1.rating - 1663.04) < 0.05 && fabs(s1.deviation - 259.40) < 0.05, "0.9 x1 -> %.2f / %.2f", s1.rating, s1.deviation);
    CHECK(fabs(s30.rating - 1946.43) < 0.05, "0.9 x30 -> %.2f", s30.rating);
    CHECK(fabs(s1k.rating - 2008.88) < 0.05, "0.9 x1000 -> %.2f", s1k.rating);
    CHECK(fabs(s1m.rating - 2011.50) < 0.05 && s1m.rating < 1500 + LP_GLICKO_SCALE * log(0.95 / 0.05), "0.9 x10^6 -> %.2f, under the ceiling 2011.5", s1m.rating);
    CHECK(fabs(s1m.deviation - 152.60) < 0.05 && s1m.deviation == lp_trust_deviation(0.9), "0.9 x10^6 stops at its trust's deviation: %.2f", s1m.deviation);
    CHECK(s30.deviation == s1m.deviation && s1k.deviation == s1m.deviation, "the floor holds however many games");
    lp_rating w1k = series(0.2, 1000, 1.0), w1m = series(0.2, 1000000, 1.0);
    CHECK(fabs(w1k.rating - 1570.36) < 0.05 && w1m.rating < 1500 + LP_GLICKO_SCALE * log(0.6 / 0.4), "0.2 x1000 -> %.2f, ceiling 1570.4", w1k.rating);
    CHECK(w1k.deviation == 350.0 && w1m.deviation == 350.0, "trust 0.2 is noted: no added certainty (%.2f)", w1m.deviation);
    lp_rating t3 = series(0.3, 1000000, 1.0);
    CHECK(t3.deviation >= 350.0 && t3.rating <= 1608.0 && t3.rating > 1607.0, "0.3 x10^6 -> %.2f / %.2f: at most its ceiling 1608, no certainty", t3.rating, t3.deviation);
    /* symmetric: a refuting series mirrors an affirming one about the anchor; a flipped trust is the other's mirror */
    lp_rating rf = series(0.9, 1000000, 0.0), nf = series(-0.9, 1000000, 1.0);
    CHECK(fabs(rf.rating - 988.50) < 0.05 && fabs((rf.rating - 1500) + (s1m.rating - 1500)) < 1e-6 && rf.deviation == s1m.deviation, "refuted -> %.2f", rf.rating);
    CHECK(nf.rating == rf.rating && nf.deviation == rf.deviation, "trust -0.9 affirming = trust 0.9 refuting");
    lp_rating z = series(0.0, 1000, 1.0), z0 = series(0.9, 0, 1.0), dr = series(0.9, 1000, 0.5);
    CHECK(z.rating == 1500 && z.deviation == 350 && z0.rating == 1500 && z0.deviation == 350, "trust 0 or no games changes nothing");
    CHECK(fabs(dr.rating - 1500) < 1e-9, "a drawn series leaves the rating at the anchor (%.6f)", dr.rating);
    /* monotone in games and in trust */
    double last = 1500; int mono = 1;
    for (uint32_t n = 1; n <= 1u << 20; n *= 2) { lp_rating x = series(0.9, n, 1.0); if (!(x.rating > last)) mono = 0; last = x.rating; }
    CHECK(mono, "more games raise an affirmed claim, never past the ceiling");
    last = 1500; double lastd = 351; mono = 1;
    for (int k = 1; k <= 19; k++) { lp_rating x = series(k / 20.0, 1000, 1.0); if (!(x.rating > last) || !(x.deviation <= lastd)) mono = 0; last = x.rating; lastd = x.deviation; }
    CHECK(mono, "higher trust stands higher and no less certain, at equal games");
    /* the per-witness floor: its trust deviation, between 30 and 350 */
    CHECK(fabs(lp_series_floor(0.95, LP_ATTEST_FLOOR) - 103.56) < 0.01 && fabs(lp_series_floor(0.9, LP_ATTEST_FLOOR) - 152.60) < 0.01
          && fabs(lp_series_floor(0.85, LP_ATTEST_FLOOR) - 195.27) < 0.01, "floors %.2f %.2f %.2f", lp_series_floor(0.95, 30), lp_series_floor(0.9, 30), lp_series_floor(0.85, 30));
    CHECK(lp_series_floor(0.669, 30) == 350 && lp_series_floor(0.1, 30) == 350 && lp_series_floor(1.0, 30) == 30 && lp_series_floor(0.999, 30) == 30, "clamped to [30, 350]");
    lp_rating h95 = series(0.95, 1000000, 1.0), h85 = series(0.85, 1000000, 1.0);
    CHECK(h95.deviation == lp_series_floor(0.95, 30) && h85.deviation == lp_series_floor(0.85, 30), "x10^6 stops at RD %.2f (0.95), %.2f (0.85)", h95.deviation, h85.deviation);
    /* certainty beyond one witness comes from others; a series never raises a deviation others took below its floor */
    lp_rating m = s1m; lp_attest_series(&m, 0.95, 1000, 1.0, LP_ATTEST_FLOOR);
    CHECK(m.deviation == lp_series_floor(0.95, 30), "a second, more trusted witness takes 152.6 to %.2f", m.deviation);
    double before = m.deviation; lp_attest_series(&m, 0.9, 1000000, 1.0, LP_ATTEST_FLOOR);
    CHECK(m.deviation == before, "a 0.9 series leaves a deviation of %.2f where it is", m.deviation);
    lp_attest_series(&m, 0.3, 1000000, 0.0, LP_ATTEST_FLOOR);
    CHECK(m.deviation == before && m.rating < 2136.42, "a low-trust refutation moves the rating (%.2f), not the certainty", m.rating);
    /* volatility is left as it was; lp_attest still plays one matchup */
    CHECK(s1m.volatility == LP_GLICKO_VOLATILITY, "volatility unchanged");
    /* The trust classes: the registry's fourteen, in its order; an undeclared label is no class. */
    CHECK(lp_trust_class_count() == 14, "%zu classes", lp_trust_class_count());
    CHECK(!strcmp(lp_trust_class_at(0)->label, "SubstrateMandate") && lp_trust_class_at(0)->prior == 1.0, "the first class is the mandate at 1");
    CHECK(lp_trust_class_named("StandardsDerived") && lp_trust_class_named("StandardsDerived")->prior == 0.95, "StandardsDerived plays at 0.95");
    CHECK(lp_trust_class_named("AcademicCurated")->prior > lp_trust_class_named("UserCuratedResource")->prior, "an academic curation ranks above a user-curated wiki");
    CHECK(lp_trust_class_named("AIModelProbe")->prior > lp_trust_class_named("UserPromptContent")->prior, "a model ranks above a user prompt");
    CHECK(lp_trust_class_named("ResponseContent")->prior < lp_trust_class_named("UserPromptContent")->prior, "Laplace's own output ranks below a user prompt");
    CHECK(lp_trust_class_named("academiccurated") == NULL && lp_trust_class_named("") == NULL, "an undeclared label is no class");
    DONE("consensus");
}
