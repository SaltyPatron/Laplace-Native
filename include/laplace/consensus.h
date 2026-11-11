#ifndef LAPLACE_CONSENSUS_H
#define LAPLACE_CONSENSUS_H

#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A record vertex immediately before a claim names its declared shared
 * dependence class. Its entity is the canonical text of a registry label;
 * it changes the observation's root and trust, never the corpus witness.
 * Role 6 remains reserved; append this role in the existing three-bit field. */
#define LP_SAID_DEPENDENCE 7u
LP_API const lp_trust_class *lp_trust_class_for_id(lp_text *text, const lp_id *id);

typedef struct {
    lp_id dependence_root;
    uint64_t games;
    double wins;                    /* sum of outcomes; draws contribute 0.5 */
    double trust;                   /* reliability, in [0,1], not a rating */
    double witness_deviation;       /* source uncertainty in rating points; 0 asks for its trust floor */
    lp_rating opponent;             /* the comparison anchor; separate from the source's reliability */
} lp_series_entry;

typedef struct {
    double rating, deviation, volatility;
    uint64_t matches, witnesses;     /* reported games and independent informative dependence roots */
    unsigned iterations;
} lp_standing;

/* An attestation plays against the fixed, known 1500 anchor by default. Its
 * witness still has nonzero uncertainty, independently of that anchor. */
LP_API lp_series_entry lp_series_observation(lp_id root, uint64_t games, double wins, double trust);

/* An attestation row as a series entry: the one law for every reader of attestation. games in [1, 2^53] and whole;
 * wins (the sum of outcomes) in [0, games]; signed trust in [-1, 1], where a negative trust is a witness that
 * reliably says the opposite: its outcomes are reversed (wins = games - wins) and it weighs |trust|. false: the row is
 * not a valid observation and *out is untouched. */
/* Testimony under a trunk, tallied: the one law for every walk that counts what a trunk's records say. An edge to a
 * claim, under a parent that stands count times, is said games = count (count x times where the vertex is a record, a
 * count the source states), tokens = count x times, and wins = games x the outcome's score. A tally adds them and keeps
 * the least positive position. */
typedef struct { double games, tokens, wins; uint32_t position; } lp_tally;
static inline void lp_tally_add(lp_tally *t, double games, double tokens, double wins, uint32_t position){
    t->games += games; t->tokens += tokens; t->wins += wins;
    if (position && (!t->position || position < t->position)) t->position = position;
}
static inline void lp_tally_edge(lp_tally *t, double count, uint64_t times, bool record, uint32_t outcome, uint32_t spare,
                                 uint32_t position){
    double games = record ? count * (double)times : count;
    lp_tally_add(t, games, count * (double)times, games * lp_outcome_score(outcome, spare), position);
}
LP_API bool lp_series_attested(lp_id root, double games, double wins, double trust, lp_series_entry *out);

/* Trust bounds each root's mean vote: s_eff = 0.5 + t*(wins/games - 0.5).
 * The root has ONE shared uncertain performance x_r, not games independent
 * copies of the witness's reliability. In Glicko-2 units the joint posterior is
 *
 * -(mu-mu0)^2/(2*phi0^2) - sum (x_r-mu)^2/(2*omega_r^2)
 * + sum n_r [s_eff*log(E_r) + (1-s_eff)*log(1-E_r)],
 * E_r = logistic(g(phi_opponent)*(x_r-mu_opponent)).
 *
 * This is strictly concave. Profiling x_r gives slope (x_r-mu)/omega_r^2
 * and information J_r/(1+omega_r^2*J_r), where J_r=n_r*g^2*E_r*(1-E_r).
 * Thus source information saturates at 1/omega_r^2; independent roots add
 * information. No post-hoc rating clamp, repeated-game loop or input-order
 * dependent sequence of updates is used. Illinois updates volatility from the
 * converged profile curvature and score, followed by Glicko-2 RD propagation.
 *
 * A finite source uncertainty changes the single-source asymptote: it is the
 * prior/source precision-weighted performance, not the unregularized logit
 * target. Independent corroboration can exceed that single-source asymptote.
 * For identical roots with 0 < s_eff < 1, let a = mu_opponent +
 * logit(s_eff)/g(phi_opponent), p0 = 1/phi0^2, and p = 1/omega^2.
 * As games -> infinity, k independent roots give
 *   mu_limit(k) = (p0*mu0 + k*p*a)/(p0 + k*p).
 * Copies folded into one root retain k=1. The source-information limit is
 * k*p; a flat repeated-game likelihood would instead give unbounded source
 * precision and the same target a for every k, violating that distinction.
 * This shared-source model is Laplace's adaptation; lp_glicko2 separately
 * implements Glickman's conventional rating-period update and worked example.
 *
 * Fold identical roots by summing games/wins, maximum trust, and conservative
 * maximum source/opponent uncertainty. Opponent ratings in one root must agree.
 * Canonical sorting makes every input permutation bit-identical. Invalid data,
 * overflow or numerical failure returns false without modifying out.
 * lp_series recomputes from the stock prior; lp_series_update uses an explicit
 * prior for a genuinely new rating period. Replaying a whole witness set uses
 * lp_series, never a previously updated standing as its prior. */
LP_API bool lp_series(const lp_series_entry *entries, size_t n, lp_standing *out);
LP_API bool lp_series_update(const lp_rating *prior, const lp_series_entry *entries, size_t n, lp_standing *out);

typedef struct {
    lp_id dependence_root;
    uint64_t games;
    double information;            /* profile curvature in inverse squared Glicko-2 units */
} lp_series_information;

/* The same posterior, with each informative root's actual contribution to its
 * precision. Roots are returned in canonical ID order. Shared-root observations
 * are folded before measuring information, so copying a source cannot create
 * independent precision. A capacity of n always suffices. Insufficient capacity
 * reports the required count through written, sets ENOSPC, and leaves out and
 * information untouched; other failures leave every output untouched. */
LP_API bool lp_series_evaluate(const lp_rating *prior, const lp_series_entry *entries, size_t n,
                              lp_standing *out, lp_series_information *information,
                              size_t capacity, size_t *written);

/* The source precision bound, in inverse squared Glicko-2 units. Derived from
 * g(phi)=t, with a positive absolute RD floor. Zero trust gives zero information;
 * reliability never enters only as an opponent-deviation change. */
LP_API double lp_witness_precision(double trust, double deviation_floor);

#ifdef __cplusplus
}
#endif
#endif
