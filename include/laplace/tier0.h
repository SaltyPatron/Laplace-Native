#ifndef LAPLACE_TIER0_H
#define LAPLACE_TIER0_H

#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The file holds exactly the Unicode scalar values, in ascending memcmp(id) order.
 * LP_NCP remains the size of the Unicode codespace for UCD property tables. */
#define LP_TIER0_COUNT 1112064u

static inline bool lp_unicode_scalar(uint32_t cp){
    return cp < LP_NCP && (cp < 0xd800u || cp > 0xdfffu);
}

/* O(1) lookups over a validated mapping. No surrogate, missing ID or unknown mapping
 * is an atom. The returned records belong to the read-only mapped file. */
LP_API const lp_tier0_record *lp_tier0_at(const lp_tier0_record *table, uint32_t cp);
LP_API const lp_tier0_record *lp_tier0_find(const lp_tier0_record *table, const lp_id *id);

/* Build the complete cache from the UCD/DUCET scalar order. The caller provides
 * LP_TIER0_COUNT output records. Native owns projection, quantization, identities
 * and ID ordering; container readers supply only the Unicode collation order. */
LP_API bool lp_tier0_build(const uint32_t *order, size_t count, lp_tier0_record *out);
/* Read the Unicode release's ucd/ and uca/ files, derive its total scalar order,
 * and construct the cache through the same builder. No process-global parser state. */
LP_API bool lp_tier0_from_ucd(const char *root, lp_tier0_record *out);
/* Publish the complete artifact by replacing its destination atomically. An I/O
 * failure leaves an existing cache intact. */
LP_API bool lp_tier0_write(const char *path, const lp_tier0_record *records);

#ifdef __cplusplus
}
#endif
#endif
