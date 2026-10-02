/* The perf-caches' files, one way: a file memory-mapped read-only and shared for the life of the process, the files
 * beside it found by its path, its layout read a line at a time, and a name looked up by the standard's rule. Tier 0,
 * its flags and the highway are each a mapping and a layout read through these. */
#define _GNU_SOURCE
#include "laplace/laplace.h"
#include "internal.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

const void *lp_map_file(const char *path, size_t want, size_t *size){
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || (want && (size_t)st.st_size != want)) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd);
    if (p == MAP_FAILED) return NULL;
    if (size) *size = (size_t)st.st_size;
    return p;
}

static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char kind[16], path[4096]; const void *p; } cache[16]; static int ncache;
const void *lp_cached(const char *kind, const char *path, const void *(*open)(const char *path)){
    const void *p = NULL;
    pthread_mutex_lock(&cache_mu);
    for (int i = 0; i < ncache && !p; i++) if (!strcmp(cache[i].kind, kind) && !strcmp(cache[i].path, path)) p = cache[i].p;
    if (!p && (p = open(path)) && ncache < 16) { snprintf(cache[ncache].kind, 16, "%s", kind); snprintf(cache[ncache].path, 4096, "%s", path); cache[ncache++].p = p; }
    pthread_mutex_unlock(&cache_mu);
    return p;
}

void lp_beside(const char *path, const char *ending, char *out, size_t cap){ snprintf(out, cap, "%s%s", path, ending); }

const char *lp_tier0_sibling(const char *env, const char *ending, char *buf, size_t cap){
    const char *p = getenv(env); if (p && *p) return p;
    snprintf(buf, cap, "%s", lp_tier0_path()); char *d = strrchr(buf, '.'), *s = strrchr(buf, '/');
    if (d && (!s || d > s)) *d = 0;
    size_t n = strlen(buf); snprintf(buf + n, cap - n, "%s", ending);
    return buf;
}

bool lp_lines(const char *path, int max, void (*each)(void *ctx, char **field, int n), void *ctx){
    FILE *f = fopen(path, "r"); if (!f) return false;
    char *line = NULL, *field[16]; size_t cap = 0; if (max > 16) max = 16;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#') continue;
        char *save = NULL; int n = 0;
        for (char *t = strtok_r(line, "\t\n", &save); t && n < max; t = strtok_r(NULL, "\t\n", &save)) field[n++] = t;
        if (n) each(ctx, field, n);
    }
    free(line); fclose(f);
    return true;
}

int64_t lp_name_find(const void *records, size_t n, size_t stride, size_t name_off, size_t say_off, const char *name){
    size_t nl = strlen(name); const uint8_t *r = (const uint8_t *)records;
    for (size_t i = 0; i < n; i++) {
        const char *a = (const char *)(r + i * stride + name_off), *b = (const char *)(r + i * stride + say_off);
        if (lp_name_same(a, strlen(a), name, nl) || lp_name_same(b, strlen(b), name, nl)) return (int64_t)i;
    }
    return -1;
}
