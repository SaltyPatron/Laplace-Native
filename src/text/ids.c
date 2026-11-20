/* Text identities are the UAX #29 decomposition's trunk, never a flat hash of a string's codepoint sequence. */
#include "laplace/tier0.h"
#include <errno.h>
#include <limits.h>

bool lp_text_id(lp_text *context, const char *utf8, size_t len, lp_id *out){
    if (!context || !utf8 || !len || !out || len > INT32_MAX) { errno = EINVAL; return false; }
    const uint8_t *bytes = (const uint8_t *)utf8;
    for (size_t i = 0; i < len; ) {
        size_t start = i; uint32_t cp; uint8_t canonical[4];
        if (!lp_utf8_next(bytes, len, &i, &cp)) { errno = EILSEQ; return false; }
        size_t n = lp_utf8_put(cp, canonical);
        if (i - start != n || memcmp(bytes + start, canonical, n)) { errno = EILSEQ; return false; }
    }
    lp_ref trunk = lp_text_decompose(context, bytes, len, NULL, NULL);
    *out = trunk.id;
    return true;
}
