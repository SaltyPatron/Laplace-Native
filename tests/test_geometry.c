/* IDs in geometry: packing round-trips for every codepoint ID, values stay in [0.25, 0.5), and EWKB paths have the
 * prototype's layout (POINT ZM for one run, LINESTRING ZM otherwise, run length in M). */
#include "laplace/geometry.h"
#include "laplace/tier0.h"
#include "check.h"
#include <string.h>
#include <math.h>

typedef struct { double xyzm[32][4]; lp_ewkb_ring rings[8]; size_t vertices, parts; } Decoded;
static bool decode_part(void *context, const lp_ewkb_span *span){
    Decoded *d = context;
    if (d->parts == 8 || span->count > 32 - d->vertices) return false;
    d->rings[d->parts++] = (lp_ewkb_ring){ d->xyzm[d->vertices], span->count };
    for (size_t i = 0; i < span->count; i++) if (!lp_ewkb_vertex(span, i, d->xyzm[d->vertices + i])) return false;
    d->vertices += span->count; return true;
}

static size_t big_line(const double *xyzm, uint32_t count, uint32_t type, uint8_t *out){
    out[0] = 0; lp_put_be(out + 1, type, 4); lp_put_be(out + 5, count, 4);
    for (size_t i = 0; i < (size_t)count * 4; i++) {
        uint64_t bits; memcpy(&bits, xyzm + i, 8); lp_put_be(out + 9 + i * 8, bits, 8);
    }
    return 9 + (size_t)count * 32;
}

