/* IDs in geometry: packing round-trips for every codepoint ID, values stay in [0.25, 0.5), and EWKB paths have the
 * prototype's layout (POINT ZM for one run, LINESTRING ZM otherwise, run length in M). */
#include "laplace/laplace.h"
#include "check.h"
#include <string.h>
#include <math.h>

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
    /* a record's claim vertices: how each was said (outcome, position) above what it is said to be, its score in the
     * spare bits when it is none of win, draw and loss; a vertex written before any of it reads as a win, at no place */
    { lp_id ids[4]; for (int i = 0; i < 4; i++) lp_id_codepoint('p' + (uint32_t)i, &ids[i]);
      uint32_t sp[4] = { 0 }, oc[4]; double score[4] = { 1.0, 0.5, 0.0, 0.7311234 };
      for (int i = 0; i < 4; i++) oc[i] = lp_outcome_of(score[i], &sp[i]);
      CHECK(oc[0] == LP_OUTCOME_WIN && oc[1] == LP_OUTCOME_DRAW && oc[2] == LP_OUTCOME_LOSS && oc[3] == LP_OUTCOME_SCORE && !sp[0] && !sp[1] && !sp[2] && lp_spare_tag(sp[3]) == LP_SPARE_SCORE,
            "win, draw and loss are outcomes alone; any other score is carried in the spare bits");
      uint64_t ms[4] = { lp_m_full(3, LP_SAID_CLAIM, oc[0], 1), lp_m_full(1, LP_SAID_CLAIM, oc[1], 2), lp_m_full(2, LP_SAID_TUPLE, oc[2], 300000), lp_m_full(1, LP_SAID_CLAIM, oc[3], 0) };
      uint8_t pb[256]; size_t pl = lp_ewkb_runs_spare(ids, ms, sp, 4, pb, sizeof pb); lp_vertex pv[4];
      CHECK(lp_path_vertices(pb, pl, pv, 4) == 4, "four vertices");
      CHECK(pv[0].run == 3 && pv[0].said == LP_SAID_CLAIM && pv[0].outcome == LP_OUTCOME_WIN && pv[0].position == 1, "run, said, outcome and position read back");
      CHECK(pv[1].outcome == LP_OUTCOME_DRAW && pv[1].position == 2 && pv[2].said == LP_SAID_TUPLE && pv[2].outcome == LP_OUTCOME_LOSS && pv[2].position == LP_M_POSITION_MAX && pv[2].run == 2,
            "a position past 18 bits is the most they hold; said and run are untouched by what is above them");
      CHECK(!memcmp(&pv[3].id, &ids[3], 16) && lp_outcome_score(pv[3].outcome, pv[3].spare) == lp_score_carried(score[3]) && fabs(lp_score_carried(score[3]) - score[3]) <= 1.0 / (2.0 * LP_SCORE_ONE),
            "a score reads back to within half of 2^-24, and its vertex is still its ID");
      int exact = 1; for (uint32_t k = 1u << 23; k < LP_SCORE_ONE; k += 977) { float f = (float)k / (float)LP_SCORE_ONE; if (lp_score_carried(f) != (double)f) exact = 0; }
      CHECK(exact, "every float score at or above one half is carried exactly");
      uint8_t old[64]; lp_id one[1] = { ids[0] }; uint64_t m1[1] = { (uint64_t)lp_m_of(5, LP_SAID_CLAIM) }; size_t ol = lp_ewkb_runs(one, m1, 1, old, sizeof old); lp_vertex ov;
      CHECK(lp_path_vertices(old, ol, &ov, 1) == 1 && ov.run == 5 && ov.outcome == LP_OUTCOME_WIN && ov.position == 0 && lp_outcome_score(ov.outcome, ov.spare) == 1.0, "a vertex written without them is a win at no place"); }
    DONE("geometry");
}
