#ifndef LAPLACE_HIGHWAY_H
#define LAPLACE_HIGHWAY_H
#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t lp_mask;
#define LP_HIGHWAY_RETIRED UINT32_MAX

/* Versioned, checksummed, single-file cache. All descriptors, textual lookup
 * keys, identifier bytes and crosswalk edges travel with the mapped records.
 * layout/keys are tab-separated descriptor rows, parsed only at map time.
 * A key is list, source key, local slot. A cross-list reference is source
 * namespace, source key, target list, target slot; it names the existing target
 * record without adding slots or changing content identity. A namespace/key
 * may name multiple targets; all are retained. Such rows select envelope
 * version 3; version 2 files remain readable. Earlier version 3 readers reject
 * multiple distinct targets rather than selecting one.
 * identifiers contains nrec entries: a little-endian uint32 part count followed
 * by that many nonempty NUL-terminated UTF-8 text components. One component is
 * an identifier string; multiple components compose an ordered identifier
 * tuple (a FrameNet frame and FE/LU name). Zero parts denotes a retired slot.
 * The caller publishes the completed file atomically. No side files are used. */
LP_API bool lp_highway_write(const char *path,
    const lp_tier0_record *records, size_t nrec, const lp_edge *edges, size_t nedges,
    const char *layout, size_t layout_bytes, const char *keys, size_t key_bytes,
    const char *identifiers, size_t identifier_bytes);
/* Display text, owned by the mapping; NULL outside the list. A tuple's display
 * is not its decomposition: use its ordered components below for identity. */
LP_API const char *lp_highway_identifier(const lp_highway *, const lp_list *, uint32_t slot);
/* Ordered identifier components, owned by the mapping. Decompose each with
 * Tier-0, then compose the resulting refs when the count exceeds one. */
LP_API size_t lp_highway_identifier_parts(const lp_highway *, const lp_list *, uint32_t slot, const char *const **parts);

/* Membership in any list, through the cache's map-time ID index. No lookup
 * allocation, name scan, coordinate computation or database call. */
LP_API bool lp_highway_contains(const lp_highway *highway, const lp_id *id);
/* Canonical coordinates and tier for any active identifier or cached closure
 * entity. The immutable mapping owns the record; NULL means absent. */
LP_API const lp_tier0_record *lp_highway_find(const lp_highway *highway, const lp_id *id);
/* Resolve a source's key, including release-specific pointer namespaces. The
 * mapping owns the existing target record; no composition or allocation is
 * performed. A namespace need not be a mask list. NULL means absent or
 * ambiguous: a singular lookup never selects one of several targets. */
LP_API const lp_tier0_record *lp_highway_resolve(const lp_highway *, const char *source_namespace, const char *key);
/* Enumerate every distinct canonical target without lookup allocation. Start
 * cursor at zero and keep it for this namespace/key until NULL is returned.
 * The mapping owns the records. No target is ranked or merged with another. */
LP_API const lp_tier0_record *lp_highway_resolve_next(const lp_highway *, const char *source_namespace, const char *key, uint32_t *cursor);

/* Field bits in the one 64-bit carrier; zero for an invalid descriptor. */
LP_API lp_mask lp_mask_field(const lp_bank *bank);
/* Set an enum's slot, or add a row-role flag. Invalid slots leave the mask unchanged. */
LP_API bool lp_mask_put(lp_mask *mask, const lp_bank *bank, uint32_t slot);
LP_API bool lp_mask_has(lp_mask mask, const lp_bank *bank, uint32_t slot);
/* The enum slot, or the only set flag. -1 means absent/invalid, -2 means multiple flags. */
LP_API int64_t lp_mask_get(lp_mask mask, const lp_bank *bank);
LP_API bool lp_mask_clear(lp_mask *mask, const lp_bank *bank);
/* Merge two carriers, rejecting conflicting enum values and unknown/reserved bits.
 * Flags are unioned. Failure leaves out unchanged. */
LP_API bool lp_mask_merge(const lp_highway *highway, lp_mask a, lp_mask b, lp_mask *out);
/* Add the frozen slot identified by id, without allocating or minting an entity.
 * A different enum value already present is a conflict and leaves mask unchanged. */
LP_API bool lp_mask_add_id(const lp_highway *highway, lp_mask *mask, const lp_id *id);

#ifdef __cplusplus
}
#endif
#endif
