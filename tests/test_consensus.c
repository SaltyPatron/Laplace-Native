/* Consensus: Glickman's worked Glicko-2 example, the trust-to-deviation mapping, and signed trust. */
#include "laplace/laplace.h"
#include "check.h"
#include <math.h>
#include <string.h>

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
