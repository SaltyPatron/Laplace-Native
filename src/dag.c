#include "laplace/laplace.h"
#include <errno.h>
#include <stdlib.h>

typedef struct { size_t node, cursor, depth, pending; } DagFrame;
bool lp_dag_levels(size_t n, lp_dag_child_reader reader, void *context, size_t *levels){
    if (!reader || (n && !levels)) { errno = EINVAL; return false; }
    if (n > SIZE_MAX / sizeof *levels) { errno = EOVERFLOW; return false; }
    if (!n) return true;
    memset(levels, 0, n * sizeof *levels);
    size_t capacity = n < 64 ? n : 64, used = 0;
    DagFrame *stack = malloc(capacity * sizeof *stack);
    if (!stack) { errno = ENOMEM; return false; }
    for (size_t node = 0; node < n; node++) {
        if (levels[node]) continue;
        stack[used++] = (DagFrame){.node = node, .depth = 1}; levels[node] = SIZE_MAX;
        while (used) {
            DagFrame *frame = &stack[used - 1]; size_t child, distance;
            int result = reader(context, frame->node, &frame->cursor, &child, &distance);
            if (result == 1) {
                size_t depth = frame->depth; levels[frame->node] = depth; used--;
                if (used) {
                    DagFrame *parent = &stack[used - 1];
                    if (parent->pending >= SIZE_MAX - depth) { errno = EOVERFLOW; goto fail; }
                    depth += parent->pending;
                    if (parent->depth < depth) parent->depth = depth;
                }
                continue;
            }
            if (result != 0 || child >= n) { errno = EINVAL; goto fail; }
            if (levels[child] == SIZE_MAX) { errno = ELOOP; goto fail; }
            if (levels[child]) {
                if (distance >= SIZE_MAX - levels[child]) { errno = EOVERFLOW; goto fail; }
                size_t depth = levels[child] + distance;
                if (frame->depth < depth) frame->depth = depth;
                continue;
            }
            frame->pending = distance;
            if (used == capacity) {
                size_t limit = SIZE_MAX / sizeof *stack;
                if (capacity == limit) { errno = EOVERFLOW; goto fail; }
                size_t grown = capacity > limit / 2 ? limit : capacity * 2;
                if (grown > n) grown = n;
                DagFrame *next = realloc(stack, grown * sizeof *stack);
                if (!next) { errno = ENOMEM; goto fail; }
                stack = next; capacity = grown;
            }
            stack[used++] = (DagFrame){.node = child, .depth = 1}; levels[child] = SIZE_MAX;
        }
    }
    free(stack); return true;
fail:
    free(stack); return false;
}

typedef struct { size_t node, cursor; } ComponentFrame;
bool lp_graph_components(size_t n,lp_dag_child_reader reader,void *context,size_t *components,size_t *count){
    if(!reader || !count || (n && !components)){errno=EINVAL;return false;}
    *count=0;if(!n)return true;
    if(n>SIZE_MAX/(3*sizeof(size_t)) || n>SIZE_MAX/sizeof(ComponentFrame)){errno=EOVERFLOW;return false;}
    size_t *storage=calloc(n,3*sizeof *storage);
    ComponentFrame *frames=malloc(n*sizeof *frames);
    if(!storage || !frames){free(storage);free(frames);errno=ENOMEM;return false;}
    size_t *index=storage,*low=storage+n,*active=storage+2*n;
    for(size_t i=0;i<n;i++)components[i]=SIZE_MAX;
    size_t clock=0,used=0,live=0,groups=0;
    for(size_t root=0;root<n;root++){
        if(index[root])continue;
        index[root]=low[root]=++clock;active[live++]=root;
        frames[used++]=(ComponentFrame){.node=root};
        while(used){
            ComponentFrame *frame=frames+used-1;size_t node=frame->node,child,distance;
            int result=reader(context,node,&frame->cursor,&child,&distance);
            if(result==0){
                if(child>=n){errno=EINVAL;goto components_fail;}
                if(!index[child]){
                    index[child]=low[child]=++clock;active[live++]=child;
                    frames[used++]=(ComponentFrame){.node=child};
                }else if(components[child]==SIZE_MAX && index[child]<low[node])low[node]=index[child];
                continue;
            }
            if(result!=1){errno=EINVAL;goto components_fail;}
            if(low[node]==index[node]){
                size_t member;
                do{member=active[--live];components[member]=groups;}while(member!=node);
                groups++;
            }
            used--;
            if(used && low[node]<low[frames[used-1].node])low[frames[used-1].node]=low[node];
        }
    }
    *count=groups;free(frames);free(storage);return true;
components_fail:
    free(frames);free(storage);return false;
}
