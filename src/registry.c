/* The governed registries (manifest/): the relations and their frozen bits, the qualifier families, the entity types,
 * the language vocabulary and the lexical vocabularies, read as their files write them. Nothing here names a relation
 * or a value: every record keeps every field it declares, and the typed lookups below read those fields by name.
 *
 * A TOML registry is read as records: each [[name]] opens one record of that kind; [name] opens a single table; a
 * field is key = "string" | number | true | false | [ list, possibly over several lines ]. A TSV registry is its
 * header row and its rows; lines that begin with # are its provenance. Unknown syntax is an error, never skipped. */
#define _GNU_SOURCE
#include "laplace/laplace.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *key, *val; } Field;                               /* a list is kept as its items joined by \x1f */
struct lp_record { char *kind; Field *f; size_t n, cap; };
struct lp_registry { char *path; lp_record *r; size_t n, cap; char err[256]; };

static char *dupn(const char *s, size_t n){ char *d = malloc(n + 1); memcpy(d, s, n); d[n] = 0; return d; }
static void put(lp_record *r, const char *k, size_t kn, char *v){
    if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 8; r->f = realloc(r->f, sizeof(Field) * r->cap); }
    r->f[r->n].key = dupn(k, kn); r->f[r->n].val = v; r->n++;
}
static lp_record *open_record(lp_registry *g, const char *kind, size_t kn){
    if (g->n == g->cap) { g->cap = g->cap ? g->cap * 2 : 64; g->r = realloc(g->r, sizeof(lp_record) * g->cap); }
    lp_record *r = &g->r[g->n++]; memset(r, 0, sizeof *r); r->kind = dupn(kind, kn); return r;
}
/* One scalar from p: a quoted string (with \" and \\), or a bare word up to a delimiter. *out is malloc'd. */
static const char *scalar(const char *p, char **out){
    if (*p == '"') { p++; size_t cap = 64, n = 0; char *s = malloc(cap);
        for (; *p && *p != '"'; p++) { char c = *p; if (c == '\\' && p[1]) c = *++p; if (n + 2 > cap) s = realloc(s, cap *= 2); s[n++] = c; }
        if (*p != '"') { free(s); return NULL; } s[n] = 0; *out = s; return p + 1; }
    const char *b = p; while (*p && *p != ',' && *p != ']' && *p != '#' && !isspace((unsigned char)*p)) p++;
    if (p == b) return NULL; *out = dupn(b, (size_t)(p - b)); return p;
}
static const char *skip(const char *p){ while (*p && isspace((unsigned char)*p)) p++; return p; }

static int read_toml(lp_registry *g, FILE *f){
    char *line = NULL; size_t cap = 0; long no = 0; lp_record *cur = NULL;
    char *list = NULL; size_t ln = 0, lcap = 0; char lkey[128]; int inlist = 0;
    while (getline(&line, &cap, f) > 0) { no++; const char *p = skip(line);
        if (inlist) goto items;
        if (!*p || *p == '#') continue;
        if (*p == '[') { int two = p[1] == '['; const char *b = p + 1 + two, *e = strchr(b, ']');
            if (!e || (two && e[1] != ']')) goto bad;
            cur = open_record(g, b, (size_t)(e - b)); continue; }
        { const char *k = p; while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) p++;
          size_t kn = (size_t)(p - k); p = skip(p); if (!kn || *p != '=') goto bad; p = skip(p + 1);
          if (!cur) cur = open_record(g, "", 0);                          /* fields before any table: a record of no kind */
          if (*p == '[') { inlist = 1; ln = 0; snprintf(lkey, sizeof lkey, "%.*s", (int)kn, k); p++; goto items; }
          char *v; const char *q = scalar(p, &v); if (!q) goto bad; q = skip(q);
          if (*q && *q != '#') { free(v); goto bad; }
          put(cur, k, kn, v); continue; }
      items:
        for (;;) { p = skip(p);
            if (!*p || *p == '#') break;                                   /* the list goes on on the next line */
            if (*p == ']') { inlist = 0; put(cur, lkey, strlen(lkey), list ? dupn(list, ln) : dupn("", 0)); free(list); list = NULL; ln = lcap = 0;
                p = skip(p + 1); if (*p && *p != '#') goto bad; break; }
            char *v; const char *q = scalar(p, &v); if (!q) goto bad;
            size_t vl = strlen(v); if (ln + vl + 2 > lcap) { lcap = (ln + vl + 2) * 2; list = realloc(list, lcap); }
            if (ln) list[ln++] = '\x1f'; memcpy(list + ln, v, vl); ln += vl; free(v);
            p = skip(q); if (*p == ',') p++; }
        continue;
      bad:
        snprintf(g->err, sizeof g->err, "%s:%ld: not a field, a table or a list item: %.80s", g->path, no, line); free(line); free(list); return 0;
    }
    free(line);
    if (inlist) { snprintf(g->err, sizeof g->err, "%s: a list is not closed", g->path); free(list); return 0; }
    return 1;
}
static int read_tsv(lp_registry *g, FILE *f){
    char *line = NULL; size_t cap = 0; char **head = NULL; size_t nh = 0; long no = 0;
    while (getline(&line, &cap, f) > 0) { no++; size_t l = strlen(line); while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l) continue;
        if (line[0] == '#') { lp_record *r = open_record(g, "#", 1); put(r, "note", 4, dupn(line + 1, l - 1)); continue; }
        if (!nh) { for (char *s = line, *t; s; s = t) { t = strchr(s, '\t'); if (t) *t++ = 0; head = realloc(head, sizeof(char *) * (nh + 1)); head[nh++] = strdup(s); } continue; }
        lp_record *r = open_record(g, "row", 3); size_t c = 0;
        for (char *s = line, *t; s; s = t, c++) { t = strchr(s, '\t'); if (t) *t++ = 0;
            if (c >= nh) { snprintf(g->err, sizeof g->err, "%s:%ld: more fields than the header names", g->path, no); free(line); return 0; }
            put(r, head[c], strlen(head[c]), strdup(s)); } }
    free(line); for (size_t i = 0; i < nh; i++) free(head[i]); free(head);
    if (!nh) { snprintf(g->err, sizeof g->err, "%s: no header row", g->path); return 0; }
    return 1;
}

