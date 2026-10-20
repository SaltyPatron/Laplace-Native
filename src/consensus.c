/* Consensus: Glicko-2 (Glickman, "Example of the Glicko-2 system"), and attestations played as matchups whose
 * opponent weight g(phi) equals the witness's trust; and a witness's series of games, solved as one update. */
#include "laplace/laplace.h"
#include "laplace/consensus.h"
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

/* Rooted series use a shared source uncertainty. Repeating an observation may
 * estimate that source's performance, but cannot manufacture independent
 * knowledge of the claim. See consensus.h for the joint concave posterior. */
lp_series_entry lp_series_observation(lp_id root, uint64_t games, double wins, double trust){
    return (lp_series_entry){ .dependence_root = root, .games = games, .wins = wins, .trust = trust,
                             .opponent = { LP_GLICKO_RATING, 0, LP_GLICKO_VOLATILITY } };
}

typedef struct { double games, score, anchor, weight, precision, performance; } SeriesRoot;

static void series_likelihood(const SeriesRoot *r, double x, double *score, double *information){
    double z = r->weight * (x - r->anchor), tail = exp(-fabs(z)), denominator = 1 + tail;
    /* Compute the small tail directly: 1-logistic(z) loses significant bits
     * for decisive long series and breaks affirmation/refutation symmetry. */
    double residual = z >= 0 ? (r->score - 1) + tail / denominator : r->score - tail / denominator;
    *score = r->games * r->weight * residual;
    *information = r->games * r->weight * r->weight * tail / (denominator * denominator);
}
static int compare_number(double a, double b){ return a < b ? -1 : a > b; }
static int series_order(const void *a, const void *b){
    const lp_series_entry *x = a, *y = b; int c = lp_id_cmp(&x->dependence_root, &y->dependence_root);
    if (c) return c;
    if (x->games != y->games) return x->games < y->games ? -1 : 1;
    if ((c = compare_number(x->wins, y->wins))) return c;
    if ((c = compare_number(x->trust, y->trust))) return c;
    if ((c = compare_number(x->witness_deviation, y->witness_deviation))) return c;
    if ((c = compare_number(x->opponent.rating, y->opponent.rating))) return c;
    return compare_number(x->opponent.deviation, y->opponent.deviation);
}

/* Solve one source's latent performance at a fixed claim rating. The slope is
 * strictly decreasing; safeguarded Newton stays in the analytical bracket. */
static bool source_mode(SeriesRoot *r, double mu, double *score, double *information){
    double p = r->precision, n = r->games, w = r->weight, lo, hi;
    if (p == 0 || w == 0) { *score = *information = 0; return true; }
    double target;
    if (r->score > 0 && r->score < 1) {
        target = r->anchor + (log(r->score) - log1p(-r->score)) / w;
        lo = fmin(mu, target); hi = fmax(mu, target);
    } else {
        double reach = n * w / p;
        if (!isfinite(reach)) return false;
        lo = r->score == 0 ? mu - reach : mu;
        hi = r->score == 1 ? mu + reach : mu;
    }
    if (!isfinite(lo) || !isfinite(hi)) return false;
    double x = r->performance;
    if (!(x >= lo && x <= hi)) x = mu;
    bool converged = lo == hi;
    for (unsigned i = 0; !converged && i < 64; i++) {
        double data_score, j; series_likelihood(r, x, &data_score, &j);
        double slope = p * (mu - x) + data_score;
        if (slope > 0) lo = x; else if (slope < 0) hi = x; else { converged = true; break; }
        double next = x + slope / (p + j);
        if (next == x) { converged = true; break; }
        if (!(next > lo && next < hi)) next = lo + (hi - lo) * 0.5;
        if (hi - lo <= 1e-13 || next == x || next == lo || next == hi) { x = next; converged = true; break; }
        x = next;
    }
    if (!converged) return false;
    double data_score, j; series_likelihood(r, x, &data_score, &j);
    r->performance = x;
    *score = p * (x - mu);
    *information = j > 0 ? 1 / (1 / j + 1 / p) : 0;
    return isfinite(*score) && isfinite(*information);
}

