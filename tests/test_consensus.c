/* Consensus: Glickman's worked Glicko-2 example, the trust-to-deviation mapping, signed trust, and a witness's series
 * solved as one update. */
#include "laplace/laplace.h"
#include "laplace/consensus.h"
#include "check.h"
#include <math.h>
#include <string.h>
#include <errno.h>
#include <float.h>

static int standing_equal(const lp_standing *a, const lp_standing *b){
    return !memcmp(&a->rating, &b->rating, sizeof(double))
        && !memcmp(&a->deviation, &b->deviation, sizeof(double))
        && !memcmp(&a->volatility, &b->volatility, sizeof(double))
        && a->matches == b->matches && a->witnesses == b->witnesses && a->iterations == b->iterations;
}

static lp_series_entry observation(unsigned root, uint64_t games, double wins, double trust){
    lp_id id; lp_id_codepoint(root, &id); return lp_series_observation(id, games, wins, trust);
}

static void relabelled_roots(void){
    lp_series_entry entries[64];
    for(unsigned i=0;i<32;i++){
        uint64_t first=8*(i+1)*17,second=8*(i+1)*100;
        entries[2*i]=observation('a'+i,first,(double)first*(i%9)/8,.3+.1*(i%6));
        entries[2*i+1]=observation('a'+i,second,(double)second*((i*3+1)%9)/8,.6+.05*(i%7));
        entries[2*i].opponent.rating=entries[2*i+1].opponent.rating=1200+19*i;
        entries[2*i].opponent.deviation=entries[2*i+1].opponent.deviation=30+4*i;
    }
    lp_standing expected={0};int ready=lp_series(entries,64,&expected);
    CHECK(ready,"mixed dependence partitions solve before relabeling");
    if(!ready)return;
    for(unsigned pass=0;pass<128;pass++){
        for(unsigned i=0;i<32;i++){
            lp_id root;lp_id_codepoint(0x400+((i*(2*pass+1)+pass)%32),&root);
            entries[2*i].dependence_root=entries[2*i+1].dependence_root=root;
        }
        lp_standing actual={0};
        int equal=lp_series(entries,64,&actual) && standing_equal(&expected,&actual);
        CHECK(equal,"root labels preserve standing bits (pass %u): %.17g/%.17g/%.17g iterations %u versus %.17g/%.17g/%.17g iterations %u",
              pass,actual.rating,actual.deviation,actual.volatility,actual.iterations,
              expected.rating,expected.deviation,expected.volatility,expected.iterations);
        if(!equal)return;
    }
}

