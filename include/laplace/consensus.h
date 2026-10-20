#ifndef LAPLACE_CONSENSUS_H
#define LAPLACE_CONSENSUS_H

#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* The source precision bound, in inverse squared Glicko-2 units. Derived from
 * g(phi)=t, with a positive absolute RD floor. Zero trust gives zero information;
 * reliability never enters only as an opponent-deviation change. */
LP_API double lp_witness_precision(double trust, double deviation_floor);

#ifdef __cplusplus
}
#endif
#endif
