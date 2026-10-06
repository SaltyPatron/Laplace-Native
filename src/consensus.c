/* Consensus: Glicko-2 (Glickman, "Example of the Glicko-2 system"), and attestations played as matchups whose
 * opponent weight g(phi) equals the witness's trust; and a witness's series of games, solved as one update. */
#include "laplace/laplace.h"
#include <math.h>

#define PI 3.14159265358979323846

static double g(double phi){ return 1.0 / sqrt(1.0 + 3.0 * phi * phi / (PI * PI)); }

static void rate(lp_rating *r, const lp_rating *opp, const double *score, size_t n, double tau, int period);
void lp_glicko2(lp_rating *r, const lp_rating *opp, const double *score, size_t n, double tau){ rate(r, opp, score, n, tau, 1); }
void lp_matchup(lp_rating *r, const lp_rating *opp, double score, double tau){ rate(r, opp, &score, 1, tau, 0); }
static void rate(lp_rating *r, const lp_rating *opp, const double *score, size_t n, double tau, int period){
    const double S = LP_GLICKO_SCALE;
    double mu = (r->rating - LP_GLICKO_RATING) / S, phi = r->deviation / S, sigma = r->volatility;
    if (n == 0) { r->deviation = S * sqrt(phi * phi + sigma * sigma); return; }             /* step 6 only */
    double vinv = 0, dsum = 0;
    for (size_t j = 0; j < n; j++) {
        double muj = (opp[j].rating - LP_GLICKO_RATING) / S, gj = g(opp[j].deviation / S);
        double E = 1.0 / (1.0 + exp(-gj * (mu - muj)));
        vinv += gj * gj * E * (1.0 - E); dsum += gj * (score[j] - E);
    }
    double v = 1.0 / vinv, delta = v * dsum, a = log(sigma * sigma), eps = 1e-6;
    #define F(x) (exp(x) * (delta * delta - phi * phi - v - exp(x)) / (2.0 * (phi * phi + v + exp(x)) * (phi * phi + v + exp(x))) - ((x) - a) / (tau * tau))
    double A = a, B;
    if (delta * delta > phi * phi + v) B = log(delta * delta - phi * phi - v);
    else { double k = 1; while (F(a - k * tau) < 0) k += 1; B = a - k * tau; }
    double fA = F(A), fB = F(B);
    for (int it = 0; it < 100 && fabs(B - A) > eps; it++) {                              /* Illinois algorithm */
        double C = A + (A - B) * fA / (fB - fA), fC = F(C);
        if (fC * fB <= 0) { A = B; fA = fB; } else fA /= 2.0;
        B = C; fB = fC;
    }
    #undef F
    double sig2 = exp(A / 2.0), phistar = period ? sqrt(phi * phi + sig2 * sig2) : phi;
    double phi2 = 1.0 / sqrt(1.0 / (phistar * phistar) + 1.0 / v);
    r->rating = LP_GLICKO_RATING + S * (mu + phi2 * phi2 * dsum);
    r->deviation = S * phi2; r->volatility = sig2;
}

double lp_trust_deviation(double t){
    t = fabs(t);
    if (t >= 1.0) return 0.0;
    if (t <= 0.0) return INFINITY;
    return LP_GLICKO_SCALE * (PI / sqrt(3.0)) * sqrt(1.0 / (t * t) - 1.0);
}

void lp_attest(lp_rating *r, double trust, double score, double opp_rating, double tau, double floor){
    if (trust == 0.0) return;                                                               /* no information */
    if (trust < 0.0) score = 1.0 - score;                                                   /* reliably wrong: flip */
    lp_rating o = { opp_rating, lp_trust_deviation(trust), LP_GLICKO_VOLATILITY };
    lp_matchup(r, &o, score, tau);
    if (r->deviation < floor) r->deviation = floor;
}

/* A witness's series, solved as one update. The posterior over the claim's strength mu (Glicko-2's scale, the anchor at
 * 0) is its prior N(mu0, phi^2) times n games at s_eff against the anchor with weight g = 1:
 *   log p(mu) = -(mu - mu0)^2 / (2 phi^2) + n [s_eff ln E + (1 - s_eff) ln(1 - E)],  E = 1 / (1 + e^-mu),
 * whose slope -(mu - mu0) / phi^2 + n (s_eff - E) falls strictly, so its one zero (the mode) is found by bisection. The
 * zero lies between mu0 and logit(s_eff), and never farther than n phi^2 from mu0 (|s_eff - E| < 1): that is the
 * bracket. Halving stops when the midpoint is one of its ends, so the answer is the same bits on every machine. */
void lp_attest_series(lp_rating *r, double trust, uint32_t games, double score, double floor){
    if (trust == 0.0 || games == 0) return;                                               /* no information */
    if (score < 0.0) score = 0.0; else if (score > 1.0) score = 1.0;
    if (trust < 0.0) { score = 1.0 - score; trust = -trust; }                              /* reliably wrong: flip */
    if (trust > 1.0) trust = 1.0;
    const double S = LP_GLICKO_SCALE, n = (double)games;
    double se = 0.5 + trust * (score - 0.5);                                               /* the vote, bounded by trust */
    double mu0 = (r->rating - LP_GLICKO_RATING) / S, phi = r->deviation / S, w = n * phi * phi;
    double L = se <= 0.0 ? -INFINITY : se >= 1.0 ? INFINITY : log(se / (1.0 - se));
    double lo = mu0, hi = mu0;
    if (L > mu0) hi = L < mu0 + w ? L : mu0 + w; else lo = L > mu0 - w ? L : mu0 - w;
    for (int it = 0; it < 2000; it++) {
        double m = lo + 0.5 * (hi - lo);
        if (m <= lo || m >= hi) break;
        double slope = -(m - mu0) / (phi * phi) + n * (se - 1.0 / (1.0 + exp(-m)));
        if (slope > 0.0) lo = m; else hi = m;
    }
    double mu = lo + 0.5 * (hi - lo), E = 1.0 / (1.0 + exp(-mu));
    double solved = S / sqrt(1.0 / (phi * phi) + n * E * (1.0 - E));
    double fw = lp_series_floor(trust, floor);
    double dev = solved > fw ? solved : fw;                                                /* one witness, bounded certainty */
    if (dev > r->deviation) dev = r->deviation;                                            /* and never less certain than before */
    r->rating = LP_GLICKO_RATING + S * mu; r->deviation = dev;                             /* volatility: unchanged */
}

double lp_series_floor(double trust, double floor){
    double d = lp_trust_deviation(trust);
    if (d < floor) d = floor;
    return d > LP_GLICKO_DEVIATION ? LP_GLICKO_DEVIATION : d;
}

double lp_entry_deviation(double trust){
    double t = fabs(trust); if (t == 0.0) return LP_GLICKO_DEVIATION;
    double d = lp_trust_deviation(t); return d < LP_ATTEST_FLOOR ? LP_ATTEST_FLOOR : d;
}

/* A standing read k deviations below its rating, as log-odds of beating the anchor on Glicko-2's scale. */
static double reading(const lp_rating *r, double k){ return (r->rating - LP_GLICKO_RATING) / LP_GLICKO_SCALE - k * r->deviation / LP_GLICKO_SCALE; }
/* A claim's chance of beating the anchor, so read. */
double lp_confidence(const lp_rating *r, double k){ return 1.0 / (1.0 + exp(-reading(r, k))); }
double lp_cost(const lp_rating *r, double k, double per_hop){ return log1p(exp(-reading(r, k))) + per_hop; }   /* -ln(1 / (1 + e^-x)) */
