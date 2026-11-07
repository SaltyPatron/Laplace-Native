/* Native bootstrap entry point. Engine's `laplace tier0` calls these same APIs. */
#include "laplace/tier0.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv){
    if (argc == 3 && !strcmp(argv[1], "--shards")) {
        if (!lp_tier0_split_file(argv[2])) { perror("Tier-0 shard split"); return 1; }
        for (unsigned s = 0; s < LP_TIER0_SHARD_COUNT; s++) {
            char spath[4096];
            if (lp_tier0_shard_path(argv[2], (lp_tier0_shard)s, spath, sizeof spath))
                printf("%s: %u records (%s shard)\n", spath,
                       lp_tier0_shard_count((lp_tier0_shard)s),
                       lp_tier0_shard_name((lp_tier0_shard)s));
        }
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: laplace-tier0 UCD_ROOT OUTPUT\n"
                        "       laplace-tier0 --shards EXISTING_TIER0_BIN\n");
        return 2;
    }
    lp_tier0_record *records = malloc((size_t)LP_TIER0_COUNT * sizeof *records);
    if (!records) { perror("Tier-0 allocation"); return 1; }
    bool ok = lp_tier0_from_ucd(argv[1], records);
    if (ok) ok = lp_tier0_write(argv[2], records);
    if (!ok) { perror("Tier-0 generation"); free(records); return 1; }
    uint8_t digest[32]; char hex[65];
    lp_tier0_fingerprint(records, digest); lp_hex(digest, 32, hex);
    printf("%s: %u Unicode codepoint records (+ ascii/utf/cjk/ext shards), sorted by ID\nBLAKE3-256 %s\n",
           argv[2], LP_TIER0_COUNT, hex);
    free(records); return 0;
}
