#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
/* Tier 0: the fixed 1,114,112-point Unicode codespace, sorted by content ID and mapped read-only.
 * Codepoint ordinals are process-local acceleration addresses, never file offsets or identities. */
#include "laplace/tier0.h"
#include "internal.h"
#include "blake3.h"
#include <stdlib.h>
#include <errno.h>
#include <fenv.h>
#include <math.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <io.h>
#else
#include <unistd.h>
#endif

_Static_assert(sizeof(lp_tier0_record) == 64, "tier-0 records are 64 bytes");

double cr_sin(double), cr_cos(double);

typedef struct { uint64_t hilbert; uint32_t index; } Placement;

static int placement_order(const void *a, const void *b){
    const Placement *x = a, *y = b;
    if (x->hilbert != y->hilbert) return x->hilbert < y->hilbert ? -1 : 1;
    return x->index < y->index ? -1 : x->index > y->index;
}

static int record_order(const void *a, const void *b){
    return memcmp(((const lp_tier0_record *)a)->id.b, ((const lp_tier0_record *)b)->id.b, 16);
}

bool lp_tier0_write(const char *path, const lp_tier0_record *records){
    if (!path || !*path || !records) { errno = EINVAL; return false; }
    char temporary[4096]; FILE *file = NULL;
    static uint64_t sequence;
#ifdef _WIN32
    wchar_t destination_w[4096], temporary_w[4096];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, destination_w, 4096)) { errno = EINVAL; return false; }
    unsigned long process = (unsigned long)_getpid();
#else
    unsigned long process = (unsigned long)getpid();
#endif
    for (int attempt = 0; attempt < 32 && !file; attempt++) {
        uint64_t serial = __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
        int n = snprintf(temporary, sizeof temporary, "%s.%lu.%llu.tmp", path, process, (unsigned long long)serial);
        if (n < 0 || (size_t)n >= sizeof temporary) { errno = ENAMETOOLONG; return false; }
#ifdef _WIN32
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, temporary, -1, temporary_w, 4096)) { errno = EINVAL; return false; }
        file = _wfopen(temporary_w, L"wbx");
#else
        file = fopen(temporary, "wbx");
#endif
        if (!file && errno != EEXIST) return false;
    }
    if (!file) return false;
    bool ok = fwrite(records, sizeof *records, LP_TIER0_COUNT, file) == LP_TIER0_COUNT;
    if (ok) ok = fflush(file) == 0;
#ifdef _WIN32
    if (ok) ok = _commit(_fileno(file)) == 0;
#else
    if (ok) ok = fsync(fileno(file)) == 0;
#endif
    if (fclose(file) != 0) ok = false;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExW(temporary_w, destination_w, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) errno = EIO;
#else
        ok = rename(temporary, path) == 0;
#endif
    }
    if (!ok) {
        int error = errno;
#ifdef _WIN32
        _wremove(temporary_w);
#else
        remove(temporary);
#endif
        errno = error;
    }
    return ok;
}

typedef struct { uint16_t primary, secondary, tertiary; } CollationElement;
typedef struct { uint32_t weights, nfd; uint16_t n1, n2, n3, nd; } CollationKey;
typedef struct {
    uint8_t *assigned, *ideograph, *hancore, *ccc, *decomp_count;
    uint32_t *decomp_offset, *decomp, *element_offset, *nfd;
    uint16_t *element_count, *weights;
    CollationElement *elements;
    CollationKey *keys;
    size_t nd, dc, ne, ec, nw, wc, nn, nc;
    struct { uint32_t lo, hi, base; } implicit[64];
    size_t nimplicit;
} UnicodeOrder;

static bool reserve_array(void **p, size_t *capacity, size_t need, size_t width){
    if (need <= *capacity) return true;
    size_t n = *capacity ? *capacity : 1024;
    while (n < need) { if (n > SIZE_MAX / 2) { errno = ENOMEM; return false; } n *= 2; }
    if (n > SIZE_MAX / width) { errno = ENOMEM; return false; }
    void *next = realloc(*p, n * width);
    if (!next) return false;
    *p = next; *capacity = n; return true;
}

static void unicode_free(UnicodeOrder *u){
    free(u->assigned); free(u->ideograph); free(u->hancore); free(u->ccc);
    free(u->decomp_count); free(u->decomp_offset); free(u->decomp);
    free(u->element_offset); free(u->element_count); free(u->elements);
    free(u->keys); free(u->weights); free(u->nfd);
}

