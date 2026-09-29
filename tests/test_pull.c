/* The search frontier, and the cost of a claim from its standing (Research: Relations, measured rows). */
#include "laplace/laplace.h"
#include "check.h"
#include <math.h>
#include <string.h>

int main(void){
    lp_rating well = { 1774.3, 93.3, 0.06 }, copies = { 1675.1, 247.2, 0.06 };
    CHECK(fabs(lp_confidence(&well, 2) - 0.624) < 0.001, "12 witnesses, 1 contradiction: %g", lp_confidence(&well, 2));
    CHECK(fabs(lp_cost(&well, 2, 0) - 0.472) < 0.001, "its cost: %g", lp_cost(&well, 2, 0));
    CHECK(fabs(lp_cost(&copies, 2, 0) - 1.986) < 0.002, "copies counted once: %g", lp_cost(&copies, 2, 0));
    CHECK(fabs(lp_confidence(&well, 0) - 0.829) < 0.011, "the rating alone: %g", lp_confidence(&well, 0));

    lp_frontier *f = lp_frontier_new(); lp_id id[5000];
    for (uint32_t i = 0; i < 5000; i++) lp_id_codepoint(i + 0x4E00, &id[i]);
    for (uint32_t i = 0; i < 5000; i++) CHECK(lp_frontier_reach(f, &id[i], NULL, NULL, (double)((i * 7919u) % 5000u) + 10, 0, 1), "reach %u", i);
    CHECK(!lp_frontier_reach(f, &id[7], NULL, NULL, 1e9, 0, 2), "a dearer chain is not kept");
    CHECK(lp_frontier_reach(f, &id[7], &id[1], &id[2], 1.5, 0, 2), "a cheaper chain is");
    const lp_reached *r = lp_frontier_next(f);
    CHECK(r && !memcmp(&r->id, &id[7], 16) && r->cost == 1.5 && r->hops == 2 && !memcmp(&r->from, &id[1], 16), "the cheapest comes first");
    CHECK(!lp_frontier_reach(f, &id[7], NULL, NULL, 0.5, 0, 1), "a closed entity stays closed");
    double last = 0; size_t n = 1;
    while ((r = lp_frontier_next(f))) { CHECK(r->cost >= last, "order: %g after %g", r->cost, last); last = r->cost; n++; }
    CHECK(n == 5000 && lp_frontier_count(f) == 5000, "every entity once (%zu)", n);
    lp_frontier_free(f);
    DONE("pull");
}
