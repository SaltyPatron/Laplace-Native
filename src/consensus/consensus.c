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

bool lp_series_attested(lp_id root, double games, double wins, double trust, lp_series_entry *out){
    if (!isfinite(games) || games < 1 || games > 9007199254740992.0 || floor(games) != games) return false;
    if (!isfinite(wins) || wins < 0 || wins > games || !isfinite(trust) || fabs(trust) > 1) return false;
    *out = lp_series_observation(root, (uint64_t)games, trust < 0 ? games - wins : wins, fabs(trust));
    return true;
}

typedef struct { double games, score, anchor, weight, precision, performance, information; lp_id id; } SeriesRoot;

static void series_likelihood(const SeriesRoot *r, double x, double *score, double *information){
    double z = r->weight * (x - r->anchor), tail = exp(-fabs(z)), denominator = 1 + tail;
    /* Compute the small tail directly: 1-logistic(z) loses significant bits
     * for decisive long series and breaks affirmation/refutation symmetry. */
    double residual = z >= 0 ? (r->score - 1) + tail / denominator : r->score - tail / denominator;
    *score = r->games * r->weight * residual;
    *information = r->games * r->weight * r->weight * tail / (denominator * denominator);
}
static int compare_number(double a, double b){ return a < b ? -1 : a > b; }
static int root_order(const void *a, const void *b){
    const SeriesRoot *x=a,*y=b;int c;
    if((c=compare_number(x->games,y->games)))return c;
    if((c=compare_number(x->score,y->score)))return c;
    if((c=compare_number(x->anchor,y->anchor)))return c;
    if((c=compare_number(x->weight,y->weight)))return c;
    return compare_number(x->precision,y->precision);
}
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
 * strictly decreasing and |f''(x)| <= w * j(x) <= |f'(x)| everywhere (w <= 1),
 * so an interior Newton step |step| <= 1e-14*(1+|x|) guarantees |x - x*| <=
 * ~1e-14*(1+|x|) even when one-sided monotone convergence leaves the opposite
 * bracket end unmoved across sub-ULP slope quantization plateaus. */
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
    for (unsigned i = 0; !converged && i < 128; i++) {
        double data_score, j; series_likelihood(r, x, &data_score, &j);
        double slope = p * (mu - x) + data_score;
        if (slope > 0) lo = x; else if (slope < 0) hi = x; else { converged = true; break; }
        double step = slope / (p + j);
        double next = x + step;
        if (next == x) { converged = true; break; }
        bool in_bracket = (next > lo && next < hi);
        if (in_bracket && fabs(step) <= 1e-14 * (1.0 + fabs(x))) { x = next; converged = true; break; }
        if (!in_bracket || (i >= 16 && (i & 3u) == 3u)) next = lo + (hi - lo) * 0.5;
        double tol = 1e-13 * (1.0 + fabs(lo) + fabs(hi));
        if (hi - lo <= tol || next == x || next == lo || next == hi) { x = next; converged = true; break; }
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
        roots[i].information = j;
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
        for (k = 1; k <= 128; k++) if (volatility_slope(a - k * tau, a, phi2, v, delta, tau) >= 0) break;
        if (k > 128) return false;
        B = a - k * tau;
    }
    double fA = volatility_slope(A, a, phi2, v, delta, tau), fB = volatility_slope(B, a, phi2, v, delta, tau);
    if (!isfinite(fA) || !isfinite(fB)) return false;
    for (unsigned i = 0; i < 256; i++) {
        double tol = 1e-12 * (1.0 + fabs(A) + fabs(B));
        if (fabs(B - A) <= tol) { *out = exp(A * 0.5); return isfinite(*out) && *out > 0; }
        double C = A + (A - B) * fA / (fB - fA);
        if (!isfinite(C) || C <= fmin(A, B) || C >= fmax(A, B) || (i >= 16 && (i & 7u) == 7u))
            C = A + (B - A) * 0.5;
        if (C == A || C == B) { *out = exp(C * 0.5); return isfinite(*out) && *out > 0; }
        double fC = volatility_slope(C, a, phi2, v, delta, tau);
        if (!isfinite(fC)) return false;
        if (fC == 0) { *out = exp(C * 0.5); return isfinite(*out) && *out > 0; }
        if ((fC < 0) != (fB < 0)) { A = B; fA = fB; } else fA *= 0.5;
        B = C; fB = fC;
    }
    return false;
}

