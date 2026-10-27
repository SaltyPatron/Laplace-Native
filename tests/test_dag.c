#include "check.h"
#include <errno.h>

typedef struct { size_t count; const size_t *offsets, *children; int error; } Graph;
static int child(void *context, size_t node, size_t *cursor, size_t *out, size_t *distance){
    *distance = 1; Graph *g = context;
    if (g->error) return -1;
    size_t at = g->offsets[node] + (*cursor)++;
    if (at == g->offsets[node + 1]) return 1;
    *out = g->children[at]; return 0;
}
static int chain(void *context, size_t node, size_t *cursor, size_t *out, size_t *distance){
    *distance = 1; (void)context;
    if (!node || (*cursor)++) return 1;
    *out = node - 1; return 0;
}
static int reverse_chain(void *context, size_t node, size_t *cursor, size_t *out, size_t *distance){
    *distance = 1; size_t count = *(size_t *)context;
    if (node + 1 == count || (*cursor)++) return 1;
    *out = node + 1; return 0;
}
static int cohort(void *context, size_t node, size_t *cursor, size_t *out, size_t *distance){
    if (node >= 3 || (*cursor)++) return 1;
    *out = node + 1;
    *distance = context ? SIZE_MAX : (node % 2 == (node + 1) % 2 ? 0 : 1);
    return 0;
}
static int same_owner(void *context, size_t node, size_t *cursor, size_t *out, size_t *distance){
    int result = chain(context,node,cursor,out,distance);
    *distance = 0; return result;
}
int main(void){
    const size_t offsets[] = {0,2,4,4,5,5,6}, edges[] = {3,1,2,2,2,4};
    Graph g = {6,offsets,edges,0}; size_t levels[6];
    CHECK(lp_dag_levels(6,child,&g,levels), "shared descendants and repeated edges");
    const size_t expected[] = {3,2,1,2,1,2};
    CHECK(!memcmp(levels,expected,sizeof levels), "dependency depths ignore input order");
    size_t count = 200000, *deep = malloc(count * sizeof *deep);
    CHECK(deep, "allocate deep graph result");
    if (deep) {
        CHECK(lp_dag_levels(count,reverse_chain,&count,deep), "deep iterative traversal");
        CHECK(deep[0] == count && deep[count-1] == 1, "no recursion/depth cutoff");
        CHECK(lp_dag_levels(count,chain,NULL,deep), "already ordered chain");
        CHECK(deep[0] == 1 && deep[count-1] == count, "same depths in reverse storage order");
        free(deep);
    }
    CHECK(lp_dag_levels(4,same_owner,NULL,levels), "atomic owner may share a wave");
    CHECK(levels[0] == 1 && levels[3] == 1, "local dependencies coalesce");
    CHECK(lp_dag_levels(4,cohort,NULL,levels) && levels[0] == 4, "cross-owner edges require barriers");
    CHECK(!lp_dag_levels(4,cohort,&g,levels) && errno == EOVERFLOW, "distance overflow rejected");
    size_t cycle_offsets[] = {0,1,2}, cycle_edges[] = {1,0};
    Graph cycle = {2,cycle_offsets,cycle_edges,0};
    CHECK(!lp_dag_levels(2,child,&cycle,levels) && errno == ELOOP, "cycle rejected");
    cycle_edges[0] = 0;
    CHECK(!lp_dag_levels(2,child,&cycle,levels) && errno == ELOOP, "self-edge rejected");
    cycle_edges[0] = 2;
    CHECK(!lp_dag_levels(2,child,&cycle,levels) && errno == EINVAL, "out-of-range child rejected");
    g.error = 1;
    CHECK(!lp_dag_levels(6,child,&g,levels), "reader failure rejected");
    CHECK(lp_dag_levels(0,child,&g,NULL), "empty graph");
    CHECK(!lp_dag_levels(1,child,&g,NULL) && errno == EINVAL, "missing output rejected");
    CHECK(!lp_dag_levels(0,NULL,NULL,NULL) && errno == EINVAL, "missing reader rejected");
    DONE("dag");
}
