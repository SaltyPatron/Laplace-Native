/* Tier 0: the perf-cache of all 1,114,112 codepoints, memory-mapped read-only; the codepoint of an ID; and the
 * fingerprint that says which tier 0 an install has. */
#include "laplace/laplace.h"
#include "internal.h"
#include "blake3.h"
#include <pthread.h>
#include <stdlib.h>

_Static_assert(sizeof(lp_tier0_record) == 64, "tier-0 records are 64 bytes");

#ifndef LP_TIER0_DEFAULT
#define LP_TIER0_DEFAULT "/repos/work/tier0/tier0.bin"
#endif

const char *lp_tier0_path(void){
    const char *p = getenv("LAPLACE_TIER0");
    return p && *p ? p : LP_TIER0_DEFAULT;
}

static const void *tier0_open(const char *path){ return lp_map_file(path, (size_t)LP_NCP * sizeof(lp_tier0_record), NULL); }
const lp_tier0_record *lp_tier0_map(const char *path){ return lp_cached("tier0", path && *path ? path : lp_tier0_path(), tier0_open); }

/* IDs to codepoints: an index over the records' own IDs. An ID depends only on its codepoint, never on the table's
 * coordinates, so one index serves the process, built the first time it is asked. */
static lp_idindex *index_; static pthread_mutex_t index_mu = PTHREAD_MUTEX_INITIALIZER;
int64_t lp_tier0_codepoint(const lp_tier0_record *t0, const lp_id *id){
    lp_idindex *x = __atomic_load_n(&index_, __ATOMIC_ACQUIRE);
    if (!x) {
        pthread_mutex_lock(&index_mu);
        if (!(x = index_)) __atomic_store_n(&index_, x = lp_idindex_build(t0, LP_NCP, sizeof *t0), __ATOMIC_RELEASE);
        pthread_mutex_unlock(&index_mu);
        if (!x) return -1;
    }
    uint64_t probe = 0; return lp_idindex_find(x, id, &probe);
}

void lp_tier0_fingerprint(const lp_tier0_record *t0, uint8_t out[32]){
    blake3_hasher h; blake3_hasher_init(&h);
    blake3_hasher_update(&h, t0, (size_t)LP_NCP * sizeof(lp_tier0_record));
    blake3_hasher_finalize(&h, out, 32);
}
