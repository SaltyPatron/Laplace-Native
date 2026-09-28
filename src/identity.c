/* Identity: BLAKE3-128 over pure content. A codepoint hashes its UTF-8 bytes (surrogates in the generalized 3-byte
 * form); a composition hashes its children's IDs in order, repeats included; one child is that child. */
#include "laplace/laplace.h"
#include "blake3.h"
#include <string.h>

static size_t utf8_general(uint32_t cp, uint8_t o[4]){
    if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { o[0] = (uint8_t)(0xC0 | cp >> 6); o[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (uint8_t)(0xE0 | cp >> 12); o[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)); o[2] = (uint8_t)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (uint8_t)(0xF0 | cp >> 18); o[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)); o[3] = (uint8_t)(0x80 | (cp & 0x3F)); return 4;
}

void lp_id_codepoint(uint32_t cp, lp_id *out){
    uint8_t u[4]; size_t n = utf8_general(cp, u);
    blake3_hasher h; blake3_hasher_init(&h); blake3_hasher_update(&h, u, n); blake3_hasher_finalize(&h, out->b, 16);
}

void lp_id_compose(const lp_id *ch, size_t n, lp_id *out){
    if (n == 1) { *out = ch[0]; return; }
    blake3_hasher h; blake3_hasher_init(&h);
    blake3_hasher_update(&h, ch, 16 * n);                  /* lp_id is exactly 16 bytes, so the children are contiguous */
    blake3_hasher_finalize(&h, out->b, 16);
}

static bool utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp){
    uint8_t b = s[*i];
    if (b < 0x80) { *cp = b; *i += 1; return true; }
    int len = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 0;
    if (!len || *i + (size_t)len > n) return false;
    uint32_t c = b & (0x7Fu >> len);
    for (int k = 1; k < len; k++) { if ((s[*i + k] & 0xC0) != 0x80) return false; c = (c << 6) | (s[*i + k] & 0x3F); }
    *cp = c; *i += (size_t)len; return c < LP_NCP;
}

bool lp_id_codepoints_utf8(const char *str, size_t len, lp_id *out){
    const uint8_t *s = (const uint8_t *)str; size_t i = 0, n = 0; uint32_t cp; lp_id first, cur;
    blake3_hasher h; blake3_hasher_init(&h);
    while (i < len) {
        if (!utf8_next(s, len, &i, &cp)) return false;
        lp_id_codepoint(cp, &cur); if (n == 0) first = cur;
        blake3_hasher_update(&h, cur.b, 16); n++;
    }
    if (n == 0) return false;
    if (n == 1) { *out = first; return true; }
    blake3_hasher_finalize(&h, out->b, 16); return true;
}