static int information_order(const void *a, const void *b){
    return lp_id_cmp(&((const lp_series_information *)a)->dependence_root,
                     &((const lp_series_information *)b)->dependence_root);
}
static bool series_evaluate(const lp_rating *prior, const lp_series_entry *entries, size_t n,
                           lp_standing *out, lp_series_information *information,
                           size_t capacity, size_t *written){
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
    if (!n) { *out = result; if(written)*written=0; return true; }
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
            .performance = (prior->rating - LP_GLICKO_RATING) / LP_GLICKO_SCALE, .id=folded.dependence_root };
    }
    /* IDs decide dependence membership, never numerical summation order.
     * Publishing the same partition under its final trunk must preserve bits. */
    qsort(roots,nr,sizeof *roots,root_order);
    result.witnesses = (uint64_t)nr;
    if (!nr) { ok = true; goto done; }
    double mu0 = (prior->rating - LP_GLICKO_RATING) / LP_GLICKO_SCALE;
    double phi = prior->deviation / LP_GLICKO_SCALE, phi2 = phi * phi, pp = 1 / phi2;
    double lo = mu0 - 1, hi = mu0 + 1, slope, info, score;
    unsigned k;
    for (k = 0; k < 128; k++) {
        if (!posterior(roots, nr, lo, mu0, pp, &slope, &info, &score)) goto done;
        if (slope >= 0) break;
        lo = mu0 - 2 * (mu0 - lo);
    }
    if (k == 128) goto done;
    for (k = 0; k < 128; k++) {
        if (!posterior(roots, nr, hi, mu0, pp, &slope, &info, &score)) goto done;
        if (slope <= 0) break;
        hi = mu0 + 2 * (hi - mu0);
    }
    if (k == 128) goto done;
    double mu = mu0;
    for (k = 0; k < 64; k++) {
        if (!posterior(roots, nr, mu, mu0, pp, &slope, &info, &score)) goto done;
        result.iterations = k + 1;
        if (slope == 0) break;
        if (slope > 0) lo = mu; else hi = mu;
        double step = slope / (pp + info);
        double next = mu + step;
        if (next == mu) break;
        bool in_bracket = (next > lo && next < hi);
        if (in_bracket && fabs(step) <= 1e-13 * (1.0 + fabs(mu))) { mu = next; break; }
        if (!in_bracket || (k >= 16 && (k & 3u) == 3u)) next = lo + (hi - lo) * 0.5;
        double tol = 1e-12 * (1.0 + fabs(lo) + fabs(hi));
        if (hi - lo <= tol || next == mu || next == lo || next == hi) { mu = next; break; }
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
    if(ok && written){
        if(capacity<nr){*written=nr;failure=ENOSPC;ok=false;}
        else{
            for(size_t i=0;i<nr;i++) information[i]=(lp_series_information){
                .dependence_root=roots[i].id,.games=(uint64_t)roots[i].games,.information=roots[i].information};
            if(nr>1)qsort(information,nr,sizeof *information,information_order);
            *written=nr;
        }
    }
    lp_free(roots); lp_free(sorted);
    if (ok) *out = result; else errno = failure;
    return ok;
}

bool lp_series_evaluate(const lp_rating *prior,const lp_series_entry *entries,size_t n,
                        lp_standing *out,lp_series_information *information,size_t capacity,size_t *written){
    if(!written || (capacity && !information)){errno=EINVAL;return false;}
    return series_evaluate(prior,entries,n,out,information,capacity,written);
}
bool lp_series_update(const lp_rating *prior,const lp_series_entry *entries,size_t n,lp_standing *out){
    return series_evaluate(prior,entries,n,out,NULL,0,NULL);
}

bool lp_series(const lp_series_entry *entries, size_t n, lp_standing *out){
    lp_rating prior = lp_rating_stock(); return lp_series_update(&prior, entries, n, out);
}

