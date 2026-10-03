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
    /* the spare bits: a value beside the ID, never in it */
    uint64_t r = 0x9E3779B97F4A7C15ull; int spare_ok = 1, scan_ok = 1;
    for (int t = 0; t < 100000; t++) {
        lp_id id; for (int i = 0; i < 16; i++) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; id.b[i] = (uint8_t)r; }
        uint32_t v = (uint32_t)(r >> 11) & ((1u << LP_SPARE_BITS) - 1);
        double xyz[3], plain[3]; lp_id_to_xyz(&id, xyz); memcpy(plain, xyz, 24); lp_xyz_spare_set(xyz, v);
        lp_id back; lp_xyz_to_id(xyz, &back);
        if (memcmp(&back, &id, 16) || lp_xyz_spare(xyz) != v || lp_xyz_spare(plain) != 0) spare_ok = 0;
        for (int k = 0; k < 3; k++) { double c = xyz[k]; if (!(c >= 0.25 && c < 0.5)) scan_ok = 0; }
    }
    CHECK(spare_ok, "an ID with any value in its spare bits reads back as itself, and the value as itself");
    CHECK(scan_ok, "with a value in the spare bits, every coordinate is still in [0.25, 0.5)");
    { lp_id ids[3]; lp_id_codepoint('x', &ids[0]); lp_id_codepoint('y', &ids[1]); lp_id_codepoint('z', &ids[2]);
      uint64_t ms[3] = { 1, 2, 1 }; uint32_t sp[3] = { lp_spare_of(1, 7), 0, lp_spare_of(2, 0xABCDE) };
      uint8_t pb[256]; size_t pl = lp_ewkb_runs_spare(ids, ms, sp, 3, pb, sizeof pb); lp_vertex pv[3];
      CHECK(lp_path_vertices(pb, pl, pv, 3) == 3 && !memcmp(&pv[0].id, &ids[0], 16) && !memcmp(&pv[2].id, &ids[2], 16) && pv[1].run == 2, "a path with values reads back its IDs and runs");
      CHECK(pv[0].spare == sp[0] && pv[1].spare == 0 && pv[2].spare == sp[2] && lp_spare_tag(pv[2].spare) == 2 && lp_spare_payload(pv[2].spare) == 0xABCDE, "and each vertex's value, tag and payload"); }
    DONE("geometry");
}
