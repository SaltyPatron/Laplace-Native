#ifndef LAPLACE_HIGHWAY_H
#define LAPLACE_HIGHWAY_H
#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t lp_mask;

/* Membership in any list, through the cache's map-time ID index. No lookup
 * allocation, name scan, coordinate computation or database call. */
LP_API bool lp_highway_contains(const lp_highway *highway, const lp_id *id);

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
