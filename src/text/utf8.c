/* UTF-8 in its generalized form: every value of the codespace has bytes, surrogates included (3 bytes), so every
 * codepoint has an ID and any text decodes to the codepoints it was written with. */
#include "laplace/laplace.h"

size_t lp_utf8_put(uint32_t cp, uint8_t o[4]){
    if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { o[0] = (uint8_t)(0xC0 | cp >> 6); o[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (uint8_t)(0xE0 | cp >> 12); o[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)); o[2] = (uint8_t)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (uint8_t)(0xF0 | cp >> 18); o[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F));
    o[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)); o[3] = (uint8_t)(0x80 | (cp & 0x3F)); return 4;
}

bool lp_utf8_next(const uint8_t *s, size_t n, size_t *i, uint32_t *cp){
    uint8_t b = s[*i];
    if (b < 0x80) { *cp = b; *i += 1; return true; }
    int len = b >= 0xF8 ? 0 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 0;
    if (!len || *i + (size_t)len > n) return false;
    uint32_t c = b & (0x7Fu >> len);
    for (int k = 1; k < len; k++) { if ((s[*i + k] & 0xC0) != 0x80) return false; c = (c << 6) | (s[*i + k] & 0x3F); }
    if (c >= LP_NCP) return false;
    *cp = c; *i += (size_t)len; return true;
}
