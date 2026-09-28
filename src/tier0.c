/* Tier 0: the perf-cache of all 1,114,112 codepoints, memory-mapped read-only. */
#include "laplace/laplace.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(lp_tier0_record) == 64, "tier-0 records are 64 bytes");

const lp_tier0_record *lp_tier0_map(const char *path){
    int fd = open(path, O_RDONLY); struct stat st;
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size != (size_t)LP_NCP * sizeof(lp_tier0_record)) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0); close(fd);
    return p == MAP_FAILED ? NULL : (const lp_tier0_record *)p;
}