static bool posterior(SeriesRoot *roots, size_t n, double mu, double mu0, double prior_precision,
                      double *slope, double *information, double *data_score){
    double score = 0, info = 0;
    for (size_t i = 0; i < n; i++) {
        double s, j; if (!source_mode(roots + i, mu, &s, &j)) return false;
        score += s; info += j;
    }
    *slope = (mu0 - mu) * prior_precision + score; *information = info; *data_score = score;
    return isfinite(*slope) && isfinite(info) && isfinite(score);
}

static double volatility_slope(double x, double a, double phi2, double v, double delta, double tau){
    double ex = exp(x), denominator = phi2 + v + ex;
    return ex / denominator * ((delta * delta - phi2 - v - ex) / denominator) * 0.5 - (x - a) / (tau * tau);
}
static bool series_volatility(double sigma, double phi2, double v, double delta, double *out){
    double a = 2 * log(sigma), A = a, B, tau = LP_ATTEST_TAU;
    if (delta * delta > phi2 + v) B = log(delta * delta - phi2 - v);
    else {
        unsigned k;
        for (k = 1; k <= 64; k++) if (volatility_slope(a - k * tau, a, phi2, v, delta, tau) >= 0) break;
        if (k > 64) return false;
        B = a - k * tau;
    }
    double fA = volatility_slope(A, a, phi2, v, delta, tau), fB = volatility_slope(B, a, phi2, v, delta, tau);
    if (!isfinite(fA) || !isfinite(fB)) return false;
    for (unsigned i = 0; i < 64; i++) {
        if (fabs(B - A) <= 1e-12) { *out = exp(A * 0.5); return isfinite(*out) && *out > 0; }
        double C = A + (A - B) * fA / (fB - fA);
        if (!isfinite(C) || C <= fmin(A, B) || C >= fmax(A, B)) C = A + (B - A) * 0.5;
        double fC = volatility_slope(C, a, phi2, v, delta, tau);
        if (!isfinite(fC)) return false;
        if (fC == 0) { *out = exp(C * 0.5); return isfinite(*out) && *out > 0; }
        if ((fC < 0) != (fB < 0)) { A = B; fA = fB; } else fA *= 0.5;
        B = C; fB = fC;
    }
    return false;
}

