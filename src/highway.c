/* A single immutable mapping owns records, identifiers, banks, keys and edges. */
#include "laplace/laplace.h"
#include "laplace/highway.h"
#include "internal.h"
#include "blake3.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *lp_highway_path(void){ static char path[4096]; return lp_tier0_sibling("LAPLACE_HIGHWAY", ".highway", path, sizeof path); }

/* The layout: "records N", "edges-count N", "list NAME SAY FIRST COUNT",
 * "bank NAME LIST GROUP CARRIER WIDTH OFFSET ENCODING" and
 * "edges A B FIRST COUNT" lines; the records are the file, and the edges follow them as 8-byte pairs. A bank names its
 * list as it is read: the layout writes the banks after the lists. */
typedef struct { lp_highway *h; size_t lc, ec; uint64_t nrec, nedges; bool bad; } Reading;
static bool number(const char *text, uint64_t limit, uint64_t *out){
    if (!text || !*text) return false;
    uint64_t n = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (digit > limit || n > (limit - digit) / 10) return false;
        n = 10 * n + digit;
    }
    *out = n; return true;
}
static void layout_line(void *ctx, char **f, int n){
    Reading *r = ctx; lp_highway *h = r->h; const char *kind = f[0];
    if (r->bad) return;
    if (!strcmp(kind, "records")) r->bad = n != 2 || !number(f[1], SIZE_MAX / sizeof(lp_tier0_record), &r->nrec);
    else if (!strcmp(kind, "edges-count")) r->bad = n != 2 || !number(f[1], SIZE_MAX / sizeof(lp_edge), &r->nedges);
    else if (!strcmp(kind, "list")) {
        if (n != 5) { r->bad = true; return; }
        if (h->nbanks) { r->bad = true; return; } /* banks keep pointers into the completed list array */
        uint64_t first, count;
        if (strlen(f[1]) >= sizeof h->list[0].name || strlen(f[2]) >= sizeof h->list[0].say
            || !number(f[3], UINT32_MAX, &first) || !number(f[4], UINT32_MAX, &count)) { r->bad = true; return; }
        if (h->nlists == r->lc) {
            size_t cap = r->lc ? r->lc * 2 : 32; lp_list *lists = realloc(h->list, cap * sizeof *lists);
            if (!lists) { r->bad = true; return; } h->list = lists; r->lc = cap;
        }
        lp_list *x = &h->list[h->nlists++]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", f[1]); snprintf(x->say, sizeof x->say, "%s", f[2]); x->first = (uint32_t)first; x->count = (uint32_t)count;
    }
    else if (!strcmp(kind, "bank")) {
        uint64_t width, offset;
        if (n != 8 || strlen(f[1]) >= sizeof h->bank[0].name || strlen(f[3]) >= sizeof h->bank[0].group
            || strlen(f[4]) >= sizeof h->bank[0].carrier || !number(f[5], 64, &width) || !width
            || !number(f[6], 63, &offset) || offset + width > 64
            || (strcmp(f[7], "enum") && strcmp(f[7], "flags"))) { r->bad = true; return; }
        lp_bank *banks = realloc(h->bank, (h->nbanks + 1) * sizeof *banks);
        if (!banks) { r->bad = true; return; } h->bank = banks;
        lp_bank *x = &h->bank[h->nbanks]; memset(x, 0, sizeof *x);
        snprintf(x->name, sizeof x->name, "%s", f[1]); snprintf(x->group, sizeof x->group, "%s", f[3]); snprintf(x->carrier, sizeof x->carrier, "%s", f[4]);
        x->width = (uint16_t)width; x->offset = (uint16_t)offset; x->flags = !strcmp(f[7], "flags");
        for (size_t i = 0; i < h->nlists; i++) if (!strcmp(h->list[i].name, f[2])) x->list = &h->list[i];
        if (!x->list || !x->list->count || (x->flags ? x->list->count > width
            : (uint64_t)x->list->count > (lp_mask_field(x) >> offset))) { r->bad = true; return; }
        for (size_t i = 0; i < h->nbanks; i++)
            if (!strcmp(x->name, h->bank[i].name) || (lp_mask_field(x) & lp_mask_field(h->bank + i))) { r->bad = true; return; }
        h->nbanks++;
    }
    else if (!strcmp(kind, "edges")) {
        if (n != 5) { r->bad = true; return; }
        uint64_t first, count;
        if (strlen(f[1]) >= sizeof h->edges[0].a || strlen(f[2]) >= sizeof h->edges[0].b
            || !number(f[3], UINT32_MAX, &first) || !number(f[4], UINT32_MAX, &count)) { r->bad = true; return; }
        if (h->nedgelists == r->ec) {
            size_t cap = r->ec ? r->ec * 2 : 32; lp_edges *edges = realloc(h->edges, cap * sizeof *edges);
            if (!edges) { r->bad = true; return; } h->edges = edges; r->ec = cap;
        }
        lp_edges *x = &h->edges[h->nedgelists++]; memset(x, 0, sizeof *x);
        snprintf(x->a, sizeof x->a, "%s", f[1]); snprintf(x->b, sizeof x->b, "%s", f[2]); x->first = (uint32_t)first; x->count = (uint32_t)count;
    }
    else r->bad = true;
}
/* All integers in the envelope are little endian. The existing fixed-width
 * scalar-cache records and edge pairs use that same on-disk representation. */
