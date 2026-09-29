/* Identity: BLAKE3-128 over pure content. A codepoint hashes its UTF-8 bytes (surrogates in the generalized 3-byte
 * form); a composition hashes its children's IDs in order, repeats included; one child is that child. */
#include "laplace/laplace.h"
#include "blake3.h"
#include <string.h>

void lp_id_codepoint(uint32_t cp, lp_id *out){
    uint8_t u[4]; size_t n = lp_utf8_put(cp, u);
    blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, u, n); blake3_hasher_finalize(&h, out->b, 16);
}

void lp_id_compose(const lp_id *ch, size_t n, lp_id *out){
    if (n == 1) { *out = ch[0]; return; }
    blake3_hasher h; blake3_hasher_init(&h);
    blake3_hasher_update(&h, ch, 16 * n);                  /* lp_id is exactly 16 bytes, so the children are contiguous */
    blake3_hasher_finalize(&h, out->b, 16);
}

bool lp_id_codepoints_utf8(const char *str, size_t len, lp_id *out){
    const uint8_t *s = (const uint8_t *)str; size_t i = 0, n = 0; uint32_t cp; lp_id first, cur;
    blake3_hasher h; blake3_hasher_init(&h);
    while (i < len) {
        if (!lp_utf8_next(s, len, &i, &cp)) return false;
        lp_id_codepoint(cp, &cur); if (n == 0) first = cur;
        blake3_hasher_update(&h, cur.b, 16); n++;
    }
    if (n == 0) return false;
    if (n == 1) { *out = first; return true; }
    blake3_hasher_finalize(&h, out->b, 16); return true;
}