static void nested_geometry(void){
    lp_id ids[6]; double points[6][4];
    for (int i = 0; i < 6; i++) { lp_id_codepoint('a' + (uint32_t)i, ids + i); lp_id_to_xyz(ids + i, points[i]); points[i][3] = 1; }
    double outer[5][4], hole[4][4], second[4][4];
    const int a[] = { 0, 1, 2, 3, 0 }, b[] = { 1, 2, 3, 1 }, c[] = { 4, 5, 2, 4 };
    for (int i = 0; i < 5; i++) memcpy(outer[i], points[a[i]], 32);
    for (int i = 0; i < 4; i++) { memcpy(hole[i], points[b[i]], 32); memcpy(second[i], points[c[i]], 32); }
    lp_xyz_spare_set(outer[3], lp_spare_of(2, 17));
    uint32_t srid = 0; uint8_t polygon[1024], restored[1024], other[512], multi[2048];
    lp_ewkb_ring rings[] = { { outer[0], 5 }, { hole[0], 4 } }, other_ring = { second[0], 4 };
    size_t pn = lp_ewkb_polygon_zm(rings, 2, &srid, polygon, sizeof polygon);
    CHECK(pn == 309, "two rings and explicit SRID occupy 309 bytes (%zu)", pn);
    lp_ewkb_info info;
    CHECK(lp_ewkb_visit(polygon, pn, NULL, NULL, &info) && info.type == LP_POLYGON && info.has_srid
          && info.srid == 0 && info.vertices == 9 && info.parts == 2 && info.depth == 1, "polygon metadata and all rings");
    Decoded decoded = { 0 };
    CHECK(lp_ewkb_visit(polygon, pn, decode_part, &decoded, NULL) && decoded.parts == 2 && decoded.vertices == 9,
          "polygon reader preserves ring boundaries");
    size_t rn = lp_ewkb_polygon_zm(decoded.rings, decoded.parts, &srid, restored, sizeof restored);
    CHECK(rn == pn && !memcmp(restored, polygon, pn), "POLYGON ZM decode/write is byte-exact, including spare and M bits");
    lp_vertex vertices[20];
    CHECK(lp_path_vertices(polygon, pn, vertices, 20) == 9 && lp_id_eq(&vertices[3].id, &ids[3])
          && vertices[3].spare == lp_spare_of(2, 17) && lp_id_eq(&vertices[8].id, &ids[1]),
          "path reading reaches the fourth vertex and the hole");
    lp_id keys[20]; size_t count;
    CHECK(lp_ewkb_keys(polygon, pn, NULL, 0, &count) && count == 9, "key capacity counts every stored vertex");
    CHECK(lp_ewkb_keys(polygon, pn, keys, 20, &count) && count == 4
          && bsearch(ids + 3, keys, count, sizeof *keys, lp_id_cmp), "GIN keys include the fourth vertex and ignore spare bits");
    memset(keys, 0xa5, sizeof keys); lp_id untouched = keys[0];
    CHECK(!lp_ewkb_keys(polygon, pn, keys, 1, &count) && count == 9 && lp_id_eq(keys, &untouched),
          "a short key buffer is measured without partial writes");

    size_t on = lp_ewkb_polygon_zm(&other_ring, 1, NULL, other, sizeof other);
    lp_ewkb_slice members[] = { { polygon, pn }, { other, on } };
    size_t mn = lp_ewkb_collection_zm(LP_MULTIPOLYGON, members, 2, NULL, multi, sizeof multi);
    CHECK(mn && lp_ewkb_visit(multi, mn, NULL, NULL, &info) && info.type == LP_MULTIPOLYGON
          && info.vertices == 13 && info.parts == 3 && info.depth == 2, "MULTIPOLYGON ZM visits both polygons and their rings");
    Decoded first = { 0 }, next = { 0 };
    CHECK(lp_ewkb_visit(polygon, pn, decode_part, &first, NULL) && lp_ewkb_visit(other, on, decode_part, &next, NULL),
          "decode both polygon children");
    uint8_t p1[1024], p2[512], rebuilt[2048];
    size_t n1 = lp_ewkb_polygon_zm(first.rings, first.parts, &srid, p1, sizeof p1);
    size_t n2 = lp_ewkb_polygon_zm(next.rings, next.parts, NULL, p2, sizeof p2);
    lp_ewkb_slice rebuilt_members[] = { { p1, n1 }, { p2, n2 } };
    size_t rebuilt_n = lp_ewkb_collection_zm(LP_MULTIPOLYGON, rebuilt_members, 2, NULL, rebuilt, sizeof rebuilt);
    CHECK(rebuilt_n == mn && !memcmp(rebuilt, multi, mn), "MULTIPOLYGON ZM decode/write is byte-exact");
    CHECK(lp_ewkb_keys(multi, mn, keys, 20, &count) && count == 6
          && bsearch(ids + 5, keys, count, sizeof *keys, lp_id_cmp), "GIN includes IDs found only in the second polygon");
    CHECK(lp_path_ids(multi, mn, keys, 20) == 13 && lp_id_eq(keys + 10, ids + 5), "expanded IDs include every polygon");

    uint8_t point[64], line[256], collection[4096];
    size_t point_n = lp_ewkb_point_zm(points[0], &srid, point, sizeof point);
    size_t line_n = big_line(points[0], 3, 3002, line); /* big-endian ISO ZM, nested in EWKB */
    lp_ewkb_slice all[] = { { point, point_n }, { line, line_n }, { multi, mn } };
    size_t all_n = lp_ewkb_collection_zm(LP_GEOMETRYCOLLECTION, all, 3, &srid, collection, sizeof collection);
    CHECK(all_n && lp_ewkb_visit(collection, all_n, NULL, NULL, &info) && info.vertices == 17 && info.parts == 5
          && info.depth == 3 && info.has_srid, "nested mixed-byte-order collection has all 17 vertices");
    CHECK(lp_path_vertices(line, line_n, vertices, 20) == 3 && lp_id_eq(&vertices[2].id, ids + 2), "big-endian ZM vertex decoding");
    lp_id following[8];
    CHECK(lp_follows(line, line_n, ids, 1, following, 8) == 1 && lp_id_eq(following, ids + 1), "big-endian continuations");
    CHECK(lp_follows(polygon, pn, ids, 1, following, 8) == 1 && lp_id_eq(following, ids + 1),
          "continuations do not cross from a closed ring into a hole");
    uint8_t left[128], right[128], lines[512];
    size_t ln = lp_ewkb_line_zm(points[0], 2, NULL, left, sizeof left);
    size_t rr = lp_ewkb_line_zm(points[2], 2, NULL, right, sizeof right);
    lp_ewkb_slice pieces[] = { { left, ln }, { right, rr } };
    size_t lines_n = lp_ewkb_collection_zm(LP_MULTILINESTRING, pieces, 2, NULL, lines, sizeof lines);
    CHECK(lines_n && lp_follows(lines, lines_n, ids + 1, 1, following, 8) == 0, "continuations never join two line members");
    lp_ewkb_slice dots[] = { { point, point_n }, { point, point_n } };
    size_t dots_n = lp_ewkb_collection_zm(LP_MULTIPOINT, dots, 2, NULL, lines, sizeof lines);
    CHECK(dots_n && lp_ewkb_keys(lines, dots_n, keys, 20, &count) && count == 1, "MULTIPOINT indexes its distinct IDs");

    uint8_t empty[64]; size_t en = lp_ewkb_point_zm(NULL, NULL, empty, sizeof empty);
    CHECK(en == 37 && lp_ewkb_visit(empty, en, NULL, NULL, &info) && !info.vertices
          && lp_ewkb_keys(empty, en, keys, 20, &count) && !count, "POINT EMPTY contributes no phantom GIN key");
    en = lp_ewkb_line_zm(NULL, 0, NULL, empty, sizeof empty);
    CHECK(en == 9 && lp_ewkb_visit(empty, en, NULL, NULL, &info) && !info.vertices, "empty line");
    en = lp_ewkb_polygon_zm(NULL, 0, NULL, empty, sizeof empty);
    CHECK(en == 9 && lp_ewkb_visit(empty, en, NULL, NULL, &info) && !info.vertices, "empty polygon");
    for (uint32_t type = LP_MULTIPOINT; type <= LP_GEOMETRYCOLLECTION; type++) {
        en = lp_ewkb_collection_zm(type, NULL, 0, NULL, empty, sizeof empty);
        CHECK(en == 9 && lp_ewkb_visit(empty, en, NULL, NULL, &info) && !info.vertices, "empty multi/collection %u", type);
    }
    uint8_t guard[32]; memset(guard, 0xa5, sizeof guard);
    CHECK(lp_ewkb_polygon_zm(rings, 2, &srid, guard, sizeof guard) == pn && guard[0] == 0xa5, "short writer output stays untouched");
    CHECK(!lp_ewkb_collection_zm(LP_MULTIPOLYGON, dots, 2, NULL, guard, sizeof guard) && guard[0] == 0xa5,
          "a multipolygon writer refuses point members");
    size_t malformed = 0;
    for (size_t i = 0; i < mn; i++) malformed += lp_ewkb_visit(multi, i, NULL, NULL, NULL);
    CHECK(!malformed, "every truncated MULTIPOLYGON prefix is rejected");
    multi[mn] = 0; decoded = (Decoded){ 0 };
    CHECK(!lp_ewkb_visit(multi, mn + 1, decode_part, &decoded, NULL) && decoded.parts == 0,
          "validate the complete body before invoking a visitor");
    uint8_t invalid[128]; memcpy(invalid, point, point_n); invalid[4] &= 0x3f;
    CHECK(!lp_ewkb_visit(invalid, point_n, NULL, NULL, NULL), "reject geometry without both Z and M");
    memcpy(invalid, left, ln); memset(invalid + 5, 0xff, 4);
    CHECK(!lp_ewkb_visit(invalid, ln, NULL, NULL, NULL), "reject overflowing/truncated vertex counts");
    const uint8_t *block = (const uint8_t *)1;
    CHECK(!lp_ewkb_vertices(polygon, pn, &block) && !block, "a polygon is never exposed as one contiguous vertex block");

    uint8_t nested[2][1024]; size_t nested_n = point_n; memcpy(nested[0], point, point_n); unsigned current = 0;
    for (unsigned depth = 1; depth < LP_EWKB_MAX_DEPTH; depth++) {
        lp_ewkb_slice child = { nested[current], nested_n }; current ^= 1;
        nested_n = lp_ewkb_collection_zm(LP_GEOMETRYCOLLECTION, &child, 1, NULL, nested[current], sizeof nested[current]);
        CHECK(nested_n && lp_ewkb_visit(nested[current], nested_n, NULL, NULL, &info) && info.depth == depth + 1,
              "bounded collection depth %u", depth + 1);
    }
    lp_ewkb_slice child = { nested[current], nested_n };
    CHECK(!lp_ewkb_collection_zm(LP_GEOMETRYCOLLECTION, &child, 1, NULL, guard, sizeof guard), "writer never emits unreadable nesting");
    uint8_t *too_deep = nested[current ^ 1];
    memcpy(too_deep, (const uint8_t[]){ 1, 7, 0, 0, 0xc0, 1, 0, 0, 0 }, 9);
    memcpy(too_deep + 9, child.data, child.len);
    CHECK(!lp_ewkb_visit(too_deep, child.len + 9, NULL, NULL, NULL), "reader rejects a collection beyond its stack bound");
    uint64_t invalid_m = UINT64_MAX;
    CHECK(!lp_ewkb_runs(ids, &invalid_m, 1, guard, sizeof guard) && guard[0] == 0xa5,
          "M metadata must fit exactly into its binary64 carrier");
    CHECK(!lp_ewkb_path(NULL, 1, guard, sizeof guard), "missing child IDs do not produce a path");
    CHECK(lp_m_bits(NAN) == 1 && lp_m_bits(INFINITY) == 1, "nonfinite M never invokes an undefined integer conversion");
}