static FILE *unicode_file(const char *root, const char *relative){
    char path[4096]; int n = snprintf(path, sizeof path, "%s/%s", root, relative);
    if (n < 0 || (size_t)n >= sizeof path) { errno = ENAMETOOLONG; return NULL; }
    return fopen(path, "r");
}

static int unicode_range(const char *line, uint32_t *lo, uint32_t *hi){
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == '\r' || *line == '\n') return 0;
    char *end; unsigned long first = strtoul(line, &end, 16);
    if (end == line || first >= LP_NCP) return -1;
    unsigned long last = first;
    if (end[0] == '.' && end[1] == '.') {
        const char *start = end + 2; last = strtoul(start, &end, 16);
        if (end == start || last >= LP_NCP || last < first) return -1;
    }
    *lo = (uint32_t)first; *hi = (uint32_t)last; return 1;
}

static const char *unicode_field(const char *line){
    const char *p = strchr(line, ';');
    if (!p) return NULL;
    do { p++; } while (*p == ' ' || *p == '\t');
    return p;
}

static bool read_unicode(UnicodeOrder *u, const char *root){
    u->assigned = calloc(LP_NCP, 1); u->ideograph = calloc(LP_NCP, 1);
    u->hancore = calloc(LP_NCP, 1); u->ccc = calloc(LP_NCP, 1);
    u->decomp_count = calloc(LP_NCP, 1);
    u->decomp_offset = calloc(LP_NCP, sizeof(uint32_t));
    if (!u->assigned || !u->ideograph || !u->hancore || !u->ccc || !u->decomp_count || !u->decomp_offset) return false;
    const char *names[] = { "ucd/extracted/DerivedGeneralCategory.txt", "ucd/PropList.txt", "ucd/Blocks.txt" };
    char line[8192];
    for (int which = 0; which < 3; which++) {
        FILE *f = unicode_file(root, names[which]); if (!f) return false;
        bool ok = true;
        while (fgets(line, sizeof line, f)) {
            uint32_t lo, hi; int parsed = unicode_range(line, &lo, &hi);
            if (!parsed) continue;
            const char *value = unicode_field(line);
            if (parsed < 0 || !value) { ok = false; break; }
            if (which == 0) {
                uint8_t assigned = strncmp(value, "Cn", 2) && strncmp(value, "Cs", 2);
                for (uint32_t cp = lo; cp <= hi; cp++) u->assigned[cp] = assigned;
            } else if (which == 1) {
                if (!strncmp(value, "Unified_Ideograph", 17) && (value[17] == ' ' || value[17] == '#' || value[17] == '\r' || value[17] == '\n'))
                    for (uint32_t cp = lo; cp <= hi; cp++) u->ideograph[cp] = 1;
            } else {
                size_t len = strcspn(value, "#\r\n");
                while (len && (value[len - 1] == ' ' || value[len - 1] == '\t')) len--;
                if ((len == 22 && !memcmp(value, "CJK Unified Ideographs", len))
                    || (len == 28 && !memcmp(value, "CJK Compatibility Ideographs", len)))
                    for (uint32_t cp = lo; cp <= hi; cp++) u->hancore[cp] = 1;
            }
        }
        ok = ok && !ferror(f); fclose(f);
        if (!ok) { errno = EINVAL; return false; }
    }
    FILE *f = unicode_file(root, "ucd/UnicodeData.txt"); if (!f) return false;
    bool ok = true;
    while (fgets(line, sizeof line, f)) {
        char *fields[16], *p = line; int count = 1; fields[0] = p;
        while ((p = strchr(p, ';')) && count < 16) { *p++ = 0; fields[count++] = p; }
        char *end; unsigned long cp = strtoul(fields[0], &end, 16);
        if (count < 6 || end == fields[0] || *end || cp >= LP_NCP) { ok = false; break; }
        long combining = strtol(fields[3], &end, 10);
        if (*end || combining < 0 || combining > 255) { ok = false; break; }
        u->ccc[cp] = (uint8_t)combining;
        if (!fields[5][0] || fields[5][0] == '<') continue;
        u->decomp_offset[cp] = (uint32_t)u->nd;
        for (p = fields[5]; *p; p = end) {
            while (*p == ' ') p++;
            if (!*p) break;
            unsigned long part = strtoul(p, &end, 16);
            if (end == p || part >= LP_NCP || u->decomp_count[cp] == UINT8_MAX
                || !reserve_array((void **)&u->decomp, &u->dc, u->nd + 1, sizeof(uint32_t))) { ok = false; break; }
            u->decomp[u->nd++] = (uint32_t)part; u->decomp_count[cp]++;
        }
        if (!ok) break;
    }
    ok = ok && !ferror(f); fclose(f);
    if (!ok) errno = EINVAL;
    return ok;
}

