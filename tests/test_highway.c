/* The highway: the types as the resources list them, read back from the records by slot and by ID, and the edges. */
#include "laplace/laplace.h"
#include "laplace/highway.h"
#include "check.h"
#include <string.h>

static void banks_check(const lp_highway *h, const lp_tier0_record *t0){
    char manifest[4096]; snprintf(manifest, sizeof manifest, "%s/banks.tsv", LAPLACE_MANIFEST);
    FILE *registry = fopen(manifest, "r"); CHECK(registry, "open the canonical bank manifest");
    if (registry) {
        char line[1024]; size_t count = 0;
        while (fgets(line, sizeof line, registry)) {
            if (line[0] == '#' || !strncmp(line, "bank\t", 5) || line[0] == '\n' || line[0] == '\r') continue;
            char name[32], list[32], group[16], carrier[16], encoding[8], extra; unsigned width, offset;
            int fields = sscanf(line, "%31s %31s %15s %15s %u %u %7s %c", name, list, group, carrier, &width, &offset, encoding, &extra);
            CHECK(fields == 7, "bank manifest row has seven fields"); if (fields != 7) continue;
            const lp_bank *bank = lp_highway_bank(h, name); count++;
            CHECK(bank && bank->list && !strcmp(bank->list->name, list) && !strcmp(bank->group, group)
                  && !strcmp(bank->carrier, carrier) && bank->width == width && bank->offset == offset
                  && bank->flags == !strcmp(encoding, "flags"), "%s artifact metadata matches the manifest", name);
        }
        CHECK(!ferror(registry) && count == h->nbanks, "artifact contains exactly the declared banks"); fclose(registry);
    }
    const char *required[] = { "kind", "upos", "lexfile", "deprel", "vnrole" };
    for (size_t i = 0; i < sizeof required / sizeof *required; i++)
        CHECK(lp_highway_bank(h, required[i]), "required carrier bank %s", required[i]);
    lp_text *text = lp_text_new(t0); CHECK(text, "canonical text decomposer");
    if (!text) return;
    lp_mask occupied = 0, base = 0;
    for (size_t i = 0; i < h->nbanks; i++) {
        const lp_bank *bank = h->bank + i; lp_mask field = lp_mask_field(bank);
        CHECK(field && !(occupied & field), "%s has a disjoint field in 64 bits", bank->name); occupied |= field;
        CHECK(bank->list && bank->list->count, "%s has frozen identifier slots", bank->name);
        if (bank->list && bank->list->count) CHECK(lp_mask_put(&base, bank, bank->list->count - 1), "pack %s's last slot", bank->name);
    }
    for (size_t i = 0; i < h->nbanks; i++) {
        const lp_bank *bank = h->bank + i; if (!bank->list) continue;
        lp_mask field = lp_mask_field(bank);
        for (uint32_t slot = 0; slot < bank->list->count; slot++) {
            lp_mask value = base;
            CHECK(lp_mask_clear(&value, bank) && lp_mask_put(&value, bank, slot)
                  && lp_mask_get(value, bank) == slot && lp_mask_has(value, bank, slot), "%s slot %u round trip", bank->name, slot);
            CHECK((value & ~field) == (base & ~field), "%s slot %u leaves every other bank unchanged", bank->name, slot);
            const lp_tier0_record *record = lp_highway_at(h, bank->list, slot);
            lp_mask identity = 0;
            CHECK(record && lp_mask_add_id(h, &identity, &record->id) && lp_mask_has(identity, bank, slot), "%s slot %u resolved by its content ID", bank->name, slot);
        }
        lp_mask unchanged = base;
        CHECK(!lp_mask_put(&unchanged, bank, bank->list->count) && unchanged == base, "%s rejects out-of-range slots without mutation", bank->name);
        lp_mask zero = base; CHECK(lp_mask_clear(&zero, bank) && lp_mask_get(zero, bank) == -1, "%s has an unambiguous absent value", bank->name);

        char path[4096]; snprintf(path, sizeof path, "%s/slots/%s.tsv", LAPLACE_MANIFEST, bank->list->name);
        FILE *f = fopen(path, "r"); CHECK(f, "open frozen registry %s", path); if (!f) continue;
        char line[2048]; uint32_t seen = 0;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || !strncmp(line, "slot\t", 5)) continue;
            char *s = strtok(line, "\t\r\n"), *hex = strtok(NULL, "\t\r\n");
            char *status = strtok(NULL, "\t\r\n"), *key = strtok(NULL, "\t\r\n");
            CHECK(s && hex && status && key, "%s registry row has slot, ID, status, key", bank->name);
            if (!s || !hex || !status || !key) continue;
            uint32_t slot = (uint32_t)strtoul(s, NULL, 10); lp_id expected, frozen;
            CHECK(slot == seen++, "%s slots stay contiguous and frozen", bank->name);
            const lp_tier0_record *record = lp_highway_at(h, bank->list, slot);
            bool valid = strlen(hex) == 32 && lp_id_unhex(hex, &frozen) && lp_text_id(text, key, strlen(key), &expected);
            CHECK(valid && record && lp_id_eq(&expected, &frozen) && lp_id_eq(&expected, &record->id),
                  "%s slot %u is the canonical composition of %s", bank->name, slot, key);
            lp_ref composed = lp_text_decompose(text, (const uint8_t *)key, strlen(key), NULL, NULL);
            CHECK(record && !memcmp(record->m, composed.c.m, sizeof composed.c.m), "%s slot %u has the canonical centroid", bank->name, slot);
        }
        CHECK(!ferror(f) && seen == bank->list->count, "%s registry covers every slot", bank->name); fclose(f);
    }
    lp_mask merged = UINT64_C(0xaaaaaaaaaaaaaaaa), before = merged;
    CHECK(lp_mask_merge(h, base, base, &merged) && merged == base, "merging identical carriers is idempotent");
    const lp_bank *upos = lp_highway_bank(h, "upos"), *kind = lp_highway_bank(h, "kind");
    if (upos && kind) {
        lp_mask a = 0, b = 0; lp_mask_put(&a, upos, 0); lp_mask_put(&b, upos, 1);
        merged = before; CHECK(!lp_mask_merge(h, a, b, &merged) && merged == before, "conflicting enum values cannot be ORed into a third value");
        const lp_tier0_record *other = lp_highway_at(h, upos->list, 1); lp_mask original = a;
        CHECK(other && !lp_mask_add_id(h, &a, &other->id) && a == original, "adding a conflicting identifier preserves the original enum");
        a = b = 0; lp_mask_put(&a, kind, LP_KIND_CLAIM); lp_mask_put(&b, kind, LP_KIND_RECORD);
        CHECK(lp_mask_merge(h, a, b, &merged) && lp_mask_has(merged, kind, LP_KIND_CLAIM)
              && lp_mask_has(merged, kind, LP_KIND_RECORD) && lp_mask_get(merged, kind) == -2, "row roles union without losing either flag");
    }
    if (occupied != UINT64_MAX) {
        lp_mask unknown = ~occupied & (UINT64_C(0) - ~occupied);
        merged = before; CHECK(!lp_mask_merge(h, base, unknown, &merged) && merged == before, "reserved carrier bits are rejected");
    }
    lp_list edge_list = { .count = 64 }; lp_bank edge = { .width = 64, .list = &edge_list, .flags = true };
    lp_mask boundary = 0; CHECK(lp_mask_put(&boundary, &edge, 63) && boundary == (UINT64_C(1) << 63)
                              && lp_mask_get(boundary, &edge) == 63, "bit 63 has no undefined shift");
    edge.offset = 1; CHECK(!lp_mask_field(&edge) && !lp_mask_put(&boundary, &edge, 0), "overflowing field is rejected");
    CHECK(!lp_mask_field(NULL) && !lp_mask_put(NULL, upos, 0), "missing mask descriptors are rejected");
    lp_text_free(text);
}

