#ifndef LAPLACE_PERFCACHE_H
#define LAPLACE_PERFCACHE_H
#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- a perf-cache module (Sequence 7, Perfcaches)
 * One container for every perf-cache module above tier 0 that is not the highway: a deterministic, fingerprinted,
 * memory-mapped, read-only file of records in the tier-0 record form (id, the real coordinate in fixed point, its
 * Hilbert value, rank = the record's slot in its section, pad = its tier), grouped into named sections, and of pair
 * tables. The module (chess, ...) supplies only its varying law: what its records are, its section names, and the
 * address law of its dense sections; Native owns the format, the writer, the strict loader and the lookups, so a
 * generator, the engine's ingest and the PostgreSQL module read one implementation.
 *
 * Sections:
 *   LP_PERFCACHE_DENSE   a complete enumerated domain: records[first + slot], slot the module's address law, rank ==
 *                        slot. True O(1), no hash.
 *   LP_PERFCACHE_SPARSE  an admitted (selected, hot) estate: records sorted by ID, strictly ascending, rank == slot.
 *   LP_PERFCACHE_PAIRS   a deterministic relation (a, b) -> c: pairs sorted by the 32 bytes of (a, b), strictly
 *                        ascending, so a key has one value.
 * Lookup law:
 *   lp_perfcache_at      O(1): pointer arithmetic.
 *   lp_perfcache_find    O(1) expected, O(n) worst: an open-addressed, linearly probed index over every record ID,
 *                        built once at map time; a probe compares all 16 bytes, so a slot collision is a further probe,
 *                        never a wrong record. The same ID in two sections (a square that is also an en passant field)
 *                        must be the same record there, byte for byte: the loader refuses a file where it is not.
 *   lp_perfcache_pair    O(log n): binary search on the full 32-byte key; a miss is NULL, never a neighbour.
 * A miss is no statement that the content does not exist: the caller composes it canonically.
 *
 * File, little-endian:
 *   header (128)  "LPPC0001", module name[16] NUL-padded, u32 sections, u32 0, u64 records, u64 pairs, u64 file bytes,
 *                 u64 0, depends[32] (the tier-0 fingerprint the records were composed from), digest[32]
 *   sections      lp_section[sections]: records' sections first covering [0, records) in order, then pairs'
 *                 sections covering [0, pairs) in order
 *   records       lp_tier0_record[records]
 *   pairs         lp_pair[pairs]
 * digest = BLAKE3-256 of the header's first 96 bytes and everything after the header: the module's fingerprint. A
 * missing, truncated, corrupt, unsorted, unknown-version or inconsistent file is refused whole; nothing is partly
 * mapped. A file whose depends is not the tier 0 it is read with is stale (lp_perfcache_bound). */
#define LP_PERFCACHE_DENSE  1u
#define LP_PERFCACHE_SPARSE 2u
#define LP_PERFCACHE_PAIRS  3u
typedef struct { char name[32]; uint32_t kind, first, count, reserved; } lp_section;      /* 48 bytes */
typedef struct { lp_id a, b, c; } lp_pair;                                                 /* 48 bytes: (a, b) -> c */
typedef struct {
    char module[16]; uint8_t depends[32];
    const lp_section *section; size_t nsections;
    const lp_tier0_record *rec; size_t nrec;
    const lp_pair *pair; size_t npairs;
    lp_idindex *by_id; char path[4096];
} lp_perfcache;

/* Where a module goes with tier 0: $env when it is set, else tier 0's path with ending in place of its own. */
LP_API const char *lp_perfcache_beside_tier0(const char *env, const char *ending, char *out, size_t cap);
/* Write a module. Sections give each section's name, kind and count (first is computed: records' sections in the order
 * given, then pairs'); rec and pair hold their records and pairs in that order. Every invariant the loader checks is
 * checked first; the file is written to a temporary beside path, flushed, and renamed over path. fingerprint (may be
 * NULL) receives the digest. False with errno EINVAL when an invariant does not hold. */
LP_API bool lp_perfcache_write(const char *path, const char *module, const uint8_t depends[32],
                               const lp_section *sections, size_t nsections,
                               const lp_tier0_record *rec, size_t nrec, const lp_pair *pair, size_t npairs,
                               uint8_t fingerprint[32]);
/* Memory-map a module, read-only, validated whole, once per path for the life of the process. NULL when it is
 * missing or any check fails. */
LP_API const lp_perfcache *lp_perfcache_map(const char *path);
/* The fingerprint (digest) of a mapped module. */
LP_API void lp_perfcache_fingerprint(const lp_perfcache *, uint8_t out[32]);
/* Whether the module was composed from this tier 0 (its fingerprint is the module's depends). */
LP_API bool lp_perfcache_bound(const lp_perfcache *, const lp_tier0_record *t0);
LP_API const lp_section *lp_perfcache_section(const lp_perfcache *, const char *name);
LP_API const lp_tier0_record *lp_perfcache_at(const lp_perfcache *, const lp_section *, uint32_t slot);
/* The record with this ID, and (section may be NULL) the first section that holds it. */
LP_API const lp_tier0_record *lp_perfcache_find(const lp_perfcache *, const lp_id *id, const lp_section **section);
LP_API const lp_pair *lp_perfcache_pair(const lp_perfcache *, const lp_section *, const lp_id *a, const lp_id *b);

#ifdef __cplusplus
}
#endif
#endif