static int unicode_nfd(const UnicodeOrder *u, uint32_t cp, uint32_t *out, size_t cap, int depth){
    if (!cap || depth > 32) return -1;
    if (cp >= 0xac00u && cp < 0xac00u + 11172u) {
        uint32_t s = cp - 0xac00u; int count = s % 28u ? 3 : 2;
        if (cap < (size_t)count) return -1;
        out[0] = 0x1100u + s / 588u; out[1] = 0x1161u + (s % 588u) / 28u;
        if (count == 3) out[2] = 0x11a7u + s % 28u;
        return count;
    }
    if (!u->decomp_count[cp]) { out[0] = cp; return 1; }
    int count = 0;
    for (size_t i = 0; i < u->decomp_count[cp]; i++) {
        int n = unicode_nfd(u, u->decomp[u->decomp_offset[cp] + i], out + count, cap - (size_t)count, depth + 1);
        if (n < 0) return -1;
        count += n;
    }
    return count;
}

static bool read_collation(UnicodeOrder *u, const char *root){
    u->element_offset = calloc(LP_NCP, sizeof(uint32_t));
    u->element_count = calloc(LP_NCP, sizeof(uint16_t));
    if (!u->element_offset || !u->element_count) return false;
    FILE *f = unicode_file(root, "uca/allkeys.txt"); if (!f) return false;
    bool ok = true; char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *comment = strchr(line, '#'); if (comment) *comment = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '\r' || *s == '\n') continue;
        if (!strncmp(s, "@implicitweights", 16)) {
            uint32_t lo, hi; const char *value = unicode_field(s + 16); char *end;
            if (unicode_range(s + 16, &lo, &hi) != 1 || !value || u->nimplicit == 64) { ok = false; break; }
            unsigned long base = strtoul(value, &end, 16);
            if (end == value || base > UINT16_MAX) { ok = false; break; }
            u->implicit[u->nimplicit].lo = lo; u->implicit[u->nimplicit].hi = hi;
            u->implicit[u->nimplicit++].base = (uint32_t)base; continue;
        }
        if (*s == '@') continue;
        char *semi = strchr(s, ';'); if (!semi) { ok = false; break; } *semi = 0;
        char *end; unsigned long cp = strtoul(s, &end, 16);
        if (end == s || cp >= LP_NCP) { ok = false; break; }
        while (*end == ' ' || *end == '\t') end++;
        if (*end) continue; /* contractions are not a single scalar's collation */
        u->element_offset[cp] = (uint32_t)u->ne;
        for (char *p = semi + 1; (p = strchr(p, '[')); p++) {
            unsigned a, b, c;
            if (sscanf(p + 2, "%4x.%4x.%4x", &a, &b, &c) != 3 || u->element_count[cp] == UINT16_MAX - 1
                || !reserve_array((void **)&u->elements, &u->ec, u->ne + 1, sizeof(CollationElement))) { ok = false; break; }
            u->elements[u->ne++] = (CollationElement){ (uint16_t)a, (uint16_t)b, (uint16_t)c };
            u->element_count[cp]++;
        }
        if (!ok) break;
        if (!u->element_count[cp]) u->element_count[cp] = UINT16_MAX;
    }
    ok = ok && !ferror(f); fclose(f); if (!ok) errno = EINVAL;
    return ok;
}

