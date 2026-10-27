#include "check.h"

typedef struct { uint64_t counts[9][4][4], calls; bool stop; } Pairs;
static bool collect(void *context, const lp_id *a, const lp_id *b,
                    uint64_t gap, uint64_t count){
    Pairs *p = context; p->calls++;
    if (p->stop) return false;
    p->counts[gap][a->b[0]][b->b[0]] += count;
    return true;
}
static void oracle(void){
    /* Independently expand every small sequence, including adjacent equal runs,
     * empty sequences, offsets beyond the end and one-offset selections. */
    for (unsigned sample = 0; sample < 512; sample++) {
        lp_id_run runs[7]; unsigned expanded[28]; size_t n = sample % 8, count = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned id = (sample >> (i % 5)) % 4, run = 1 + ((sample + i * 3) % 4);
            runs[i] = (lp_id_run){.id = {{(uint8_t)id}}, .count = run};
            for (unsigned j = 0; j < run; j++) expanded[count++] = id;
        }
        for (uint64_t lo = 1; lo <= 8; lo++) {
            uint64_t hi = sample & 1 ? lo : 8;
            Pairs actual = {0}, expected = {0};
            for (uint64_t g = lo; g <= hi; g++)
                for (size_t i = 0; i + g < count; i++)
                    expected.counts[g][expanded[i]][expanded[i + g]]++;
            CHECK(lp_id_run_pairs(runs, n, lo, hi, collect, &actual), "valid sequence");
            CHECK(!memcmp(actual.counts, expected.counts, sizeof actual.counts),
                  "expanded oracle sample %u offsets %llu..%llu", sample,
                  (unsigned long long)lo, (unsigned long long)hi);
        }
    }
}
int main(void){
    oracle();
    Pairs p = {0}; lp_id_run run = {.id = {{1}}, .count = UINT64_MAX};
    CHECK(lp_id_run_pairs(&run, 1, 1, 8, collect, &p) && p.calls == 8,
          "maximum run is processed in eight callbacks");
    for (unsigned g = 1; g <= 8; g++)
        CHECK(p.counts[g][1][1] == UINT64_MAX - g, "exact 64-bit run multiplicity");
    p = (Pairs){0};
    lp_id_run overflow[2] = {run, {.count = 1}};
    CHECK(!lp_id_run_pairs(overflow, 2, 1, 8, collect, &p) && !p.calls,
          "overflow rejected before callbacks");
    run.count = 0;
    CHECK(!lp_id_run_pairs(&run, 1, 1, 8, collect, &p) && !p.calls, "zero run rejected");
    CHECK(!lp_id_run_pairs(NULL, 1, 1, 8, collect, &p), "missing run array rejected");
    CHECK(!lp_id_run_pairs(NULL, 0, 0, 8, collect, &p), "zero offset rejected");
    CHECK(!lp_id_run_pairs(NULL, 0, 8, 1, collect, &p), "reversed bounds rejected");
    CHECK(!lp_id_run_pairs(NULL, 0, 1, 8, NULL, &p), "missing visitor rejected");
    p.stop = true; run.count = 2;
    CHECK(!lp_id_run_pairs(&run, 1, 1, 8, collect, &p) && p.calls == 1,
          "visitor can stop processing");
    DONE("sequence");
}