bool lp_series_update(const lp_rating *prior, const lp_series_entry *entries, size_t n, lp_standing *out){
    if (!prior || !out || (n && !entries) || n > SIZE_MAX / sizeof(lp_series_entry)
        || !isfinite(prior->rating) || !isfinite(prior->deviation) || prior->deviation <= 0
        || !isfinite(prior->volatility) || prior->volatility <= 0) { errno = EINVAL; return false; }
    lp_standing result = { .rating = prior->rating, .deviation = prior->deviation, .volatility = prior->volatility };
    for (size_t i = 0; i < n; i++) {
        const lp_series_entry *e = entries + i;
        if (e->games > UINT64_C(9007199254740992) || !isfinite(e->wins) || e->wins < 0 || e->wins > (double)e->games
            || !isfinite(e->trust) || e->trust < 0 || e->trust > 1
            || !isfinite(e->witness_deviation) || e->witness_deviation < 0
            || !isfinite(e->opponent.rating) || !isfinite(e->opponent.deviation) || e->opponent.deviation < 0) { errno = EINVAL; return false; }
        if (e->games > UINT64_MAX - result.matches) { errno = EOVERFLOW; return false; }
        result.matches += e->games;
    }
    if (!n) { *out = result; return true; }
    lp_series_entry *sorted = lp_alloc(n * sizeof *sorted); memcpy(sorted, entries, n * sizeof *sorted);
    qsort(sorted, n, sizeof *sorted, series_order);
    SeriesRoot *roots = lp_zalloc(n * sizeof *roots); size_t nr = 0; bool ok = false; int failure = ERANGE;
    for (size_t i = 0; i < n;) {
        lp_series_entry folded = sorted[i++];
        while (i < n && lp_id_eq(&folded.dependence_root, &sorted[i].dependence_root)) {
            const lp_series_entry *e = sorted + i++;
            if (e->opponent.rating != folded.opponent.rating || e->games > UINT64_C(9007199254740992) - folded.games) { failure = EINVAL; goto done; }
            folded.games += e->games; folded.wins += e->wins;
            folded.trust = fmax(folded.trust, e->trust);
            folded.witness_deviation = fmax(folded.witness_deviation, e->witness_deviation);
            folded.opponent.deviation = fmax(folded.opponent.deviation, e->opponent.deviation);
        }
        if (!folded.games || folded.trust == 0) continue;
        double precision = lp_witness_precision(folded.trust, fmax(LP_ATTEST_FLOOR, folded.witness_deviation));
        double weight = g(folded.opponent.deviation / LP_GLICKO_SCALE);
        if (precision == 0 || weight == 0) continue;
        roots[nr++] = (SeriesRoot){ .games = (double)folded.games,
            .score = 0.5 + folded.trust * (folded.wins / (double)folded.games - 0.5),
            .anchor = (folded.opponent.rating - LP_GLICKO_RATING) / LP_GLICKO_SCALE,
            .weight = weight, .precision = precision,
            .performance = (prior->rating - LP_GLICKO_RATING) / LP_GLICKO_SCALE };
    }
    result.witnesses = (uint64_t)nr;
    if (!nr) { ok = true; goto done; }
    double mu0 = (prior->rating - LP_GLICKO_RATING) / LP_GLICKO_SCALE;
    double phi = prior->deviation / LP_GLICKO_SCALE, phi2 = phi * phi, pp = 1 / phi2;
    double lo = mu0 - 1, hi = mu0 + 1, slope, info, score;
    unsigned k;
    for (k = 0; k < 64; k++) {
        if (!posterior(roots, nr, lo, mu0, pp, &slope, &info, &score)) goto done;
        if (slope >= 0) break;
        lo = mu0 - 2 * (mu0 - lo);
    }
    if (k == 64) goto done;
    for (k = 0; k < 64; k++) {
        if (!posterior(roots, nr, hi, mu0, pp, &slope, &info, &score)) goto done;
        if (slope <= 0) break;
        hi = mu0 + 2 * (hi - mu0);
    }
    if (k == 64) goto done;
    double mu = mu0;
    for (k = 0; k < 64; k++) {
        if (!posterior(roots, nr, mu, mu0, pp, &slope, &info, &score)) goto done;
        result.iterations = k + 1;
        if (slope == 0) break;
        if (slope > 0) lo = mu; else hi = mu;
        double next = mu + slope / (pp + info);
        if (next == mu) break;
        if (!(next > lo && next < hi)) next = lo + (hi - lo) * 0.5;
        if (hi - lo <= 1e-12 || next == mu || next == lo || next == hi) { mu = next; break; }
        mu = next;
    }
    if (k == 64 || !posterior(roots, nr, mu, mu0, pp, &slope, &info, &score) || info <= 0) goto done;
    double sigma;
    if (!series_volatility(prior->volatility, phi2, 1 / info, score / info, &sigma)) goto done;
    result.rating = LP_GLICKO_RATING + LP_GLICKO_SCALE * mu;
    result.deviation = LP_GLICKO_SCALE / sqrt(1 / (phi2 + sigma * sigma) + info);
    result.volatility = sigma;
    ok = isfinite(result.rating) && isfinite(result.deviation) && result.deviation > 0;
done:
    lp_free(roots); lp_free(sorted);
    if (ok) *out = result; else errno = failure;
    return ok;
}

bool lp_series(const lp_series_entry *entries, size_t n, lp_standing *out){
    lp_rating prior = lp_rating_stock(); return lp_series_update(&prior, entries, n, out);
}
