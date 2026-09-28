/* IDs in geometry: packing round-trips for every codepoint ID, values stay in [0.25, 0.5), and EWKB paths have the
 * prototype's layout (POINT ZM for one run, LINESTRING ZM otherwise, run length in M). */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>

int main(void){
    size_t bad = 0, out = 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        lp_id id, back; double xyz[3]; lp_id_codepoint(cp, &id);
        lp_id_to_xyz(&id, xyz); lp_xyz_to_id(xyz, &back);
        bad += memcmp(&id, &back, 16) != 0;
        for (int k = 0; k < 3; k++) out += !(xyz[k] >= 0.25 && xyz[k] < 0.5);
    }
    CHECK(bad == 0, "%zu IDs do not round-trip", bad);
    CHECK(out == 0, "%zu packed values outside [0.25, 0.5)", out);

    lp_id a, b; lp_id_codepoint('a', &a); lp_id_codepoint('b', &b);
    lp_id seq[5] = { a, a, a, b, a };
    uint8_t buf[256]; size_t n = lp_ewkb_path(seq, 5, buf, sizeof buf);
    CHECK(n == 9 + 32 * 3, "path of three runs is %zu bytes", n);
    const uint8_t *v; size_t nv = lp_ewkb_vertices(buf, n, &v);
    CHECK(nv == 3, "three vertices, got %zu", nv);
    double m[3]; for (int i = 0; i < 3; i++) memcpy(&m[i], v + 32 * i + 24, 8);
    CHECK(m[0] == 3 && m[1] == 1 && m[2] == 1, "run lengths %g %g %g", m[0], m[1], m[2]);
    lp_id rep[4] = { a, a, a, a };
    n = lp_ewkb_path(rep, 4, buf, sizeof buf); nv = lp_ewkb_vertices(buf, n, &v);
    CHECK(n == 5 + 32 && nv == 1, "a pure repeat is one POINT ZM");
    memcpy(m, v + 24, 8); CHECK(m[0] == 4, "repeat run length %g", m[0]);
    uint32_t type; memcpy(&type, buf + 1, 4); CHECK(type == 0xC0000001u, "POINT ZM type %08x", type);
    DONE("geometry");
}
