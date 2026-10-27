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
