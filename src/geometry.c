/* IDs written into geometry, and EWKB physicality paths. */
#include "laplace/laplace.h"
#include "internal.h"
#include <string.h>

#define EWKB_Z     0x80000000u
#define EWKB_M     0x40000000u
#define EWKB_SRID  0x20000000u
#define EXP_BITS   (1021ull << 52)                      /* exponent -2: [0.25, 0.5) */
#define MASK43     ((1ull << 43) - 1)
#define MASK42     ((1ull << 42) - 1)
/* The 128-bit ID is 43 + 43 + 42 bits of the X, Y and Z mantissas; the 9 + 9 + 10 bits above them are the vertex's
 * spare bits (Identity: values from known lists, a small tag saying which layout). An ID is read through its own bits
 * only, so a value in the spare bits never changes which entity a vertex is. */
const uint64_t lp_xyz_id_mask[3] = { EXP_BITS | MASK43, EXP_BITS | MASK43, EXP_BITS | MASK42 };

void lp_id_to_xyz(const lp_id *id, double xyz[3]){
    unsigned __int128 v = 0;
    for (int i = 15; i >= 0; i--) v = (v << 8) | id->b[i];
    uint64_t part[3] = { (uint64_t)(v & MASK43), (uint64_t)((v >> 43) & MASK43), (uint64_t)(v >> 86) };
    for (int k = 0; k < 3; k++) { uint64_t bits = EXP_BITS | part[k]; memcpy(&xyz[k], &bits, 8); }
}

void lp_xyz_to_id(const double xyz[3], lp_id *out){
    uint64_t part[3];
    static const uint64_t own[3] = { MASK43, MASK43, MASK42 };
    for (int k = 0; k < 3; k++) { uint64_t bits; memcpy(&bits, &xyz[k], 8); part[k] = bits & own[k]; }
    unsigned __int128 v = (unsigned __int128)part[0] | ((unsigned __int128)part[1] << 43) | ((unsigned __int128)part[2] << 86);
    for (int i = 0; i < 16; i++) { out->b[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

/* The 28 spare bits: bits 0-8 above X's 43, 9-17 above Y's 43, 18-27 above Z's 42. */
uint32_t lp_xyz_spare(const double xyz[3]){
    uint64_t b[3]; memcpy(b, xyz, 24);
    return (uint32_t)(((b[0] >> 43) & 0x1FF) | (((b[1] >> 43) & 0x1FF) << 9) | (((b[2] >> 42) & 0x3FF) << 18));
}
void lp_xyz_spare_set(double xyz[3], uint32_t v){
    uint64_t b[3]; memcpy(b, xyz, 24);
    b[0] = (b[0] & ~(0x1FFull << 43)) | ((uint64_t)(v & 0x1FF) << 43);
    b[1] = (b[1] & ~(0x1FFull << 43)) | ((uint64_t)((v >> 9) & 0x1FF) << 43);
    b[2] = (b[2] & ~(0x3FFull << 42)) | ((uint64_t)((v >> 18) & 0x3FF) << 42);
    memcpy(xyz, b, 24);
}
static uint8_t *put32(uint8_t *p, uint32_t v){ memcpy(p, &v, 4); return p + 4; }
static uint8_t *putd(uint8_t *p, double v){ memcpy(p, &v, 8); return p + 8; }

size_t lp_ewkb_path(const lp_id *ch, size_t n, uint8_t *out, size_t cap){
    size_t runs = 0;
    for (size_t i = 0; i < n; i++) if (i == 0 || memcmp(&ch[i], &ch[i - 1], 16)) runs++;
    size_t need = runs == 1 ? 5 + 32 : 9 + 32 * runs;
    if (!out || cap < need || n == 0) return n == 0 ? 0 : need;
    uint8_t *p = out; *p++ = 1;                                                 /* little-endian */
    p = put32(p, (runs == 1 ? 1u : 2u) | EWKB_Z | EWKB_M);
    if (runs != 1) p = put32(p, (uint32_t)runs);
    for (size_t i = 0; i < n; ) {
        size_t j = i + 1; while (j < n && !memcmp(&ch[j], &ch[i], 16)) j++;
        double xyz[3]; lp_id_to_xyz(&ch[i], xyz);
        p = putd(p, xyz[0]); p = putd(p, xyz[1]); p = putd(p, xyz[2]); p = putd(p, (double)(j - i));
        i = j;
    }
    return need;
}

size_t lp_ewkb_vertices(const uint8_t *e, size_t len, const uint8_t **vertices){
    if (len < 5 || e[0] != 1) return 0;
    uint32_t type; memcpy(&type, e + 1, 4); size_t off = 5, n = 1;
    if (!(type & EWKB_Z) || !(type & EWKB_M)) return 0;
    if (type & EWKB_SRID) off += 4;
    if ((type & 0xFF) == 2) { uint32_t k; if (len < off + 4) return 0; memcpy(&k, e + off, 4); n = k; off += 4; }
    else if ((type & 0xFF) != 1) return 0;
    if (len < off + n * LP_VERTEX_BYTES) return 0;
    *vertices = e + off; return n;
}

size_t lp_ewkb_runs(const lp_id *ids, const uint64_t *m, size_t nv, uint8_t *out, size_t cap){ return lp_ewkb_runs_spare(ids, m, NULL, nv, out, cap); }
size_t lp_ewkb_runs_spare(const lp_id *ids, const uint64_t *m, const uint32_t *spare, size_t nv, uint8_t *out, size_t cap){
    size_t need = nv == 1 ? 5 + 32 : 9 + 32 * nv;
    if (!out || cap < need || nv == 0) return nv == 0 ? 0 : need;
    uint8_t *p = out; *p++ = 1;
    p = put32(p, (nv == 1 ? 1u : 2u) | EWKB_Z | EWKB_M);
    if (nv != 1) p = put32(p, (uint32_t)nv);
    for (size_t i = 0; i < nv; i++) {
        double xyz[3]; lp_id_to_xyz(&ids[i], xyz); if (spare && spare[i]) lp_xyz_spare_set(xyz, spare[i]);
        p = putd(p, xyz[0]); p = putd(p, xyz[1]); p = putd(p, xyz[2]); p = putd(p, (double)m[i]);
    }
    return need;
}

size_t lp_ewkb_point4(const double c[4], uint8_t *out, size_t cap){
    if (!out || cap < 37) return 37;
    uint8_t *p = out; *p++ = 1; p = put32(p, 1u | EWKB_Z | EWKB_M);
    for (int d = 0; d < 4; d++) p = putd(p, c[d]);
    return 37;
}
