/* Continuations: every run of the phrase inside a path, and the vertex after it. The phrase is encoded once into
 * vertex bytes, a SIMD scan finds candidate starts by the phrase's first vertex, and the rest of the window is
 * compared byte for byte; only continuations are decoded. */
#include "laplace/laplace.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>

size_t lp_scan_scalar(const uint8_t *v, size_t s, size_t n, const uint8_t key[24]){
    for (; s < n; s++) if (!memcmp(v + s * LP_VERTEX_BYTES, key, 24)) return s;
    return n;
}

typedef size_t (*scan_fn)(const uint8_t *, size_t, size_t, const uint8_t[24]);
static scan_fn pick_scan(void){
    uint32_t f = lp_cpu_active();
#if defined(LP_HAVE_AVX512)
    if (f & LP_CPU_AVX512) return lp_scan_avx512;
#endif
#if defined(LP_HAVE_AVX2)
    if (f & LP_CPU_AVX2) return lp_scan_avx2;
#endif
    (void)f; return lp_scan_scalar;
}

static double run_of(const uint8_t *v, size_t i){ double m; memcpy(&m, v + i * LP_VERTEX_BYTES + 24, 8); return m < 1 ? 1 : m; }

size_t lp_follows(const uint8_t *ewkb, size_t len, const lp_id *phrase, size_t np, lp_id *out, size_t cap){
    const uint8_t *v; size_t nv = lp_ewkb_vertices(ewkb, len, &v);
    if (!nv || !np) return 0;
    static scan_fn scan; if (!scan) scan = pick_scan();
    uint8_t *key = malloc(24 * np);
    for (size_t j = 0; j < np; j++) { double xyz[3]; lp_id_to_xyz(&phrase[j], xyz); memcpy(key + 24 * j, xyz, 24); }
    size_t found = 0;
    /* A phrase is matched against the path expanded by run length. Vertex i with run r stands for r copies. */
    for (size_t i = scan(v, 0, nv, key); i < nv; i = scan(v, i + 1, nv, key)) {
        size_t r0 = (size_t)run_of(v, i);
        for (size_t off = 0; off < r0; off++) {                              /* the phrase may start at any copy */
            size_t vi = i, left = r0 - off, j = 0; bool ok = true;
            while (j < np) {
                if (memcmp(v + vi * LP_VERTEX_BYTES, key + 24 * j, 24)) { ok = false; break; }
                j++; left--;
                if (left == 0) { vi++; if (vi >= nv) break; left = (size_t)run_of(v, vi); }
            }
            if (!ok || j < np || vi >= nv) continue;                           /* no vertex follows the phrase */
            if (found < cap) { double xyz[3]; memcpy(xyz, v + vi * LP_VERTEX_BYTES, 24); lp_xyz_to_id(xyz, &out[found]); }
            found++;
        }
    }
    free(key);
    return found;
}
