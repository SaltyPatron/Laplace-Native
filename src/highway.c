/* The highway: the types' records memory-mapped read-only, their lists and edges read once from the layout beside them. */
#define _GNU_SOURCE
#include "laplace/laplace.h"
#include "blake3.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const char *lp_highway_path(void){
    static char path[4096];
    const char *p = getenv("LAPLACE_HIGHWAY"); if (p && *p) return p;
    snprintf(path, sizeof path - 10, "%s", lp_tier0_path()); char *d = strrchr(path, '.'), *s = strrchr(path, '/');
    if (d && (!s || d > s)) *d = 0;
    strcat(path, ".highway"); return path;
}

/* The layout: "list NAME SAY FIRST COUNT" and "edges A B FIRST COUNT" lines; the records are the file, and the edges
 * follow them at the offset the first "edges" line's FIRST gives, as 8-byte pairs. */
const lp_highway *lp_highway_map(const char *path){
    if (!path || !*path) path = lp_highway_path();
    int fd = open(path, O_RDONLY); struct stat st; if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(lp_tier0_record)) { close(fd); return NULL; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd); if (m == MAP_FAILED) return NULL;
    char lp[4200]; snprintf(lp, sizeof lp, "%s.layout", path); FILE *f = fopen(lp, "r"); if (!f) { munmap(m, (size_t)st.st_size); return NULL; }
    lp_highway *h = calloc(1, sizeof *h); char *line = NULL; size_t cap = 0; size_t lc = 0, ec = 0; uint64_t nrec = 0, nedges = 0;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#') continue;
        char *save = NULL, *kind = strtok_r(line, "\t\n", &save);
        if (kind && !strcmp(kind, "records")) { char *n = strtok_r(NULL, "\t\n", &save); if (n) nrec = strtoull(n, NULL, 10); }
        else if (kind && !strcmp(kind, "edges-count")) { char *n = strtok_r(NULL, "\t\n", &save); if (n) nedges = strtoull(n, NULL, 10); }
        else if (kind && !strcmp(kind, "list")) {
            char *name = strtok_r(NULL, "\t\n", &save), *say = strtok_r(NULL, "\t\n", &save), *first = strtok_r(NULL, "\t\n", &save), *count = strtok_r(NULL, "\t\n", &save);
            if (!name || !say || !first || !count) continue;
            if (h->nlists == lc) { lc = lc ? lc * 2 : 32; h->list = realloc(h->list, lc * sizeof(lp_list)); }
            lp_list *x = &h->list[h->nlists++]; memset(x, 0, sizeof *x);
            snprintf(x->name, sizeof x->name, "%s", name); snprintf(x->say, sizeof x->say, "%s", say); x->first = (uint32_t)strtoul(first, NULL, 10); x->count = (uint32_t)strtoul(count, NULL, 10);
        }
        else if (kind && !strcmp(kind, "mask")) {
            char *name = strtok_r(NULL, "\t\n", &save), *bit = strtok_r(NULL, "\t\n", &save), *width = strtok_r(NULL, "\t\n", &save);
            if (!name || !bit || !width) continue;
            h->mask = realloc(h->mask, (h->nmasks + 1) * sizeof(lp_mask)); lp_mask *x = &h->mask[h->nmasks++]; memset(x, 0, sizeof *x);
            snprintf(x->name, sizeof x->name, "%s", name); x->bit = (uint16_t)atoi(bit); x->width = (uint16_t)atoi(width);
        }
        else if (kind && !strcmp(kind, "edges")) {
            char *a = strtok_r(NULL, "\t\n", &save), *b = strtok_r(NULL, "\t\n", &save), *first = strtok_r(NULL, "\t\n", &save), *count = strtok_r(NULL, "\t\n", &save);
            if (!a || !b || !first || !count) continue;
            if (h->nedgelists == ec) { ec = ec ? ec * 2 : 32; h->edges = realloc(h->edges, ec * sizeof(lp_edges)); }
            lp_edges *x = &h->edges[h->nedgelists++]; memset(x, 0, sizeof *x);
            snprintf(x->a, sizeof x->a, "%s", a); snprintf(x->b, sizeof x->b, "%s", b); x->first = (uint32_t)strtoul(first, NULL, 10); x->count = (uint32_t)strtoul(count, NULL, 10);
        }
    }
    free(line); fclose(f);
    if (nrec * sizeof(lp_tier0_record) + nedges * sizeof(lp_edge) > (size_t)st.st_size) { munmap(m, (size_t)st.st_size); free(h->list); free(h->edges); free(h); return NULL; }
    h->rec = m; h->nrec = nrec; h->edge = (const lp_edge *)((const uint8_t *)m + nrec * sizeof(lp_tier0_record)); h->nedges = nedges;
    for (size_t i = 0; i < h->nmasks; i++) for (size_t j = 0; j < h->nlists; j++) if (!strcmp(h->mask[i].name, h->list[j].name)) h->mask[i].list = &h->list[j];
    return h;
}

