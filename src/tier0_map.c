#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/* Tier 0 modular shard emission, branch selection, validation, and O(1) lookup tables. */
#include "tier0_internal.h"
#include "blake3.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifndef LP_TIER0_DEFAULT
#define LP_TIER0_DEFAULT "/repos/work/tier0/tier0.bin"
#endif

const char *lp_tier0_path(void){
    const char *p = getenv("LAPLACE_TIER0");
    return p && *p ? p : LP_TIER0_DEFAULT;
}

uint32_t lp_tier0_parse_branches(const char *spec){
    if (!spec) return LP_TIER0_BRANCH_ALL;
    while (*spec == ' ' || *spec == '\t') spec++;
    if (!*spec) return LP_TIER0_BRANCH_ALL;
    uint32_t mask = 0;
    const char *p = spec;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '+' || *p == '|') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '+' && *p != '|') p++;
        size_t len = (size_t)(p - start);
        if (lp_name_same(start, len, "ascii", 5)) mask |= LP_TIER0_BRANCH_ASCII;
        else if (lp_name_same(start, len, "utf", 3) || lp_name_same(start, len, "latin", 5))
            mask |= (start == spec && !*p) ? (LP_TIER0_BRANCH_ASCII | LP_TIER0_BRANCH_UTF) : LP_TIER0_BRANCH_UTF;
        else if (lp_name_same(start, len, "utf-only", 8)) mask |= LP_TIER0_BRANCH_UTF;
        else if (lp_name_same(start, len, "cjk", 3))
            mask |= (start == spec && !*p) ? LP_TIER0_BRANCH_BMP : LP_TIER0_BRANCH_CJK;
        else if (lp_name_same(start, len, "cjk-only", 8)) mask |= LP_TIER0_BRANCH_CJK;
        else if (lp_name_same(start, len, "bmp", 3)) mask |= LP_TIER0_BRANCH_BMP;
        else if (lp_name_same(start, len, "ext", 3) || lp_name_same(start, len, "supplementary", 13)
                 || lp_name_same(start, len, "planes", 6)) mask |= LP_TIER0_BRANCH_EXT;
        else if (lp_name_same(start, len, "full", 4) || lp_name_same(start, len, "all", 3))
            mask |= LP_TIER0_BRANCH_ALL;
        else return 0u;
    }
    return mask;
}

bool lp_tier0_shard_path(const char *base_path, lp_tier0_shard shard, char *out, size_t cap){
    const char *name = lp_tier0_shard_name(shard);
    if (!base_path || !*base_path || !name || !out || cap < 16) { errno = EINVAL; return false; }
    char stem[4096];
    int n = snprintf(stem, sizeof stem, "%s", base_path);
    if (n < 0 || (size_t)n >= sizeof stem) { errno = ENAMETOOLONG; return false; }
    char *at = strrchr(stem, '@');
    char *slash = strrchr(stem, '/'), *bslash = strrchr(stem, '\\');
    if (bslash > slash) slash = bslash;
    if (at && (!slash || at > slash)) *at = 0;
    size_t slen = strlen(stem);
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
        char suf[32]; int sn = snprintf(suf, sizeof suf, ".%s.bin", lp_tier0_shard_name((lp_tier0_shard)s));
        if (sn > 0 && slen > (size_t)sn && !strcmp(stem + slen - (size_t)sn, suf)) {
            stem[slen - (size_t)sn] = 0; slen -= (size_t)sn; break;
        }
    }
    if (slen > 4 && !strcmp(stem + slen - 4, ".bin")) stem[slen - 4] = 0;
    int wn = snprintf(out, cap, "%s.%s.bin", stem, name);
    if (wn < 0 || (size_t)wn >= cap) { errno = ENAMETOOLONG; return false; }
    return true;
}

static int record_cmp(const void *a, const void *b){
    return memcmp(((const lp_tier0_record *)a)->id.b, ((const lp_tier0_record *)b)->id.b, 16);
}