static int collation_elements(const UnicodeOrder *u, uint32_t cp, CollationElement out[64]){
    uint16_t explicit_count = u->element_count[cp];
    if (explicit_count) {
        size_t n = explicit_count == UINT16_MAX ? 0 : explicit_count;
        if (n > 64) return -1;
        if (n) memcpy(out, u->elements + u->element_offset[cp], n * sizeof *out);
        return (int)n;
    }
    if (cp >= 0xac00u && cp <= 0xd7a3u) {
        uint32_t s = cp - 0xac00u;
        uint32_t jamo[3] = { 0x1100u + s / 588u, 0x1161u + (s % 588u) / 28u, 0x11a7u + s % 28u };
        size_t count = 0;
        for (int j = 0; j < (s % 28u ? 3 : 2); j++) {
            size_t n = u->element_count[jamo[j]] == UINT16_MAX ? 0 : u->element_count[jamo[j]];
            if (count + n > 64) return -1;
            if (n) memcpy(out + count, u->elements + u->element_offset[jamo[j]], n * sizeof *out);
            count += n;
        }
        return (int)count;
    }
    for (size_t i = 0; i < u->nimplicit; i++) if (cp >= u->implicit[i].lo && cp <= u->implicit[i].hi && u->assigned[cp]) {
        uint32_t first = u->implicit[i].lo;
        for (size_t j = 0; j < u->nimplicit; j++) if (u->implicit[j].base == u->implicit[i].base && u->implicit[j].lo < first) first = u->implicit[j].lo;
        out[0] = (CollationElement){ (uint16_t)u->implicit[i].base, 0x20, 2 };
        out[1] = (CollationElement){ (uint16_t)((cp - first) | 0x8000u), 0, 0 }; return 2;
    }
    uint32_t base = u->ideograph[cp] ? (u->hancore[cp] ? 0xfb40u : 0xfb80u) + (cp >> 15) : 0xfbc0u + (cp >> 15);
    out[0] = (CollationElement){ (uint16_t)base, 0x20, 2 };
    out[1] = (CollationElement){ (uint16_t)((cp & 0x7fffu) | 0x8000u), 0, 0 }; return 2;
}

static bool unicode_keys(UnicodeOrder *u){
    u->keys = calloc(LP_NCP, sizeof(CollationKey)); if (!u->keys) return false;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        CollationElement elements[64]; uint32_t nfd[64];
        int count = collation_elements(u, cp, elements), nd = unicode_nfd(u, cp, nfd, 64, 0);
        if (count < 0 || nd < 0) { errno = EINVAL; return false; }
        for (int i = 0; i < nd; ) {
            if (!u->ccc[nfd[i]]) { i++; continue; }
            int end = i; while (end < nd && u->ccc[nfd[end]]) end++;
            for (int j = i + 1; j < end; j++) {
                uint32_t value = nfd[j]; int at = j;
                while (at > i && u->ccc[nfd[at - 1]] > u->ccc[value]) { nfd[at] = nfd[at - 1]; at--; }
                nfd[at] = value;
            }
            i = end;
        }
        if (u->nw > UINT32_MAX || u->nn > UINT32_MAX
            || !reserve_array((void **)&u->weights, &u->wc, u->nw + 3u * (size_t)count, sizeof(uint16_t))
            || !reserve_array((void **)&u->nfd, &u->nc, u->nn + (size_t)nd, sizeof(uint32_t))) return false;
        CollationKey *key = u->keys + cp; key->weights = (uint32_t)u->nw; key->nfd = (uint32_t)u->nn; key->nd = (uint16_t)nd;
        for (int i = 0; i < count; i++) if (elements[i].primary) { u->weights[u->nw++] = elements[i].primary; key->n1++; }
        for (int i = 0; i < count; i++) if (elements[i].secondary) { u->weights[u->nw++] = elements[i].secondary; key->n2++; }
        for (int i = 0; i < count; i++) if (elements[i].tertiary) { u->weights[u->nw++] = elements[i].tertiary; key->n3++; }
        memcpy(u->nfd + u->nn, nfd, (size_t)nd * sizeof(uint32_t)); u->nn += (size_t)nd;
    }
    return true;
}

