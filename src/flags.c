/* The flags that go with tier 0: the records memory-mapped read-only, and their layout read once. */
#define _GNU_SOURCE
#include "laplace/laplace.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(lp_flags) == 32, "flags are 256 bits");

bool lp_name_same(const char *a, size_t al, const char *b, size_t bl){
    size_t i = 0, j = 0;
    for (;;) {
        while (i < al && (a[i] == '_' || a[i] == '-' || a[i] == ' ')) i++;
        while (j < bl && (b[j] == '_' || b[j] == '-' || b[j] == ' ')) j++;
        if (i == al || j == bl) return i == al && j == bl;
        if ((a[i] | 32) != (b[j] | 32)) return false;
        i++; j++;
    }
}

const char *lp_flags_path(void){
    static char path[4096];
    const char *p = getenv("LAPLACE_FLAGS"); if (p && *p) return p;
    snprintf(path, sizeof path - 8, "%s", lp_tier0_path()); char *d = strrchr(path, '.'), *s = strrchr(path, '/');
    if (d && (!s || d > s)) *d = 0;
    strcat(path, ".flags"); return path;
}

const lp_layout *lp_flags_map(const char *path){
    if (!path || !*path) path = lp_flags_path();
    int fd = open(path, O_RDONLY); struct stat st; if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != (size_t)LP_NCP * sizeof(lp_flags)) { close(fd); return NULL; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd); if (m == MAP_FAILED) return NULL;
    char lp[4200]; snprintf(lp, sizeof lp, "%s.layout", path); FILE *f = fopen(lp, "r"); if (!f) { munmap(m, (size_t)st.st_size); return NULL; }
    lp_layout *l = calloc(1, sizeof *l); l->flags = m; char *line = NULL; size_t cap = 0; size_t fc = 0, vc = 0;
    while (getline(&line, &cap, f) > 0) {
        if (line[0] == '#') continue;
        char *save = NULL, *bit = strtok_r(line, "\t\n", &save), *width = strtok_r(NULL, "\t\n", &save), *name = strtok_r(NULL, "\t\n", &save),
             *say = strtok_r(NULL, "\t\n", &save), *vals = strtok_r(NULL, "\t\n", &save);
        if (!bit || !width || !name || !say) continue;
        if (l->nfields == fc) { fc = fc ? fc * 2 : 128; l->field = realloc(l->field, fc * sizeof(lp_field)); }
        lp_field *x = &l->field[l->nfields++]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", name); snprintf(x->say, sizeof x->say, "%s", say);
        x->bit = (uint16_t)atoi(bit); x->width = (uint16_t)atoi(width); x->first = (uint32_t)l->nvalues;
        for (char *vs = NULL, *v = vals ? strtok_r(vals, " ", &vs) : NULL; v; v = strtok_r(NULL, " ", &vs)) {
            char *eq = strchr(v, '='); if (!eq) continue; *eq = 0;
            if (l->nvalues == vc) { vc = vc ? vc * 2 : 1024; l->value = realloc(l->value, vc * sizeof(lp_value)); }
            lp_value *y = &l->value[l->nvalues++]; snprintf(y->name, sizeof y->name, "%s", v); snprintf(y->say, sizeof y->say, "%s", eq + 1); x->nvalues++;
        }
    }
    free(line); fclose(f);
    return l;
}

const lp_field *lp_flags_field(const lp_layout *l, const char *property){
    size_t pl = strlen(property);
    for (size_t i = 0; i < l->nfields; i++)
        if (lp_name_same(l->field[i].name, strlen(l->field[i].name), property, pl) || lp_name_same(l->field[i].say, strlen(l->field[i].say), property, pl)) return &l->field[i];
    return NULL;
}

uint32_t lp_flags_get(const lp_layout *l, uint32_t cp, const lp_field *f){
    if (cp >= LP_NCP) return 0;
    const uint8_t *r = l->flags[cp].b; uint32_t v = 0;
    for (unsigned b = 0; b < f->width; b++) v |= (uint32_t)(r[(f->bit + b) >> 3] >> ((f->bit + b) & 7) & 1) << b;
    return v;
}

int32_t lp_flags_value(const lp_layout *l, const lp_field *f, const char *value){
    size_t vl = strlen(value);
    if (!f->nvalues) return lp_name_same("Y", 1, value, vl) || lp_name_same("Yes", 3, value, vl) || lp_name_same("true", 4, value, vl) ? 1
                          : lp_name_same("N", 1, value, vl) || lp_name_same("No", 2, value, vl) || lp_name_same("false", 5, value, vl) ? 0 : -1;
    for (uint32_t k = 0; k < f->nvalues; k++) {
        const lp_value *y = &l->value[f->first + k];
        if (lp_name_same(y->name, strlen(y->name), value, vl) || lp_name_same(y->say, strlen(y->say), value, vl)) return (int32_t)k;
    }
    return -1;
}