bool lp_tier0_write_shards(const char *base_path, const lp_tier0_record *records){
    if (!base_path || !*base_path || !records) { errno = EINVAL; return false; }
    lp_idindex *idx = lp_idindex_build(records, LP_TIER0_COUNT, sizeof *records);
    if (!idx) return false;
    lp_tier0_record *shards[LP_TIER0_SHARD_COUNT] = { 0 };
    size_t counts[LP_TIER0_SHARD_COUNT] = { 0 };
    bool ok = true;
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
        shards[s] = malloc((size_t)lp_tier0_shard_count((lp_tier0_shard)s) * sizeof(lp_tier0_record));
        if (!shards[s]) { ok = false; break; }
    }
    if (ok) {
        for (uint32_t cp = 0; cp < LP_NCP; cp++) {
            lp_id id; lp_id_codepoint(cp, &id);
            uint64_t probe = 0; int64_t slot = lp_idindex_find(idx, &id, &probe);
            if (slot < 0) { errno = EINVAL; ok = false; break; }
            lp_tier0_shard s = lp_tier0_shard_of(cp);
            if (counts[s] >= lp_tier0_shard_count(s)) { errno = EINVAL; ok = false; break; }
            shards[s][counts[s]++] = records[slot];
        }
    }
    for (unsigned s = 0; ok && s < LP_TIER0_SHARD_COUNT; s++) {
        if (counts[s] != lp_tier0_shard_count((lp_tier0_shard)s)) { errno = EINVAL; ok = false; break; }
        qsort(shards[s], counts[s], sizeof(lp_tier0_record), record_cmp);
        char spath[4096];
        if (!lp_tier0_shard_path(base_path, (lp_tier0_shard)s, spath, sizeof spath)
            || !lp_tier0_write_records(spath, shards[s], counts[s])) ok = false;
    }
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) free(shards[s]);
    lp_idindex_free(idx);
    return ok;
}

bool lp_tier0_split_file(const char *path){
    const char *target = path && *path ? path : lp_tier0_path();
    const lp_tier0_record *records = lp_map_file(target, (size_t)LP_TIER0_COUNT * sizeof(lp_tier0_record), NULL);
    if (!records) return false;
    bool ok = lp_tier0_write_shards(target, records);
    lp_unmap_file(records, (size_t)LP_TIER0_COUNT * sizeof(lp_tier0_record));
    return ok;
}

typedef struct Tier0Index {
    const lp_tier0_record *table;
    size_t count;
    uint32_t branches;
    bool owned_table;
    lp_idindex *ids;
    uint32_t *slot_of_codepoint, *codepoint_of_slot;
    struct Tier0Index *next;
} Tier0Index;

static Tier0Index *indexes;
static lp_lock index_mu = LP_LOCK_INIT;

static const Tier0Index *index_for(const lp_tier0_record *table){
    for (const Tier0Index *x = __atomic_load_n(&indexes, __ATOMIC_ACQUIRE); x; x = x->next)
        if (x->table == table) return x;
    return NULL;
}

static void index_free(Tier0Index *x){
    if (!x) return;
    if (x->owned_table) free((void *)x->table);
    lp_idindex_free(x->ids); free(x->slot_of_codepoint); free(x->codepoint_of_slot); free(x);
}

static const lp_tier0_record *publish_index(const lp_tier0_record *table, size_t count,
                                            uint32_t branches, bool owned_table){
    Tier0Index *x = calloc(1, sizeof *x);
    unsigned char *ranks = calloc(LP_TIER0_COUNT, 1);
    if (!x || !ranks) goto invalid;
    x->table = table; x->count = count; x->branches = branches; x->owned_table = owned_table;
    x->slot_of_codepoint = malloc((size_t)LP_TIER0_COUNT * sizeof(uint32_t));
    x->codepoint_of_slot = malloc(count * sizeof(uint32_t));
    if (!x->slot_of_codepoint || !x->codepoint_of_slot) goto invalid;
    memset(x->slot_of_codepoint, 0xff, (size_t)LP_TIER0_COUNT * sizeof(uint32_t));

    for (size_t i = 0; i < count; i++) {
        lp_coord c; memcpy(c.m, table[i].m, sizeof c.m);
        if ((i && memcmp(table[i - 1].id.b, table[i].id.b, 16) >= 0)
            || table[i].rank >= LP_TIER0_COUNT || ranks[table[i].rank]++
            || table[i].pad || !lp_coord_inside(&c) || lp_hilbert4(&c) != table[i].hilbert)
            goto invalid;
    }
    x->ids = lp_idindex_build(table, count, sizeof *table);
    if (!x->ids) goto invalid;
    size_t matched = 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        if (!(branches & (1u << lp_tier0_shard_of(cp)))) continue;
        lp_id id; lp_id_codepoint(cp, &id);
        uint64_t probe = 0; int64_t slot = lp_idindex_find(x->ids, &id, &probe);
        if (slot < 0 || (size_t)slot >= count) goto invalid;
        x->slot_of_codepoint[cp] = (uint32_t)slot;
        x->codepoint_of_slot[slot] = cp;
        matched++;
    }
    if (matched != count) goto invalid;
    free(ranks);
    lp_lock_take(&index_mu);
    x->next = indexes;
    __atomic_store_n(&indexes, x, __ATOMIC_RELEASE);
    lp_lock_give(&index_mu);
    return table;

invalid:
    free(ranks);
    if (x) x->owned_table = false;
    index_free(x);
    if (owned_table) free((void *)table);
    errno = EINVAL;
    return NULL;
}