lp_registry *lp_registry_load(const char *path, char *err, size_t errlen){
    FILE *f = fopen(path, "r"); if (!f) { if (err) snprintf(err, errlen, "%s: cannot be read", path); return NULL; }
    lp_registry *g = calloc(1, sizeof *g); g->path = strdup(path);
    size_t pl = strlen(path); int ok = pl > 4 && !strcmp(path + pl - 4, ".tsv") ? read_tsv(g, f) : read_toml(g, f);
    fclose(f);
    if (!ok) { if (err) snprintf(err, errlen, "%s", g->err); lp_registry_free(g); return NULL; }
    return g;
}
void lp_registry_free(lp_registry *g){
    if (!g) return;
    for (size_t i = 0; i < g->n; i++) { lp_record *r = &g->r[i]; free(r->kind); for (size_t j = 0; j < r->n; j++) { free(r->f[j].key); free(r->f[j].val); } free(r->f); }
    free(g->r); free(g->path); free(g);
}
size_t lp_registry_count(const lp_registry *g){ return g ? g->n : 0; }
const lp_record *lp_registry_at(const lp_registry *g, size_t i){ return g && i < g->n ? &g->r[i] : NULL; }
const char *lp_record_kind(const lp_record *r){ return r ? r->kind : NULL; }
const char *lp_record_get(const lp_record *r, const char *key){
    if (!r) return NULL; for (size_t j = 0; j < r->n; j++) if (!strcmp(r->f[j].key, key)) return r->f[j].val; return NULL;
}
size_t lp_record_fields(const lp_record *r){ return r ? r->n : 0; }
const char *lp_record_key(const lp_record *r, size_t j){ return r && j < r->n ? r->f[j].key : NULL; }
/* The k-th item of a list field (items are kept joined by \x1f); its length in *len. NULL past the end. */
const char *lp_list_item(const char *list, size_t k, size_t *len){
    if (!list) return NULL; const char *p = list;
    for (size_t i = 0; i < k; i++) { p = strchr(p, '\x1f'); if (!p) return NULL; p++; }
    if (!*p && k) return NULL;
    const char *e = strchr(p, '\x1f'); *len = e ? (size_t)(e - p) : strlen(p); return p;
}
/* The first record of this kind whose field key equals val. */
const lp_record *lp_registry_find(const lp_registry *g, const char *kind, const char *key, const char *val){
    for (size_t i = 0; g && i < g->n; i++) { const lp_record *r = &g->r[i]; if (kind && strcmp(r->kind, kind)) continue;
        const char *v = lp_record_get(r, key); if (v && !strcmp(v, val)) return r; }
    return NULL;
}
