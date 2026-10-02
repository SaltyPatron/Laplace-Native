/* Identity: BLAKE3-128 over pure content. A codepoint hashes its UTF-8 bytes (surrogates in the generalized 3-byte
 * form); a composition hashes its children's IDs in order, repeats included; one child is that child. */
#include "laplace/laplace.h"
#include "internal.h"
#include "blake3.h"
#include "blake3_impl.h"
#include <string.h>

/* The standard BLAKE3 hash of an input, its first 16 bytes. An input of one chunk (1,024 bytes, 64 children), which
 * is nearly every node, is hashed by the compressions the hasher would make, without the hasher: no 1.9 KB state set
 * up, no 16-byte update per child (Research: Performance, BLAKE3 for many small inputs). The same bits: the chunk's
 * blocks chained from the IV, the first CHUNK_START, the last CHUNK_END and ROOT at its own length, counter 0. */
void lp_hash16(const void *in, size_t len, lp_id *out){
    if (len == 0 || len > BLAKE3_CHUNK_LEN) {
        blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, in, len); blake3_hasher_finalize(&h, out->b, 16); return; }
    const uint8_t *p = in; uint32_t cv[8]; memcpy(cv, IV, sizeof cv);
    size_t nb = (len + BLAKE3_BLOCK_LEN - 1) / BLAKE3_BLOCK_LEN;
    for (size_t b = 0; b + 1 < nb; b++) blake3_compress_in_place(cv, p + b * BLAKE3_BLOCK_LEN, BLAKE3_BLOCK_LEN, 0, (uint8_t)(b == 0 ? CHUNK_START : 0));
    uint8_t last[BLAKE3_BLOCK_LEN], o[64]; size_t ll = len - (nb - 1) * BLAKE3_BLOCK_LEN;
    memcpy(last, p + (nb - 1) * BLAKE3_BLOCK_LEN, ll); memset(last + ll, 0, BLAKE3_BLOCK_LEN - ll);
    blake3_compress_xof(cv, last, (uint8_t)ll, 0, (uint8_t)((nb == 1 ? CHUNK_START : 0) | CHUNK_END | ROOT), o);
    memcpy(out->b, o, 16);
}

void lp_id_codepoint(uint32_t cp, lp_id *out){
    uint8_t u[4]; size_t n = lp_utf8_put(cp, u);
    lp_hash16(u, n, out);
}

void lp_id_compose(const lp_id *ch, size_t n, lp_id *out){
    if (n == 1) { *out = ch[0]; return; }
    lp_hash16(ch, 16 * n, out);                           /* lp_id is exactly 16 bytes, so the children are contiguous */
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
