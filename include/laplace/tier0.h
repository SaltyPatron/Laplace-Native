#ifndef LAPLACE_TIER0_H
#define LAPLACE_TIER0_H

#include "laplace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Projection uses the entire fixed Unicode codespace. Selecting text characters
 * or modular script shards must never change the Super-Fibonacci population or
 * global DUCET ranks: every codepoint keeps its canonical 16-byte ID and 4D S^3
 * coordinate across all shard and branch configurations. */
#define LP_TIER0_COUNT LP_NCP

/* Disjoint script/plane shards partitioning the 1,114,112 Unicode codespace:
 *   ASCII (128 records, 8 KB):         U+0000..U+007F
 *   UTF   (22,112 records, 1.35 MB):   non-CJK BMP scalars (Latin, Greek, Cyrillic, Semitic, Indic, punctuation, math)
 *   CJK   (115,920 records, 7.08 MB):  Hangul Jamo/Syllables, CJK Radicals/Symbols, Kana, Han BMP + Planes 2-3, Fullwidth
 *   EXT   (975,952 records, 59.57 MB): Supplementary non-CJK planes + generalized surrogate codespace */
typedef enum {
    LP_TIER0_SHARD_ASCII = 0,
    LP_TIER0_SHARD_UTF   = 1,
    LP_TIER0_SHARD_CJK   = 2,
    LP_TIER0_SHARD_EXT   = 3,
    LP_TIER0_SHARD_COUNT = 4
} lp_tier0_shard;

enum {
    LP_TIER0_BRANCH_ASCII = 1u << LP_TIER0_SHARD_ASCII,
    LP_TIER0_BRANCH_UTF   = 1u << LP_TIER0_SHARD_UTF,
    LP_TIER0_BRANCH_CJK   = 1u << LP_TIER0_SHARD_CJK,
    LP_TIER0_BRANCH_EXT   = 1u << LP_TIER0_SHARD_EXT,
    LP_TIER0_BRANCH_BMP   = LP_TIER0_BRANCH_ASCII | LP_TIER0_BRANCH_UTF | LP_TIER0_BRANCH_CJK,
    LP_TIER0_BRANCH_ALL   = (1u << LP_TIER0_SHARD_COUNT) - 1u
};

#define LP_TIER0_ASCII_COUNT 128u
#define LP_TIER0_UTF_COUNT   22112u
#define LP_TIER0_CJK_COUNT   115920u
#define LP_TIER0_EXT_COUNT   975952u

static inline bool lp_unicode_scalar(uint32_t cp){
    return cp < LP_NCP && (cp < 0xd800u || cp > 0xdfffu);
}

static inline lp_tier0_shard lp_tier0_shard_of(uint32_t cp){
    if (cp <= 0x007fu) return LP_TIER0_SHARD_ASCII;
    if ((cp >= 0x1100u && cp <= 0x11ffu) ||
        (cp >= 0x2e80u && cp <= 0x9fffu) ||
        (cp >= 0xac00u && cp <= 0xd7afu) ||
        (cp >= 0xf900u && cp <= 0xfaffu) ||
        (cp >= 0xff00u && cp <= 0xffefu) ||
        (cp >= 0x20000u && cp <= 0x323afu))
        return LP_TIER0_SHARD_CJK;
    if (cp <= 0xffffu && (cp < 0xd800u || cp > 0xdfffu))
        return LP_TIER0_SHARD_UTF;
    return LP_TIER0_SHARD_EXT;
}

static inline uint32_t lp_tier0_shard_count(lp_tier0_shard shard){
    switch (shard) {
        case LP_TIER0_SHARD_ASCII: return LP_TIER0_ASCII_COUNT;
        case LP_TIER0_SHARD_UTF:   return LP_TIER0_UTF_COUNT;
        case LP_TIER0_SHARD_CJK:   return LP_TIER0_CJK_COUNT;
        case LP_TIER0_SHARD_EXT:   return LP_TIER0_EXT_COUNT;
        default:                   return 0u;
    }
}

static inline const char *lp_tier0_shard_name(lp_tier0_shard shard){
    switch (shard) {
        case LP_TIER0_SHARD_ASCII: return "ascii";
        case LP_TIER0_SHARD_UTF:   return "utf";
        case LP_TIER0_SHARD_CJK:   return "cjk";
        case LP_TIER0_SHARD_EXT:   return "ext";
        default:                   return NULL;
    }
}

static inline uint32_t lp_tier0_branch_record_count(uint32_t branches){
    uint32_t total = 0;
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++)
        if (branches & (1u << s)) total += lp_tier0_shard_count((lp_tier0_shard)s);
    return total;
}

/* Parse a branch/profile specification ("ascii", "utf", "cjk", "bmp", "full", or
 * '+'/','-separated shard names like "ascii+utf", "ascii+cjk", "cjk-only") into a
 * LP_TIER0_BRANCH_* mask; returns 0 on unknown tokens. NULL or "" selects LP_TIER0_BRANCH_ALL. */
LP_API uint32_t lp_tier0_parse_branches(const char *spec);

/* Derive the sibling shard path (<stem>.<shard>.bin) from a base tier0.bin path. */
LP_API bool lp_tier0_shard_path(const char *base_path, lp_tier0_shard shard, char *out, size_t cap);

/* Map a configured subset of Tier-0 branches from modular shard files (<stem>.<shard>.bin)
 * or by filtering the canonical full artifact at base_path. */
LP_API const lp_tier0_record *lp_tier0_map_branches(const char *base_path, uint32_t branches);

/* Active LP_TIER0_BRANCH_* bitmask and record count of a mapped Tier-0 table. */
LP_API uint32_t lp_tier0_active_branches(const lp_tier0_record *table);
LP_API size_t   lp_tier0_record_count(const lp_tier0_record *table);

/* O(1) lookups over a validated mapping. Missing IDs, codepoints outside the
 * active branches or codespace, and unknown mappings are rejected. */
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
/* Publish the complete artifact and its modular shard files (.ascii.bin, .utf.bin,
 * .cjk.bin, .ext.bin) by replacing their destinations atomically. */
LP_API bool lp_tier0_write(const char *path, const lp_tier0_record *records);
LP_API bool lp_tier0_write_shards(const char *base_path, const lp_tier0_record *records);
/* Split an existing validated full tier0.bin on disk into its four modular shard files. */
LP_API bool lp_tier0_split_file(const char *path);

#ifdef __cplusplus
}
#endif
#endif
