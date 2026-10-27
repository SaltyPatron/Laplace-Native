/* Interval overlap over ordered runs; work depends on stored runs and requested
 * offsets, never on the expanded number of occurrences. */
#include "laplace/laplace.h"

bool lp_id_run_pairs(const lp_id_run *runs, size_t n, uint64_t lo,
                     uint64_t hi, lp_id_pair_visit visit, void *context){
    if ((!runs && n) || !visit || !lo || lo > hi) return false;
    uint64_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (!runs[i].count || runs[i].count > UINT64_MAX - count) return false;
        count += runs[i].count;
    }
    if (count <= lo) return true;
    if (hi >= count) hi = count - 1;
    for (uint64_t gap = lo;; gap++) {
        size_t a = 0, b = 0;
        uint64_t skip = gap;
        while (skip >= runs[b].count) skip -= runs[b++].count;
        uint64_t left = runs[a].count, right = runs[b].count - skip;
        for (;;) {
            uint64_t times = left < right ? left : right;
            if (!visit(context, &runs[a].id, &runs[b].id, gap, times)) return false;
            left -= times; right -= times;
            if (!right && ++b == n) break;
            if (!left) left = runs[++a].count;
            if (!right) right = runs[b].count;
        }
        if (gap == hi) break;
    }
    return true;
}
