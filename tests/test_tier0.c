/* Verify the installed Tier-0 artifact independently of the optimized identity and fingerprint paths.
 * A second artifact argument compares two independently generated tables byte for byte. */
#include "laplace/tier0.h"
#include "check.h"
#include "blake3.h"
#include <limits.h>
#include <math.h>
#include <fenv.h>

_Static_assert(sizeof(lp_tier0_record) == 64, "Tier-0 record layout changed");
_Static_assert(LP_TIER0_COUNT == 1112064u, "the cache must contain only Unicode scalars");

static void wall_regressions(void){
    lp_coord c = { { INT64_MIN, INT64_MIN, INT64_MIN, INT64_MIN } };
    CHECK(!lp_coord_inside(&c), "extreme coordinates must not wrap inside the wall");
    for (int axis = 0; axis < 4; axis++) {
        memset(&c, 0, sizeof c); c.m[axis] = INT64_C(1) << 53;
        CHECK(lp_coord_inside(&c), "positive wall on axis %d", axis);
        c.m[axis]++;
        CHECK(!lp_coord_inside(&c), "one grid unit beyond positive wall on axis %d", axis);
        c.m[axis] = -(INT64_C(1) << 53);
        CHECK(lp_coord_inside(&c), "negative wall on axis %d", axis);
        c.m[axis]--;
        CHECK(!lp_coord_inside(&c), "one grid unit beyond negative wall on axis %d", axis);
        c.m[axis] = INT64_MIN;
        CHECK(!lp_coord_inside(&c), "INT64_MIN on axis %d", axis);
        c.m[axis] = INT64_MAX;
        CHECK(!lp_coord_inside(&c), "INT64_MAX on axis %d", axis);
    }
    for (int axis = 0; axis < 4; axis++) c.m[axis] = INT64_C(1) << 52;
    CHECK(lp_coord_inside(&c), "four half axes lie exactly on the wall");
    c.m[3]++;
    CHECK(!lp_coord_inside(&c), "wall is checked in all four dimensions");
    double bad[4] = { NAN, 0, 0, 0 };
    CHECK(!lp_coord_of_xyzm(bad, &c), "NaN is not a coordinate");
    bad[0] = INFINITY;
    CHECK(!lp_coord_of_xyzm(bad, &c), "infinity is not a coordinate");
    bad[0] = 0x1p-54;
    CHECK(!lp_coord_of_xyzm(bad, &c), "half a fixed-point grid unit is rejected");
    c = (lp_coord){ { (INT64_C(1) << 52) - 1, 0, 0, 0 } };
    uint32_t grid[4]; lp_hilbert4_decode(lp_hilbert4(&c), grid);
    CHECK(grid[0] == 49151u && grid[1] == 32768u && grid[2] == 32768u && grid[3] == 32768u,
          "one fixed-point unit below a grid boundary stays below it");
}

static bool file_fingerprint(const char *path, uint8_t digest[32]){
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "cannot open Tier-0 artifact %s", path);
    if (!f) return false;
    blake3_hasher h; blake3_hasher_init(&h);
    uint8_t bytes[65536]; size_t n, total = 0;
    while ((n = fread(bytes, 1, sizeof bytes, f)) != 0) {
        blake3_hasher_update(&h, bytes, n); total += n;
    }
    bool ok = !ferror(f) && total == (size_t)LP_TIER0_COUNT * sizeof(lp_tier0_record);
    CHECK(ok, "Tier-0 artifact read failed or has the wrong size: %zu bytes", total);
    CHECK(fclose(f) == 0, "cannot close Tier-0 artifact %s", path);
    blake3_hasher_finalize(&h, digest, 32);
    return ok;
}