static void rooted_series(void){
    relabelled_roots();
    const double trusts[] = {0.5,0.85,0.99};
    const uint64_t counts[] = {1,10,1000,1000000};
    for (size_t t = 0; t < 3; t++) {
        double precision = lp_witness_precision(trusts[t], LP_ATTEST_FLOOR);
        double phi = LP_GLICKO_DEVIATION / LP_GLICKO_SCALE;
        double performance = log((1 + trusts[t]) / (1 - trusts[t]));
        double asymptote = LP_GLICKO_RATING + LP_GLICKO_SCALE * precision * performance / (1 / (phi * phi) + precision);
        double last = LP_GLICKO_RATING, last_rd = LP_GLICKO_DEVIATION + 1; lp_standing out = {0};
        for (size_t c = 0; c < 4; c++) {
            lp_series_entry e = observation('a', counts[c], (double)counts[c], trusts[t]);
            bool ok = lp_series(&e, 1, &out);
            CHECK(ok && isfinite(out.rating) && isfinite(out.deviation) && isfinite(out.volatility),
                  "rooted series t=%g n=%llu converges", trusts[t], (unsigned long long)counts[c]);
            CHECK(out.rating > last && out.rating <= asymptote + 1e-9 && out.deviation < last_rd,
                  "more games approach the joint-posterior ceiling %.9f: %.9f / %.9f", asymptote, out.rating, out.deviation);
            CHECK(out.iterations <= 64 && out.matches == counts[c] && out.witnesses == 1,
                  "series is solved in bounded iterations, not per game");
            printf("series: trust=%.2f roots=1 games=%llu rating=%.9f RD=%.9f iterations=%u limit=%.9f\n",
                   trusts[t], (unsigned long long)counts[c], out.rating, out.deviation, out.iterations, asymptote);
            last = out.rating; last_rd = out.deviation;
        }
        CHECK(fabs(out.rating - asymptote) < 0.05, "million-game result matches the analytical source/prior asymptote");
        lp_series_entry e[2] = {observation('a', 1000000, 1000000, trusts[t]), observation('b', 1000000, 1000000, trusts[t])};
        lp_standing corroborated;
        CHECK(lp_series(e, 2, &corroborated), "independent corroboration solves");
        CHECK(corroborated.rating > asymptote && corroborated.deviation < out.deviation && corroborated.witnesses == 2,
              "independent sources exceed one source's certainty-limited rating ceiling: %.9f > %.9f, RD %.9f < %.9f",
              corroborated.rating, asymptote, corroborated.deviation, out.deviation);
        double independent_limit = LP_GLICKO_RATING + LP_GLICKO_SCALE * 2 * precision * performance / (1 / (phi * phi) + 2 * precision);
        CHECK(fabs(corroborated.rating - independent_limit) < 0.05, "two-root asymptote follows summed independent precision");
        printf("series: trust=%.2f roots=2 games=2000000 rating=%.9f RD=%.9f iterations=%u limit=%.9f\n",
               trusts[t], corroborated.rating, corroborated.deviation, corroborated.iterations, independent_limit);
        if (trusts[t] == .85) {
            /* Independent 28-digit decimal solution of the joint stationary
             * equations and volatility root, using bisection throughout. */
            CHECK(fabs(out.rating - 1832.827344606) < 1e-8 && fabs(out.deviation - 170.547513798) < 1e-8
                  && fabs(corroborated.rating - 1877.652957569) < 1e-8 && fabs(corroborated.deviation - 128.453983205) < 1e-8,
                  "Native matches independently computed decimal reference standings");
        }
    }
    lp_series_entry dependent[] = {observation('a', 1000, 600, .5), observation('a', 10000, 10000, .85)};
    lp_series_entry combined = observation('a', 11000, 10600, .85);
    lp_standing folded, one;
    CHECK(lp_series(dependent, 2, &folded) && lp_series(&combined, 1, &one) && standing_equal(&folded, &one),
          "one dependence root is bit-identical to summed games/wins and maximum trust");
    lp_series_entry entries[] = {observation('a', 1000, 600, .5), observation('b', 17, 4.5, .9),
                               observation('a', 2000, 1500, .85), observation('c', 200, 175, .95)};
    lp_standing expected; CHECK(lp_series(entries, 4, &expected), "mixed source result solves");
    unsigned random = 17;
    for (int pass = 0; pass < 128; pass++) {
        for (size_t i = 3; i > 0; i--) {
            random = random * 1664525u + 1013904223u; size_t j = random % (i + 1);
            lp_series_entry tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp;
        }
        lp_standing actual;
        CHECK(lp_series(entries, 4, &actual) && standing_equal(&expected, &actual), "permutation %d has identical standing bits", pass);
    }
    lp_series_entry zero = observation('a', 1000000, 1000000, 0); lp_standing out;
    CHECK(lp_series(&zero, 1, &out) && out.rating == LP_GLICKO_RATING && out.deviation == LP_GLICKO_DEVIATION
          && out.volatility == LP_GLICKO_VOLATILITY && out.witnesses == 0, "zero trust carries no information however many games");
    lp_series_entry good[4];
    for (unsigned i = 0; i < 3; i++) good[i] = observation('a' + i, 1000, 1000, .85);
    CHECK(lp_series(good, 3, &expected), "three independent curated sources solve");
    good[3] = observation('z', 1000000, 0, .001);
    CHECK(lp_series(good, 4, &out) && fabs(out.rating - expected.rating) < .01,
          "one low-trust dependence root cannot overwhelm independent corroboration by repeating a million times");
    lp_series_entry hard = observation('a', 1000000, 1000000, 1);
    CHECK(lp_series(&hard, 1, &out) && isfinite(out.rating) && out.rating > LP_GLICKO_RATING, "fully trusted decisive evidence stays finite without a rating clamp");
    lp_standing win = out; hard.wins = 0;
    CHECK(lp_series(&hard, 1, &out) && fabs(win.rating + out.rating - 2 * LP_GLICKO_RATING) < 1e-8
          && fabs(win.deviation - out.deviation) < 1e-8, "affirmation/refutation symmetry: %.17g / %.17g vs %.17g / %.17g",
          win.rating, win.deviation, out.rating, out.deviation);
    lp_standing sentinel = {123,456,.07,89,10,11}; out = sentinel;
    lp_series_entry bad = observation('a', 1, 2, .85);
    CHECK(!lp_series(&bad, 1, &out) && standing_equal(&out, &sentinel), "invalid outcomes do not mutate the output");
    bad.wins = 1; bad.trust = NAN;
    CHECK(!lp_series(&bad, 1, &out) && standing_equal(&out, &sentinel), "NaN trust is rejected");
    bad.trust = -.5;
    CHECK(!lp_series(&bad, 1, &out) && standing_equal(&out, &sentinel), "rooted reliability is validated in [0,1]");
    lp_series_entry inconsistent[] = {observation('a',10,10,.85),observation('a',10,10,.85)};
    inconsistent[1].opponent.rating = 1600; out = sentinel;
    CHECK(!lp_series(inconsistent, 2, &out) && standing_equal(&out, &sentinel),
          "one dependence root cannot silently mix different comparison anchors");
    lp_series_entry wide = observation('a',1000000,1000000,.85); wide.opponent.deviation = DBL_MAX;
    CHECK(lp_series(&wide,1,&out) && out.rating == LP_GLICKO_RATING && out.deviation == LP_GLICKO_DEVIATION,
          "an uninformative comparison contributes no numerical artifact");
    CHECK(lp_series(NULL, 0, &out) && out.rating == LP_GLICKO_RATING && !out.matches && !out.witnesses,
          "an empty witness set is the stock unknown standing");
    CHECK(lp_witness_precision(0,30) == 0 && lp_witness_precision(.1,30) < lp_witness_precision(.5,30)
          && lp_witness_precision(.5,30) < lp_witness_precision(.85,30) && isfinite(lp_witness_precision(1,30)),
          "source certainty increases with trust and always has a nonzero deviation floor");
}

/* A stock claim (1500/350) after one witness's series. */
static lp_rating series(double t, uint32_t n, double s){ lp_rating r = lp_rating_stock(); lp_attest_series(&r, t, n, s, LP_ATTEST_FLOOR); return r; }

int main(void){
    rooted_series();
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
