#ifndef LAPLACE_TIER0_H
#define LAPLACE_TIER0_H

#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Projection uses the entire fixed Unicode codespace. Selecting text characters
 * must never change the Super-Fibonacci population or global DUCET ranks. */
#define LP_TIER0_COUNT LP_NCP

static inline bool lp_unicode_scalar(uint32_t cp){
    return cp < LP_NCP && (cp < 0xd800u || cp > 0xdfffu);
}

/* O(1) lookups over a validated mapping. Missing IDs, codepoints outside the
 * codespace and unknown mappings are rejected. Text validity is a separate rule. */
LP_API const lp_tier0_record *lp_tier0_at(const lp_tier0_record *table, uint32_t cp);
LP_API const lp_tier0_record *lp_tier0_find(const lp_tier0_record *table, const lp_id *id);
LP_API bool lp_tier0_contains(const lp_tier0_record *table, const lp_id *id);

/* Build the complete cache from the UCD/DUCET codespace order. The caller provides
 * LP_TIER0_COUNT output records. Native owns projection, quantization, identities
 * and ID ordering; container readers supply only the Unicode collation order. */
LP_API bool lp_tier0_build(const uint32_t *order, size_t count, lp_tier0_record *out);
/* Read the Unicode release's ucd/ and uca/ files, derive its total codespace order,
 * and construct the cache through the same builder. No process-global parser state. */
LP_API bool lp_tier0_from_ucd(const char *root, lp_tier0_record *out);
/* Publish the complete artifact by replacing its destination atomically. An I/O
 * failure leaves an existing cache intact. */
LP_API bool lp_tier0_write(const char *path, const lp_tier0_record *records);

#ifdef __cplusplus
}
#endif
#endif
