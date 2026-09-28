/* Consensus: Glicko-2 (Glickman, "Example of the Glicko-2 system"), and attestations played as matchups whose
 * opponent weight g(phi) equals the witness's trust. */
#include "laplace/laplace.h"
#include <math.h>

#define PI 3.14159265358979323846

static double g(double phi){ return 1.0 / sqrt(1.0 + 3.0 * phi * phi / (PI * PI)); }

void lp_glicko2(lp_rating *r, const lp_rating *opp, const double *score, size_t n, double tau){
    const double S = LP_GLICKO_SCALE;
    double mu = (r->rating - 1500.0) / S, phi = r->deviation / S, sigma = r->volatility;
    if (n == 0) { r->deviation = S * sqrt(phi * phi + sigma * sigma); return; }             /* step 6 only */
    double vinv = 0, dsum = 0;
    for (size_t j = 0; j < n; j++) {
        double muj = (opp[j].rating - 1500.0) / S, gj = g(opp[j].deviation / S);
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
    double sig2 = exp(A / 2.0), phistar = sqrt(phi * phi + sig2 * sig2);
    double phi2 = 1.0 / sqrt(1.0 / (phistar * phistar) + 1.0 / v);
    r->rating = 1500.0 + S * (mu + phi2 * phi2 * dsum);
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
    lp_rating o = { opp_rating, lp_trust_deviation(trust), 0.06 };
    lp_glicko2(r, &o, &score, 1, tau);
    if (r->deviation < floor) r->deviation = floor;
}
