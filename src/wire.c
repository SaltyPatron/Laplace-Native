/* PostgreSQL's binary forms, as bytes: what a client sends the database and reads back. An array is a header (dimensions,
 * whether it holds NULLs, the element type, its length and lower bound) and each element as its length and bytes; a
 * COPY stream is a signature, a header, rows of fields each as its length and bytes, and a trailer. Every value is
 * big-endian. Nothing here speaks to a server: the bytes go wherever the caller sends them. */
#include "laplace/laplace.h"
#include <string.h>

void lp_pg_array(lp_buf *b, uint32_t elem_oid, size_t n){
    lp_buf_be(b, 1, 4); lp_buf_be(b, 0, 4); lp_buf_be(b, elem_oid, 4); lp_buf_be(b, (uint32_t)n, 4); lp_buf_be(b, 1, 4);
}
void lp_pg_ids(lp_buf *b, uint32_t elem_oid, const lp_id *ids, size_t n){
    lp_pg_array(b, elem_oid, n);
    uint8_t *p = lp_buf_room(b, 20 * n);
    for (size_t i = 0; i < n; i++, p += 20) { lp_put_be(p, 16, 4); memcpy(p + 4, ids[i].b, 16); }
}
size_t lp_pg_ids_read(const uint8_t *a, size_t len, lp_id *out, size_t cap){
    if (len < 12 || lp_be(a, 4) == 0) return 0;                       /* no dimensions: the empty array */
    if (len < 20 || lp_be(a, 4) != 1) return 0;
    size_t n = (size_t)lp_be(a + 12, 4), at = 20, k = 0;
    for (size_t i = 0; i < n && at + 4 <= len; i++) {
        uint32_t l = (uint32_t)lp_be(a + at, 4); at += 4;
        if (l == 0xFFFFFFFFu) continue;                                   /* a NULL: no ID */
        if (l != 16 || at + 16 > len) return k;
        if (k < cap) memcpy(out[k].b, a + at, 16);
        k++; at += 16;
    }
    return k;
}

static const uint8_t COPY_HEADER[19] = { 'P','G','C','O','P','Y','\n',0xFF,'\r','\n',0, 0,0,0,0, 0,0,0,0 };
static void copy_flush(lp_copy *c){
    if (!c->buf.n) return;
    if (!c->failed && !c->flush(c->ctx, c->buf.b, c->buf.n)) c->failed = true;
    c->bytes += c->buf.n; c->buf.n = 0;
}
void lp_copy_start(lp_copy *c, size_t flush_at, lp_copy_flush_fn flush, void *ctx){
    c->flush_at = flush_at ? flush_at : (size_t)1 << 22; c->flush = flush; c->ctx = ctx; c->rows = c->bytes = 0; c->failed = false; c->buf.n = 0;
    lp_buf_put(&c->buf, COPY_HEADER, sizeof COPY_HEADER);
}
void lp_copy_row(lp_copy *c, uint16_t fields){
    if (c->buf.n >= c->flush_at) copy_flush(c);
    lp_buf_be(&c->buf, fields, 2); c->rows++;
}
void lp_copy_field(lp_copy *c, const void *p, uint32_t len){ lp_buf_be(&c->buf, len, 4); lp_buf_put(&c->buf, p, len); }
bool lp_copy_end(lp_copy *c){
    lp_buf_be(&c->buf, 0xFFFF, 2); copy_flush(c);
    return !c->failed;
}