enum { HW_HEADER = 96 };
typedef struct { const char *display; const char **part; uint32_t count; } Identifier;
typedef struct {
    lp_highway public; const uint8_t *mapping; size_t size; Identifier *identifier;
    const char **parts; char *labels;
    uint8_t fingerprint[32]; lp_strmap *lists,*relations; uint32_t **row;
} Highway;
static uint64_t read64(const uint8_t *p){
    uint64_t n = 0; for (unsigned i = 0; i < 8; i++) n |= (uint64_t)p[i] << (8 * i); return n;
}
static uint32_t read32(const uint8_t *p){
    uint32_t n=0; for (unsigned i=0; i<4; i++) n|=(uint32_t)p[i]<<(8*i); return n;
}
static void write64(uint8_t *p, uint64_t n){ for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(n >> (8 * i)); }
static bool bytes_add(size_t *total, size_t count, size_t stride){
    if (count > (SIZE_MAX - *total) / stride) return false;
    *total += count * stride; return true;
}
static void digest(const uint8_t *header, const void *body, size_t bytes, uint8_t out[32]){
    blake3_hasher hash; blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, header, 64); blake3_hasher_update(&hash, body, bytes);
    blake3_hasher_finalize(&hash, out, 32);
}
bool lp_highway_write(const char *path, const lp_tier0_record *records, size_t nrec,
    const lp_edge *edges, size_t nedges, const char *layout, size_t nl,
    const char *keys_text, size_t nk, const char *identifiers, size_t ni){
    size_t size = HW_HEADER;
    if (!path || !records || !nrec || nrec > UINT32_MAX || nedges > UINT32_MAX
        || (nedges && !edges) || !layout || !nl || (nk && !keys_text) || !identifiers || !ni
        || !bytes_add(&size,nrec,sizeof *records) || !bytes_add(&size,nedges,sizeof *edges)
        || !bytes_add(&size,nl,1) || !bytes_add(&size,nk,1) || !bytes_add(&size,ni,1)) return false;
    uint8_t header[HW_HEADER] = {0}; memcpy(header,"LPHW0002",8);
    write64(header+16,nrec); write64(header+24,nedges); write64(header+32,nl);
    write64(header+40,nk); write64(header+48,ni); write64(header+56,size);
    blake3_hasher hash; blake3_hasher_init(&hash); blake3_hasher_update(&hash,header,64);
    blake3_hasher_update(&hash,records,nrec*sizeof *records);
    blake3_hasher_update(&hash,edges,nedges*sizeof *edges);
    blake3_hasher_update(&hash,layout,nl); blake3_hasher_update(&hash,keys_text,nk);
    blake3_hasher_update(&hash,identifiers,ni); blake3_hasher_finalize(&hash,header+64,32);
    FILE *file = fopen(path,"wb"); if (!file) return false;
    bool ok = fwrite(header,1,sizeof header,file)==sizeof header
        && fwrite(records,sizeof *records,nrec,file)==nrec
        && (!nedges || fwrite(edges,sizeof *edges,nedges,file)==nedges)
        && fwrite(layout,1,nl,file)==nl && (!nk || fwrite(keys_text,1,nk,file)==nk)
        && fwrite(identifiers,1,ni,file)==ni;
    if (fclose(file)) ok = false;
    if (!ok) remove(path);
    return ok;
}
static bool lines(const char *bytes, size_t size, int fields, void (*visit)(void *,char **,int), void *ctx){
    if (!size) return true;
    if (bytes[size-1]!='\n' || memchr(bytes,0,size)) return false;
    char *copy = malloc(size+1); if (!copy) return false; memcpy(copy,bytes,size); copy[size]=0;
    char *line = copy;
    while (*line) {
        char *end = strchr(line,'\n'); if (!end) { free(copy); return false; } *end=0;
        if (end>line && end[-1]=='\r') end[-1]=0;
        if (*line && *line!='#') {
            char *f[9]; int n=0; f[n++]=line;
            for (char *p=line; *p; p++) if (*p=='\t') { *p=0; if (n==fields) { free(copy); return false; } f[n++]=p+1; }
            visit(ctx,f,n);
        }
        line=end+1;
    }
    free(copy); return true;
}
typedef struct { lp_highway *h; bool bad; } KeyReading;
static void key_line(void *ctx, char **f, int n){
    KeyReading *r=ctx; if (r->bad) return;
    if (n!=3 || !*f[1] || strlen(f[1])>1000) { r->bad=true; return; }
    uint32_t li=0; while (li<r->h->nlists && strcmp(r->h->list[li].name,f[0])) li++;
    uint64_t slot;
    if (li==r->h->nlists || !number(f[2],UINT32_MAX,&slot) || slot>=r->h->list[li].count) { r->bad=true; return; }
    char key[1004]; size_t len=strlen(f[1]); memcpy(key,&li,4); memcpy(key+4,f[1],len);
    bool fresh; uint32_t *value=lp_strmap_get(r->h->keys,key,4+len,&fresh);
    if (!fresh && *value!=slot) r->bad=true;
    *value=(uint32_t)slot;
}
static bool identifiers_read(Highway *mapped,const uint8_t *bytes,size_t size,size_t count){
    const uint8_t *at=bytes,*end=bytes+size; size_t nparts=0,label_bytes=0;
    for (size_t i=0; i<count; i++) {
        if ((size_t)(end-at)<4) return false;
        uint32_t n=read32(at); at+=4;
        if ((!n)!=(mapped->public.rec[i].pad==LP_HIGHWAY_RETIRED) || n>(size_t)(end-at)/2
            || !bytes_add(&nparts,n,1) || !bytes_add(&label_bytes,1,3)) return false;
        for (uint32_t j=0; j<n; j++) {
            const uint8_t *zero=memchr(at,0,(size_t)(end-at));
            if (!zero || zero==at || !bytes_add(&label_bytes,(size_t)(zero-at),1)
                || !bytes_add(&label_bytes,1,2)) return false;
            at=zero+1;
        }
    }
    if (at!=end || nparts>SIZE_MAX/sizeof *mapped->parts) return false;
    mapped->identifier=calloc(count,sizeof *mapped->identifier);
    mapped->parts=calloc(nparts ? nparts : 1,sizeof *mapped->parts);
    mapped->labels=malloc(label_bytes ? label_bytes : 1);
    if (!mapped->identifier || !mapped->parts || !mapped->labels) return false;
    at=bytes; const char **part=mapped->parts; char *label=mapped->labels;
    for (size_t i=0; i<count; i++) {
        Identifier *id=mapped->identifier+i; id->count=read32(at); at+=4; id->part=part;
        for (uint32_t j=0; j<id->count; j++) { *part++=(const char *)at; at+=strlen((const char *)at)+1; }
        if (id->count==1) id->display=id->part[0];
        else if (id->count) {
            id->display=label; *label++='[';
            for (uint32_t j=0; j<id->count; j++) {
                if (j) { *label++=','; *label++=' '; }
                size_t n=strlen(id->part[j]); memcpy(label,id->part[j],n); label+=n;
            }
            *label++=']'; *label++=0;
        }
    }
    return true;
}
static const void *highway_open(const char *path){
    size_t size; const uint8_t *m=lp_map_file(path,0,&size); if (!m) return NULL;
    Highway *mapped=NULL; bool ok=size>=HW_HEADER && !memcmp(m,"LPHW0002",8) && !read64(m+8);
    uint64_t nr=0,ne=0,nl=0,nk=0,ni=0; size_t expected=HW_HEADER; uint8_t fp[32];
    if (ok) {
        nr=read64(m+16); ne=read64(m+24); nl=read64(m+32); nk=read64(m+40); ni=read64(m+48);
        ok=nr && nr<=UINT32_MAX && ne<=UINT32_MAX && nl && ni && nl<=SIZE_MAX && nk<=SIZE_MAX && ni<=SIZE_MAX
            && bytes_add(&expected,(size_t)nr,sizeof(lp_tier0_record)) && bytes_add(&expected,(size_t)ne,sizeof(lp_edge))
            && bytes_add(&expected,(size_t)nl,1) && bytes_add(&expected,(size_t)nk,1) && bytes_add(&expected,(size_t)ni,1)
            && expected==size && read64(m+56)==size;
    }
    if (ok) { digest(m,m+HW_HEADER,size-HW_HEADER,fp); ok=!memcmp(fp,m+64,32); }
    if (ok) { mapped=calloc(1,sizeof *mapped); ok=mapped!=NULL; }
    lp_highway *h=mapped ? &mapped->public : NULL;
    if (ok) {
        mapped->mapping=m; mapped->size=size; memcpy(mapped->fingerprint,fp,32);
        h->rec=(const lp_tier0_record *)(m+HW_HEADER); h->nrec=(size_t)nr;
        h->edge=(const lp_edge *)(h->rec+nr); h->nedges=(size_t)ne;
        const char *layout=(const char *)(h->edge+ne);
        Reading r={.h=h}; ok=lines(layout,(size_t)nl,8,layout_line,&r) && !r.bad && r.nrec==nr && r.nedges==ne;
        size_t covered=0;
        for (size_t i=0; ok && i<h->nlists; i++) {
            const lp_list *list=h->list+i;
            ok=list->count && list->first==covered && list->count<=nr-covered;
            for (size_t j=0; ok && j<i; j++) ok=strcmp(list->name,h->list[j].name)!=0;
            for (uint32_t j=0; ok && j<list->count; j++) ok=h->rec[covered+j].rank==j;
            covered+=list->count;
        }
        ok=ok && covered==nr; covered=0;
        if (ok) {
            mapped->lists=lp_strmap_lasting(sizeof(uint32_t));
            mapped->relations=lp_strmap_lasting(sizeof(uint32_t));
            mapped->row=calloc(h->nedgelists+1,sizeof *mapped->row); ok=mapped->row!=NULL;
            for (size_t i=0; ok && i<h->nlists; i++) {
                const lp_list *list=h->list+i; bool fresh;
                uint32_t *at=lp_strmap_get(mapped->lists,list->name,strlen(list->name),&fresh);
                if (!fresh && *at!=i) { ok=false; break; } *at=(uint32_t)i;
                at=lp_strmap_get(mapped->lists,list->say,strlen(list->say),&fresh);
                if (!fresh && *at!=i) { ok=false; break; } *at=(uint32_t)i;
            }
        }
        for (size_t i=0; ok && i<h->nedgelists; i++) {
            const lp_edges *e=h->edges+i; const lp_list *a=lp_highway_list(h,e->a), *b=lp_highway_list(h,e->b);
            ok=a && b && e->count && e->first==covered && e->count<=ne-covered;
            for (size_t j=0; ok && j<i; j++) ok=strcmp(e->a,h->edges[j].a) || strcmp(e->b,h->edges[j].b);
            for (size_t j=0; ok && j<e->count; j++) {
                const lp_edge *edge=h->edge+covered+j;
                ok=edge->from<a->count && edge->to<b->count
                    && (!j || edge[-1].from<edge->from || (edge[-1].from==edge->from && edge[-1].to<edge->to));
            }
            if (ok) {
                mapped->row[i]=calloc((size_t)a->count+1,sizeof(uint32_t)); ok=mapped->row[i]!=NULL;
                if (ok) {
                    for (size_t j=0; j<e->count; j++) mapped->row[i][h->edge[covered+j].from+1]++;
                    for (size_t j=1; j<=a->count; j++) mapped->row[i][j]+=mapped->row[i][j-1];
                    char key[65]; size_t na=strlen(e->a),nb=strlen(e->b); memcpy(key,e->a,na); key[na]='\t'; memcpy(key+na+1,e->b,nb);
                    bool fresh; uint32_t *at=lp_strmap_get(mapped->relations,key,na+1+nb,&fresh);
                    if (!fresh) ok=false; *at=(uint32_t)i;
                }
            }
            covered+=e->count;
        }
        ok=ok && covered==ne;
        if (ok) {
            h->keys=lp_strmap_lasting(sizeof(uint32_t)); KeyReading r={h,false};
            ok=lines(layout+nl,(size_t)nk,3,key_line,&r) && !r.bad;
        }
        if (ok) ok=identifiers_read(mapped,(const uint8_t *)(layout+nl+nk),(size_t)ni,(size_t)nr);
        if (ok) { h->by_id=lp_idindex_build(h->rec,h->nrec,sizeof *h->rec); ok=h->by_id!=NULL; }
    }
    if (!ok) {
        if (h) {
            if (mapped->row) for (size_t i=0; i<h->nedgelists; i++) free(mapped->row[i]);
            free(mapped->row); lp_strmap_free(mapped->lists); lp_strmap_free(mapped->relations);
            free(h->list); free(h->bank); free(h->edges); lp_strmap_free(h->keys); lp_idindex_free(h->by_id);
            free(mapped->identifier); free(mapped->parts); free(mapped->labels); free(mapped);
        }
        lp_unmap_file(m,size); return NULL;
    }
    snprintf(h->path,sizeof h->path,"%s",path); return h;
}
const char *lp_highway_identifier(const lp_highway *h,const lp_list *list,uint32_t slot){
    if (!h || !list || list<h->list || list>=h->list+h->nlists || slot>=list->count) return NULL;
    return ((const Highway *)h)->identifier[(size_t)list->first+slot].display;
}
size_t lp_highway_identifier_parts(const lp_highway *h,const lp_list *list,uint32_t slot,const char *const **parts){
    if (parts) *parts=NULL;
    if (!h || !list || list<h->list || list>=h->list+h->nlists || slot>=list->count) return 0;
    const Identifier *id=((const Highway *)h)->identifier+(size_t)list->first+slot;
    if (parts && id->count) *parts=id->part;
    return id->count;
}
const lp_highway *lp_highway_map(const char *path){ return lp_cached("highway", path && *path ? path : lp_highway_path(), highway_open); }

