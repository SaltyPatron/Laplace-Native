/* Tier 0: the perf-cache of all 1,114,112 codepoints, memory-mapped read-only; the codepoint of an ID; and the
 * fingerprint that says which tier 0 an install has. */
#include "laplace/laplace.h"
#include "blake3.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(lp_tier0_record) == 64, "tier-0 records are 64 bytes");

#ifndef LP_TIER0_DEFAULT
#define LP_TIER0_DEFAULT "/repos/work/tier0/tier0.bin"
#endif

const char *lp_tier0_path(void){
    const char *p = getenv("LAPLACE_TIER0");
    return p && *p ? p : LP_TIER0_DEFAULT;
}

const lp_tier0_record *lp_tier0_map(const char *path){
    if (!path || !*path) path = lp_tier0_path();
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != (size_t)LP_NCP * sizeof(lp_tier0_record)) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd);
    return p == MAP_FAILED ? NULL : (const lp_tier0_record *)p;
}

/* IDs to codepoints: open addressing over the IDs' own bits (they are hashes), holding codepoint + 1. An ID depends
 * only on its codepoint, never on the table's coordinates, so one table serves the process. */
#define SLOTS (1u << 22)
static uint32_t *slot; static const lp_tier0_record *slot_t0; static pthread_mutex_t slot_mu = PTHREAD_MUTEX_INITIALIZER;
static void slots_build(const lp_tier0_record *t0){
    uint32_t *s = calloc(SLOTS, sizeof *s); if (!s) return;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        uint64_t h; memcpy(&h, t0[cp].id.b, 8); uint32_t k = (uint32_t)(h & (SLOTS - 1));
        while (s[k]) k = (k + 1) & (SLOTS - 1);
        s[k] = cp + 1;
    }
    slot_t0 = t0; slot = s;
}

int64_t lp_tier0_codepoint(const lp_tier0_record *t0, const lp_id *id){
    if (!__atomic_load_n(&slot, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&slot_mu); if (!slot) slots_build(t0); pthread_mutex_unlock(&slot_mu);
        if (!slot) return -1;
    }
    uint64_t h; memcpy(&h, id->b, 8); uint32_t k = (uint32_t)(h & (SLOTS - 1));
    while (slot[k]) { if (!memcmp(slot_t0[slot[k] - 1].id.b, id->b, 16)) return (int64_t)slot[k] - 1; k = (k + 1) & (SLOTS - 1); }
    return -1;
}

void lp_tier0_fingerprint(const lp_tier0_record *t0, uint8_t out[32]){
    blake3_hasher h; blake3_hasher_init(&h);
    blake3_hasher_update(&h, t0, (size_t)LP_NCP * sizeof(lp_tier0_record));
    blake3_hasher_finalize(&h, out, 32);
}