const lp_list *lp_highway_list(const lp_highway *h, const char *name){
    size_t nl = strlen(name);
    for (size_t i = 0; i < h->nlists; i++)
        if (lp_name_same(h->list[i].name, strlen(h->list[i].name), name, nl) || lp_name_same(h->list[i].say, strlen(h->list[i].say), name, nl)) return &h->list[i];
    return NULL;
}
const lp_tier0_record *lp_highway_at(const lp_highway *h, const lp_list *l, uint32_t slot){
    if (!l || slot >= l->count || (size_t)l->first + slot >= h->nrec) return NULL;
    return &h->rec[l->first + slot];
}
/* by_id: open addressing over every record, built once; a record's place is its index + 1 */
static uint64_t hkey(const lp_id *id){ uint64_t k; memcpy(&k, id->b, 8); return k; }
int64_t lp_highway_slot(const lp_highway *hc, const lp_list *l, const lp_id *id){
    lp_highway *h = (lp_highway *)hc;
    if (!h->by_id) {
        size_t n = 1; while (n < h->nrec * 2 + 2) n <<= 1;
        uint32_t *t = calloc(n, sizeof(uint32_t));
        for (size_t i = 0; i < h->nrec; i++) { uint64_t k = hkey(&h->rec[i].id) & (n - 1); while (t[k]) k = (k + 1) & (n - 1); t[k] = (uint32_t)i + 1; }
        __atomic_store_n(&h->nby, n, __ATOMIC_RELEASE); __atomic_store_n(&h->by_id, t, __ATOMIC_RELEASE);
    }
    size_t n = h->nby; uint64_t k = hkey(id) & (n - 1);
    while (h->by_id[k]) { const lp_tier0_record *r = &h->rec[h->by_id[k] - 1];
        if (!memcmp(&r->id, id, 16) && (!l || ((size_t)(r - h->rec) >= l->first && (size_t)(r - h->rec) < (size_t)l->first + l->count))) return (int64_t)((size_t)(r - h->rec) - (l ? l->first : 0));
        k = (k + 1) & (n - 1); }
    return -1;
}
size_t lp_highway_edges(const lp_highway *h, const char *a, uint32_t slot, const char *b, const lp_edge **out){
    for (size_t i = 0; i < h->nedgelists; i++) {
        const lp_edges *e = &h->edges[i]; if (strcmp(e->a, a) || strcmp(e->b, b)) continue;
        const lp_edge *p = h->edge + e->first; size_t lo = 0, hi = e->count;              /* sorted by from: binary search to the first */
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (p[mid].from < slot) lo = mid + 1; else hi = mid; }
        size_t n = 0; while (lo + n < e->count && p[lo + n].from == slot) n++;
        *out = p + lo; return n;
    }
    *out = NULL; return 0;
}
void lp_highway_fingerprint(const lp_highway *h, uint8_t out[32]){
    blake3_hasher hs; blake3_hasher_init(&hs);
    blake3_hasher_update(&hs, h->rec, h->nrec * sizeof(lp_tier0_record));
    blake3_hasher_update(&hs, h->edge, h->nedges * sizeof(lp_edge));
    blake3_hasher_finalize(&hs, out, 32);
}

const lp_mask *lp_highway_mask(const lp_highway *h, const char *name){
    for (size_t i = 0; i < h->nmasks; i++) if (!strcmp(h->mask[i].name, name)) return &h->mask[i];
    return NULL;
}
int32_t lp_highway_mask_bit(const lp_highway *h, const lp_id *id){
    int64_t i = lp_highway_slot(h, NULL, id); if (i < 0) return -1;
    for (size_t k = 0; k < h->nmasks; k++) { const lp_list *l = h->mask[k].list; if (!l) continue;
        if ((size_t)i >= l->first && (size_t)i < (size_t)l->first + l->count) { uint32_t slot = (uint32_t)i - l->first; return slot < h->mask[k].width ? (int32_t)(h->mask[k].bit + slot) : -1; } }
    return -1;
}