int main(void){
    size_t bad = 0, out = 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        if (!lp_unicode_scalar(cp)) continue;
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
    { lp_id ids[3]; lp_id_codepoint('0', &ids[0]); lp_id_codepoint('1', &ids[1]); ids[2]=ids[1];
      uint64_t old_m[]={1,2}, new_m[]={1,(uint64_t)lp_m_of(2,LP_SAID_TUPLE)};
      uint8_t old[160], fresh[160], enriched[160];
      size_t ol=lp_ewkb_runs(ids,old_m,2,old,sizeof old), nl=lp_ewkb_runs(ids,new_m,2,fresh,sizeof fresh);
      size_t size=lp_ewkb_enrich_path(lp_path_of(old,ol),lp_path_of(fresh,nl),enriched,sizeof enriched);
      lp_vertex vertices[3];
      CHECK(size && lp_path_vertices(enriched,size,vertices,3)==2 && vertices[1].run==2 && vertices[1].said==LP_SAID_TUPLE,
            "a missing annotation is enriched without changing a repeated child");
      uint8_t replay[160]; size_t rs=lp_ewkb_enrich_path(lp_path_of(enriched,size),lp_path_of(old,ol),replay,sizeof replay);
      CHECK(rs==size && !memcmp(replay,enriched,size),"replay cannot erase an existing annotation");
      uint64_t split_m[]={1,(uint64_t)lp_m_of(1,LP_SAID_TUPLE),(uint64_t)lp_m_of(1,LP_SAID_TUPLE)};
      nl=lp_ewkb_runs(ids,split_m,3,fresh,sizeof fresh);
      CHECK(lp_ewkb_enrich_path(lp_path_of(old,ol),lp_path_of(fresh,nl),replay,sizeof replay)==size && !memcmp(replay,enriched,size),
            "different run splits preserve the same ordered occurrences");
      uint64_t conflict_m[]={1,(uint64_t)lp_m_of(2,LP_SAID_RECORD)};
      nl=lp_ewkb_runs(ids,conflict_m,2,fresh,sizeof fresh);
      CHECK(!lp_ewkb_enrich_path(lp_path_of(enriched,size),lp_path_of(fresh,nl),NULL,0),"distinct nonzero annotations conflict");
      conflict_m[1]=lp_m_full(2,LP_SAID_TUPLE,LP_OUTCOME_LOSS,0);
      nl=lp_ewkb_runs(ids,conflict_m,2,fresh,sizeof fresh);
      CHECK(!lp_ewkb_enrich_path(lp_path_of(enriched,size),lp_path_of(fresh,nl),NULL,0),"an affirmation is not an absent outcome");
      conflict_m[1]=lp_m_full(2,LP_SAID_TUPLE,LP_OUTCOME_WIN,1);
      nl=lp_ewkb_runs(ids,conflict_m,2,fresh,sizeof fresh);
      CHECK(!lp_ewkb_enrich_path(lp_path_of(enriched,size),lp_path_of(fresh,nl),NULL,0),"positions cannot be rewritten");
      conflict_m[1]=lp_m_full(3,LP_SAID_TUPLE,0,0); nl=lp_ewkb_runs(ids,conflict_m,2,fresh,sizeof fresh);
      CHECK(!lp_ewkb_enrich_path(lp_path_of(enriched,size),lp_path_of(fresh,nl),NULL,0),"multiplicity remains identity data");
      lp_id reverse[]={ids[1],ids[0]}; nl=lp_ewkb_runs(reverse,new_m,2,fresh,sizeof fresh);
      CHECK(!lp_ewkb_enrich_path(lp_path_of(enriched,size),lp_path_of(fresh,nl),NULL,0),"child order remains identity data");
    }
    nested_geometry();
    DONE("geometry");
}