int main(void){
    const lp_tier0_record *t0 = lp_tier0_map(LAPLACE_TIER0);
    CHECK(t0, "map the complete Tier-0 scalar cache");
    const lp_highway *h = lp_highway_map(NULL);
    CHECK(h != NULL, "cannot map the highway at %s (generate it with: laplace highway)", lp_highway_path());
    if (!h || !t0) DONE("highway");
    const lp_list *upos = lp_highway_list(h, "upos"), *deprel = lp_highway_list(h, "Universal dependency relation"), *ili = lp_highway_list(h, "ili");
    CHECK(upos && upos->count == 17, "UD's parts of speech: %u", upos ? upos->count : 0);
    CHECK(deprel && deprel->count >= 37, "UD's relations by the list's name as it is said");
    lp_id noun; lp_id_codepoints_utf8("NOUN", 4, &noun);
    int64_t s = lp_highway_slot(h, upos, &noun);
    CHECK(s == 7, "NOUN is UD's 8th part of speech: slot %lld", (long long)s);
    const lp_tier0_record *r = lp_highway_at(h, upos, 7);
    CHECK(r && !memcmp(&r->id, &noun, 16) && r->rank == 7, "the record of slot 7 is NOUN's content");
    CHECK(lp_highway_at(h, upos, 17) == NULL, "past the list's end");
    CHECK(lp_highway_slot(h, deprel, &noun) == -1, "NOUN is not a relation");
    CHECK(ili && ili->count > 100000, "CILI's concepts: %u", ili ? ili->count : 0);
    const lp_edge *e; size_t n = lp_highway_edges(h, "pbroleset", 0, "vnclass", &e);
    const lp_list *pb = lp_highway_list(h, "pbroleset"), *vn = lp_highway_list(h, "vnclass");
    CHECK(pb && vn, "PropBank's rolesets and VerbNet's classes are listed");
    size_t any = 0; for (uint32_t i = 0; pb && i < pb->count && !any; i++) any = lp_highway_edges(h, "pbroleset", i, "vnclass", &e);
    CHECK(any > 0, "some roleset maps to a class");
    if (any) { CHECK(e[0].to < vn->count, "an edge lands inside the list it names"); }
    (void)n;
    const lp_bank *kb = lp_highway_bank(h, "kind"), *ub = lp_highway_bank(h, "upos"), *db = lp_highway_bank(h, "deprel");
    CHECK(kb && kb->list && kb->list->count == 4 && kb->flags && !strcmp(kb->carrier, "row") && kb->width == 8, "kind: four named row roles in the flag field");
    CHECK(ub && ub->list == upos && !strcmp(ub->group, "lexical") && !strcmp(ub->carrier, "entity"), "upos: a lexical bank on the entity");
    CHECK(db && !strcmp(db->group, "structural") && !strcmp(db->carrier, "occurrence"), "deprel: a structural bank on the occurrence");
    const lp_bank *fb = lp_highway_bank(h, "wnframe"); const lp_list *wnframe = lp_highway_list(h, "wnframe");     /* WordNet's verb frames: what a verb sense is used in */
    CHECK(fb && !strcmp(fb->group, "lexical") && !strcmp(fb->carrier, "entity") && fb->width == 6, "wnframe: a lexical enum field on the entity");
    CHECK(wnframe && fb && fb->list == wnframe && wnframe->count == 35, "WordNet's 35 identifier slots");
    int32_t bit; const lp_bank *nb = lp_highway_bank_of(h, &noun, &bit);
    CHECK(nb == ub && bit == 7, "NOUN is frozen slot 7 of the upos field");
    lp_id none; memset(&none, 0x5a, 16);
    CHECK(lp_highway_contains(h, &noun), "membership includes a listed identifier");
    CHECK(!lp_highway_contains(h, &none) && !lp_highway_contains(NULL, &noun)
          && !lp_highway_contains(h, NULL), "membership rejects absent IDs and mappings");
    const lp_list *memberships[] = {upos, deprel, ili, pb, vn};
    for (size_t i = 0; i < sizeof memberships / sizeof *memberships; i++) {
        const lp_list *list = memberships[i];
        const lp_tier0_record *first = list ? lp_highway_at(h, list, 0) : NULL;
        CHECK(first && lp_highway_contains(h, &first->id), "membership spans every tested namespace %zu", i);
    }
    CHECK(lp_highway_bank_of(h, &none, &bit) == NULL && bit == -1, "no type, no bank");
    uint8_t fp[32]; lp_highway_fingerprint(h, fp); int zero = 1; for (int i = 0; i < 32; i++) zero &= fp[i] == 0;
    CHECK(!zero, "a fingerprint");
    banks_check(h, t0);
    DONE("highway");
}