bool lp_highway_contains(const lp_highway *h, const lp_id *id){
    if (!h || !id) return false;
    uint64_t probe = 0;
    for (int64_t at; (at=lp_idindex_find(h->by_id,id,&probe))>=0; )
        if (h->rec[at].pad!=LP_HIGHWAY_RETIRED) return true;
    return false;
}

const lp_list *lp_highway_list(const lp_highway *h, const char *name){
    if (!h || !name) return NULL;
    const uint32_t *at=lp_strmap_lookup(((const Highway *)h)->lists,name,strlen(name));
    return at ? h->list+*at : NULL;
}
const lp_tier0_record *lp_highway_at(const lp_highway *h, const lp_list *l, uint32_t slot){
    if (!l || slot >= l->count || (size_t)l->first + slot >= h->nrec) return NULL;
    const lp_tier0_record *record=&h->rec[l->first+slot];
    return record->pad==LP_HIGHWAY_RETIRED ? NULL : record;
}

/* Published mappings already own their immutable ID index. Queries never
 * allocate or take an initialization lock while record workers are running. */
static const lp_idindex *by_id(const lp_highway *h){ return h->by_id; }

/* The record's place among all the highway's records, in list l (any list when l is NULL), or -1. */
static int64_t record_of(const lp_highway *h, const lp_list *l, const lp_id *id){
    const lp_idindex *x = by_id(h); uint64_t probe = 0;
    for (int64_t i; (i = lp_idindex_find(x, id, &probe)) >= 0; )
        if (h->rec[i].pad!=LP_HIGHWAY_RETIRED && (!l || ((size_t)i >= l->first && (size_t)i < (size_t)l->first + l->count))) return i;
    return -1;
}
int64_t lp_highway_slot(const lp_highway *h, const lp_list *l, const lp_id *id){
    int64_t i = record_of(h, l, id); return i < 0 ? -1 : i - (l ? (int64_t)l->first : 0);
}
size_t lp_highway_edges(const lp_highway *h, const char *a, uint32_t slot, const char *b, const lp_edge **out){
    if (!out) return 0; *out=NULL;
    if (!h || !a || !b) return 0;
    size_t na=strlen(a),nb=strlen(b); if (na>=32 || nb>=32) return 0;
    char key[65]; memcpy(key,a,na); key[na]='\t'; memcpy(key+na+1,b,nb);
    const Highway *mapped=(const Highway *)h;
    const uint32_t *at=lp_strmap_lookup(mapped->relations,key,na+1+nb);
    if (!at) return 0;
    const lp_list *list=lp_highway_list(h,a); if (!list || slot>=list->count) return 0;
    const uint32_t *row=mapped->row[*at];
    *out=h->edge+h->edges[*at].first+row[slot]; return row[slot+1]-row[slot];
}
void lp_highway_fingerprint(const lp_highway *h, uint8_t out[32]){
    memcpy(out, ((const Highway *)h)->fingerprint, 32);
}