int main(int argc, char **argv){
    wall_regressions();
    const char *path = argc > 1 ? argv[1] : lp_tier0_path();
    const lp_tier0_record *t0 = lp_tier0_map(path);
    CHECK(t0 != NULL, "cannot map Tier-0 artifact %s", path);
    if (!t0) DONE("tier0");
    unsigned char *ranks = calloc(LP_TIER0_COUNT, 1);
    uint32_t *order = malloc((size_t)LP_TIER0_COUNT * sizeof *order);
    CHECK(ranks != NULL && order != NULL, "cannot allocate the rank-permutation check");
    if (!ranks || !order) { free(ranks); free(order); DONE("tier0"); }
    size_t ids_bad = 0, rank_bad = 0, coord_bad = 0, hilbert_bad = 0, pad_bad = 0;
    size_t scalar_count = 0, atom_bad = 0, reverse_bad = 0, point_bad = 0;
    size_t absent = 0, unsorted = 0;
    for (size_t i = 1; i < LP_TIER0_COUNT; i++)
        unsorted += memcmp(t0[i - 1].id.b, t0[i].id.b, 16) >= 0;
    for (uint32_t cp = 0; cp < LP_NCP; cp++) {
        if (!lp_unicode_scalar(cp)) continue;
        const lp_tier0_record *record = lp_tier0_at(t0, cp);
        scalar_count++;
        if (!record) { absent++; continue; }
        uint8_t utf8[4]; size_t len = lp_utf8_put(cp, utf8); lp_id expected;
        blake3_hasher h; blake3_hasher_init(&h);
        blake3_hasher_update(&h, utf8, len); blake3_hasher_finalize(&h, expected.b, 16);
        ids_bad += memcmp(expected.b, record->id.b, 16) != 0;
        if (record->rank >= LP_TIER0_COUNT) rank_bad++;
        else { rank_bad += ranks[record->rank]++ != 0; order[record->rank] = cp; }
        lp_coord c, decoded; memcpy(c.m, record->m, sizeof c.m);
        double xyzm[4]; lp_coord_xyzm(&c, xyzm);
        coord_bad += !lp_coord_inside(&c) || !lp_coord_of_xyzm(xyzm, &decoded)
                  || memcmp(&c, &decoded, sizeof c) != 0;
        uint8_t ewkb[37] = { 0 }; const uint8_t *vertex = NULL; uint32_t type;
        size_t encoded = lp_ewkb_coord(&c, ewkb, sizeof ewkb);
        memcpy(&type, ewkb + 1, sizeof type);
        point_bad += encoded != sizeof ewkb || ewkb[0] != 1 || type != UINT32_C(0xc0000001)
                  || lp_ewkb_vertices(ewkb, encoded, &vertex) != 1
                  || memcmp(vertex, xyzm, sizeof xyzm) != 0;
        hilbert_bad += lp_hilbert4(&c) != record->hilbert;
        pad_bad += record->pad != 0;
        lp_ref atom = lp_ref_atom(t0, cp);
        atom_bad += atom.tier != 0 || memcmp(atom.id.b, expected.b, 16) != 0
                 || memcmp(atom.c.m, c.m, sizeof c.m) != 0;
        reverse_bad += lp_tier0_codepoint(t0, &expected) != (int64_t)cp;
    }
    free(ranks);
    CHECK(ids_bad == 0, "%zu IDs differ from BLAKE3 of their UTF-8 bytes", ids_bad);
    CHECK(rank_bad == 0, "%zu invalid or duplicate DUCET ranks", rank_bad);
    CHECK(coord_bad == 0, "%zu invalid single-point 4D coordinates", coord_bad);
    CHECK(point_bad == 0, "%zu coordinates fail the POINT ZM byte-exact round trip", point_bad);
    CHECK(hilbert_bad == 0, "%zu Hilbert values differ from the coordinates", hilbert_bad);
    CHECK(pad_bad == 0, "%zu records have nondeterministic padding", pad_bad);
    CHECK(atom_bad == 0, "%zu atoms differ from their cached ID or coordinate", atom_bad);
    CHECK(reverse_bad == 0, "%zu IDs resolve to the wrong codepoint", reverse_bad);
    CHECK(scalar_count == 1112064u, "wrong scalar count: %zu", scalar_count);
    CHECK(absent == 0, "%zu Unicode scalars are missing", absent);
    CHECK(unsorted == 0, "%zu adjacent IDs are not strictly increasing", unsorted);
    CHECK(lp_tier0_at(t0, 0xd800u) == NULL && lp_tier0_at(t0, 0xdfffu) == NULL,
          "surrogates are not Tier-0 atoms");
    CHECK(lp_tier0_at(t0, LP_NCP) == NULL && lp_tier0_at(NULL, 'a') == NULL,
          "invalid codepoints and unknown mappings are rejected");

    if (!absent && !rank_bad) {
        lp_tier0_record *rebuilt = malloc((size_t)LP_TIER0_COUNT * sizeof *rebuilt);
        CHECK(rebuilt != NULL, "cannot allocate the full native cache rebuild");
        if (rebuilt) {
            for (int pass = 0; pass < 2; pass++) {
                int rounding = fegetround();
                CHECK(fesetround(pass ? FE_DOWNWARD : FE_UPWARD) == 0, "set caller rounding mode");
                int requested = fegetround();
                bool built = lp_tier0_build(order, LP_TIER0_COUNT, rebuilt);
                CHECK(built, "full native scalar cache generation %d", pass);
                CHECK(fegetround() == requested, "generation preserves its caller's rounding mode");
                CHECK(fesetround(rounding) == 0, "restore caller rounding mode");
                if (built) CHECK(memcmp(t0, rebuilt, (size_t)LP_TIER0_COUNT * sizeof *t0) == 0,
                                 "native generation %d differs from the UCD-generated artifact", pass);
            }
            uint32_t first = order[0]; order[0] = 0xd800u;
            CHECK(!lp_tier0_build(order, LP_TIER0_COUNT, rebuilt), "generation rejects surrogates");
            order[0] = order[1];
            CHECK(!lp_tier0_build(order, LP_TIER0_COUNT, rebuilt), "generation rejects duplicate scalars");
            order[0] = first;
            CHECK(!lp_tier0_build(order, LP_TIER0_COUNT - 1, rebuilt), "generation rejects incomplete orders");
            free(rebuilt);
        }
    }
    free(order);

    uint8_t mapped[32], streamed[32]; char hex[65];
    lp_tier0_fingerprint(t0, mapped);
    if (file_fingerprint(path, streamed))
        CHECK(memcmp(mapped, streamed, 32) == 0, "mapped and streamed BLAKE3 fingerprints differ");
    lp_hex(mapped, 32, hex); printf("tier0 BLAKE3-256: %s\n", hex);
    if (argc > 2) {
        const lp_tier0_record *other = lp_tier0_map(argv[2]);
        CHECK(other != NULL, "cannot map independently generated artifact %s", argv[2]);
        if (other) {
            CHECK(memcmp(t0, other, (size_t)LP_TIER0_COUNT * sizeof *t0) == 0,
                  "independent Tier-0 generations are not byte-identical");
            uint8_t second[32]; lp_tier0_fingerprint(other, second);
            CHECK(memcmp(mapped, second, 32) == 0, "independent Tier-0 fingerprints differ");
        }
    }
    DONE("tier0");
}
