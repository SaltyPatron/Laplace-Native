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
static int ring(void *context,size_t node,size_t *cursor,size_t *out,size_t *distance){
    *distance=1;if((*cursor)++)return 1;
    *out=(node+1)%(*(size_t *)context);return 0;
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
    size_t components[6], groups=99;
    g.error=0;
    CHECK(lp_graph_components(6,child,&g,components,&groups) && groups==6,"DAG components stay distinct");
    for(size_t i=0;i<g.count;i++)for(size_t edge=g.offsets[i];edge<g.offsets[i+1];edge++)
        CHECK(components[g.children[edge]]<components[i],"component dependencies precede consumers");
    const size_t joined_offsets[]={0,1,2,4,5,6,6},joined_edges[]={1,0,1,3,4,3};
    Graph joined={6,joined_offsets,joined_edges,0};
    CHECK(lp_graph_components(6,child,&joined,components,&groups) && groups==4,"cycles coalesce into logical groups");
    CHECK(components[0]==components[1] && components[3]==components[4] && components[0]!=components[3],"disconnected cycles stay separate");
    CHECK(components[1]<components[2] && components[3]<components[2],"both provider groups precede their consumer");
    size_t self_offsets[]={0,2,2},self_edges[]={0,1};Graph self={2,self_offsets,self_edges,0};
    CHECK(lp_graph_components(2,child,&self,components,&groups) && groups==2 && components[1]<components[0],"self reference retains external dependency");
    deep=malloc(count*sizeof *deep);
    CHECK(deep,"allocate deep component result");
    if(deep){
        CHECK(lp_graph_components(count,reverse_chain,&count,deep,&groups) && groups==count,"deep components use no C recursion");
        CHECK(deep[0]==count-1 && deep[count-1]==0,"deep providers precede consumers");
        CHECK(lp_graph_components(count,ring,&count,deep,&groups) && groups==1,"deep cycle is one logical component");
        CHECK(deep[0]==0 && deep[count-1]==0,"deep cycle includes every member");
        free(deep);
    }
    uint32_t random=0x427169e3;
    for(size_t trial=0;trial<128;trial++){
        size_t offsets[13],edges[144],used=0,part[12];bool reaches[12][12]={{0}};
        for(size_t i=0;i<12;i++){
            offsets[i]=used;reaches[i][i]=true;
            for(size_t j=0;j<12;j++){
                random=random*1664525u+1013904223u;
                if(random%11==0){edges[used++]=j;reaches[i][j]=true;}
            }
        }
        offsets[12]=used;Graph fixture={12,offsets,edges,0};
        for(size_t k=0;k<12;k++)for(size_t i=0;i<12;i++)for(size_t j=0;j<12;j++)
            reaches[i][j]|=reaches[i][k] && reaches[k][j];
        CHECK(lp_graph_components(12,child,&fixture,part,&groups),"varied component graph");
        for(size_t i=0;i<12;i++)for(size_t j=0;j<12;j++)
            CHECK((part[i]==part[j])==(reaches[i][j] && reaches[j][i]),"components agree with independent mutual reachability");
        for(size_t i=0;i<12;i++)for(size_t edge=offsets[i];edge<offsets[i+1];edge++)
            CHECK(part[edges[edge]]<=part[i],"varied external dependencies precede consumers");
    }
    cycle_edges[0]=2;
    CHECK(!lp_graph_components(2,child,&cycle,components,&groups) && errno==EINVAL,"component reader rejects invalid child");
    g.error=1;
    CHECK(!lp_graph_components(6,child,&g,components,&groups) && errno==EINVAL,"component reader failure propagates");
    CHECK(lp_graph_components(0,child,&g,NULL,&groups) && groups==0,"empty component graph");
    CHECK(!lp_graph_components(1,child,&g,NULL,&groups) && errno==EINVAL,"component output required");
    CHECK(!lp_graph_components(0,child,&g,NULL,NULL) && errno==EINVAL,"component count required");
    DONE("dag");
}
