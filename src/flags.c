/* The flags that go with tier 0: the records memory-mapped read-only, and their layout read once. */
#include "laplace/laplace.h"
#include "internal.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

const char *lp_flags_path(void){ static char path[4096]; return lp_tier0_sibling("LAPLACE_FLAGS", ".flags", path, sizeof path); }

/* The layout: "BIT WIDTH NAME SAY [VALUE=SAY ...]", a field a line, its values in the standard's own order. */
typedef struct { lp_layout *l; size_t fc, vc; } Reading;
static void field_line(void *ctx, char **f, int n){
    Reading *r = ctx; lp_layout *l = r->l; if (n < 4) return;
    if (l->nfields == r->fc) { r->fc = r->fc ? r->fc * 2 : 128; l->field = realloc(l->field, r->fc * sizeof(lp_field)); }
    lp_field *x = &l->field[l->nfields++]; memset(x, 0, sizeof *x);
    snprintf(x->name, sizeof x->name, "%s", f[2]); snprintf(x->say, sizeof x->say, "%s", f[3]);
    x->bit = (uint16_t)atoi(f[0]); x->width = (uint16_t)atoi(f[1]); x->first = (uint32_t)l->nvalues;
    for (char *vs = NULL, *v = n > 4 ? strtok_r(f[4], " ", &vs) : NULL; v; v = strtok_r(NULL, " ", &vs)) {
        char *eq = strchr(v, '='); if (!eq) continue; *eq = 0;
        if (l->nvalues == r->vc) { r->vc = r->vc ? r->vc * 2 : 1024; l->value = realloc(l->value, r->vc * sizeof(lp_value)); }
        lp_value *y = &l->value[l->nvalues++]; snprintf(y->name, sizeof y->name, "%s", v); snprintf(y->say, sizeof y->say, "%s", eq + 1); x->nvalues++;
    }
}
static const void *flags_open(const char *path){
    const lp_flags *m = lp_map_file(path, (size_t)LP_NCP * sizeof(lp_flags), NULL); if (!m) return NULL;
    char lp[4200]; lp_beside(path, ".layout", lp, sizeof lp);
    Reading r = { calloc(1, sizeof(lp_layout)), 0, 0 }; if (!r.l) return NULL; r.l->flags = m;
    if (!lp_lines(lp, 5, field_line, &r)) { free(r.l); return NULL; }
    return r.l;
}
const lp_layout *lp_flags_map(const char *path){ return lp_cached("flags", path && *path ? path : lp_flags_path(), flags_open); }

const lp_field *lp_flags_field(const lp_layout *l, const char *property){
    int64_t i = lp_name_find(l->field, l->nfields, sizeof(lp_field), offsetof(lp_field, name), offsetof(lp_field, say), property);
    return i < 0 ? NULL : &l->field[i];
}

/* A field's bits, read as one word: the field starts at its bit and runs width bits, its first bit the lowest. */
uint32_t lp_flags_get(const lp_layout *l, uint32_t cp, const lp_field *f){
    if (cp >= LP_NCP) return 0;
    const uint8_t *r = l->flags[cp].b; unsigned at = f->bit >> 3, shift = f->bit & 7;
    uint64_t w = 0; for (unsigned k = 0; k < 8 && at + k < 32; k++) w |= (uint64_t)r[at + k] << (8 * k);
    return (uint32_t)((w >> shift) & (f->width >= 32 ? 0xFFFFFFFFull : (1ull << f->width) - 1));
}

int32_t lp_flags_value(const lp_layout *l, const lp_field *f, const char *value){
    size_t vl = strlen(value);
    if (!f->nvalues) return lp_name_same("Y", 1, value, vl) || lp_name_same("Yes", 3, value, vl) || lp_name_same("true", 4, value, vl) ? 1
                          : lp_name_same("N", 1, value, vl) || lp_name_same("No", 2, value, vl) || lp_name_same("false", 5, value, vl) ? 0 : -1;
    return (int32_t)lp_name_find(&l->value[f->first], f->nvalues, sizeof(lp_value), offsetof(lp_value, name), offsetof(lp_value, say), value);
}
