/* What differs between POSIX and Windows, in one place: a file mapped read-only, a lock that needs no setup call,
 * reading a line, and splitting a line. Everything else in Laplace-Native is standard C17. */
#ifndef LP_PLATFORM_H
#define LP_PLATFORM_H
#include <stddef.h>
#include <stdio.h>

/* The whole file mapped read-only and shared, its size in *size when size is given; NULL when it cannot be opened or
 * mapped, is empty, or (want not 0) is not exactly want bytes. */
const void *lp_map_file(const char *path, size_t want, size_t *size);
void lp_unmap_file(const void *p, size_t size);

#ifdef _WIN32
typedef struct { void *state; } lp_lock;            /* an SRWLOCK: one pointer, all zero when unlocked */
#define LP_LOCK_INIT { 0 }
ptrdiff_t lp_getline(char **line, size_t *cap, FILE *f);
#define getline lp_getline
#define strtok_r strtok_s                             /* the same contract under Microsoft's name */
#else
#include <pthread.h>
typedef pthread_mutex_t lp_lock;
#define LP_LOCK_INIT PTHREAD_MUTEX_INITIALIZER
#endif
void lp_lock_take(lp_lock *l);
void lp_lock_give(lp_lock *l);

#endif