static int weight_order(const uint16_t *a, size_t na, const uint16_t *b, size_t nb){
    size_t count = na < nb ? na : nb;
    for (size_t i = 0; i < count; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return na < nb ? -1 : na > nb;
}

static int scalar_order(const UnicodeOrder *u, uint32_t a, uint32_t b){
    const CollationKey *x = u->keys + a, *y = u->keys + b;
    const uint16_t *wx = u->weights + x->weights, *wy = u->weights + y->weights; int cmp;
    if ((cmp = weight_order(wx, x->n1, wy, y->n1))) return cmp;
    if ((cmp = weight_order(wx + x->n1, x->n2, wy + y->n1, y->n2))) return cmp;
    if ((cmp = weight_order(wx + x->n1 + x->n2, x->n3, wy + y->n1 + y->n2, y->n3))) return cmp;
    size_t count = x->nd < y->nd ? x->nd : y->nd;
    for (size_t i = 0; i < count; i++) if (u->nfd[x->nfd + i] != u->nfd[y->nfd + i])
        return u->nfd[x->nfd + i] < u->nfd[y->nfd + i] ? -1 : 1;
    if (x->nd != y->nd) return x->nd < y->nd ? -1 : 1;
    return a < b ? -1 : a > b;
}

bool lp_tier0_from_ucd(const char *root, lp_tier0_record *out){
    if (!root || !*root || !out) { errno = EINVAL; return false; }
    UnicodeOrder u = { 0 };
    uint32_t *order = NULL, *scratch = NULL;
    bool ok = read_unicode(&u, root) && read_collation(&u, root) && unicode_keys(&u);
    if (!ok) goto done;
    order = malloc((size_t)LP_TIER0_COUNT * sizeof(uint32_t));
    scratch = malloc((size_t)LP_TIER0_COUNT * sizeof(uint32_t));
    if (!order || !scratch) { ok = false; goto done; }
    size_t count = 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) order[count++] = cp;
    /* Context-carrying merge sort keeps generation reentrant on both Windows and POSIX. */
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t start = 0; start < count; start += 2 * width) {
            size_t mid = start + width < count ? start + width : count;
            size_t end = mid + width < count ? mid + width : count;
            size_t a = start, b = mid, at = start;
            while (a < mid && b < end) scratch[at++] = scalar_order(&u, order[a], order[b]) <= 0 ? order[a++] : order[b++];
            while (a < mid) scratch[at++] = order[a++];
            while (b < end) scratch[at++] = order[b++];
        }
        uint32_t *swap = order; order = scratch; scratch = swap;
    }
    ok = lp_tier0_build(order, count, out);
done:
    free(order); free(scratch); unicode_free(&u);
    return ok;
}

bool lp_tier0_build(const uint32_t *order, size_t count, lp_tier0_record *out){
    if (!order || !out || count != LP_TIER0_COUNT) { errno = EINVAL; return false; }
    unsigned char *seen = calloc(LP_TIER0_COUNT, 1);
    double (*points)[4] = NULL; Placement *walk = NULL;
    if (!seen) return false;
    for (size_t i = 0; i < count; i++) {
        uint32_t cp = order[i];
        if (cp >= LP_NCP || seen[cp]++) { free(seen); errno = EINVAL; return false; }
    }
    free(seen);
    points = malloc(count * sizeof *points); walk = malloc(count * sizeof *walk);
    if (!points || !walk) { free(points); free(walk); errno = ENOMEM; return false; }
    int rounding = fegetround();
    if (rounding < 0 || fesetround(FE_TONEAREST) != 0) {
        free(points); free(walk); errno = EDOM; return false;
    }
    const double phi = sqrt(2.0), psi = 1.533751168755204288118041;
    const double twopi = 2.0 * 3.141592653589793;
    for (size_t i = 0; i < count; i++) {
        double s = (double)i + 0.5, t = s / (double)count, r = sqrt(t), R = sqrt(1.0 - t);
        double alpha = twopi * s / phi, beta = twopi * s / psi;
        points[i][0] = r * cr_sin(alpha); points[i][1] = r * cr_cos(alpha);
        points[i][2] = R * cr_sin(beta); points[i][3] = R * cr_cos(beta);
        uint32_t grid[4];
        for (int d = 0; d < 4; d++) grid[d] = lp_hilbert4_axis(points[i][d]);
        walk[i] = (Placement){ lp_hilbert4_grid(grid), (uint32_t)i };
    }
    qsort(walk, count, sizeof *walk, placement_order);
    const __int128 limit = (__int128)1 << 106;
    for (size_t rank = 0; rank < count; rank++) {
        lp_tier0_record *record = out + rank;
        const double *point = points[walk[rank].index]; lp_coord c;
        /* The canonical 2^-53 grid truncates toward zero before enforcing the exact wall. */
        for (int d = 0; d < 4; d++) c.m[d] = (int64_t)(point[d] * LP_FIXED_ONE);
        for (;;) {
            __int128 norm = 0;
            for (int d = 0; d < 4; d++) norm += (__int128)c.m[d] * c.m[d];
            if (norm <= limit) break;
            int largest = 0;
            for (int d = 1; d < 4; d++) if (llabs(c.m[d]) > llabs(c.m[largest])) largest = d;
            c.m[largest] -= c.m[largest] > 0 ? 1 : -1;
        }
        lp_id_codepoint(order[rank], &record->id);
        memcpy(record->m, c.m, sizeof c.m);
        record->hilbert = lp_hilbert4(&c); record->rank = (uint32_t)rank; record->pad = 0;
    }
    int restored = fesetround(rounding);
    free(points); free(walk);
    if (restored != 0) { errno = EDOM; return false; }
    qsort(out, count, sizeof *out, record_order);
    return true;
}