bool lp_witness_allowed(const lp_authority_scope *scope, const lp_id *witness, double trust){
    if (!witness) return false;
    if (!scope || !scope->scoped) return true;
    if (trust < scope->min_trust) return false;
    for (size_t i = 0; i < scope->witness_count; i++) {
        if (lp_id_eq(&scope->witnesses[i], witness)) return true;
    }
    return false;
}

bool lp_attest_series_scoped(const lp_rating *prior, const lp_witness_series *entries, size_t n,
                             const lp_authority_scope *scope, lp_standing *out,
                             lp_series_information *information, size_t capacity, size_t *written){
    if (!out || (n && !entries)) { errno = EINVAL; return false; }
    lp_rating p = prior ? *prior : lp_rating_stock();
    if (!scope || !scope->scoped) {
        lp_series_entry *raw = n ? lp_alloc(n * sizeof *raw) : NULL;
        for (size_t i = 0; i < n; i++) raw[i] = entries[i].entry;
        bool ok;
        if (written || information || capacity) {
            size_t dummy = 0;
            ok = lp_series_evaluate(&p, raw, n, out, information, capacity, written ? written : &dummy);
        } else {
            ok = lp_series_update(&p, raw, n, out);
        }
        lp_free(raw);
        return ok;
    }
    lp_series_entry *kept = n ? lp_alloc(n * sizeof *kept) : NULL;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (lp_witness_allowed(scope, &entries[i].witness, entries[i].trust))
            kept[m++] = entries[i].entry;
    }
    bool ok;
    if (written || information || capacity) {
        size_t dummy = 0;
        ok = lp_series_evaluate(&p, kept, m, out, information, capacity, written ? written : &dummy);
    } else {
        ok = lp_series_update(&p, kept, m, out);
    }
    lp_free(kept);
    return ok;
}

bool lp_series_scoped(const lp_witness_series *entries, size_t n,
                      const lp_authority_scope *scope, lp_standing *out){
    lp_rating prior = lp_rating_stock();
    return lp_attest_series_scoped(&prior, entries, n, scope, out, NULL, 0, NULL);
}

typedef struct { size_t row; double rem; lp_id id; int has_id; } FisherRem;
static int fisher_rem_cmp(const void *a, const void *b){
    const FisherRem *x = (const FisherRem *)a, *y = (const FisherRem *)b;
    if (x->rem != y->rem) return x->rem > y->rem ? -1 : 1;
    if (x->has_id) return lp_id_cmp(&x->id, &y->id);
    return x->row < y->row ? -1 : (x->row > y->row ? 1 : 0);
}

bool lp_fisher_shares_bp(const double *weights, const lp_id *ids, size_t n, int32_t *shares_bp){
    if (!n) return true;
    if (!weights || !shares_bp) { errno = EINVAL; return false; }
    double total = 0.0;
    size_t positive = 0;
    for (size_t i = 0; i < n; i++) {
        shares_bp[i] = 0;
        if (!isfinite(weights[i]) || weights[i] < 0.0) { errno = EINVAL; return false; }
        total += weights[i];
        if (weights[i] > 0.0) positive++;
    }
    if (!positive) return true;
    if (!isfinite(total) || total <= 0.0) { errno = EINVAL; return false; }

    FisherRem *rem = lp_alloc(positive * sizeof *rem);
    size_t at = 0;
    int32_t allocated = 0;
    for (size_t i = 0; i < n; i++) {
        if (weights[i] <= 0.0) continue;
        double exact = 10000.0 * (weights[i] / total);
        double fl = floor(exact);
        int32_t pts = fl > 10000.0 ? 10000 : (int32_t)fl;
        shares_bp[i] = pts;
        allocated += pts;
        FisherRem r = { .row = i, .rem = exact - (double)pts, .has_id = ids != NULL };
        if (ids) r.id = ids[i];
        rem[at++] = r;
    }
    if (allocated > 10000 || (size_t)(10000 - allocated) > positive) {
        lp_free(rem);
        errno = ERANGE;
        return false;
    }
    lp_sort(rem, positive, sizeof *rem, fisher_rem_cmp);
    int32_t need = 10000 - allocated;
    for (int32_t i = 0; i < need; i++) shares_bp[rem[i].row]++;
    lp_free(rem);
    return true;
}
