/* Native bootstrap entry point. Engine's `laplace tier0` calls these same APIs. */
#include "laplace/tier0.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv){
    if (argc != 3) { fprintf(stderr, "usage: laplace-tier0 UCD_ROOT OUTPUT\n"); return 2; }
    lp_tier0_record *records = malloc((size_t)LP_TIER0_COUNT * sizeof *records);
    if (!records) { perror("Tier-0 allocation"); return 1; }
    bool ok = lp_tier0_from_ucd(argv[1], records);
    if (ok) ok = lp_tier0_write(argv[2], records);
    if (!ok) { perror("Tier-0 generation"); free(records); return 1; }
    uint8_t digest[32]; char hex[65];
    lp_tier0_fingerprint(records, digest); lp_hex(digest, 32, hex);
    printf("%s: %u Unicode scalar records, sorted by ID\nBLAKE3-256 %s\n", argv[2], LP_TIER0_COUNT, hex);
    free(records); return 0;
}