#ifndef LP_TIER0_DEFAULT
#define LP_TIER0_DEFAULT "/repos/work/tier0/tier0.bin"
#endif

const char *lp_tier0_path(void){
    const char *p = getenv("LAPLACE_TIER0");
    return p && *p ? p : LP_TIER0_DEFAULT;
}

typedef struct Tier0Index {
    const lp_tier0_record *table;
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
    lp_idindex_free(x->ids); free(x->slot_of_codepoint); free(x->codepoint_of_slot); free(x);
}

static const void *tier0_open(const char *path){
    const size_t bytes = (size_t)LP_TIER0_COUNT * sizeof(lp_tier0_record);
    const lp_tier0_record *table = lp_map_file(path, bytes, NULL);
    if (!table) return NULL;
    Tier0Index *x = calloc(1, sizeof *x);
    unsigned char *ranks = calloc(LP_TIER0_COUNT, 1);
    if (!x || !ranks) goto invalid;
    x->table = table;
    x->slot_of_codepoint = malloc((size_t)LP_TIER0_COUNT * sizeof(uint32_t));
    x->codepoint_of_slot = malloc((size_t)LP_TIER0_COUNT * sizeof(uint32_t));
    if (!x->slot_of_codepoint || !x->codepoint_of_slot) goto invalid;

    /* Refuse corrupt, unsorted, duplicated or differently shaped artifacts before publishing a mapping. */
    for (size_t i = 0; i < LP_TIER0_COUNT; i++) {
        lp_coord c; memcpy(c.m, table[i].m, sizeof c.m);
        if ((i && memcmp(table[i - 1].id.b, table[i].id.b, 16) >= 0)
            || table[i].rank >= LP_TIER0_COUNT || ranks[table[i].rank]++
            || table[i].pad || !lp_coord_inside(&c) || lp_hilbert4(&c) != table[i].hilbert)
            goto invalid;
    }
    x->ids = lp_idindex_build(table, LP_TIER0_COUNT, sizeof *table);
    if (!x->ids) goto invalid;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        lp_id id; lp_id_codepoint(cp, &id);
        uint64_t probe = 0; int64_t slot = lp_idindex_find(x->ids, &id, &probe);
        if (slot < 0) goto invalid;
        x->slot_of_codepoint[cp] = (uint32_t)slot;
        x->codepoint_of_slot[slot] = cp;
    }
    free(ranks);
    lp_lock_take(&index_mu);
    x->next = indexes;
    __atomic_store_n(&indexes, x, __ATOMIC_RELEASE);
    lp_lock_give(&index_mu);
    return table;

invalid:
    free(ranks); index_free(x); lp_unmap_file(table, bytes); errno = EINVAL;
    return NULL;
}

const lp_tier0_record *lp_tier0_map(const char *path){
    return lp_cached("tier0", path && *path ? path : lp_tier0_path(), tier0_open);
}

const lp_tier0_record *lp_tier0_at(const lp_tier0_record *table, uint32_t cp){
    if (cp >= LP_NCP) return NULL;
    const Tier0Index *x = index_for(table);
    return x ? table + x->slot_of_codepoint[cp] : NULL;
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
    blake3_hasher h; blake3_hasher_init(&h);
    blake3_hasher_update(&h, t0, (size_t)LP_TIER0_COUNT * sizeof(lp_tier0_record));
    blake3_hasher_finalize(&h, out, 32);
}