static const lp_tier0_record *load_from_shards(const char *base_path, uint32_t branches){
    size_t total = lp_tier0_branch_record_count(branches);
    if (!total) { errno = EINVAL; return NULL; }
    if ((branches & (branches - 1u)) == 0) {
        for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) if (branches == (1u << s)) {
            char spath[4096];
            if (!lp_tier0_shard_path(base_path, (lp_tier0_shard)s, spath, sizeof spath)) return NULL;
            const size_t bytes = total * sizeof(lp_tier0_record);
            const lp_tier0_record *mapped = lp_map_file(spath, bytes, NULL);
            if (!mapped) return NULL;
            const lp_tier0_record *ok = publish_index(mapped, total, branches, false);
            if (!ok) lp_unmap_file(mapped, bytes);
            return ok;
        }
    }
    const lp_tier0_record *mapped[LP_TIER0_SHARD_COUNT] = { 0 };
    size_t sizes[LP_TIER0_SHARD_COUNT] = { 0 };
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
        if (!(branches & (1u << s))) continue;
        char spath[4096];
        if (!lp_tier0_shard_path(base_path, (lp_tier0_shard)s, spath, sizeof spath)) goto fail;
        sizes[s] = (size_t)lp_tier0_shard_count((lp_tier0_shard)s) * sizeof(lp_tier0_record);
        mapped[s] = lp_map_file(spath, sizes[s], NULL);
        if (!mapped[s]) goto fail;
    }
    lp_tier0_record *merged = malloc(total * sizeof(lp_tier0_record));
    if (!merged) goto fail;
    size_t cursors[LP_TIER0_SHARD_COUNT] = { 0 }, counts[LP_TIER0_SHARD_COUNT] = { 0 };
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++)
        if (mapped[s]) counts[s] = lp_tier0_shard_count((lp_tier0_shard)s);
    for (size_t out = 0; out < total; out++) {
        int best = -1;
        for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
            if (cursors[s] >= counts[s]) continue;
            if (best < 0 || memcmp(mapped[s][cursors[s]].id.b, mapped[best][cursors[best]].id.b, 16) < 0)
                best = (int)s;
        }
        merged[out] = mapped[best][cursors[best]++];
    }
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++)
        if (mapped[s]) lp_unmap_file(mapped[s], sizes[s]);
    return publish_index(merged, total, branches, true);

fail:
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++)
        if (mapped[s]) lp_unmap_file(mapped[s], sizes[s]);
    return NULL;
}

static const lp_tier0_record *filter_full_file(const char *base_path, uint32_t branches){
    const size_t full_bytes = (size_t)LP_TIER0_COUNT * sizeof(lp_tier0_record);
    const lp_tier0_record *full = lp_map_file(base_path, full_bytes, NULL);
    if (!full) return NULL;
    size_t total = lp_tier0_branch_record_count(branches);
    lp_idindex *idx = lp_idindex_build(full, LP_TIER0_COUNT, sizeof *full);
    lp_tier0_record *sub = malloc(total * sizeof(lp_tier0_record));
    if (!idx || !sub) {
        lp_idindex_free(idx); free(sub); lp_unmap_file(full, full_bytes); return NULL;
    }
    size_t at = 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        if (!(branches & (1u << lp_tier0_shard_of(cp)))) continue;
        lp_id id; lp_id_codepoint(cp, &id);
        uint64_t probe = 0; int64_t slot = lp_idindex_find(idx, &id, &probe);
        if (slot < 0 || at >= total) {
            lp_idindex_free(idx); free(sub); lp_unmap_file(full, full_bytes); errno = EINVAL; return NULL;
        }
        sub[at++] = full[slot];
    }
    lp_idindex_free(idx);
    lp_unmap_file(full, full_bytes);
    if (at != total) { free(sub); errno = EINVAL; return NULL; }
    qsort(sub, total, sizeof *sub, record_cmp);
    return publish_index(sub, total, branches, true);
}