const lp_bank *lp_highway_bank(const lp_highway *h, const char *name){
    if (!h || !name) return NULL;
    for (size_t i = 0; i < h->nbanks; i++) if (!strcmp(h->bank[i].name, name)) return &h->bank[i];
    return NULL;
}
const lp_bank *lp_highway_bank_of(const lp_highway *h, const lp_id *id, int32_t *bit){
    if (bit) *bit = -1;
    if (!h || !id) return NULL;
    const lp_idindex *index = by_id(h); uint64_t probe = 0;
    for (int64_t i; (i = lp_idindex_find(index, id, &probe)) >= 0; ) {
        for (size_t k = 0; k < h->nbanks; k++) { const lp_list *l = h->bank[k].list; if (!l) continue;
            if ((size_t)i >= l->first && (size_t)i < (size_t)l->first + l->count) { uint32_t slot = (uint32_t)i - l->first;
                const lp_bank *bank = h->bank + k;
                if (bank->flags ? slot >= bank->width : (uint64_t)slot + 1 > (lp_mask_field(bank) >> bank->offset)) continue;
                if (bit) *bit = (int32_t)slot; return bank; } }
    }
    return NULL;
}

/* The immutable textual-key index is built and validated before publication. */
int64_t lp_highway_key(const lp_highway *h,const lp_list *l,const char *key){
    if (!h || !l || !key || l<h->list || l>=h->list+h->nlists) return -1;
    size_t kl=strlen(key); char k[1004]; if (!kl || kl>1000) return -1;
    uint32_t li=(uint32_t)(l-h->list); memcpy(k,&li,4); memcpy(k+4,key,kl);
    int64_t i=lp_strmap_find(h->keys,k,4+kl);
    return i<0 ? -1 : (int64_t)*(uint32_t *)lp_strmap_at(h->keys,(size_t)i);
}