static const void *tier0_open(const char *raw_path){
    char base[4096];
    int n = snprintf(base, sizeof base, "%s", raw_path);
    if (n < 0 || (size_t)n >= sizeof base) { errno = ENAMETOOLONG; return NULL; }
    uint32_t branches = LP_TIER0_BRANCH_ALL;
    char *at = strrchr(base, '@'), *slash = strrchr(base, '/'), *bslash = strrchr(base, '\\');
    if (bslash > slash) slash = bslash;
    if (at && (!slash || at > slash)) {
        *at = 0;
        branches = lp_tier0_parse_branches(at + 1);
        if (!branches) { errno = EINVAL; return NULL; }
    } else {
        size_t blen = strlen(base);
        for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
            char suf[32]; int sn = snprintf(suf, sizeof suf, ".%s.bin", lp_tier0_shard_name((lp_tier0_shard)s));
            if (sn > 0 && blen > (size_t)sn && !strcmp(base + blen - (size_t)sn, suf)) {
                size_t bytes = (size_t)lp_tier0_shard_count((lp_tier0_shard)s) * sizeof(lp_tier0_record);
                const lp_tier0_record *mapped = lp_map_file(base, bytes, NULL);
                if (!mapped) return NULL;
                const lp_tier0_record *ok = publish_index(mapped, lp_tier0_shard_count((lp_tier0_shard)s), 1u << s, false);
                if (!ok) lp_unmap_file(mapped, bytes);
                return ok;
            }
        }
    }
    if (branches != LP_TIER0_BRANCH_ALL) {
        const lp_tier0_record *sh = load_from_shards(base, branches);
        return sh ? sh : filter_full_file(base, branches);
    }
    const size_t bytes = (size_t)LP_TIER0_COUNT * sizeof(lp_tier0_record);
    const lp_tier0_record *table = lp_map_file(base, bytes, NULL);
    if (table) {
        const lp_tier0_record *ok = publish_index(table, LP_TIER0_COUNT, LP_TIER0_BRANCH_ALL, false);
        if (!ok) lp_unmap_file(table, bytes);
        return ok;
    }
    return load_from_shards(base, LP_TIER0_BRANCH_ALL);
}

const lp_tier0_record *lp_tier0_map_branches(const char *base_path, uint32_t branches){
    if (!branches || (branches & ~LP_TIER0_BRANCH_ALL)) { errno = EINVAL; return NULL; }
    const char *base = base_path && *base_path ? base_path : lp_tier0_path();
    if (branches == LP_TIER0_BRANCH_ALL) return lp_cached("tier0", base, tier0_open);
    char key[4160];
    int pos = snprintf(key, sizeof key, "%s@", base);
    if (pos < 0 || (size_t)pos >= sizeof key) { errno = ENAMETOOLONG; return NULL; }
    bool first = true;
    for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
        if (!(branches & (1u << s))) continue;
        int w = snprintf(key + pos, sizeof key - (size_t)pos, "%s%s",
                         first ? "" : "+", lp_tier0_shard_name((lp_tier0_shard)s));
        if (w < 0 || (size_t)w >= sizeof key - (size_t)pos) { errno = ENAMETOOLONG; return NULL; }
        pos += w; first = false;
    }
    return lp_cached("tier0", key, tier0_open);
}

const lp_tier0_record *lp_tier0_map(const char *path){
    if (!path || !*path) {
        const char *prof = getenv("LAPLACE_TIER0_PROFILE");
        if (!prof || !*prof) prof = getenv("LAPLACE_TIER0_BRANCHES");
        if (prof && *prof) return lp_tier0_map_branches(lp_tier0_path(), lp_tier0_parse_branches(prof));
        return lp_cached("tier0", lp_tier0_path(), tier0_open);
    }
    return lp_cached("tier0", path, tier0_open);
}

uint32_t lp_tier0_active_branches(const lp_tier0_record *table){
    const Tier0Index *x = index_for(table);
    return x ? x->branches : 0u;
}

size_t lp_tier0_record_count(const lp_tier0_record *table){
    const Tier0Index *x = index_for(table);
    return x ? x->count : 0u;
}

const lp_tier0_record *lp_tier0_at(const lp_tier0_record *table, uint32_t cp){
    if (cp >= LP_NCP) return NULL;
    const Tier0Index *x = index_for(table);
    if (!x) return NULL;
    uint32_t slot = x->slot_of_codepoint[cp];
    return slot < x->count ? table + slot : NULL;
}

const lp_tier0_record *lp_tier0_find(const lp_tier0_record *table, const lp_id *id){
    const Tier0Index *x = index_for(table);
    if (!x || !id) return NULL;
    uint64_t probe = 0; int64_t slot = lp_idindex_find(x->ids, id, &probe);
    return slot < 0 ? NULL : table + slot;
}

bool lp_tier0_contains(const lp_tier0_record *table, const lp_id *id){
    return lp_tier0_find(table, id) != NULL;
}

int64_t lp_tier0_codepoint(const lp_tier0_record *table, const lp_id *id){
    const Tier0Index *x = index_for(table);
    if (!x || !id) return -1;
    uint64_t probe = 0; int64_t slot = lp_idindex_find(x->ids, id, &probe);
    return slot < 0 ? -1 : (int64_t)x->codepoint_of_slot[slot];
}

void lp_tier0_fingerprint(const lp_tier0_record *t0, uint8_t out[32]){
    const Tier0Index *x = index_for(t0);
    size_t count = x ? x->count : (size_t)LP_TIER0_COUNT;
    blake3_hasher h; blake3_hasher_init(&h);
    blake3_hasher_update(&h, t0, count * sizeof(lp_tier0_record));
    blake3_hasher_finalize(&h, out, 32);
}
